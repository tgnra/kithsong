/* Discovery over real mDNS multicast: two instances in this process, plus a fake peer whose
 * packets the test sends itself with mdns.h. Skipped (exit 77) when multicast isn't
 * available, e.g. in a sandbox with no network. Checks that each instance sees the other
 * with the right TXT fields, that a group change and a rename reach the other, that a goodbye takes a
 * peer offline at once, that a silent peer times out after two scan intervals, and that
 * peers speaking another protocol version are ignored. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* the socket API mdns.h uses */
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mdns.h"
#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77
#define INTERVAL_S 2 /* so a silent peer is offline after 4 s */
#define FAKE_INSTANCE "Fake (00000001)._kithsong._tcp.local."
#define FAKE_DEV "0192f1e4-7c3a-7b21-9d4e-000000000001"
#define FAKE_OLD_INSTANCE "Old (00000099)._kithsong._tcp.local."
#define FAKE_OLD_DEV "0192f1e4-7c3a-7b21-9d4e-000000000099"

typedef struct seen_peer {
  char dev[MC_UID_STRLEN];
  char name[64];
  char kind[16];
  char grp[MC_GROUP_HASH_STRLEN];
  char address[64];
  uint16_t port;
  int version;
  int online;
} seen_peer;

typedef struct observer {
  seen_peer peers[8];
  int n;
} observer;

static mc_mutex lock;

static void copy(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src ? src : "(null)");
}

static void on_peer(const mc_peer_info *info, int online, void *user) {
  observer *o = user;
  mc_mutex_lock(&lock);
  seen_peer *p = NULL;
  for (int i = 0; i < o->n && !p; i++)
    if (!strcmp(o->peers[i].dev, info->device_uid)) p = &o->peers[i];
  if (!p && o->n < 8) p = &o->peers[o->n++];
  if (p) {
    copy(p->dev, sizeof p->dev, info->device_uid);
    copy(p->name, sizeof p->name, info->name);
    copy(p->kind, sizeof p->kind, info->kind);
    copy(p->grp, sizeof p->grp, info->group_hash);
    copy(p->address, sizeof p->address, info->address);
    p->port = info->port;
    p->version = info->version;
    p->online = online;
  }
  mc_mutex_unlock(&lock);
}

/* Whether o has seen dev at all, and if so a copy of its latest state. */
static int lookup(observer *o, const char *dev, seen_peer *out) {
  int found = 0;
  mc_mutex_lock(&lock);
  for (int i = 0; i < o->n; i++)
    if (!strcmp(o->peers[i].dev, dev)) {
      if (out) *out = o->peers[i];
      found = 1;
    }
  mc_mutex_unlock(&lock);
  return found;
}

/* Waits until o has dev in the given online state (and with group hash grp, unless NULL).
 * Returns how long it took in ms, or -1 on timeout. */
static long wait_for(observer *o, const char *dev, int online, const char *grp, unsigned ms,
                     seen_peer *out) {
  uint64_t start = mc_mono_ms();
  for (;;) {
    seen_peer p;
    if (lookup(o, dev, &p) && p.online == online && (!grp || !strcmp(p.grp, grp))) {
      if (out) *out = p;
      return (long)(mc_mono_ms() - start);
    }
    if (mc_mono_ms() - start > ms) return -1;
    mc_sleep_ms(20);
  }
}

static mdns_string_t str(const char *s) {
  mdns_string_t m;
  m.str = s;
  m.length = strlen(s);
  return m;
}

static mdns_record_t txt(const char *instance, const char *key, const char *value) {
  mdns_record_t r;
  memset(&r, 0, sizeof r);
  r.name = str(instance);
  r.type = MDNS_RECORDTYPE_TXT;
  r.data.txt.key = str(key);
  r.data.txt.value = str(value);
  return r;
}

/* Announces a fake peer once, with PTR, SRV and TXT but no A record, so the receiver has
 * to fall back on the packet's source address. */
