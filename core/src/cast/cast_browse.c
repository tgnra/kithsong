/* Browsing for Cast devices: see cast_browse.h.
 *
 * A Cast device answers a PTR question for _googlecast._tcp.local. with its instance
 * ("Chromecast-<hex>._googlecast._tcp.local."), and nearly always puts the rest alongside:
 *   <instance>   SRV  0 0 8009 <uuid>.local.
 *   <instance>   TXT  id=<hex> fn=Living Room TV md=Chromecast ve=05 ...
 *   <uuid>.local A    192.168.1.23
 * What's missing is asked for: SRV and TXT of an instance seen without them, A of a host seen
 * without an address (failing which, the packet's source address does). A device is listed
 * once its id, port and address are known.
 *
 * Questions go out at once, then 1, 2, 4... seconds apart, up to every 20 s. A device not
 * heard for three of those (or a minute, whichever is less) is gone, as is one that says
 * goodbye (TTL 0). The list is reported SETTLE_MS after starting whatever it holds, so one
 * kept from an earlier run loses what no longer answers. Interfaces are re-listed and deaf sockets reopened as discovery.c does. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* inet_ntop */
#endif

#include "cast/cast_browse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

#include "discovery/msock.h"
#include "mdns.h"
#include "util/platform.h"

#define SERVICE "_googlecast._tcp.local."

enum {
  DNS_NAME_LEN = 256,
  FIRST_INTERVAL_MS = 1000,
  MAX_INTERVAL_MS = 20000,
  EXPIRE_MS = 60000, /* at the most */
  SETTLE_MS = 2500,  /* devices have answered the first two questions by then */
  TXT_MAX = 24,
  SEND_BYTES = 1024,
  RECV_BYTES = 9216
};

typedef struct entry {
  char instance[DNS_NAME_LEN];
  char target[DNS_NAME_LEN]; /* the SRV host, "" until known */
  mc_cast_device device;     /* host "" until known */
  int has_srv, has_txt;
  int addr_from_a;           /* host is the target's A/AAAA, not just where a packet came from */
  uint64_t seen_ms;
  uint64_t asked_ms;         /* when what's missing was last asked for */
} entry;

struct cast_browser {
  cast_browse_cb cb;
  void *user;
  mc_thread thread;
  mc_mutex lock;
  int stop; /* under lock */

  /* The browser's thread only. */
  mc_msocks ms;
  entry *entries;
  int nentries;
  int changed;
  char from[MC_CAST_ADDR_LEN]; /* the source of the packet being read */
  uint32_t sendbuf[SEND_BYTES / 4];
  uint32_t recvbuf[RECV_BYTES / 4];
};

static int lower(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int name_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (lower((unsigned char)*a) != lower((unsigned char)*b)) return 0;
  return *a == *b;
}

/* "<label>._googlecast._tcp.local." */
static int is_instance(const char *name) {
  size_t n = strlen(name), s = strlen(SERVICE);
  return n > s + 1 && name[n - s - 1] == '.' && name_eq(name + n - s, SERVICE);
}

