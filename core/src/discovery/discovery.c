/* mDNS/DNS-SD announce and browse on a background thread, using mjansson/mdns. See
 * docs/sync-protocol.md, "Discovery".
 *
 * We are both responder and browser. One IPv4 socket per multicast-capable interface,
 * loopback included (each sends out of its own interface and advertises that interface's
 * address), plus one IPv6 socket on the default interface when IPv6 works. All are bound to
 * port 5353, so queries we send get multicast answers, which every instance on the link
 * hears; that is also how two instances in one process see each other
 * (SO_REUSEADDR/SO_REUSEPORT, which mdns_socket_open_ipv4/6 set). mdns_socket_listen()
 * hands us both questions, which we answer, and answers, which we collect per packet and
 * then merge into the peer table.
 *
 * Our records, for device "Desk" with device_uid ending in 12345678:
 *   _musicapp._tcp.local.                   PTR  Desk (12345678)._musicapp._tcp.local.
 *   Desk (12345678)._musicapp._tcp.local.   SRV  0 0 <port> musicapp-12345678.local.
 *   Desk (12345678)._musicapp._tcp.local.   TXT  v=1 dev=... name=Desk kind=desktop grp=...
 *   musicapp-12345678.local.                A    <interface address>, AAAA <global IPv6>
 * The uid suffix keeps instance and host names unique without probing.
 *
 * The thread sleeps in select() until the next scan, announcement or peer expiry, or until
 * a byte arrives on a loopback "wake" socket (portable, unlike a pipe, which Windows can't
 * select on); stop and set_group send that byte.
 *
 * Interfaces are re-listed at every scan, and the IPv4 sockets reopened when they changed
 * (e.g. a phone moving between networks). Android: getifaddrs() needs API level 24, and the
 * frontend must hold a WifiManager.MulticastLock, or incoming multicast is filtered. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* getifaddrs, IFF_*, inet_ntop */
#endif

#include "musiccore/discovery.h"

#include <stdio.h>
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

#include <mbedtls/sha256.h>

#include "discovery/local_addr.h"
#include "mdns.h"
#include "util/platform.h"

#define SERVICE "_musicapp._tcp.local."
#define DNS_SD "_services._dns-sd._udp.local."

enum {
  MAX_SOCKS = 16,
  NAME_MAX_BYTES = 63,   /* of the device name we announce */
  LABEL_NAME_BYTES = 52, /* of the name in the instance label: 63 minus " (12345678)" */
  SHORT_UID = 8,
  DNS_NAME_LEN = 256,
  KIND_LEN = 16,
  ADDR_LEN = 64,
  RX_MAX = 8, /* services and hosts collected from one packet */
  TXT_MAX = 16,
  TXT_COUNT = 6, /* the records we send: v, dev, name, kind, grp, host */
  SEND_BYTES = 2048,
  RECV_BYTES = 9216,    /* the largest mDNS packet (RFC 6762) */
  REANNOUNCE_MS = 1000, /* announcements go out twice, a second apart (RFC 6762 8.3) */
  NO_WAKE_MS = 200      /* select() timeout cap if the wake socket couldn't be opened */
};

typedef struct msock {
  int fd;
  int ipv6;
  struct in_addr addr4; /* interface this socket sends from; INADDR_ANY for the default */
} msock;

typedef struct peer {
  char dev[MC_UID_STRLEN];
  char name[DNS_NAME_LEN];
  char host[DNS_NAME_LEN];
  char kind[KIND_LEN];
  char grp[MC_GROUP_HASH_STRLEN];
  int version;
  char instance[DNS_NAME_LEN];
  char address[ADDR_LEN];
  uint16_t port;
  uint64_t seen_ms;      /* last heard */
  uint64_t addr_seen_ms; /* last heard at this address */
} peer;

/* A service instance as described by the packet being read. */
typedef struct rx_service {
  char instance[DNS_NAME_LEN];
  int goodbye;
  int has_srv;
  uint16_t port;
  char target[DNS_NAME_LEN];
  int has_txt;
  char dev[MC_UID_STRLEN];
  char name[DNS_NAME_LEN];
  char host[DNS_NAME_LEN];
  char kind[KIND_LEN];
  char grp[MC_GROUP_HASH_STRLEN];
  int version;
} rx_service;

typedef struct rx_host {
  char name[DNS_NAME_LEN];
  int has_a, has_aaaa;
  struct in_addr a;
  struct in6_addr aaaa;
} rx_host;

struct mc_discovery {
  /* Fixed at start. */
  char device_uid[MC_UID_STRLEN];
  char host[NAME_MAX_BYTES + 1];
  char kind[KIND_LEN];
  char instance[DNS_NAME_LEN];
  char hostname[DNS_NAME_LEN];
  uint16_t port;
  uint64_t interval_ms;
  mc_peer_cb cb;
  void *user;
  mc_thread thread;
  int wsa; /* WSAStartup succeeded */

