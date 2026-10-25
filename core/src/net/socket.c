#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "net/socket.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define SEND_FLAGS 0
static int sock_error(void) {
  return WSAGetLastError();
}
#define WOULD_BLOCK(e) ((e) == WSAEWOULDBLOCK)
#define CONNECTING(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#define RETRY_ACCEPT(e) ((e) == WSAEWOULDBLOCK || (e) == WSAECONNRESET || (e) == WSAEINTR)
typedef int io_len; /* send() and recv() take and return int */
#define IO_MAX INT_MAX

#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL /* a closed peer is an error, not SIGPIPE */
#else
#define SEND_FLAGS 0 /* SO_NOSIGPIPE is set per socket instead */
#endif
static int sock_error(void) {
  return errno;
}
#define WOULD_BLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
#define CONNECTING(e) ((e) == EINPROGRESS || (e) == EINTR)
#define RETRY_ACCEPT(e) \
  ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR || (e) == ECONNABORTED || (e) == EPROTO)
typedef size_t io_len;
#define IO_MAX ((size_t)SSIZE_MAX)
#endif

struct mc_socket {
  sock_t fd;
  char peer[INET6_ADDRSTRLEN + 8]; /* "[addr]:port" */
};

struct mc_listener {
  sock_t fds[2]; /* IPv4, then IPv6 if available */
  int nfds;
  uint16_t port;
  /* mc_listener_stop() sends a datagram to this loopback socket, which accept also waits
   * on. The same trick works on both platforms, unlike a pipe. */
  sock_t wake;
  struct sockaddr_in wake_addr;
  mc_mutex lock;
  int stopped;
};

/* --- Setup --- */

#ifdef _WIN32
static INIT_ONCE g_wsa_once = INIT_ONCE_STATIC_INIT;
static int g_wsa_ok;

static BOOL CALLBACK wsa_startup(PINIT_ONCE once, PVOID param, PVOID *ctx) {
  (void)once;
  (void)param;
  (void)ctx;
  WSADATA data;
  g_wsa_ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
  return TRUE;
}

mc_status mc_socket_init(void) {
  InitOnceExecuteOnce(&g_wsa_once, wsa_startup, NULL, NULL);
  return g_wsa_ok ? MC_OK : MC_ERR_NET;
}
#else
mc_status mc_socket_init(void) {
  return MC_OK;
}
#endif

/* --- Helpers --- */

/* Non-blocking, not inherited by child processes, and no SIGPIPE where that's per socket. */
static int prepare(sock_t fd) {
#ifdef _WIN32
  u_long on = 1;
  if (ioctlsocket(fd, (long)FIONBIO, &on) != 0) return -1;
  SetHandleInformation((HANDLE)fd, HANDLE_FLAG_INHERIT, 0);
#else
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#endif
  return 0;
}

/* Milliseconds left until deadline: -1 for no deadline, 0 once it has passed. */
static int remaining(uint64_t deadline, int timeout_ms) {
  if (timeout_ms < 0) return -1;
  uint64_t now = mc_mono_ms();
  if (now >= deadline) return 0;
  uint64_t left = deadline - now;
  return left > INT_MAX ? INT_MAX : (int)left;
}

static uint64_t deadline_of(int timeout_ms) {
  return timeout_ms < 0 ? 0 : mc_mono_ms() + (uint64_t)timeout_ms;
}

/* Waits until one of fds can be read (or written, with want_write) or has failed, for up
 * to timeout_ms (-1: forever). Returns its index, -1 on timeout, -2 on error. Errors and
 * hang-ups count as ready, so the next call on that socket reports them. */
static int wait_ready(const sock_t *fds, int n, int want_write, int timeout_ms) {
#ifdef _WIN32
  /* select() rather than WSAPoll(), which misses failed connects on older Windows 10. */
  fd_set rd, wr, ex;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  for (int i = 0; i < n; i++) {
    if (want_write)
      FD_SET(fds[i], &wr);
    else
      FD_SET(fds[i], &rd);
    FD_SET(fds[i], &ex); /* where Windows reports a failed connect */
  }
  struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int rc = select(0, &rd, &wr, &ex, timeout_ms < 0 ? NULL : &tv);
  if (rc == 0) return -1;
  if (rc < 0) return -2;
  for (int i = 0; i < n; i++)
    if (FD_ISSET(fds[i], &rd) || FD_ISSET(fds[i], &wr) || FD_ISSET(fds[i], &ex)) return i;
  return -1;
#else
  struct pollfd pf[4];
  if (n > 4) return -2;
  for (int i = 0; i < n; i++) {
    pf[i].fd = fds[i];
    pf[i].events = want_write ? POLLOUT : POLLIN;
    pf[i].revents = 0;
  }
  int rc = poll(pf, (nfds_t)n, timeout_ms);
  if (rc == 0) return -1;
  if (rc < 0) return errno == EINTR ? -1 : -2; /* callers loop on their deadline */
  for (int i = 0; i < n; i++)
    if (pf[i].revents) return i;
  return -1;
#endif
}

