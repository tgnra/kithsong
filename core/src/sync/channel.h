/* A sync connection: TCP, the preamble, frames, and once the Noise handshake is done, an
 * encrypted channel of typed messages <type: u8> <payload>. See "Transport" in
 * docs/sync-protocol.md. Internal to core; one thread at a time per channel. */
#ifndef MC_SYNC_CHANNEL_H
#define MC_SYNC_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"
#include "sync/msg.h"
#include "sync/noise.h"

/* The type byte and payload fill one Noise plaintext. */
#define MC_CHANNEL_MAX_PAYLOAD (MC_NOISE_MAX_PLAINTEXT - 1)
/* Default bound on each send or receive. */
#define MC_CHANNEL_TIMEOUT_MS 30000

typedef struct mc_channel mc_channel;

/* Connects and sends the preamble for mode (MC_MODE_PAIRING or MC_MODE_SESSION). */
mc_status mc_channel_connect(const char *host, uint16_t port, int mode, int timeout_ms,
                             mc_channel **out);
/* Accepts one connection and reads its preamble. Like mc_listener_accept(), returns MC_OK
 * with *out = NULL on timeout or once the listener is stopped. A connection with a bad
 * preamble is closed and reported as MC_ERR_FORMAT; keep accepting. */
mc_status mc_channel_accept(mc_listener *l, int timeout_ms, mc_channel **out);
void mc_channel_close(mc_channel *ch);

int mc_channel_mode(const mc_channel *ch);
int mc_channel_is_initiator(const mc_channel *ch);
int mc_channel_is_encrypted(const mc_channel *ch);
const char *mc_channel_peer_addr(const mc_channel *ch);
/* Bounds every later send and receive; MC_SOCKET_FOREVER for none. */
void mc_channel_set_timeout(mc_channel *ch, int timeout_ms);

/* Has the last word on a peer's static key, e.g. that it is still a non-removed member of
 * this device's group. Non-zero accepts. */
typedef int (*mc_channel_accept_fn)(void *user, const unsigned char key[MC_NOISE_KEY_LEN]);

/* Noise KK, for sessions. KK needs the peer's static key before the first message:
 *  - the initiator passes exactly one, the key of the device it is calling;
 *  - the responder doesn't know who called yet, so passes every key it would talk to (the
 *    group's members). It finds the caller by trying each against the first message,
 *    which only the holder of that key's private half can produce.
 * accept (may be NULL) then sees the key. MC_ERR_NOT_FOUND if no candidate matches or
 * accept refuses, MC_ERR_FORMAT if the peer fails to authenticate. Either way the channel
 * is unusable afterwards. */
mc_status mc_channel_handshake_kk(mc_channel *ch, const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                  const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                  const unsigned char (*peers)[MC_NOISE_KEY_LEN], size_t n_peers,
                                  mc_channel_accept_fn accept, void *user);
/* Noise XXpsk3, for pairing; see pair.h. MC_ERR_FORMAT on a wrong psk, seen only by the
 * responder: the initiator finishes its side first and learns of it from B's next
 * message (or the closed connection). */
mc_status mc_channel_handshake_xxpsk3(mc_channel *ch, const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                      const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                      const unsigned char psk[MC_NOISE_PSK_LEN]);
/* The peer's static key once a handshake has succeeded, else NULL. */
const unsigned char *mc_channel_peer_key(const mc_channel *ch);

/* Encrypted messages, after a handshake. len <= MC_CHANNEL_MAX_PAYLOAD. A received payload
 * points into the channel and stays valid until the next receive. MC_ERR_FORMAT if a
 * message fails to decrypt; the channel is then unusable. */
mc_status mc_channel_send(mc_channel *ch, uint8_t type, const void *payload, size_t len);
mc_status mc_channel_recv(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                          size_t *len);

/* Clear-text messages, for the pairing messages before the handshake. Refused once the
 * channel is encrypted, as the encrypted ones are refused before. */
mc_status mc_channel_send_clear(mc_channel *ch, uint8_t type, const void *payload, size_t len);
mc_status mc_channel_recv_clear(mc_channel *ch, uint8_t *type, const unsigned char **payload,
                                size_t *len);

#endif