  mc_mutex lock;
  /* Under lock. */
  int stop;
  int group_changed;
  char next_grp[MC_GROUP_HASH_STRLEN];
  int name_changed;
  char next_name[NAME_MAX_BYTES + 1];

  /* Discovery thread only (and start, before the thread exists). */
  char grp[MC_GROUP_HASH_STRLEN];
  char name[NAME_MAX_BYTES + 1]; /* TXT "name" */
  int wake_fd;
  struct sockaddr_in wake_addr;
  msock socks[MAX_SOCKS];
  int nsocks;
  struct in_addr addrs4[MAX_SOCKS]; /* this device's interface addresses */
  int naddrs4;
  struct in6_addr addr6; /* a global IPv6 address, for AAAA */
  int has_addr6;
  peer *peers;
  int npeers;
  /* The packet being read. */
  const msock *cur;
  rx_service rx[RX_MAX];
  int nrx;
  rx_host rxh[RX_MAX];
  int nrxh;
  struct sockaddr_storage rx_from;
  size_t rx_from_len;
  uint32_t sendbuf[SEND_BYTES / 4]; /* mdns wants 32-bit aligned buffers */
  uint32_t recvbuf[RECV_BYTES / 4];
};

/* --- Small helpers -------------------------------------------------------------------- */

static int lower(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* DNS names compare case-insensitively (ASCII only). */
static int name_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
  return *a == *b;
}

/* "<label>._musicapp._tcp.local." */
static int is_instance(const char *name) {
  size_t n = strlen(name), s = strlen(SERVICE);
  return n > s + 1 && name[n - s - 1] == '.' && name_eq(name + n - s, SERVICE);
}

/* Length of s cut to at most max bytes without splitting a UTF-8 sequence. */
static size_t utf8_cut(const char *s, size_t max) {
  size_t n = strlen(s);
  if (n <= max) return n;
  n = max;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

static void copy_str(char *dst, size_t cap, const char *src, size_t len) {
  if (len >= cap) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static int valid_group_hash(const char *h) {
  if (!h || !*h) return 1;
  for (int i = 0; i < 16; i++)
    if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
  return h[16] == '\0';
}

static int valid_kind(const char *kind) {
  return kind && (!strcmp(kind, "desktop") || !strcmp(kind, "phone") || !strcmp(kind, "watch"));
}

static void set_nonblocking(int fd) {
#ifdef _WIN32
  u_long on = 1;
  ioctlsocket((SOCKET)fd, FIONBIO, &on);
#else
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

static int usable_ipv6(const struct in6_addr *a) {
  static const unsigned char loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  static const unsigned char any[16] = {0};
  const unsigned char *b = a->s6_addr;
  if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return 0; /* link-local: needs a scope id */
  if (b[0] == 0 && b[1] == 0 && b[10] == 0xff && b[11] == 0xff) return 0; /* v4-mapped */
  return memcmp(b, loopback, 16) != 0 && memcmp(b, any, 16) != 0;
}

/* --- Interfaces and sockets ----------------------------------------------------------- */

/* This device's IPv4 interface addresses, and its first global IPv6 address. Skips
 * point-to-point interfaces and those that are down or can't multicast. */
static void list_addresses(struct in_addr *v4, int max, int *n4, struct in6_addr *v6,
                           int *has6) {
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
        if (usable_ipv6(&in6.sin6_addr)) {
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
      if (usable_ipv6(&in6.sin6_addr)) {
        *v6 = in6.sin6_addr;
        *has6 = 1;
      }
    }
  }
  freeifaddrs(list);
#endif
}

static void add_sock(mc_discovery *d, int fd, int ipv6, struct in_addr addr4) {
  if (d->nsocks == MAX_SOCKS) {
    mdns_socket_close(fd);
    return;
  }
  d->socks[d->nsocks].fd = fd;
  d->socks[d->nsocks].ipv6 = ipv6;
  d->socks[d->nsocks].addr4 = addr4;
  d->nsocks++;
}

/* One socket per IPv4 interface address, or one on the default interface if none opens. */
static void open_ipv4(mc_discovery *d) {
  struct sockaddr_in sa;
  int opened = 0;
  for (int i = 0; i < d->naddrs4 && d->nsocks < MAX_SOCKS; i++) {
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(MDNS_PORT);
    sa.sin_addr = d->addrs4[i];
    int fd = mdns_socket_open_ipv4(&sa);
    if (fd < 0) continue;
#ifdef IP_MULTICAST_ALL
    /* Linux otherwise delivers every interface's multicast to every socket on the port. */
    int off = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &off, sizeof off);
#endif
    add_sock(d, fd, 0, d->addrs4[i]);
    opened++;
  }
  if (opened) return;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons(MDNS_PORT);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  int fd = mdns_socket_open_ipv4(&sa);
  if (fd >= 0) add_sock(d, fd, 0, sa.sin_addr);
}

static void open_ipv6(mc_discovery *d) {
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof sa);
  sa.sin6_family = AF_INET6;
  sa.sin6_addr = in6addr_any;
  sa.sin6_port = htons(MDNS_PORT);
  int fd = mdns_socket_open_ipv6(&sa);
  struct in_addr none;
  none.s_addr = htonl(INADDR_ANY);
  if (fd >= 0) add_sock(d, fd, 1, none);
}