static void describe_peer(const struct sockaddr *sa, char *out, size_t cap) {
  char host[INET6_ADDRSTRLEN] = "?";
  unsigned port = 0;
  if (sa->sa_family == AF_INET) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
    inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
    port = ntohs(in->sin_port);
    snprintf(out, cap, "%s:%u", host, port);
  } else if (sa->sa_family == AF_INET6) {
    const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
    inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
    port = ntohs(in6->sin6_port);
    snprintf(out, cap, "[%s]:%u", host, port);
  } else {
    snprintf(out, cap, "?");
  }
}

static mc_status wrap(sock_t fd, const struct sockaddr *peer, mc_socket **out) {
  mc_socket *s = calloc(1, sizeof *s);
  if (!s) {
    close_sock(fd);
    return MC_ERR_NOMEM;
  }
  int one = 1;
  /* Frames are small and request/response; don't let Nagle hold them back. */
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  s->fd = fd;
  describe_peer(peer, s->peer, sizeof s->peer);
  *out = s;
  return MC_OK;
}

/* --- Listener --- */

static sock_t listen_on(int family, uint16_t port) {
  sock_t fd = socket(family, SOCK_STREAM, IPPROTO_TCP);
  if (fd == BAD_SOCK) return BAD_SOCK;
  int one = 1;
  struct sockaddr_storage ss;
  memset(&ss, 0, sizeof ss);
  socklen_t len;
  if (family == AF_INET6) {
    /* A separate IPv4 socket takes the v4 traffic, so this one is v6 only everywhere. */
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&one, sizeof one);
    struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&ss;
    in6->sin6_family = AF_INET6;
    in6->sin6_addr = in6addr_any;
    in6->sin6_port = htons(port);
    len = sizeof *in6;
  } else {
    struct sockaddr_in *in = (struct sockaddr_in *)&ss;
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = htonl(INADDR_ANY);
    in->sin_port = htons(port);
    len = sizeof *in;
  }
#ifndef _WIN32
  /* Lets a fixed port be reused straight after a restart. On Windows this option would let
   * another process steal the port, and the default already behaves. */
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  if (prepare(fd) != 0 || bind(fd, (struct sockaddr *)&ss, len) != 0 || listen(fd, SOMAXCONN) != 0) {
    close_sock(fd);
    return BAD_SOCK;
  }
  return fd;
}

mc_status mc_listener_open(uint16_t port, mc_listener **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_status st = mc_socket_init();
  if (st != MC_OK) return st;

  mc_listener *l = calloc(1, sizeof *l);
  if (!l) return MC_ERR_NOMEM;
  l->fds[0] = l->fds[1] = l->wake = BAD_SOCK;
  mc_mutex_init(&l->lock);

  /* IPv4 first, so that an ephemeral port can then be claimed for IPv6 too. */
  l->fds[0] = listen_on(AF_INET, port);
  if (l->fds[0] == BAD_SOCK) goto fail;
  l->nfds = 1;
  struct sockaddr_in bound;
  socklen_t len = sizeof bound;
  if (getsockname(l->fds[0], (struct sockaddr *)&bound, &len) != 0) goto fail;
  l->port = ntohs(bound.sin_port);
  /* No IPv6, or the port is taken there: carry on with IPv4 alone. */
  l->fds[1] = listen_on(AF_INET6, l->port);
  if (l->fds[1] != BAD_SOCK) l->nfds = 2;

  l->wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (l->wake == BAD_SOCK || prepare(l->wake) != 0) goto fail;
  memset(&l->wake_addr, 0, sizeof l->wake_addr);
  l->wake_addr.sin_family = AF_INET;
  l->wake_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  len = sizeof l->wake_addr;
  if (bind(l->wake, (struct sockaddr *)&l->wake_addr, len) != 0 ||
      getsockname(l->wake, (struct sockaddr *)&l->wake_addr, &len) != 0)
    goto fail;

  *out = l;
  return MC_OK;

fail:
  fprintf(stderr, "musiccore: listen on port %u failed (error %d)\n", (unsigned)port,
          sock_error());
  mc_listener_close(l);
  return MC_ERR_NET;
}

uint16_t mc_listener_port(const mc_listener *l) {
  return l->port;
}

int mc_listener_stopped(mc_listener *l) {
  mc_mutex_lock(&l->lock);
  int stopped = l->stopped;
  mc_mutex_unlock(&l->lock);
  return stopped;
}

void mc_listener_stop(mc_listener *l) {
  mc_mutex_lock(&l->lock);
  l->stopped = 1;
  mc_mutex_unlock(&l->lock);
  char byte = 0;
  sendto(l->wake, &byte, 1, 0, (const struct sockaddr *)&l->wake_addr, sizeof l->wake_addr);
}

