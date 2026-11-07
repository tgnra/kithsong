#include "sync/channel.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/platform_util.h>

#include "sync/frame.h"
#include "util/platform.h"

/* The most messages a send reads ahead (see watch): the receiver of files pipelines a few
 * FILE_REQs, so a CANCEL may follow them. */
#define MC_CHANNEL_AHEAD_MAX 8

struct mc_channel {
  mc_socket *sock;
  int initiator;
  int mode;
  int timeout_ms;
  int encrypted;
  int broken; /* a failed handshake, receive or decrypt: refuse everything after */
  int peer_cancelled; /* the peer's CANCEL came */
  /* mc_channel_cancel(): CANCEL_* bits, changed atomically, as the canceller's thread and
   * the channel's own meet here; the notice is armed before CANCEL_ARMED is set. */
  int cancel_state;
  uint8_t notice_type;
  unsigned char notice[MC_CHANNEL_NOTICE_MAX];
  size_t notice_len;
  /* The peer may send CANCEL (a notice is armed): each send first reads whatever has come,
   * as the peer that sent it may reset the connection soon, and on Windows that drops what
   * wasn't read. Messages read so wait in ahead[] (plaintext, oldest at ahead_head) for the
   * receives; a failure, in pending_st, for the receive after them. */
  int watch;
  unsigned char *ahead[MC_CHANNEL_AHEAD_MAX];
  size_t ahead_len[MC_CHANNEL_AHEAD_MAX];
  int ahead_head, ahead_count;
  mc_status pending_st;
  void (*idle)(void *user);
  void *idle_user;
  unsigned char preamble[MC_PREAMBLE_LEN];
  mc_noise_cipher send, recv;
  unsigned char peer_key[MC_NOISE_KEY_LEN];
  int has_peer_key;
  /* Separate buffers, so a received payload stays valid while replying. sbuf points into
   * sframe past room for the frame header, so a frame goes out in one send. */
  unsigned char sframe[MC_FRAME_HDR_LEN + MC_FRAME_MAX];
  unsigned char *sbuf;
  unsigned char rbuf[MC_FRAME_MAX];
};

#define CANCEL_ARMED 1   /* a notice to send */
#define CANCEL_SENDING 2 /* the channel's thread is in a send */
#define CANCEL_ASKED 4   /* mc_channel_cancel() was called */
#define CANCEL_SENT 8    /* the notice went out, and only the send side is shut down */

/* How long closing a channel whose notice went out waits for the peer to close first. */
#define DRAIN_MS 1000

static int g_default_timeout_ms = MC_CHANNEL_TIMEOUT_MS;

int mc_channel_default_timeout(void) {
  return g_default_timeout_ms;
}

void mc_channel_set_default_timeout(int timeout_ms) {
  g_default_timeout_ms = timeout_ms;
}

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
  ch->timeout_ms = g_default_timeout_ms;
  ch->sbuf = ch->sframe + MC_FRAME_HDR_LEN;
  make_preamble(ch->preamble, mode);
  return ch;
}