static void close_socks(mc_discovery *d, int ipv6) {
  int kept = 0;
  for (int i = 0; i < d->nsocks; i++) {
    if (d->socks[i].ipv6 == ipv6)
      mdns_socket_close(d->socks[i].fd);
    else
      d->socks[kept++] = d->socks[i];
  }
  d->nsocks = kept;
}

/* A loopback UDP socket that other threads send a byte to, to wake select(). */
static int open_wake(mc_discovery *d) {
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
  d->wake_addr = sa;
  return fd;
}

static void wake(mc_discovery *d) {
  if (d->wake_fd >= 0)
    sendto(d->wake_fd, "w", 1, 0, (const struct sockaddr *)&d->wake_addr, sizeof d->wake_addr);
}

/* --- Our records ---------------------------------------------------------------------- */

typedef struct records {
  mdns_record_t ptr, srv, a, aaaa;
  mdns_record_t txt[TXT_COUNT];
  int has_a, has_aaaa;
} records;

static mdns_string_t mstr(const char *s) {
  mdns_string_t m;
  m.str = s;
  m.length = strlen(s);
  return m;
}

static mdns_record_t txt_record(const mc_discovery *d, const char *key, const char *value) {
  mdns_record_t r;
  memset(&r, 0, sizeof r);
  r.name = mstr(d->instance);
  r.type = MDNS_RECORDTYPE_TXT;
  r.data.txt.key = mstr(key);
  r.data.txt.value = mstr(value);
  return r;
}

/* The records as sent from socket s: its A record is that socket's interface address. */
static void make_records(const mc_discovery *d, const msock *s, records *r) {
  memset(r, 0, sizeof *r);
  r->ptr.name = mstr(SERVICE);
  r->ptr.type = MDNS_RECORDTYPE_PTR;
  r->ptr.data.ptr.name = mstr(d->instance);

  r->srv.name = mstr(d->instance);
  r->srv.type = MDNS_RECORDTYPE_SRV;
  r->srv.data.srv.port = d->port;
  r->srv.data.srv.name = mstr(d->hostname);

  /* A socket not tied to one interface advertises the first non-loopback address. */
  struct in_addr a4 = s->addr4;
  for (int i = 0; a4.s_addr == htonl(INADDR_ANY) && i < d->naddrs4; i++)
    if ((ntohl(d->addrs4[i].s_addr) >> 24) != 127) a4 = d->addrs4[i];
  if (a4.s_addr != htonl(INADDR_ANY)) {
    r->a.name = mstr(d->hostname);
    r->a.type = MDNS_RECORDTYPE_A;
    r->a.data.a.addr.sin_family = AF_INET;
    r->a.data.a.addr.sin_addr = a4;
    r->has_a = 1;
  }
  if (d->has_addr6) {
    r->aaaa.name = mstr(d->hostname);
    r->aaaa.type = MDNS_RECORDTYPE_AAAA;
    r->aaaa.data.aaaa.addr.sin6_family = AF_INET6;
    r->aaaa.data.aaaa.addr.sin6_addr = d->addr6;
    r->has_aaaa = 1;
  }

  static const char version[] = {'0' + MC_DISCOVERY_VERSION, '\0'};
  r->txt[0] = txt_record(d, "v", version);
  r->txt[1] = txt_record(d, "dev", d->device_uid);
  r->txt[2] = txt_record(d, "name", d->name);
  r->txt[3] = txt_record(d, "kind", d->kind);
  r->txt[4] = txt_record(d, "grp", d->grp);
  r->txt[5] = txt_record(d, "host", d->host);
}

/* Collects the records that accompany an answer: SRV if wanted, then A, AAAA and TXT. */
static size_t extras(const records *r, int with_srv, int with_a, int with_aaaa,
                     mdns_record_t *out) {
  size_t n = 0;
  if (with_srv) out[n++] = r->srv;
  if (with_a && r->has_a) out[n++] = r->a;
  if (with_aaaa && r->has_aaaa) out[n++] = r->aaaa;
  for (int i = 0; i < TXT_COUNT; i++) out[n++] = r->txt[i];
  return n;
}

