/* mDNS sockets: see msock.h, and discovery.c for the reasons behind each choice.
 *
 * Android: getifaddrs() needs API level 24, and the frontend must hold a
 * WifiManager.MulticastLock, or incoming multicast is filtered. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* getifaddrs, IFF_* */
#endif

#include "discovery/msock.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include "mdns.h"
#include "util/platform.h"

enum { NO_WAKE_MS = 200 }; /* select() timeout cap if the wake socket couldn't be opened */

static void set_nonblocking(int fd) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket((SOCKET)fd, FIONBIO, &on);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

int mc_msocks_usable_ipv6(const struct in6_addr *a) {
  static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  static const unsigned char any[16] = {0};
  const unsigned char *b = a->s6_addr;
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0; /* link-local: needs a scope id */
  if (b[0] == 0 && b[1] == 0 && b[10] == 0xff && b[11] == 0xff) return 0; /* v4-mapped */
  return memcmp(b, loopback, 16) != 0 && memcmp(b, any, 16) != 0;
}

void mc_msocks_list_addresses(struct in_addr *v4, int max, int *n4, struct in6_addr *v6, int *has6) {
  *n4 = 0;
  *has6 = 0;
#ifdef _WIN32
  ULONG size = 16 * 1024;
  IP_ADAPTER_ADDRESSES *list = NULL;
  ULONG ret = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 4 && ret == ERROR_BUFFER_OVERFLOW; tries++) {
    free(list);
    list = malloc(size);
    if (!list) return;
    ret = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_ANYCAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                               NULL, list, &size);
  }
  if (ret != NO_ERROR) {
    free(list);
    return;
  }
  for (IP_ADAPTER_ADDRESSES *ad = list; ad; ad = ad->Next) {
    if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    if (ad->IfType == IF_TYPE_PPP || ad->TunnelType == TUNNEL_TYPE_TEREDO) continue;
    if (ad->Flags & IP_ADAPTER_NO_MULTICAST) continue;
    for (IP_ADAPTER_UNICAST_ADDRESS *u = ad->FirstUnicastAddress; u; u = u->Next) {
      const struct sockaddr *sa = u->Address.lpSockaddr;
      if (sa->sa_family == AF_INET && *n4 < max) {
        struct sockaddr_in in;
        memcpy(&in, sa, sizeof in);
        v4[(*n4)++] = in.sin_addr;
      } else if (sa->sa_family == AF_INET6 && !*has6) {
        struct sockaddr_in6 in6;
        memcpy(&in6, sa, sizeof in6);
        if (mc_msocks_usable_ipv6(&in6.sin6_addr)) {
          *v6 = in6.sin6_addr;
          *has6 = 1;
        }
      }
    }
  }
  free(list);
#else
  /* Loopback is included, although it lacks IFF_MULTICAST (it carries multicast fine), so
   * instances on one machine find each other even where a host firewall drops multicast
   * looped back on the LAN interface, as NixOS's default one does. Only local processes
   * hear it. */
  struct ifaddrs *list = NULL;
  if (getifaddrs(&list) != 0) return;
  for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
    unsigned flags = ifa->ifa_flags;
    if (!ifa->ifa_addr) continue;
    if (!(flags & IFF_UP) || !(flags & IFF_RUNNING)) continue;
    if (!(flags & (IFF_MULTICAST | IFF_LOOPBACK)) || (flags & IFF_POINTOPOINT)) continue;
    if (ifa->ifa_addr->sa_family == AF_INET && *n4 < max) {
      struct sockaddr_in in;
      memcpy(&in, ifa->ifa_addr, sizeof in);
      v4[(*n4)++] = in.sin_addr;
    } else if (ifa->ifa_addr->sa_family == AF_INET6 && !*has6) {
      struct sockaddr_in6 in6;
      memcpy(&in6, ifa->ifa_addr, sizeof in6);
      if (mc_msocks_usable_ipv6(&in6.sin6_addr)) {
        *v6 = in6.sin6_addr;
        *has6 = 1;
      }
    }
  }
  freeifaddrs(list);
#endif
}

static void add_sock(mc_msocks *m, int fd, int ipv6, struct in_addr addr4) {
  if (m->nsocks == MC_MSOCKS_MAX) {
    mdns_socket_close(fd);
    return;
  }
  mc_msock *s = &m->socks[m->nsocks++];
  s->fd = fd;
  s->ipv6 = ipv6;
  s->addr4 = addr4;
  s->heard_ms = mc_mono_ms();
  s->send_failed = 0;
}

/* An mDNS socket on IPv4 interface addr4 (INADDR_ANY: the default one), or on the default
 * IPv6 interface. -1 if it can't be opened. */
static int open_one(int ipv6, struct in_addr addr4) {
  if (ipv6) {
    struct sockaddr_in6 sa;
    memset(&sa, 0, sizeof sa);
    sa.sin6_family = AF_INET6;
    sa.sin6_addr = in6addr_any;
    sa.sin6_port = htons(MDNS_PORT);
    return mdns_socket_open_ipv6(&sa);
  }
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(MDNS_PORT);
  sa.sin_addr = addr4;
  int fd = mdns_socket_open_ipv4(&sa);
#ifdef IP_MULTICAST_ALL
  /* Linux otherwise delivers every interface's multicast to every socket on the port. */
  if (fd >= 0 && addr4.s_addr != htonl(INADDR_ANY)) {
    int off = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &off, sizeof off);
  }
#endif
  return fd;
}

