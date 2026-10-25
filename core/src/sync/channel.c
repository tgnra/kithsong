#include "sync/channel.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/platform_util.h>

#include "sync/frame.h"

struct mc_channel {
  mc_socket *sock;
  int initiator;
  int mode;
  int timeout_ms;
  int encrypted;
  int broken; /* a failed handshake or decrypt: refuse everything after */
  unsigned char preamble[MC_PREAMBLE_LEN];
  mc_noise_cipher send, recv;
  unsigned char peer_key[MC_NOISE_KEY_LEN];
  int has_peer_key;
  /* Separate buffers, so a received payload stays valid while replying. */
  unsigned char sbuf[MC_FRAME_MAX];
  unsigned char rbuf[MC_FRAME_MAX];
};

static void make_preamble(unsigned char out[MC_PREAMBLE_LEN], int mode) {
  out[0] = 'M';
  out[1] = 'A';
  out[2] = MC_PROTOCOL_VERSION;
  out[3] = (unsigned char)mode;
}

static mc_channel *channel_new(mc_socket *sock, int initiator, int mode) {
  mc_channel *ch = calloc(1, sizeof *ch);
  if (!ch) return NULL;
  ch->sock = sock;
  ch->initiator = initiator;
  ch->mode = mode;
  ch->timeout_ms = MC_CHANNEL_TIMEOUT_MS;
  make_preamble(ch->preamble, mode);
  return ch;
}

mc_status mc_channel_connect(const char *host, uint16_t port, int mode, int timeout_ms,
                             mc_channel **out) {
  if (!host || !out || (mode != MC_MODE_PAIRING && mode != MC_MODE_SESSION))
    return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_socket *sock;
  mc_status st = mc_socket_connect(host, port, timeout_ms, &sock);
  if (st != MC_OK) return st;
  mc_channel *ch = channel_new(sock, 1, mode);
  if (!ch) {
    mc_socket_close(sock);
    return MC_ERR_NOMEM;
  }
  if ((st = mc_socket_send_all(sock, ch->preamble, MC_PREAMBLE_LEN, ch->timeout_ms)) != MC_OK) {
    mc_channel_close(ch);
    return st;
  }
  *out = ch;
  return MC_OK;
}