typedef enum send_mode { SEND_ANNOUNCE, SEND_GOODBYE } send_mode;

static void announce(mc_discovery *d, send_mode mode) {
  for (int i = 0; i < d->nsocks; i++) {
    records r;
    mdns_record_t extra[4 + TXT_COUNT];
    make_records(d, &d->socks[i], &r);
    size_t n = extras(&r, 1, 1, 1, extra);
    if (mode == SEND_ANNOUNCE)
      mdns_announce_multicast(d->socks[i].fd, d->sendbuf, sizeof d->sendbuf, r.ptr, NULL, 0,
                              extra, n);
    else
      mdns_goodbye_multicast(d->socks[i].fd, d->sendbuf, sizeof d->sendbuf, r.ptr, NULL, 0,
                             extra, n);
  }
}

static void browse(mc_discovery *d) {
  for (int i = 0; i < d->nsocks; i++)
    mdns_query_send(d->socks[i].fd, MDNS_RECORDTYPE_PTR, MDNS_STRING_CONST(SERVICE),
                    d->sendbuf, sizeof d->sendbuf, 0);
}

/* Answers a question about our service type, instance or host name, unicast if the
 * question asked for that. */
static void answer_question(mc_discovery *d, const struct sockaddr *from, size_t addrlen,
                            uint16_t query_id, uint16_t rtype, uint16_t rclass,
                            const char *qname) {
  int any = rtype == MDNS_RECORDTYPE_ANY;
  records r;
  mdns_record_t answer, extra[4 + TXT_COUNT];
  size_t n;
  make_records(d, d->cur, &r);
  if (name_eq(qname, DNS_SD) && (rtype == MDNS_RECORDTYPE_PTR || any)) {
    memset(&answer, 0, sizeof answer);
    answer.name = mstr(DNS_SD);
    answer.type = MDNS_RECORDTYPE_PTR;
    answer.data.ptr.name = mstr(SERVICE);
    n = 0;
  } else if (name_eq(qname, SERVICE) && (rtype == MDNS_RECORDTYPE_PTR || any)) {
    answer = r.ptr;
    n = extras(&r, 1, 1, 1, extra);
  } else if (name_eq(qname, d->instance) &&
             (rtype == MDNS_RECORDTYPE_SRV || rtype == MDNS_RECORDTYPE_TXT || any)) {
    /* mdns can't send TXT as the answer itself; TXT then rides along as additional. */
    answer = r.srv;
    n = extras(&r, 0, 1, 1, extra);
  } else if (name_eq(qname, d->hostname) && (rtype == MDNS_RECORDTYPE_A || any) && r.has_a) {
    answer = r.a;
    n = extras(&r, 0, 0, 1, extra);
  } else if (name_eq(qname, d->hostname) && (rtype == MDNS_RECORDTYPE_AAAA || any) &&
             r.has_aaaa) {
    answer = r.aaaa;
    n = extras(&r, 0, 1, 0, extra);
  } else {
    return;
  }
  if (rclass & MDNS_UNICAST_RESPONSE)
    mdns_query_answer_unicast(d->cur->fd, from, addrlen, d->sendbuf, sizeof d->sendbuf,
                              query_id, (mdns_record_type_t)rtype, qname, strlen(qname), answer,
                              NULL, 0, extra, n);
  else
    mdns_query_answer_multicast(d->cur->fd, d->sendbuf, sizeof d->sendbuf, answer, NULL, 0,
                                extra, n);
}

/* --- Reading answers ------------------------------------------------------------------ */

static rx_service *rx_service_for(mc_discovery *d, const char *instance) {
  if (!*instance) return NULL;
  for (int i = 0; i < d->nrx; i++)
    if (name_eq(d->rx[i].instance, instance)) return &d->rx[i];
  if (d->nrx == RX_MAX) return NULL;
  rx_service *r = &d->rx[d->nrx++];
  memset(r, 0, sizeof *r);
  copy_str(r->instance, sizeof r->instance, instance, strlen(instance));
  r->version = -1;
  return r;
}

static rx_host *rx_host_for(mc_discovery *d, const char *name, int create) {
  for (int i = 0; i < d->nrxh; i++)
    if (name_eq(d->rxh[i].name, name)) return &d->rxh[i];
  if (!create || d->nrxh == RX_MAX) return NULL;
  rx_host *h = &d->rxh[d->nrxh++];
  memset(h, 0, sizeof *h);
  copy_str(h->name, sizeof h->name, name, strlen(name));
  return h;
}

static int key_is(const mdns_record_txt_t *t, const char *key) {
  return t->key.length == strlen(key) && memcmp(t->key.str, key, t->key.length) == 0;
}