static int fake_announce(int sock, const char *instance, const char *dev, const char *version) {
  uint32_t buf[512];
  mdns_record_t ptr, extra[6];
  memset(&ptr, 0, sizeof ptr);
  ptr.name = str("_kithsong._tcp.local.");
  ptr.type = MDNS_RECORDTYPE_PTR;
  ptr.data.ptr.name = str(instance);
  memset(&extra[0], 0, sizeof extra[0]);
  extra[0].name = str(instance);
  extra[0].type = MDNS_RECORDTYPE_SRV;
  extra[0].data.srv.port = 4242;
  extra[0].data.srv.name = str("fake-peer.local.");
  extra[1] = txt(instance, "v", version);
  extra[2] = txt(instance, "dev", dev);
  extra[3] = txt(instance, "name", "Fake");
  extra[4] = txt(instance, "kind", "watch");
  extra[5] = txt(instance, "grp", "");
  return mdns_announce_multicast(sock, buf, sizeof buf, ptr, NULL, 0, extra, 6);
}

int main(void) {
  mc_mutex_init(&lock);

  char hash[MC_GROUP_HASH_STRLEN], hash2[MC_GROUP_HASH_STRLEN];
  /* printf %s 0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e | sha256sum: d9aace3493bcd914... */
  CHECK(mc_discovery_group_hash("0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", hash) == MC_OK);
  CHECK(strcmp(hash, "d9aace3493bcd914") == 0);
  CHECK(mc_discovery_group_hash("0192f1e4-7c3a-7b21-9d4e-000000000002", hash2) == MC_OK);
  CHECK(strcmp(hash2, "604f01cb000a8d2d") == 0);
  CHECK(mc_discovery_group_hash(NULL, hash) == MC_ERR_INVALID_ARG);
  CHECK(mc_discovery_group_hash("", hash) == MC_ERR_INVALID_ARG);

  observer seen_by_a = {0}, seen_by_b = {0};
  char uid_a[MC_UID_STRLEN], uid_b[MC_UID_STRLEN];
  CHECK(mc_uid_new(uid_a) == MC_OK);
  CHECK(mc_uid_new(uid_b) == MC_OK);
  mc_discovery *a = NULL, *b = NULL;
  CHECK(mc_discovery_start(uid_a, "Desk", "tablet", NULL, 1, 1, on_peer, &seen_by_a, &a) ==
        MC_ERR_INVALID_ARG);
  CHECK(mc_discovery_start(uid_a, "Desk", "desktop", "D9AACE3493BCD914", 1, 1, on_peer,
                           &seen_by_a, &a) == MC_ERR_INVALID_ARG);
  CHECK(mc_discovery_start(uid_a, "Desk", "desktop", NULL, 0, 1, on_peer, &seen_by_a, &a) ==
        MC_ERR_INVALID_ARG);
  CHECK(!a);

  mc_status st = mc_discovery_start(uid_a, "Desk", "desktop", NULL, 40001, INTERVAL_S, on_peer,
                                    &seen_by_a, &a);
  if (st == MC_ERR_NET) {
    printf("skipped: can't open mDNS sockets (no network interface, or multicast blocked)\n");
    return test_failures ? 1 : SKIP;
  }
  CHECK(st == MC_OK);
  /* A dot in the name must not split the instance label; TXT keeps it as is. */
  st = mc_discovery_start(uid_b, "Tania's phone.2", "phone", hash, 40002, INTERVAL_S, on_peer,
                          &seen_by_b, &b);
  CHECK(st == MC_OK);
  if (!a || !b) {
    mc_discovery_stop(a);
    mc_discovery_stop(b);
    return TEST_RESULT();
  }

  seen_peer pa, pb;
  if (wait_for(&seen_by_a, uid_b, 1, NULL, 5000, &pb) < 0 ||
      wait_for(&seen_by_b, uid_a, 1, NULL, 5000, &pa) < 0) {
    mc_discovery_stop(a);
    mc_discovery_stop(b);
    printf("skipped: the two instances didn't see each other within 5 s; multicast is "
           "probably filtered on this network\n");
    return test_failures ? 1 : SKIP;
  }
  printf("ran: A sees B at %s:%u, B sees A at %s:%u\n", pb.address, pb.port, pa.address,
         pa.port);
  CHECK_STR(pb.name, "Tania's phone.2");
  CHECK_STR(pb.kind, "phone");
  CHECK(strcmp(pb.grp, hash) == 0);
  CHECK(pb.port == 40002);
  CHECK(pb.version == MC_DISCOVERY_VERSION);
  CHECK(pb.address[0]);
  CHECK_STR(pa.name, "Desk");
  CHECK_STR(pa.kind, "desktop");
  CHECK_STR(pa.grp, "");
  CHECK(pa.port == 40001);
  CHECK(pa.address[0]);
  CHECK(!lookup(&seen_by_a, uid_a, NULL)); /* nobody reports itself */
  CHECK(!lookup(&seen_by_b, uid_b, NULL));

  /* Joining and leaving a group is re-announced at once. */
  CHECK(mc_discovery_set_group(a, "nothex") == MC_ERR_INVALID_ARG);
  CHECK(mc_discovery_set_group(a, hash2) == MC_OK);
  CHECK(wait_for(&seen_by_b, uid_a, 1, hash2, 1500, NULL) >= 0);
  CHECK(mc_discovery_set_group(a, NULL) == MC_OK);
  CHECK(wait_for(&seen_by_b, uid_a, 1, "", 1500, NULL) >= 0);

  /* So is a rename: TXT "name" changes, the instance label doesn't. */
  CHECK(mc_discovery_set_name(a, NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_discovery_set_name(a, "Living room") == MC_OK);
  uint64_t renamed = mc_mono_ms();
  for (;;) {
    if (lookup(&seen_by_b, uid_a, &pa) && !strcmp(pa.name, "Living room")) break;
    if (mc_mono_ms() - renamed > 1500) break;
    mc_sleep_ms(20);
  }
  CHECK_STR(pa.name, "Living room");
  CHECK(pa.online);

  /* A fake peer announces once and then falls silent; one on protocol version 2 is
   * ignored. */
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  /* Sent on the default interface and on loopback, which discovery also listens on, in
   * case a firewall drops the first. */
  struct sockaddr_in loopback;
  memset(&loopback, 0, sizeof loopback);
  loopback.sin_family = AF_INET;
  loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int socks[2] = {mdns_socket_open_ipv4(NULL), mdns_socket_open_ipv4(&loopback)}, sent = 0;
  for (int i = 0; i < 2; i++) {
    if (socks[i] < 0) continue;
    if (fake_announce(socks[i], FAKE_OLD_INSTANCE, FAKE_OLD_DEV, "2") == 0 &&
        fake_announce(socks[i], FAKE_INSTANCE, FAKE_DEV, "1") == 0)
      sent = 1;
    mdns_socket_close(socks[i]);
  }
  CHECK(sent);
  if (sent) {
    seen_peer pf;
    CHECK(wait_for(&seen_by_a, FAKE_DEV, 1, NULL, 1500, &pf) >= 0);
    CHECK_STR(pf.kind, "watch");
    CHECK(pf.port == 4242);
    CHECK(pf.address[0]); /* from the packet's source address */
    long ms = wait_for(&seen_by_a, FAKE_DEV, 0, NULL, (2 * INTERVAL_S + 3) * 1000, NULL);
    printf("silent peer went offline after %ld ms\n", ms);
    CHECK(ms >= (2 * INTERVAL_S - 1) * 1000);
    CHECK(!lookup(&seen_by_a, FAKE_OLD_DEV, NULL));
    CHECK(!lookup(&seen_by_b, FAKE_OLD_DEV, NULL));
  }
#ifdef _WIN32
  WSACleanup();
#endif

  /* B's goodbye takes it offline at A right away, long before two scan intervals pass. */
  uint64_t t0 = mc_mono_ms();
  mc_discovery_stop(b);
  unsigned stop_ms = (unsigned)(mc_mono_ms() - t0);
  long ms = wait_for(&seen_by_a, uid_b, 0, NULL, 1000, NULL);
  printf("stopping B took %u ms; A saw it go offline %ld ms later\n", stop_ms, ms);
  CHECK(stop_ms < 1000); /* woken, not waiting out the interval */
  CHECK(ms >= 0);

  t0 = mc_mono_ms();
  mc_discovery_stop(a);
  CHECK(mc_mono_ms() - t0 < 1000); /* woken, not waiting out the interval */
  mc_discovery_stop(NULL);
  mc_mutex_destroy(&lock);
  return TEST_RESULT();
}