mc_status mc_channel_connect(const char *host, uint16_t port, int mode, int timeout_ms,
                             mc_channel **out) {
  if (!host || !out || (mode != MC_MODE_PAIRING && mode != MC_MODE_SESSION && mode != MC_MODE_REACH))
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

mc_status mc_channel_accept_raw(mc_listener *l, int timeout_ms, mc_channel **out) {
  if (!l || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_socket *sock;
  mc_status st = mc_listener_accept(l, timeout_ms, &sock);
  if (st != MC_OK || !sock) return st;
  mc_channel *ch = channel_new(sock, 0, 0);
  if (!ch) {
    mc_socket_close(sock);
    return MC_ERR_NOMEM;
  }
  ch->broken = 1; /* until the preamble checks out */
  *out = ch;
  return MC_OK;
}

mc_status mc_channel_read_preamble(mc_channel *ch) {
  if (!ch || ch->initiator || ch->mode) return MC_ERR_INVALID_ARG;
  unsigned char pre[MC_PREAMBLE_LEN];
  mc_status st = mc_socket_recv_exact(ch->sock, pre, sizeof pre, ch->timeout_ms);
  if (st != MC_OK) return st;
  if (pre[0] != 'M' || pre[1] != 'A' || pre[2] != MC_PROTOCOL_VERSION ||
      (pre[3] != MC_MODE_PAIRING && pre[3] != MC_MODE_SESSION && pre[3] != MC_MODE_REACH))
    return MC_ERR_FORMAT;
  ch->mode = pre[3];
  make_preamble(ch->preamble, ch->mode);
  ch->broken = 0;
  return MC_OK;
}

mc_status mc_channel_accept(mc_listener *l, int timeout_ms, mc_channel **out) {
  mc_status st = mc_channel_accept_raw(l, timeout_ms, out);
  if (st != MC_OK || !*out) return st;
  if ((st = mc_channel_read_preamble(*out)) != MC_OK) {
    mc_channel_close(*out);
    *out = NULL;
  }
  return st;
}

void mc_channel_close(mc_channel *ch) {
  if (!ch) return;
  /* Closing with the peer's messages unread would reset the connection, maybe before the
   * peer has read the notice. */
  if (__atomic_load_n(&ch->cancel_state, __ATOMIC_SEQ_CST) & CANCEL_SENT)
    mc_socket_drain(ch->sock, DRAIN_MS);
  for (int i = 0; i < ch->ahead_count; i++) {
    int k = (ch->ahead_head + i) % MC_CHANNEL_AHEAD_MAX;
    mbedtls_platform_zeroize(ch->ahead[k], ch->ahead_len[k]);
    free(ch->ahead[k]);
  }
  mc_socket_close(ch->sock);
  mbedtls_platform_zeroize(ch, sizeof *ch); /* keys and any plaintext left in the buffers */
  free(ch);
}

void mc_channel_abort(mc_channel *ch) {
  if (ch) mc_socket_shutdown(ch->sock);
}

/* Sends the armed notice if it fits in the socket's buffer now, then shuts down: only the
 * send side if it went, so the peer can read it (the channel's own thread stops receiving
 * by itself), else both. 1 if it went. By whichever thread mc_channel_cancel()'s bits leave
 * it to, the only one using the send side. */
static int send_notice(mc_channel *ch, int frame_whole) {
  size_t len = ch->notice_len;
  mc_status st = MC_ERR_NET;
  if (frame_whole && ch->encrypted && !ch->broken) {
    ch->sbuf[0] = ch->notice_type;
    memcpy(ch->sbuf + 1, ch->notice, len);
    st = mc_noise_encrypt(&ch->send, NULL, 0, ch->sbuf, len + 1, ch->sbuf);
    if (st == MC_OK) st = mc_frame_send_in_place(ch->sock, ch->sbuf, len + 1 + MC_NOISE_TAG_LEN, 0);
    mbedtls_platform_zeroize(ch->sbuf, len + 1 + MC_NOISE_TAG_LEN);
  }
  if (st != MC_OK) {
    mc_socket_shutdown(ch->sock);
    return 0;
  }
  __atomic_fetch_or(&ch->cancel_state, CANCEL_SENT, __ATOMIC_SEQ_CST);
  mc_socket_shutdown_send(ch->sock);
  return 1;
}

void mc_channel_arm_cancel(mc_channel *ch, uint8_t type, const void *payload, size_t len) {
  if (!ch || len > MC_CHANNEL_NOTICE_MAX || (!payload && len)) return;
  if (__atomic_load_n(&ch->cancel_state, __ATOMIC_SEQ_CST) & CANCEL_ARMED) return;
  ch->notice_type = type;
  if (len) memcpy(ch->notice, payload, len);
  ch->notice_len = len;
  ch->watch = 1;
  __atomic_fetch_or(&ch->cancel_state, CANCEL_ARMED, __ATOMIC_SEQ_CST);
}

int mc_channel_cancel(mc_channel *ch) {
  if (!ch) return 1;
  int was = __atomic_fetch_or(&ch->cancel_state, CANCEL_ASKED, __ATOMIC_SEQ_CST);
  if (was & CANCEL_ASKED) return 1; /* the first call did it, or left it to the send */
  if (!(was & CANCEL_ARMED)) {
    mc_socket_shutdown(ch->sock);
    return 1;
  }
  if (was & CANCEL_SENDING) return 0; /* the send under way sends it once its frame is out */
  return !send_notice(ch, 1); /* sent: a receive under way may still wait on the peer */
}

int mc_channel_readable(mc_channel *ch) {
  return ch && (ch->ahead_count || ch->pending_st || mc_socket_wait_readable(ch->sock, 0));
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

void mc_channel_set_idle(mc_channel *ch, void (*idle)(void *user), void *user) {
  ch->idle = idle;
  ch->idle_user = user;
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
      st = mc_noise_write_message(hs, NULL, 0, ch->sbuf, MC_FRAME_MAX, &n);
      if (st == MC_OK) st = mc_frame_send_in_place(ch->sock, ch->sbuf, n, ch->timeout_ms);
    } else {
      st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
      size_t ignored;
      if (st == MC_OK)
        st = mc_noise_read_message(hs, ch->rbuf, n, ch->sbuf, MC_FRAME_MAX, &ignored);
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
      if (mc_noise_read_message(&hs, ch->rbuf, n, ch->sbuf, MC_FRAME_MAX, &ignored) == MC_OK)
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

static mc_status recv_frame(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                           size_t *len, int timeout_ms);
static mc_status read_frame(mc_channel *ch, unsigned char *buf, size_t *n, int timeout_ms);

static int cancel_asked(mc_channel *ch) {
  return __atomic_load_n(&ch->cancel_state, __ATOMIC_SEQ_CST) & CANCEL_ASKED;
}

/* Before a send, while watching for the peer's CANCEL: reads the messages that have started
 * to arrive, while there's room for them. MC_ERR_CANCELLED if one was CANCEL; the others wait
 * for the receives. Reads into sbuf, which the send fills only after, so the last payload
 * received stays valid. */
static mc_status read_ahead(mc_channel *ch) {
  while (ch->watch && !ch->pending_st && ch->ahead_count < MC_CHANNEL_AHEAD_MAX &&
         mc_socket_wait_readable(ch->sock, 0)) {
    size_t n = 0;
    mc_status st = read_frame(ch, ch->sbuf, &n, ch->timeout_ms);
    unsigned char *msg = st == MC_OK ? malloc(n - MC_NOISE_TAG_LEN) : NULL;
    if (msg) memcpy(msg, ch->sbuf, n - MC_NOISE_TAG_LEN);
    mbedtls_platform_zeroize(ch->sbuf, n);
    if (st == MC_ERR_CANCELLED) {
      ch->broken = 1; /* the peer is gone */
      return st;
    }
    if (!msg) {
      /* The send goes ahead, as it would have without looking. */
      ch->pending_st = st == MC_OK ? MC_ERR_NOMEM : st;
      break;
    }
    int k = (ch->ahead_head + ch->ahead_count++) % MC_CHANNEL_AHEAD_MAX;
    ch->ahead[k] = msg;
    ch->ahead_len[k] = n - MC_NOISE_TAG_LEN;
  }
  return MC_OK;
}

/* After a send failed: a CANCEL the peer sent before closing may be waiting unread, with
 * whatever it sent before it. Read without waiting; what was read is dropped, so nothing
 * more is received. */
static mc_status check_cancelled(mc_channel *ch, mc_status st) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  for (int i = 0; i < 16 && !ch->broken && mc_socket_wait_readable(ch->sock, 0); i++)
    if (recv_frame(ch, &type, &payload, &len, 0) == MC_ERR_CANCELLED) return MC_ERR_CANCELLED;
  ch->broken = 1;
  return st;
}

mc_status mc_channel_send(mc_channel *ch, uint8_t type, const void *payload, size_t len) {
  if (!ch || !ch->encrypted || ch->broken || len > MC_CHANNEL_MAX_PAYLOAD || (!payload && len))
    return MC_ERR_INVALID_ARG;
  if (read_ahead(ch) == MC_ERR_CANCELLED) return MC_ERR_CANCELLED;
  if (__atomic_fetch_or(&ch->cancel_state, CANCEL_SENDING, __ATOMIC_SEQ_CST) & CANCEL_ASKED) {
    /* mc_channel_cancel() has the send side: it sends the notice and shuts down. */
    __atomic_fetch_and(&ch->cancel_state, ~CANCEL_SENDING, __ATOMIC_SEQ_CST);
    return MC_ERR_NET;
  }
  ch->sbuf[0] = type;
  if (len) memcpy(ch->sbuf + 1, payload, len);
  mc_status st = mc_noise_encrypt(&ch->send, NULL, 0, ch->sbuf, len + 1, ch->sbuf);
  if (st == MC_OK)
    st = mc_frame_send_in_place(ch->sock, ch->sbuf, len + 1 + MC_NOISE_TAG_LEN, ch->timeout_ms);
  mbedtls_platform_zeroize(ch->sbuf, len + 1 + MC_NOISE_TAG_LEN);
  int was = __atomic_fetch_and(&ch->cancel_state, ~CANCEL_SENDING, __ATOMIC_SEQ_CST);
  if ((was & CANCEL_ASKED) && (was & CANCEL_ARMED)) {
    /* mc_channel_cancel() came while this send held the send side, and left it to us. */
    send_notice(ch, st == MC_OK);
    return MC_ERR_NET;
  }
  if (st == MC_ERR_NET) st = check_cancelled(ch, st);
  return st;
}

/* Waits for a message to start coming, within the timeout, running idle meanwhile. */
static mc_status wait_idling(mc_channel *ch) {
  uint64_t t0 = mc_mono_ms();
  while (!mc_socket_wait_readable(ch->sock, MC_CHANNEL_IDLE_MS)) {
    if (cancel_asked(ch)) return MC_ERR_NET;
    if (ch->timeout_ms >= 0 && mc_mono_ms() - t0 >= (uint64_t)ch->timeout_ms) return MC_ERR_NET;
    ch->idle(ch->idle_user);
  }
  return MC_OK;
}

/* Receives and decrypts a frame into buf (MC_FRAME_MAX). MC_ERR_CANCELLED if it's CANCEL. */
static mc_status read_frame(mc_channel *ch, unsigned char *buf, size_t *n, int timeout_ms) {
  mc_status st = mc_frame_recv(ch->sock, buf, MC_FRAME_MAX, n, timeout_ms);
  if (st == MC_OK && *n < 1 + MC_NOISE_TAG_LEN) st = MC_ERR_FORMAT; /* not even a type byte */
  if (st == MC_OK) st = mc_noise_decrypt(&ch->recv, NULL, 0, buf, *n, buf);
  if (st == MC_OK && buf[0] == MC_MSG_CANCEL) {
    ch->peer_cancelled = 1;
    st = MC_ERR_CANCELLED;
  }
  return st;
}

/* The message in rbuf, plain_len bytes with its type. */
static void received(mc_channel *ch, size_t plain_len, uint8_t *type,
                     const unsigned char **payload, size_t *len) {
  *type = ch->rbuf[0];
  *payload = ch->rbuf + 1;
  *len = plain_len - 1;
}

static mc_status recv_frame(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                           size_t *len, int timeout_ms) {
  size_t n = 0;
  mc_status st = read_frame(ch, ch->rbuf, &n, timeout_ms);
  if (st != MC_OK) {
    ch->broken = 1; /* after CANCEL too: the peer is gone */
    return st;
  }
  received(ch, n - MC_NOISE_TAG_LEN, type, payload, len);
  return MC_OK;
}

mc_status mc_channel_recv(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                          size_t *len) {
  if (!ch || !type || !payload || !len || !ch->encrypted || ch->broken) return MC_ERR_INVALID_ARG;
  /* Cancelled here: the peer may keep sending until it reads the notice, but this end is done. */
  mc_status st = cancel_asked(ch) ? MC_ERR_NET : MC_OK;
  if (st == MC_OK && ch->ahead_count) {
    int k = ch->ahead_head;
    size_t n = ch->ahead_len[k];
    memcpy(ch->rbuf, ch->ahead[k], n);
    mbedtls_platform_zeroize(ch->ahead[k], n);
    free(ch->ahead[k]);
    ch->ahead_head = (k + 1) % MC_CHANNEL_AHEAD_MAX;
    ch->ahead_count--;
    received(ch, n, type, payload, len);
    return MC_OK;
  }
  if (st == MC_OK) st = ch->pending_st;
  if (st == MC_OK && ch->idle) st = wait_idling(ch);
  if (st == MC_OK) st = recv_frame(ch, type, payload, len, ch->timeout_ms);
  if (st == MC_OK && cancel_asked(ch)) st = MC_ERR_NET;
  if (st != MC_OK) ch->broken = 1;
  return st;
}

int mc_channel_peer_cancelled(const mc_channel *ch) {
  return ch && ch->peer_cancelled;
}

mc_status mc_channel_recv_after_pair(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                                     size_t *len, int *clear) {
  if (!ch || !type || !payload || !len || !clear || !ch->encrypted || ch->broken)
    return MC_ERR_INVALID_ARG;
  size_t n;
  mc_status st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
  if (st != MC_OK) {
    ch->broken = 1;
    return st;
  }
  /* Decrypting in place would wipe the frame on failure, so try a copy. */
  if (n >= 1 + MC_NOISE_TAG_LEN) {
    memcpy(ch->sbuf, ch->rbuf, n);
    if (mc_noise_decrypt(&ch->recv, NULL, 0, ch->sbuf, n, ch->sbuf) == MC_OK) {
      memcpy(ch->rbuf, ch->sbuf, n - MC_NOISE_TAG_LEN);
      mbedtls_platform_zeroize(ch->sbuf, n);
      *clear = 0;
      *type = ch->rbuf[0];
      *payload = ch->rbuf + 1;
      *len = n - 1 - MC_NOISE_TAG_LEN;
      return MC_OK;
    }
  }
  ch->broken = 1; /* B derived other keys, so nothing more can be read */
  if (n < 1 || ch->rbuf[0] != MC_MSG_PAIR_REFUSE) return MC_ERR_FORMAT;
  *clear = 1;
  *type = ch->rbuf[0];
  *payload = ch->rbuf + 1;
  *len = n - 1;
  return MC_OK;
}

mc_status mc_channel_send_clear(mc_channel *ch, uint8_t type, const void *payload, size_t len) {
  /* Allowed after a failed handshake: B still tells A of a wrong code in the clear. */
  if (!ch || ch->encrypted || len > MC_FRAME_MAX - 1 || (!payload && len))
    return MC_ERR_INVALID_ARG;
  ch->sbuf[0] = type;
  if (len) memcpy(ch->sbuf + 1, payload, len);
  return mc_frame_send_in_place(ch->sock, ch->sbuf, len + 1, ch->timeout_ms);
}

mc_status mc_channel_recv_clear(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                                size_t *len) {
  if (!ch || !type || !payload || !len || ch->encrypted || ch->broken) return MC_ERR_INVALID_ARG;
  size_t n = 0;
  mc_status st = ch->idle ? wait_idling(ch) : MC_OK;
  if (st == MC_OK) st = mc_frame_recv(ch->sock, ch->rbuf, sizeof ch->rbuf, &n, ch->timeout_ms);
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