static void read_txt(rx_service *r, const void *data, size_t size, size_t off, size_t len) {
  mdns_record_txt_t txt[TXT_MAX];
  size_t n = mdns_record_parse_txt(data, size, off, len, txt, TXT_MAX);
  r->has_txt = 1;
  for (size_t i = 0; i < n; i++) {
    const char *v = txt[i].value.str ? txt[i].value.str : "";
    size_t vlen = txt[i].value.length;
    if (key_is(&txt[i], "dev")) {
      if (vlen < MC_UID_STRLEN) copy_str(r->dev, sizeof r->dev, v, vlen);
    } else if (key_is(&txt[i], "name")) {
      copy_str(r->name, sizeof r->name, v, vlen);
    } else if (key_is(&txt[i], "kind")) {
      copy_str(r->kind, sizeof r->kind, v, vlen);
    } else if (key_is(&txt[i], "host")) {
      copy_str(r->host, sizeof r->host, v, vlen);
    } else if (key_is(&txt[i], "grp")) {
      if (vlen < MC_GROUP_HASH_STRLEN) copy_str(r->grp, sizeof r->grp, v, vlen);
    } else if (key_is(&txt[i], "v")) {
      int ver = vlen ? 0 : -1;
      for (size_t k = 0; k < vlen && ver >= 0; k++)
        ver = v[k] >= '0' && v[k] <= '9' && ver < 10000 ? ver * 10 + (v[k] - '0') : -1;
      r->version = ver;
    }
  }
}

static void read_name(const void *data, size_t size, size_t off, char out[DNS_NAME_LEN]) {
  mdns_string_t s = mdns_string_extract(data, size, &off, out, DNS_NAME_LEN - 1);
  out[s.str ? s.length : 0] = '\0';
}

static int on_record(int sock, const struct sockaddr *from, size_t addrlen,
                     mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype, uint16_t rclass,
                     uint32_t ttl, const void *data, size_t size, size_t name_offset,
                     size_t name_length, size_t record_offset, size_t record_length,
                     void *user) {
  (void)sock;
  (void)name_length;
  mc_discovery *d = user;
  char name[DNS_NAME_LEN], target[DNS_NAME_LEN];
  read_name(data, size, name_offset, name);
  if (!d->rx_from_len && addrlen <= sizeof d->rx_from) {
    memcpy(&d->rx_from, from, addrlen);
    d->rx_from_len = addrlen;
  }

  if (entry == MDNS_ENTRYTYPE_QUESTION) {
    answer_question(d, from, addrlen, query_id, rtype, rclass, name);
    return 0;
  }

  /* A goodbye is the same records with TTL 0. */
  rx_service *r;
  rx_host *h;
  if (rtype == MDNS_RECORDTYPE_PTR && name_eq(name, SERVICE)) {
    mdns_string_t t = mdns_record_parse_ptr(data, size, record_offset, record_length, target,
                                            sizeof target - 1);
    target[t.str ? t.length : 0] = '\0';
    if (is_instance(target) && (r = rx_service_for(d, target)) && ttl == 0) r->goodbye = 1;
  } else if (rtype == MDNS_RECORDTYPE_SRV && is_instance(name) &&
             (r = rx_service_for(d, name))) {
    mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length,
                                                  target, sizeof target - 1);
    if (!srv.name.str) return 0;
    r->has_srv = 1;
    r->port = srv.port;
    copy_str(r->target, sizeof r->target, srv.name.str, srv.name.length);
    if (ttl == 0) r->goodbye = 1;
  } else if (rtype == MDNS_RECORDTYPE_TXT && is_instance(name) &&
             (r = rx_service_for(d, name))) {
    read_txt(r, data, size, record_offset, record_length);
    if (ttl == 0) r->goodbye = 1;
  } else if (rtype == MDNS_RECORDTYPE_A && ttl && (h = rx_host_for(d, name, 1))) {
    struct sockaddr_in a;
    mdns_record_parse_a(data, size, record_offset, record_length, &a);
    h->a = a.sin_addr;
    h->has_a = a.sin_addr.s_addr != htonl(INADDR_ANY);
  } else if (rtype == MDNS_RECORDTYPE_AAAA && ttl && (h = rx_host_for(d, name, 1))) {
    struct sockaddr_in6 a6;
    mdns_record_parse_aaaa(data, size, record_offset, record_length, &a6);
    h->aaaa = a6.sin6_addr;
    h->has_aaaa = usable_ipv6(&a6.sin6_addr);
  }
  return 0;
}

/* The address to reach service r at: its host's A record, else AAAA, else the address the
 * packet came from. */