static void copy_n(char *dst, size_t cap, const char *src, size_t len) {
  if (len >= cap) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static int key_is(const char *key, size_t len, const char *want) {
  return len == strlen(want) && !memcmp(key, want, len);
}

void cast_browse_txt(mc_cast_device *device, const char *key, size_t key_len, const char *value,
                     size_t value_len) {
  if (!value) value_len = 0;
  if (key_is(key, key_len, "id")) copy_n(device->id, sizeof device->id, value, value_len);
  else if (key_is(key, key_len, "fn")) copy_n(device->name, sizeof device->name, value, value_len);
  else if (key_is(key, key_len, "md")) copy_n(device->model, sizeof device->model, value, value_len);
}

static void read_name(const void *data, size_t size, size_t off, char out[DNS_NAME_LEN]) {
  mdns_string_t s = mdns_string_extract(data, size, &off, out, DNS_NAME_LEN - 1);
  out[s.str ? s.length : 0] = '\0';
}

static entry *entry_for(cast_browser *b, const char *instance, int create) {
  for (int i = 0; i < b->nentries; i++)
    if (name_eq(b->entries[i].instance, instance)) return &b->entries[i];
  if (!create) return NULL;
  entry *grown = realloc(b->entries, (size_t)(b->nentries + 1) * sizeof *grown);
  if (!grown) return NULL;
  b->entries = grown;
  entry *e = &b->entries[b->nentries++];
  memset(e, 0, sizeof *e);
  copy_n(e->instance, sizeof e->instance, instance, strlen(instance));
  e->seen_ms = mc_mono_ms();
  return e;
}

static int complete(const entry *e) {
  return e->has_srv && e->has_txt && e->device.id[0] && e->device.host[0] && e->device.port;
}

/* Drops entry i: a goodbye, or not heard for too long. */
static void drop(cast_browser *b, int i) {
  if (complete(&b->entries[i])) b->changed = 1;
  b->entries[i] = b->entries[--b->nentries];
}

static void set_host(cast_browser *b, entry *e, const char *addr, int from_a) {
  if (!addr[0] || (e->addr_from_a && !from_a)) return;
  if (strcmp(e->device.host, addr) != 0) {
    copy_n(e->device.host, sizeof e->device.host, addr, strlen(addr));
    if (complete(e)) b->changed = 1;
  }
  e->addr_from_a = e->addr_from_a || from_a;
}

/* Gives every entry whose SRV target is host the address. */
static void host_address(cast_browser *b, const char *host, const char *addr) {
  for (int i = 0; i < b->nentries; i++)
    if (b->entries[i].target[0] && name_eq(b->entries[i].target, host)) set_host(b, &b->entries[i], addr, 1);
}

static int on_record(int sock, const struct sockaddr *from, size_t addrlen, mdns_entry_type_t type,
                     uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void *data,
                     size_t size, size_t name_offset, size_t name_length, size_t record_offset,
                     size_t record_length, void *user) {
  (void)sock, (void)query_id, (void)rclass, (void)name_length;
  cast_browser *b = user;
  if (type == MDNS_ENTRYTYPE_QUESTION) return 0;
  if (!b->from[0] && from) {
    if (from->sa_family == AF_INET && addrlen >= sizeof(struct sockaddr_in)) {
      struct sockaddr_in in;
      memcpy(&in, from, sizeof in);
      inet_ntop(AF_INET, &in.sin_addr, b->from, sizeof b->from);
    }
  }
  char name[DNS_NAME_LEN], target[DNS_NAME_LEN];
  read_name(data, size, name_offset, name);
  uint64_t now = mc_mono_ms();
  entry *e;
  if (rtype == MDNS_RECORDTYPE_PTR && name_eq(name, SERVICE)) {
    mdns_string_t t = mdns_record_parse_ptr(data, size, record_offset, record_length, target, sizeof target - 1);
    target[t.str ? t.length : 0] = '\0';
    if (!is_instance(target)) return 0;
    if (ttl == 0) {
      if ((e = entry_for(b, target, 0))) drop(b, (int)(e - b->entries));
    } else if ((e = entry_for(b, target, 1))) {
      e->seen_ms = now;
    }
  } else if (rtype == MDNS_RECORDTYPE_SRV && is_instance(name) && (e = entry_for(b, name, ttl != 0))) {
    if (ttl == 0) {
      drop(b, (int)(e - b->entries));
      return 0;
    }
    mdns_record_srv_t srv = mdns_record_parse_srv(data, size, record_offset, record_length, target, sizeof target - 1);
    if (!srv.name.str) return 0;
    copy_n(e->target, sizeof e->target, srv.name.str, srv.name.length);
    if (e->device.port != srv.port && complete(e)) b->changed = 1;
    e->device.port = srv.port;
    e->seen_ms = now;
    if (!e->has_srv) {
      e->has_srv = 1;
      if (complete(e)) b->changed = 1;
    }
  } else if (rtype == MDNS_RECORDTYPE_TXT && is_instance(name) && (e = entry_for(b, name, ttl != 0))) {
    if (ttl == 0) {
      drop(b, (int)(e - b->entries));
      return 0;
    }
    mdns_record_txt_t txt[TXT_MAX];
    size_t n = mdns_record_parse_txt(data, size, record_offset, record_length, txt, TXT_MAX);
    mc_cast_device was = e->device;
    for (size_t i = 0; i < n; i++)
      cast_browse_txt(&e->device, txt[i].key.str, txt[i].key.length, txt[i].value.str, txt[i].value.length);
    int had = e->has_txt;
    e->has_txt = 1;
    e->seen_ms = now;
    if (complete(e) && (!had || memcmp(&was, &e->device, sizeof was) != 0)) b->changed = 1;
  } else if (rtype == MDNS_RECORDTYPE_A && ttl) {
    struct sockaddr_in a;
    mdns_record_parse_a(data, size, record_offset, record_length, &a);
    char addr[MC_CAST_ADDR_LEN];
    if (a.sin_addr.s_addr != htonl(INADDR_ANY) && inet_ntop(AF_INET, &a.sin_addr, addr, sizeof addr))
      host_address(b, name, addr);
  } else if (rtype == MDNS_RECORDTYPE_AAAA && ttl) {
    struct sockaddr_in6 a6;
    mdns_record_parse_aaaa(data, size, record_offset, record_length, &a6);
    char addr[MC_CAST_ADDR_LEN];
    int has_v4 = 0;
    for (int i = 0; i < b->nentries; i++)
      if (b->entries[i].target[0] && name_eq(b->entries[i].target, name) && b->entries[i].addr_from_a &&
          !strchr(b->entries[i].device.host, ':'))
        has_v4 = 1;
    /* IPv4 first: an IPv6 one only for a device with no IPv4 address. */
    if (!has_v4 && mc_msocks_usable_ipv6(&a6.sin6_addr) && inet_ntop(AF_INET6, &a6.sin6_addr, addr, sizeof addr))
      host_address(b, name, addr);
  }
  return 0;
}

static void on_packet(void *user, mc_msock *s) {
  cast_browser *b = user;
  b->from[0] = '\0';
  uint64_t start = mc_mono_ms();
  mdns_socket_listen(s->fd, b->recvbuf, sizeof b->recvbuf, on_record, b);
  /* What the packet named but gave no address for takes its source, which is the device's own
   * as long as it answered for itself, as Cast devices do. Its A record wins once it comes. */
  for (int i = 0; i < b->nentries && b->from[0]; i++)
    if (!b->entries[i].device.host[0] && b->entries[i].seen_ms >= start) set_host(b, &b->entries[i], b->from, 0);
}

static void send_query(cast_browser *b, mdns_record_type_t type, const char *name) {
  for (int i = 0; i < b->ms.nsocks; i++)
    if (mdns_query_send(b->ms.socks[i].fd, type, name, strlen(name), b->sendbuf, sizeof b->sendbuf, 0) < 0)
      b->ms.socks[i].send_failed = 1;
}

/* Asks again for what entries are missing, at most once a second each. */
static void ask_missing(cast_browser *b, uint64_t now) {
  for (int i = 0; i < b->nentries; i++) {
    entry *e = &b->entries[i];
    if (complete(e) || now - e->asked_ms < 1000) continue;
    e->asked_ms = now;
    if (!e->has_srv) send_query(b, MDNS_RECORDTYPE_SRV, e->instance);
    if (!e->has_txt) send_query(b, MDNS_RECORDTYPE_TXT, e->instance);
    if (e->has_srv && e->target[0] && !e->addr_from_a) send_query(b, MDNS_RECORDTYPE_A, e->target);
  }
}

static int by_name(const void *a, const void *b) {
  const mc_cast_device *x = a, *y = b;
  const char *nx = x->name[0] ? x->name : x->model, *ny = y->name[0] ? y->name : y->model;
  int c = strcmp(nx, ny);
  return c ? c : strcmp(x->id, y->id);
}

/* Tells the callback what's known, if it changed. Two instances with the same id (a device
 * that changed its instance name) are listed once. */
static void report(cast_browser *b) {
  if (!b->changed) return;
  b->changed = 0;
  mc_cast_device *list = malloc((size_t)(b->nentries ? b->nentries : 1) * sizeof *list);
  if (!list) return;
  int n = 0;
  for (int i = 0; i < b->nentries; i++) {
    if (!complete(&b->entries[i])) continue;
    int dup = 0;
    for (int k = 0; k < n && !dup; k++) dup = !strcmp(list[k].id, b->entries[i].device.id);
    if (!dup) list[n++] = b->entries[i].device;
  }
  qsort(list, (size_t)n, sizeof *list, by_name);
  b->cb(b->user, list, n);
  free(list);
}

static void run(void *arg) {
  cast_browser *b = arg;
  uint64_t interval = FIRST_INTERVAL_MS, now = mc_mono_ms();
  send_query(b, MDNS_RECORDTYPE_PTR, SERVICE);
  uint64_t next = now + interval, settled = now + SETTLE_MS;
  for (;;) {
    mc_mutex_lock(&b->lock);
    int stop = b->stop;
    mc_mutex_unlock(&b->lock);
    if (stop) break;
    now = mc_mono_ms();
    if (now >= next) {
      if (!mc_msocks_refresh(&b->ms)) mc_msocks_reopen_deaf(&b->ms, now, interval + interval / 2 + 1000);
      send_query(b, MDNS_RECORDTYPE_PTR, SERVICE);
      interval = interval * 2 > MAX_INTERVAL_MS ? MAX_INTERVAL_MS : interval * 2;
      next = now + interval;
    }
    if (settled && now >= settled) { /* what a list from before still has, but nothing answered for, goes */
      b->changed = 1;
      settled = 0;
    }
    ask_missing(b, now);
    uint64_t expire = 3 * MAX_INTERVAL_MS < EXPIRE_MS ? 3 * MAX_INTERVAL_MS : EXPIRE_MS;
    for (int i = b->nentries - 1; i >= 0; i--)
      if (now - b->entries[i].seen_ms > expire) drop(b, i);
    report(b);
    uint64_t wait = next > now ? next - now : 0;
    if (wait > 1000) wait = 1000; /* for ask_missing() and expiry */
    mc_msocks_wait(&b->ms, wait, on_packet, b);
  }
}

mc_status cast_browser_start(cast_browse_cb cb, void *user, cast_browser **out) {
  if (!cb || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return MC_ERR_NET;
#endif
  cast_browser *b = calloc(1, sizeof *b);
  if (!b) {
#ifdef _WIN32
    WSACleanup();
#endif
    return MC_ERR_NOMEM;
  }
  b->cb = cb;
  b->user = user;
  mc_mutex_init(&b->lock);
  mc_msocks_open(&b->ms);
  mc_status st = mc_thread_start(&b->thread, run, b);
  if (st != MC_OK) {
    mc_msocks_close(&b->ms);
    mc_mutex_destroy(&b->lock);
    free(b);
#ifdef _WIN32
    WSACleanup();
#endif
    return st;
  }
  *out = b;
  return MC_OK;
}

void cast_browser_stop(cast_browser *b) {
  if (!b) return;
  mc_mutex_lock(&b->lock);
  b->stop = 1;
  mc_mutex_unlock(&b->lock);
  mc_msocks_wake(&b->ms);
  mc_thread_join(&b->thread);
  mc_msocks_close(&b->ms);
  free(b->entries);
  mc_mutex_destroy(&b->lock);
  free(b);
#ifdef _WIN32
  WSACleanup();
#endif
}
