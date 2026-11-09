/* A TLS connection to a Cast device, over net/socket.c with Mbed TLS. Cast devices present
 * a self-signed certificate (their identity is checked by Google's device authentication,
 * which a sender may skip), so the certificate isn't verified: the connection is private
 * from others on the network, but the device is whoever answered at its address. A
 * connection is used by one thread at a time. Internal to core. */
#ifndef MC_CAST_CAST_TLS_H
#define MC_CAST_CAST_TLS_H

#include <stddef.h>

#include "kithcore/kithcore.h"

typedef struct cast_tls cast_tls;

/* Connects to host:port and completes the handshake within timeout_ms. */
mc_status cast_tls_connect(const char *host, unsigned port, int timeout_ms, cast_tls **out);
/* Sends all len bytes. MC_ERR_NET on error or timeout (of inactivity, as mc_socket_send_all). */
mc_status cast_tls_write(cast_tls *t, const void *buf, size_t len, int timeout_ms);
/* Reads what has arrived, up to len bytes, waiting up to timeout_ms for it: how many bytes,
 * 0 if the time ran out, -1 if the connection failed or closed. */
long cast_tls_read(cast_tls *t, void *buf, size_t len, int timeout_ms);
/* Reads exactly len bytes, timeout_ms being one of inactivity. MC_ERR_NET on error, close or
 * timeout. */
mc_status cast_tls_read_exact(cast_tls *t, void *buf, size_t len, int timeout_ms);
/* This end's address of the connection (mc_socket_local_address()). */
mc_status cast_tls_local_address(const cast_tls *t, char *out, size_t cap);
/* Makes a read or write blocked on another thread fail at once. The only call that may be
 * made from another thread. */
void cast_tls_shutdown(cast_tls *t);
void cast_tls_close(cast_tls *t);

#endif