static void rx_address(mc_discovery *d, const rx_service *r, char out[ADDR_LEN]) {
  out[0] = '\0';
  const rx_host *h = r->target[0] ? rx_host_for(d, r->target, 0) : NULL;
  if (h && h->has_a) {
    inet_ntop(AF_INET, &h->a, out, ADDR_LEN);
  } else if (h && h->has_aaaa) {
    inet_ntop(AF_INET6, &h->aaaa, out, ADDR_LEN);
  } else if (d->rx_from_len) {
    const struct sockaddr *sa = (const struct sockaddr *)&d->rx_from;
    if (sa->sa_family == AF_INET) {
      struct sockaddr_in in;
      memcpy(&in, sa, sizeof in);
      inet_ntop(AF_INET, &in.sin_addr, out, ADDR_LEN);
    } else if (sa->sa_family == AF_INET6) {
      struct sockaddr_in6 in6;
      memcpy(&in6, sa, sizeof in6);
      inet_ntop(AF_INET6, &in6.sin6_addr, out, ADDR_LEN);
      size_t n = strlen(out);
      if (in6.sin6_scope_id && n + 12 < ADDR_LEN)
        snprintf(out + n, ADDR_LEN - n, "%%%u", (unsigned)in6.sin6_scope_id);
    }
  }
}

/* --- Peer table ----------------------------------------------------------------------- */

static void notify(mc_discovery *d, const peer *p, int online) {
  mc_peer_info info;
  info.device_uid = p->dev;
  info.name = p->name;
  info.kind = p->kind;
  info.host = p->host;
  info.group_hash = p->grp;
  info.version = p->version;
  info.address = p->address;
  info.port = p->port;
  d->cb(&info, online, d->user);
}

static int find_peer(const mc_discovery *d, const rx_service *r) {
  for (int i = 0; i < d->npeers; i++) {
    if (r->has_txt && r->dev[0] ? !strcmp(d->peers[i].dev, r->dev)
                                : name_eq(d->peers[i].instance, r->instance))
      return i;
  }
  return -1;
}

static void drop_peer(mc_discovery *d, int i) {
  peer gone = d->peers[i];
  d->peers[i] = d->peers[--d->npeers];
  notify(d, &gone, 0);
}

static int set_field(char *dst, size_t cap, const char *src) {
  if (!strcmp(dst, src)) return 0;
  copy_str(dst, cap, src, strlen(src));
  return 1;
}

/* Merges what the packet just read said about each service into the peer table. */
static void merge_packet(mc_discovery *d) {
  uint64_t now = mc_mono_ms();
  for (int k = 0; k < d->nrx; k++) {
    const rx_service *r = &d->rx[k];
    if (r->has_txt && r->dev[0] ? !strcmp(r->dev, d->device_uid)
                                : name_eq(r->instance, d->instance))
      continue; /* our own */
    int i = find_peer(d, r);
    if (r->goodbye || (r->has_txt && r->version != MC_DISCOVERY_VERSION)) {
      if (i >= 0) drop_peer(d, i);
      continue;
    }
    int changed = 0;
    if (i < 0) {
      if (!r->has_txt || !r->dev[0] || !r->has_srv) continue; /* not enough to report */
      peer *grown = realloc(d->peers, (size_t)(d->npeers + 1) * sizeof *grown);
      if (!grown) continue;
      d->peers = grown;
      i = d->npeers++;
      memset(&d->peers[i], 0, sizeof d->peers[i]);
      copy_str(d->peers[i].dev, sizeof d->peers[i].dev, r->dev, strlen(r->dev));
      changed = 1;
    }
    peer *p = &d->peers[i];
    if (r->has_txt) {
      changed |= set_field(p->name, sizeof p->name, r->name);
      changed |= set_field(p->kind, sizeof p->kind, r->kind);
      changed |= set_field(p->host, sizeof p->host, r->host);
      changed |= set_field(p->grp, sizeof p->grp, r->grp);
      changed |= p->version != r->version;
      p->version = r->version;
    }
    if (r->has_srv && r->port != p->port) {
      p->port = r->port;
      changed = 1;
    }
    set_field(p->instance, sizeof p->instance, r->instance);
    /* A peer on several interfaces answers with a different address on each. Stick with
     * the one we have until it's been silent for a scan interval, rather than flapping. */
    char addr[ADDR_LEN];
    rx_address(d, r, addr);
    if (addr[0] && !strcmp(addr, p->address)) {
      p->addr_seen_ms = now;
    } else if (addr[0] && (!p->address[0] || now - p->addr_seen_ms > d->interval_ms)) {
      set_field(p->address, sizeof p->address, addr);
      p->addr_seen_ms = now;
      changed = 1;
    }
    p->seen_ms = now;
    if (changed) notify(d, p, 1);
  }
}

/* Drops peers not heard for two scan intervals; returns when the next one would expire. */
static uint64_t expire_peers(mc_discovery *d, uint64_t now) {
  uint64_t next = UINT64_MAX;
  for (int i = d->npeers - 1; i >= 0; i--) {
    uint64_t due = d->peers[i].seen_ms + 2 * d->interval_ms;
    if (due <= now)
      drop_peer(d, i);
    else if (due < next)
      next = due;
  }
  return next;
}

