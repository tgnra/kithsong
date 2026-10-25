/* Minimal blocking-style TCP over POSIX sockets and Winsock, with a timeout on every call.
 * Sockets are non-blocking underneath; each call waits with poll()/select() until it is done
 * or its deadline passes. Internal to core. */
#ifndef MC_NET_SOCKET_H
#define MC_NET_SOCKET_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

/* Timeouts are in milliseconds for the whole call; MC_SOCKET_FOREVER waits indefinitely. */
#define MC_SOCKET_FOREVER (-1)

typedef struct mc_socket mc_socket;
typedef struct mc_listener mc_listener;

/* Process-wide setup: WSAStartup on Windows, nothing elsewhere. Every function below calls
 * it, so callers never need to; safe from any thread, and cheap after the first call. */
mc_status mc_socket_init(void);

/* Listens on all interfaces: IPv4, plus IPv6 on the same port where the OS has it.
 * port 0 picks an ephemeral port; mc_listener_port() tells which. */
mc_status mc_listener_open(uint16_t port, mc_listener **out);
uint16_t mc_listener_port(const mc_listener *l);
/* Waits for one connection. Returns MC_OK with *out = NULL if the timeout passed or the
 * listener was stopped (see mc_listener_stopped()). */
mc_status mc_listener_accept(mc_listener *l, int timeout_ms, mc_socket **out);
/* Wakes a blocked mc_listener_accept() and makes every later one return at once. The only
 * listener function that may be called from another thread. */
void mc_listener_stop(mc_listener *l);
int mc_listener_stopped(mc_listener *l);
void mc_listener_close(mc_listener *l);

/* Connects to host (an IPv4/IPv6 literal or a name) and port. Name lookup itself isn't
 * bounded by the timeout; discovery hands out addresses, so it's rarely used. */
mc_status mc_socket_connect(const char *host, uint16_t port, int timeout_ms, mc_socket **out);
/* Sends all len bytes. MC_ERR_NET on error, timeout or a closed peer. */
mc_status mc_socket_send_all(mc_socket *s, const void *buf, size_t len, int timeout_ms);
/* Receives exactly len bytes. MC_ERR_NET on error, timeout or a closed peer. */
mc_status mc_socket_recv_exact(mc_socket *s, void *buf, size_t len, int timeout_ms);
/* The peer's address as text, for logs. */
const char *mc_socket_peer(const mc_socket *s);
void mc_socket_close(mc_socket *s);

#endif