/* One socket per IPv4 interface address, or one on the default interface if none opens. */
static void open_ipv4(mc_msocks *m) {
  int opened = 0;
  for (int i = 0; i < m->naddrs4 && m->nsocks < MC_MSOCKS_MAX; i++) {
    int fd = open_one(0, m->addrs4[i]);
    if (fd < 0) continue;
    add_sock(m, fd, 0, m->addrs4[i]);
    opened++;
  }
  if (opened) return;
  struct in_addr any;
  any.s_addr = htonl(INADDR_ANY);
  int fd = open_one(0, any);
  if (fd >= 0) add_sock(m, fd, 0, any);
}

static void open_ipv6(mc_msocks *m) {
  struct in_addr none;
  none.s_addr = htonl(INADDR_ANY);
  int fd = open_one(1, none);
  if (fd >= 0) add_sock(m, fd, 1, none);
}

static int has_ipv6(const mc_msocks *m) {
  for (int i = 0; i < m->nsocks; i++)
    if (m->socks[i].ipv6) return 1;
  return 0;
}

static void close_socks(mc_msocks *m, int ipv6) {
  int kept = 0;
  for (int i = 0; i < m->nsocks; i++) {
    if (m->socks[i].ipv6 == ipv6)
      mdns_socket_close(m->socks[i].fd);
    else
      m->socks[kept++] = m->socks[i];
  }
  m->nsocks = kept;
}

/* A loopback UDP socket that other threads send a byte to, to wake select() (portable,
 * unlike a pipe, which Windows can't select on). */
static int open_wake(mc_msocks *m) {
  int fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) return -1;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof sa;
  if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
      getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
    mdns_socket_close(fd);
    return -1;
  }
  set_nonblocking(fd);
  m->wake_addr = sa;
  return fd;
}

void mc_msocks_open(mc_msocks *m) {
  memset(m, 0, sizeof *m);
  m->wake_fd = -1;
  mc_msocks_list_addresses(m->addrs4, MC_MSOCKS_MAX, &m->naddrs4, &m->addr6, &m->has_addr6);
  open_ipv4(m);
  open_ipv6(m);
  m->wake_fd = open_wake(m);
}

void mc_msocks_close(mc_msocks *m) {
  close_socks(m, 0);
  close_socks(m, 1);
  if (m->wake_fd >= 0) mdns_socket_close(m->wake_fd);
  m->wake_fd = -1;
}

void mc_msocks_wake(mc_msocks *m) {
  if (m->wake_fd >= 0)
    sendto(m->wake_fd, "w", 1, 0, (const struct sockaddr *)&m->wake_addr, sizeof m->wake_addr);
}

int mc_msocks_refresh(mc_msocks *m) {
  struct in_addr v4[MC_MSOCKS_MAX];
  int n4;
  mc_msocks_list_addresses(v4, MC_MSOCKS_MAX, &n4, &m->addr6, &m->has_addr6);
  if (n4 == m->naddrs4 && !memcmp(v4, m->addrs4, (size_t)n4 * sizeof v4[0]) && m->nsocks) {
    if (has_ipv6(m)) return 0;
    int before = m->nsocks;
    open_ipv6(m);
    return m->nsocks != before;
  }
  memcpy(m->addrs4, v4, sizeof v4);
  m->naddrs4 = n4;
  /* The IPv6 socket too: it joined the group on whatever was the default interface. */
  close_socks(m, 0);
  close_socks(m, 1);
  open_ipv4(m);
  open_ipv6(m);
  return 1;
}

int mc_msocks_reopen_deaf(mc_msocks *m, uint64_t now, uint64_t deaf_ms) {
  int reopened = 0;
  for (int i = 0; i < m->nsocks; i++) {
    mc_msock *s = &m->socks[i];
    if (!s->send_failed && now - s->heard_ms < deaf_ms) continue;
    int fd = open_one(s->ipv6, s->addr4);
    s->heard_ms = now;
    if (fd < 0) continue;
    s->send_failed = 0;
    mdns_socket_close(s->fd);
    s->fd = fd;
    reopened++;
  }
  return reopened;
}

static void fd_add(fd_set *set, int fd, int *maxfd) {
#ifdef _WIN32
  FD_SET((SOCKET)fd, set);
#else
  FD_SET(fd, set);
#endif
  if (fd > *maxfd) *maxfd = fd;
}

static int fd_ready(fd_set *set, int fd) {
#ifdef _WIN32
  return FD_ISSET((SOCKET)fd, set);
#else
  return FD_ISSET(fd, set);
#endif
}

void mc_msocks_wait(mc_msocks *m, uint64_t ms, void (*on_packet)(void *user, mc_msock *s), void *user) {
  if (m->wake_fd < 0 && ms > NO_WAKE_MS) ms = NO_WAKE_MS;
  fd_set set;
  FD_ZERO(&set);
  int maxfd = -1;
  for (int i = 0; i < m->nsocks; i++) fd_add(&set, m->socks[i].fd, &maxfd);
  if (m->wake_fd >= 0) fd_add(&set, m->wake_fd, &maxfd);
  if (maxfd < 0) { /* no socket at all: Windows' select() would fail at once */
    mc_sleep_ms((uint32_t)(ms > NO_WAKE_MS ? NO_WAKE_MS : ms));
    return;
  }
  struct timeval tv;
  tv.tv_sec = (long)(ms / 1000);
  tv.tv_usec = (long)(ms % 1000 * 1000);
  if (select(maxfd + 1, &set, NULL, NULL, &tv) <= 0) return;

  if (m->wake_fd >= 0 && fd_ready(&set, m->wake_fd)) {
    char junk[16];
    while (recv(m->wake_fd, junk, sizeof junk, 0) > 0) {
    }
  }
  for (int i = 0; i < m->nsocks; i++) {
    if (!fd_ready(&set, m->socks[i].fd)) continue;
    m->socks[i].heard_ms = mc_mono_ms();
    on_packet(user, &m->socks[i]);
  }
}