/* --- Thread --------------------------------------------------------------------------- */

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

/* Waits up to ms for packets or a wake-up, and handles whatever arrived. */
static void wait_and_read(mc_discovery *d, uint64_t ms) {
  if (d->wake_fd < 0 && ms > NO_WAKE_MS) ms = NO_WAKE_MS;
  fd_set set;
  FD_ZERO(&set);
  int maxfd = -1;
  for (int i = 0; i < d->nsocks; i++) fd_add(&set, d->socks[i].fd, &maxfd);
  if (d->wake_fd >= 0) fd_add(&set, d->wake_fd, &maxfd);
  struct timeval tv;
  tv.tv_sec = (long)(ms / 1000);
  tv.tv_usec = (long)(ms % 1000 * 1000);
  if (select(maxfd + 1, &set, NULL, NULL, &tv) <= 0) return;

  if (d->wake_fd >= 0 && fd_ready(&set, d->wake_fd)) {
    char junk[16];
    while (recv(d->wake_fd, junk, sizeof junk, 0) > 0) {
    }
  }
  for (int i = 0; i < d->nsocks; i++) {
    if (!fd_ready(&set, d->socks[i].fd)) continue;
    d->cur = &d->socks[i];
    d->nrx = d->nrxh = 0;
    d->rx_from_len = 0;
    mdns_socket_listen(d->socks[i].fd, d->recvbuf, sizeof d->recvbuf, on_record, d);
    merge_packet(d);
  }
}

/* Re-lists interfaces, and reopens the IPv4 sockets if their addresses changed. Returns
 * whether it did. */
static int refresh_interfaces(mc_discovery *d) {
  struct in_addr v4[MAX_SOCKS];
  int n4;
  list_addresses(v4, MAX_SOCKS, &n4, &d->addr6, &d->has_addr6);
  if (n4 == d->naddrs4 && !memcmp(v4, d->addrs4, (size_t)n4 * sizeof v4[0])) return 0;
  memcpy(d->addrs4, v4, sizeof v4);
  d->naddrs4 = n4;
  close_socks(d, 0);
  open_ipv4(d);
  return 1;
}

static void run(void *arg) {
  mc_discovery *d = arg;
  uint64_t now = mc_mono_ms();
  announce(d, SEND_ANNOUNCE);
  browse(d);
  uint64_t next_scan = now + d->interval_ms, reannounce = now + REANNOUNCE_MS;
  for (;;) {
    mc_mutex_lock(&d->lock);
    int stop = d->stop, changed = d->group_changed || d->name_changed;
    if (d->group_changed) memcpy(d->grp, d->next_grp, sizeof d->grp);
    if (d->name_changed) memcpy(d->name, d->next_name, sizeof d->name);
    d->group_changed = d->name_changed = 0;
    mc_mutex_unlock(&d->lock);
    if (stop) break;

    now = mc_mono_ms();
    if (now >= next_scan) {
      if (refresh_interfaces(d)) changed = 1;
      browse(d);
      next_scan = now + d->interval_ms;
    }
    if (changed) {
      announce(d, SEND_ANNOUNCE);
      reannounce = now + REANNOUNCE_MS;
    } else if (reannounce && now >= reannounce) {
      announce(d, SEND_ANNOUNCE);
      reannounce = 0;
    }
    uint64_t deadline = expire_peers(d, now);
    if (next_scan < deadline) deadline = next_scan;
    if (reannounce && reannounce < deadline) deadline = reannounce;
    wait_and_read(d, deadline > now ? deadline - now : 0);
  }
  announce(d, SEND_GOODBYE);
}

/* --- API ------------------------------------------------------------------------------ */

static void discovery_free(mc_discovery *d) {
  close_socks(d, 0);
  close_socks(d, 1);
  if (d->wake_fd >= 0) mdns_socket_close(d->wake_fd);
  free(d->peers);
  mc_mutex_destroy(&d->lock);
#ifdef _WIN32
  if (d->wsa) WSACleanup();
#endif
  free(d);
}

mc_status mc_discovery_start(const char *device_uid, const char *device_name, const char *kind,
                             const char *group_hash, uint16_t port, int scan_interval_s,
                             mc_peer_cb cb, void *user, mc_discovery **out) {
  if (out) *out = NULL;
  if (!device_uid || !*device_uid || strlen(device_uid) >= MC_UID_STRLEN || !device_name ||
      !valid_kind(kind) || !valid_group_hash(group_hash) || !port || scan_interval_s < 0 ||
      !cb || !out)
    return MC_ERR_INVALID_ARG;
  mc_discovery *d = calloc(1, sizeof *d);
  if (!d) return MC_ERR_NOMEM;
  d->wake_fd = -1;
  mc_mutex_init(&d->lock);
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    discovery_free(d);
    return MC_ERR_NET;
  }
  d->wsa = 1;