mc_status mc_listener_accept(mc_listener *l, int timeout_ms, mc_socket **out) {
  if (!l || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  uint64_t deadline = deadline_of(timeout_ms);
  sock_t fds[3];
  int n = 0;
  for (int i = 0; i < l->nfds; i++) fds[n++] = l->fds[i];
  fds[n++] = l->wake;

  for (;;) {
    if (mc_listener_stopped(l)) return MC_OK;
    int left = remaining(deadline, timeout_ms);
    if (left == 0) return MC_OK;
    int ready = wait_ready(fds, n, 0, left);
    if (ready == -2) return MC_ERR_NET;
    if (ready < 0) continue; /* timed out or interrupted; the deadline check decides */
    if (fds[ready] == l->wake) {
      char drain[16];
      while (recv(l->wake, drain, sizeof drain, 0) > 0) {
      }
      continue;
    }
    struct sockaddr_storage peer;
    socklen_t len = sizeof peer;
    sock_t c = accept(fds[ready], (struct sockaddr *)&peer, &len);
    if (c == BAD_SOCK) {
      int e = sock_error();
      if (RETRY_ACCEPT(e)) continue; /* the client gave up between poll and accept */
      return MC_ERR_NET;
    }
    if (prepare(c) != 0) {
      close_sock(c);
      return MC_ERR_NET;
    }
    return wrap(c, (struct sockaddr *)&peer, out);
  }
}

void mc_listener_close(mc_listener *l) {
  if (!l) return;
  for (int i = 0; i < 2; i++)
    if (l->fds[i] != BAD_SOCK) close_sock(l->fds[i]);
  if (l->wake != BAD_SOCK) close_sock(l->wake);
  mc_mutex_destroy(&l->lock);
  free(l);
}

/* --- Connections --- */

/* One connect attempt to one address: MC_OK, or MC_ERR_NET with fd closed. */
static mc_status connect_one(const struct addrinfo *ai, uint64_t deadline, int timeout_ms,
                             sock_t *out) {
  sock_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == BAD_SOCK) return MC_ERR_NET;
  if (prepare(fd) != 0) goto fail;
  if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) != 0) {
    if (!CONNECTING(sock_error())) goto fail;
    for (;;) {
      int left = remaining(deadline, timeout_ms);
      if (left == 0) goto fail;
      int ready = wait_ready(&fd, 1, 1, left);
      if (ready == -2) goto fail;
      if (ready == 0) break;
    }
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err != 0) goto fail;
  }
  *out = fd;
  return MC_OK;

fail:
  close_sock(fd);
  return MC_ERR_NET;
}

mc_status mc_socket_connect(const char *host, uint16_t port, int timeout_ms, mc_socket **out) {
  if (!host || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_status st = mc_socket_init();
  if (st != MC_OK) return st;

  char service[8];
  snprintf(service, sizeof service, "%u", (unsigned)port);
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICSERV;
  if (getaddrinfo(host, service, &hints, &res) != 0 || !res) return MC_ERR_NET;

  uint64_t deadline = deadline_of(timeout_ms);
  st = MC_ERR_NET;
  for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    sock_t fd;
    if (connect_one(ai, deadline, timeout_ms, &fd) == MC_OK) {
      st = wrap(fd, ai->ai_addr, out);
      break;
    }
    if (remaining(deadline, timeout_ms) == 0) break;
  }
  freeaddrinfo(res);
  return st;
}

mc_status mc_socket_send_all(mc_socket *s, const void *buf, size_t len, int timeout_ms) {
  if (!s || (!buf && len)) return MC_ERR_INVALID_ARG;
  const char *p = buf;
  uint64_t deadline = deadline_of(timeout_ms);
  while (len > 0) {
    io_len chunk = (io_len)(len > IO_MAX ? IO_MAX : len);
    long n = (long)send(s->fd, p, chunk, SEND_FLAGS);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && WOULD_BLOCK(sock_error())) {
      int left = remaining(deadline, timeout_ms);
      if (left == 0 || wait_ready(&s->fd, 1, 1, left) == -2) return MC_ERR_NET;
      continue;
    }
    return MC_ERR_NET;
  }
  return MC_OK;
}

mc_status mc_socket_recv_exact(mc_socket *s, void *buf, size_t len, int timeout_ms) {
  if (!s || (!buf && len)) return MC_ERR_INVALID_ARG;
  char *p = buf;
  uint64_t deadline = deadline_of(timeout_ms);
  while (len > 0) {
    io_len chunk = (io_len)(len > IO_MAX ? IO_MAX : len);
    long n = (long)recv(s->fd, p, chunk, 0);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && WOULD_BLOCK(sock_error())) {
      int left = remaining(deadline, timeout_ms);
      if (left == 0 || wait_ready(&s->fd, 1, 0, left) == -2) return MC_ERR_NET;
      continue;
    }
    return MC_ERR_NET; /* 0 is an orderly close mid-message */
  }
  return MC_OK;
}

const char *mc_socket_peer(const mc_socket *s) {
  return s->peer;
}

void mc_socket_close(mc_socket *s) {
  if (!s) return;
  close_sock(s->fd);
  free(s);
}
