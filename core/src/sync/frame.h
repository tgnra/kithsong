/* Frames on a sync connection: <len: u16 big-endian> <bytes[len]>. See "Transport" in
 * docs/sync-protocol.md. Internal to core. */
#ifndef MC_SYNC_FRAME_H
#define MC_SYNC_FRAME_H

#include <stddef.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"

/* The largest frame body, set by the 16-bit length. A Noise message has the same limit. */
#define MC_FRAME_MAX 65535

/* Sends one frame of len <= MC_FRAME_MAX bytes. timeout_ms bounds the header and the body
 * separately. */
mc_status mc_frame_send(mc_socket *s, const void *data, size_t len, int timeout_ms);
/* Receives one frame into buf. MC_ERR_FORMAT if it is longer than cap, which leaves the
 * connection unusable; a cap of MC_FRAME_MAX always suffices. */
mc_status mc_frame_recv(mc_socket *s, void *buf, size_t cap, size_t *len, int timeout_ms);

#endif