#endif

  strcpy(d->device_uid, device_uid);
  copy_str(d->name, sizeof d->name, device_name, utf8_cut(device_name, NAME_MAX_BYTES));
  char host[256];
  if (mc_hostname(host, sizeof host) == MC_OK)
    copy_str(d->host, sizeof d->host, host, utf8_cut(host, NAME_MAX_BYTES));
  strcpy(d->kind, kind);
  if (group_hash) strcpy(d->grp, group_hash);
  d->port = port;
  int interval_s = scan_interval_s ? scan_interval_s : MC_DISCOVERY_DEFAULT_INTERVAL_S;
  d->interval_ms = (uint64_t)interval_s * 1000;
  d->cb = cb;
  d->user = user;

  /* "<name> (<uid tail>)": one DNS label, so no dots or control characters. */
  size_t uid_len = strlen(device_uid);
  const char *tail = device_uid + (uid_len > SHORT_UID ? uid_len - SHORT_UID : 0);
  char label[NAME_MAX_BYTES + 1];
  size_t n = utf8_cut(d->name, LABEL_NAME_BYTES);
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)d->name[i];
    label[i] = c == '.' ? '-' : c < 0x20 ? ' ' : (char)c;
  }
  label[n] = '\0';
  snprintf(d->instance, sizeof d->instance, "%s (%s)." SERVICE, n ? label : "musicapp", tail);
  snprintf(d->hostname, sizeof d->hostname, "musicapp-%s.local.", tail);

  list_addresses(d->addrs4, MAX_SOCKS, &d->naddrs4, &d->addr6, &d->has_addr6);
  open_ipv4(d);
  open_ipv6(d);
  if (!d->nsocks) {
    discovery_free(d);
    return MC_ERR_NET;
  }
  d->wake_fd = open_wake(d);
  mc_status st = mc_thread_start(&d->thread, run, d);
  if (st != MC_OK) {
    discovery_free(d);
    return st;
  }
  *out = d;
  return MC_OK;
}

mc_status mc_discovery_set_group(mc_discovery *disc, const char *group_hash) {
  if (!disc || !valid_group_hash(group_hash)) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&disc->lock);
  strcpy(disc->next_grp, group_hash ? group_hash : "");
  disc->group_changed = 1;
  mc_mutex_unlock(&disc->lock);
  wake(disc);
  return MC_OK;
}

mc_status mc_discovery_set_name(mc_discovery *disc, const char *device_name) {
  if (!disc || !device_name) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&disc->lock);
  copy_str(disc->next_name, sizeof disc->next_name, device_name,
           utf8_cut(device_name, NAME_MAX_BYTES));
  disc->name_changed = 1;
  mc_mutex_unlock(&disc->lock);
  wake(disc);
  return MC_OK;
}

void mc_discovery_stop(mc_discovery *disc) {
  if (!disc) return;
  mc_mutex_lock(&disc->lock);
  disc->stop = 1;
  mc_mutex_unlock(&disc->lock);
  wake(disc);
  mc_thread_join(&disc->thread);
  discovery_free(disc);
}

mc_status mc_discovery_group_hash(const char *group_uid, char out[MC_GROUP_HASH_STRLEN]) {
  static const char hex[] = "0123456789abcdef";
  if (!group_uid || !*group_uid || !out) return MC_ERR_INVALID_ARG;
  unsigned char digest[32];
  if (mbedtls_sha256((const unsigned char *)group_uid, strlen(group_uid), digest, 0) != 0)
    return MC_ERR_IO;
  for (int i = 0; i < 8; i++) {
    out[2 * i] = hex[digest[i] >> 4];
    out[2 * i + 1] = hex[digest[i] & 15];
  }
  out[16] = '\0';
  return MC_OK;
}

int mc_local_addresses(char (*out)[MC_LOCAL_ADDR_STRLEN], int max) {
  struct in_addr v4[MAX_SOCKS];
  struct in6_addr v6;
  int n4 = 0, has6 = 0, n = 0;
  list_addresses(v4, MAX_SOCKS, &n4, &v6, &has6);
  for (int i = 0; i < n4 && n < max; i++) {
    if ((ntohl(v4[i].s_addr) >> 24) == 127) continue; /* loopback reaches no one else */
    if (inet_ntop(AF_INET, &v4[i], out[n], MC_LOCAL_ADDR_STRLEN)) n++;
  }
  if (has6 && n < max && inet_ntop(AF_INET6, &v6, out[n], MC_LOCAL_ADDR_STRLEN)) n++;
  return n;
}