mc_status mc_channel_accept(mc_listener *l, int timeout_ms, mc_channel **out) {
  if (!l || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_socket *sock;
  mc_status st = mc_listener_accept(l, timeout_ms, &sock);
  if (st != MC_OK || !sock) return st;

  unsigned char pre[MC_PREAMBLE_LEN];
  st = mc_socket_recv_exact(sock, pre, sizeof pre, MC_CHANNEL_TIMEOUT_MS);
  if (st != MC_OK) {
    mc_socket_close(sock);
    return st;
  }
  if (pre[0] != 'M' || pre[1] != 'A' || pre[2] != MC_PROTOCOL_VERSION ||
      (pre[3] != MC_MODE_PAIRING && pre[3] != MC_MODE_SESSION)) {
    mc_socket_close(sock);
    return MC_ERR_FORMAT;
  }
  mc_channel *ch = channel_new(sock, 0, pre[3]);
  if (!ch) {
    mc_socket_close(sock);
    return MC_ERR_NOMEM;
  }
  *out = ch;
  return MC_OK;
}

void mc_channel_close(mc_channel *ch) {
  if (!ch) return;
  mc_socket_close(ch->sock);
  mbedtls_platform_zeroize(ch, sizeof *ch); /* keys and any plaintext left in the buffers */
  free(ch);
}

int mc_channel_mode(const mc_channel *ch) {
  return ch->mode;
}

int mc_channel_is_initiator(const mc_channel *ch) {
  return ch->initiator;
}

int mc_channel_is_encrypted(const mc_channel *ch) {
  return ch->encrypted;
}

const char *mc_channel_peer_addr(const mc_channel *ch) {
  return mc_socket_peer(ch->sock);
}

void mc_channel_set_timeout(mc_channel *ch, int timeout_ms) {
  ch->timeout_ms = timeout_ms;
}

const unsigned char *mc_channel_peer_key(const mc_channel *ch) {
  return ch->has_peer_key ? ch->peer_key : NULL;
}

/* --- Handshakes --- */

/* Runs hs to completion, from whichever message is next. Handshake payloads are empty;
 * a peer's non-empty one is ignored. */
static mc_status run_handshake(mc_channel *ch, mc_noise_handshake *hs) {
  mc_status st = MC_OK;
  while (st == MC_OK && !mc_noise_handshake_complete(hs)) {
    size_t n;
    if (mc_noise_handshake_my_turn(hs)) {
      st = mc_noise_write_message(hs, NULL, 0, ch->sbuf, sizeof ch->sbuf, &n);
      if (st == MC_OK) st = mc_frame_send(ch->sock, ch->sbuf, n, ch->timeout_ms);
    } else {
      st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
      size_t ignored;
      if (st == MC_OK)
        st = mc_noise_read_message(hs, ch->rbuf, n, ch->sbuf, sizeof ch->sbuf, &ignored);
    }
  }
  return st;
}

/* After a complete handshake: keep the transport keys and the peer's key. */
static mc_status finish_handshake(mc_channel *ch, mc_noise_handshake *hs) {
  const unsigned char *rs = mc_noise_remote_static(hs);
  mc_status st = rs ? mc_noise_split(hs, &ch->send, &ch->recv) : MC_ERR_FORMAT;
  if (st == MC_OK) {
    memcpy(ch->peer_key, rs, MC_NOISE_KEY_LEN);
    ch->has_peer_key = 1;
    ch->encrypted = 1;
  }
  return st;
}

mc_status mc_channel_handshake_kk(mc_channel *ch, const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                  const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                  const unsigned char (*peers)[MC_NOISE_KEY_LEN], size_t n_peers,
                                  mc_channel_accept_fn accept, void *user) {
  if (!ch || !s_priv || !s_pub || (!peers && n_peers) || (ch->initiator && n_peers != 1))
    return MC_ERR_INVALID_ARG;
  if (ch->broken || ch->encrypted) return MC_ERR_INVALID_ARG;
  mc_noise_handshake hs;
  memset(&hs, 0, sizeof hs);
  mc_status st;

  if (ch->initiator) {
    if (accept && !accept(user, peers[0])) {
      st = MC_ERR_NOT_FOUND;
      goto done;
    }
    st = mc_noise_handshake_init(&hs, MC_NOISE_KK, 1, ch->preamble, MC_PREAMBLE_LEN, s_priv,
                                 s_pub, peers[0], NULL);
    if (st == MC_OK) st = run_handshake(ch, &hs);
  } else {
    /* Find the caller: the one candidate under which the first message authenticates. */
    size_t n, ignored;
    st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
    if (st != MC_OK) goto done;
    st = MC_ERR_NOT_FOUND;
    for (size_t i = 0; i < n_peers && st != MC_OK; i++) {
      if (mc_noise_handshake_init(&hs, MC_NOISE_KK, 0, ch->preamble, MC_PREAMBLE_LEN, s_priv,
                                  s_pub, peers[i], NULL) != MC_OK)
        break;
      if (mc_noise_read_message(&hs, ch->rbuf, n, ch->sbuf, sizeof ch->sbuf, &ignored) == MC_OK)
        st = MC_OK;
      else
        mc_noise_handshake_wipe(&hs);
    }
    if (st != MC_OK) goto done;
    /* Refuse before answering, so a refused caller never completes the handshake. */
    if (accept && !accept(user, mc_noise_remote_static(&hs))) {
      st = MC_ERR_NOT_FOUND;
      goto done;
    }
    st = run_handshake(ch, &hs);
  }
  if (st == MC_OK) st = finish_handshake(ch, &hs);

done:
  mc_noise_handshake_wipe(&hs);
  if (st != MC_OK) ch->broken = 1;
  return st;
}

mc_status mc_channel_handshake_xxpsk3(mc_channel *ch, const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                      const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                      const unsigned char psk[MC_NOISE_PSK_LEN]) {
  if (!ch || !s_priv || !s_pub || !psk || ch->broken || ch->encrypted) return MC_ERR_INVALID_ARG;
  mc_noise_handshake hs;
  mc_status st = mc_noise_handshake_init(&hs, MC_NOISE_XXPSK3, ch->initiator, ch->preamble,
                                         MC_PREAMBLE_LEN, s_priv, s_pub, NULL, psk);
  if (st == MC_OK) st = run_handshake(ch, &hs);
  if (st == MC_OK) st = finish_handshake(ch, &hs);
  mc_noise_handshake_wipe(&hs);
  if (st != MC_OK) ch->broken = 1;
  return st;
}

/* --- Messages --- */

mc_status mc_channel_send(mc_channel *ch, uint8_t type, const void *payload, size_t len) {
  if (!ch || !ch->encrypted || ch->broken || len > MC_CHANNEL_MAX_PAYLOAD || (!payload && len))
    return MC_ERR_INVALID_ARG;
  ch->sbuf[0] = type;
  if (len) memcpy(ch->sbuf + 1, payload, len);
  mc_status st = mc_noise_encrypt(&ch->send, NULL, 0, ch->sbuf, len + 1, ch->sbuf);
  if (st == MC_OK) st = mc_frame_send(ch->sock, ch->sbuf, len + 1 + MC_NOISE_TAG_LEN, ch->timeout_ms);
  mbedtls_platform_zeroize(ch->sbuf, len + 1 + MC_NOISE_TAG_LEN);
  return st;
}

mc_status mc_channel_recv(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                          size_t *len) {
  if (!ch || !type || !payload || !len || !ch->encrypted || ch->broken) return MC_ERR_INVALID_ARG;
  size_t n;
  mc_status st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
  if (st == MC_OK && n < 1 + MC_NOISE_TAG_LEN) st = MC_ERR_FORMAT; /* not even a type byte */
  if (st == MC_OK) st = mc_noise_decrypt(&ch->recv, NULL, 0, ch->rbuf, n, ch->rbuf);
  if (st != MC_OK) {
    ch->broken = 1;
    return st;
  }
  *type = ch->rbuf[0];
  *payload = ch->rbuf + 1;
  *len = n - 1 - MC_NOISE_TAG_LEN;
  return MC_OK;
}

mc_status mc_channel_send_clear(mc_channel *ch, uint8_t type, const void *payload, size_t len) {
  if (!ch || ch->encrypted || ch->broken || len > MC_FRAME_MAX - 1 || (!payload && len))
    return MC_ERR_INVALID_ARG;
  ch->sbuf[0] = type;
  if (len) memcpy(ch->sbuf + 1, payload, len);
  return mc_frame_send(ch->sock, ch->sbuf, len + 1, ch->timeout_ms);
}

mc_status mc_channel_recv_clear(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                                size_t *len) {
  if (!ch || !type || !payload || !len || ch->encrypted || ch->broken) return MC_ERR_INVALID_ARG;
  size_t n;
  mc_status st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
  if (st == MC_OK && n < 1) st = MC_ERR_FORMAT;
  if (st != MC_OK) {
    ch->broken = 1;
    return st;
  }
  *type = ch->rbuf[0];
  *payload = ch->rbuf + 1;
  *len = n - 1;
  return MC_OK;
}
