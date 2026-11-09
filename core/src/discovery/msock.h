/* A set of mDNS sockets, one per multicast-capable IPv4 interface plus one IPv6 socket on the
 * default interface, all bound to port 5353, with a loopback "wake" socket to cut a wait
 * short. What Kithsong's own discovery (discovery.c) and the Google Cast browser
 * (cast/cast_browse.c) both run on; see discovery.c for why it's shaped like this. Internal to
 * core.
 *
 * A set belongs to one thread, the one that waits on it, except mc_msocks_wake(), which any
 * thread may call between mc_msocks_open() and mc_msocks_close(). */
#ifndef MC_DISCOVERY_MSOCK_H
#define MC_DISCOVERY_MSOCK_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#endif

enum { MC_MSOCKS_MAX = 16 };

typedef struct mc_msock {
  int fd;
  int ipv6;
  struct in_addr addr4; /* interface this socket sends from; INADDR_ANY for the default */
  uint64_t heard_ms;    /* opened, or last had a packet (our own looped back included) */
  int send_failed;      /* set by the owner when a send on it fails */
} mc_msock;

typedef struct mc_msocks {
  mc_msock socks[MC_MSOCKS_MAX];
  int nsocks;
  struct in_addr addrs4[MC_MSOCKS_MAX]; /* this device's interface addresses */
  int naddrs4;
  struct in6_addr addr6; /* a global IPv6 address, for AAAA */
  int has_addr6;
  int wake_fd;
  struct sockaddr_in wake_addr;
} mc_msocks;

/* Lists the interfaces and opens a socket on each, and the wake socket. With no network
 * there may be no socket at all; mc_msocks_refresh() opens them once there is one. */
void mc_msocks_open(mc_msocks *m);
void mc_msocks_close(mc_msocks *m);
/* Re-lists interfaces, and reopens every socket if their addresses changed. Opens an IPv6
 * socket if there's none yet. Returns whether the sockets changed. */
int mc_msocks_refresh(mc_msocks *m);
/* Reopens each socket that has heard nothing for deaf_ms, or failed to send. A new socket is
 * opened before the old one closes; if it can't be, the old one stays. Returns how many were
 * reopened. */
int mc_msocks_reopen_deaf(mc_msocks *m, uint64_t now, uint64_t deaf_ms);
/* Wakes a thread in mc_msocks_wait(). Any thread. */
void mc_msocks_wake(mc_msocks *m);
/* Waits up to ms for packets or a wake-up. For each socket with a packet waiting, marks it
 * heard and calls on_packet, which reads it (mdns_socket_listen()). */
void mc_msocks_wait(mc_msocks *m, uint64_t ms, void (*on_packet)(void *user, mc_msock *s), void *user);

/* An IPv6 address worth advertising or connecting to: not link-local, v4-mapped, loopback
 * or unspecified. */
int mc_msocks_usable_ipv6(const struct in6_addr *a);
/* This device's IPv4 interface addresses (up to max), and its first global IPv6 address.
 * Skips point-to-point interfaces and those that are down or can't multicast; loopback is
 * included on POSIX (see msock.c). */
void mc_msocks_list_addresses(struct in_addr *v4, int max, int *n4, struct in6_addr *v6, int *has6);

#endif
