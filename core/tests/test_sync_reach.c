/* Reachability: reach checks between two devices, a firewall on either side (a closed port
 * standing in for it), a device of an older version that closes the check, and the evidence
 * sessions leave. Two devices in one process over loopback, discovery off, as in
 * test_sync_rate.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000

/* --- Scratch dirs --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  char (*more)[512] = realloc(n->items, (size_t)(n->count + 1) * sizeof *more);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd')
      remove_tree(path);
    else
      mc_remove(path);
  }
  free(n.items);
  mc_rmdir(dir);
}

/* --- A device --- */

typedef struct dev {
  const char *label;
  char config[1024], libdir[1024];
  mc_library *lib;
  mc_sync *sync;
  char uid[MC_UID_STRLEN];

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes, results, ndone, reach_events, self_events;
  mc_pair_reason reason;
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name;
  (void)attempts;
  mc_mutex_lock(&d->m);
  snprintf(d->code, sizeof d->code, "%s", code);
  d->codes++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_pair_result(void *user, const mc_pair_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->reason = r->reason;
  d->results++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_sync_done(void *user, const mc_sync_result *r) {
  dev *d = user;
  (void)r;
  mc_mutex_lock(&d->m);
  d->ndone++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_reach(void *user, const char *uid) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->reach_events++;
  if (uid && !strcmp(uid, d->uid)) d->self_events++;
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done,
                                        on_reach, NULL, NULL, NULL};

static int dev_open(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) == MC_OK);
  if (!d->sync) return 0;
  snprintf(d->uid, sizeof d->uid, "%s", mc_sync_device_uid(d->sync));
  return 1;
}

static int dev_start(dev *d) {
  mc_sync_set_test_options(d->sync, 0, 0);
  mc_sync_set_test_manual(d->sync, 1);
  CHECK(mc_sync_start(d->sync) == MC_OK);
  char hints[64];
  snprintf(hints, sizeof hints, "[\"127.0.0.1:%u\"]", (unsigned)mc_sync_port(d->sync));
  mc_sync_set_test_hints(d->sync, hints);
  return mc_sync_port(d->sync) != 0;
}

static void dev_free(dev *d) {
  mc_sync_free(d->sync);
  mc_library_close(d->lib);
  mc_cond_destroy(&d->c);
  mc_mutex_destroy(&d->m);
}

/* Tells x that uid listens at port on loopback, as discovery would. */
static void point_at(dev *x, const char *uid, uint16_t port) {
  mc_sync_test_peer(x->sync, uid, "peer", "desktop", "", "127.0.0.1", port, 1);
}

/* A port nothing listens on: as good as one a firewall rejects. Not a freed ephemeral port,
 * which a test running alongside may take. */
static uint16_t closed_port(void) {
  mc_socket *s = NULL;
  if (mc_socket_connect("127.0.0.1", 1, 1000, &s) == MC_OK) {
    mc_socket_close(s);
    return 0;
  }
  return 1;
}

/* x's peer entry for uid: whether a reach check is running or asked for. */
static int reach_pending(dev *x, const char *uid) {
  int pending = 0;
  mc_mutex_lock(&x->sync->lock);
  for (int i = 0; i < x->sync->npeers; i++) {
    mc_sync_peer *p = &x->sync->peers[i];
    if (!strcmp(p->uid, uid)) pending = p->reaching || p->reach_now;
  }
  mc_mutex_unlock(&x->sync->lock);
  return pending;
}

/* x checks uid and waits for the check to end. */
static int check(dev *x, const char *uid) {
  CHECK(mc_sync_check_reach(x->sync, uid) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  while (reach_pending(x, uid) && mc_mono_ms() < end) mc_sleep_ms(20);
  return !reach_pending(x, uid);
}

/* Waits until x's view of uid is to_them and to_us. The device checked records what it
 * learnt after answering, so its view may lag a moment behind the checker's. */
static int wait_reach(dev *x, const char *uid, mc_sync_reach to_them, mc_sync_reach to_us) {
  uint64_t end = mc_mono_ms() + 5000;
  mc_sync_reach t = MC_REACH_UNKNOWN, u = MC_REACH_UNKNOWN;
  do {
    if (mc_sync_reach_peer(x->sync, uid, &t, &u) == MC_OK && t == to_them && u == to_us) return 1;
    mc_sleep_ms(20);
  } while (mc_mono_ms() < end);
  fprintf(stderr, "%s sees %.8s as %d/%d, wanted %d/%d\n", x->label, uid, t, u, to_them, to_us);
  return 0;
}

static mc_sync_self_reach self_reach(dev *x) {
  mc_sync_self_reach r;
  CHECK(mc_sync_reach_self(x->sync, &r) == MC_OK);
  return r;
}

/* x pairs with y by address and they finish their first session. */
static int pair(dev *x, dev *y) {
  int codes = y->codes, sx = x->ndone, sy = y->ndone;
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  char code[MC_PAIR_CODE_LEN + 1] = "";
  mc_mutex_lock(&y->m);
  while (y->codes == codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 50);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_mutex_lock(&x->m);
  while ((!x->results || x->ndone <= sx) && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 50);
  int paired = x->results && x->reason == MC_PAIR_PAIRED;
  mc_mutex_unlock(&x->m);
  mc_mutex_lock(&y->m);
  while (y->ndone <= sy && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 50);
  mc_mutex_unlock(&y->m);
  CHECK(paired);
  return paired;
}

/* An older version: accepts one connection, and closes it at the unknown mode. */
static void old_peer_main(void *arg) {
  mc_listener *l = arg;
  mc_socket *s = NULL;
  if (mc_listener_accept(l, WAIT_MS, &s) == MC_OK && s) {
    unsigned char pre[4];
    mc_socket_recv_exact(s, pre, sizeof pre, WAIT_MS);
    mc_socket_close(s);
  }
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_reach.d", scratch);
  remove_tree(root);

  static dev d1, d2;
  if (!dev_open(&d1, root, "one") || !dev_open(&d2, root, "two")) return TEST_RESULT();
  dev *a = &d1, *b = &d2;

  /* Not started: nothing to say. */
  {
    mc_sync_self_reach r;
    CHECK(mc_sync_reach_self(a->sync, &r) == MC_ERR_INVALID_ARG);
    CHECK(mc_sync_check_reach(a->sync, NULL) == MC_ERR_INVALID_ARG);
  }
  if (!dev_start(a) || !dev_start(b)) return TEST_RESULT();
  CHECK(self_reach(a).state == MC_REACH_UNKNOWN);
  CHECK(self_reach(a).port == mc_sync_port(a->sync));
  CHECK(mc_sync_check_reach(a->sync, b->uid) == MC_ERR_NOT_FOUND);
  CHECK(mc_sync_reach_peer(a->sync, b->uid, NULL, NULL) == MC_ERR_NOT_FOUND);

  /* Nearby, not paired, nothing in the way: both ways work, and both ends learn it. */
  point_at(a, b->uid, mc_sync_port(b->sync));
  point_at(b, a->uid, mc_sync_port(a->sync));
  CHECK(wait_reach(a, b->uid, MC_REACH_UNKNOWN, MC_REACH_UNKNOWN));
  CHECK(check(a, b->uid));
  CHECK(wait_reach(a, b->uid, MC_REACH_OK, MC_REACH_OK));
  CHECK(wait_reach(b, a->uid, MC_REACH_OK, MC_REACH_OK));
  {
    mc_sync_self_reach r = self_reach(a);
    CHECK(r.state == MC_REACH_OK && r.ok == 1 && r.blocked == 0);
    mc_mutex_lock(&a->m);
    CHECK(a->reach_events >= 2 && a->self_events == 1);
    mc_mutex_unlock(&a->m);
    mc_sync_nearby_list *nearby = NULL;
    CHECK(mc_sync_list_nearby(a->sync, &nearby) == MC_OK);
    CHECK(nearby && nearby->count == 1 && nearby->items[0].reach == MC_REACH_OK &&
          nearby->items[0].reach_us == MC_REACH_OK);
    mc_sync_nearby_list_free(nearby);
  }

  /* A firewall on a: b can't call back. a learns others can't reach it; b, that it can't
   * reach a, though a reaches b. */
  uint16_t closed = closed_port();
  CHECK(closed != 0);
  mc_sync_set_test_reach_port(a->sync, closed);
  CHECK(check(a, b->uid));
  CHECK(wait_reach(a, b->uid, MC_REACH_OK, MC_REACH_BLOCKED));
  CHECK(wait_reach(b, a->uid, MC_REACH_BLOCKED, MC_REACH_OK));
  {
    mc_sync_self_reach r = self_reach(a);
    CHECK(r.state == MC_REACH_BLOCKED && r.ok == 0 && r.blocked == 1);
    CHECK(self_reach(b).state == MC_REACH_OK);
  }
  /* Fixed: checking again clears it. */
  mc_sync_set_test_reach_port(a->sync, 0);
  CHECK(check(a, b->uid));
  CHECK(wait_reach(a, b->uid, MC_REACH_OK, MC_REACH_OK));
  CHECK(wait_reach(b, a->uid, MC_REACH_OK, MC_REACH_OK));
  CHECK(self_reach(a).state == MC_REACH_OK);

  /* A firewall on b, seen from a: b is online (discovery sees it) but a can't connect. */
  point_at(a, b->uid, closed);
  CHECK(check(a, b->uid));
  CHECK(wait_reach(a, b->uid, MC_REACH_BLOCKED, MC_REACH_OK));
  point_at(a, b->uid, mc_sync_port(b->sync));
  CHECK(check(a, b->uid));
  CHECK(wait_reach(a, b->uid, MC_REACH_OK, MC_REACH_OK));

  /* An older version closes the check: a reaches it, and whether it reaches a stays
   * unknown. */
  {
    mc_listener *l = NULL;
    CHECK(mc_listener_open(0, &l) == MC_OK);
    char old_uid[MC_UID_STRLEN];
    CHECK(mc_uid_new(old_uid) == MC_OK);
    mc_thread t;
    if (l && mc_thread_start(&t, old_peer_main, l) == MC_OK) {
      point_at(a, old_uid, mc_listener_port(l));
      CHECK(check(a, old_uid));
      CHECK(wait_reach(a, old_uid, MC_REACH_OK, MC_REACH_UNKNOWN));
      mc_thread_join(&t);
      CHECK(self_reach(a).state == MC_REACH_OK); /* unknown doesn't count either way */
    }
    mc_listener_close(l);
  }

  /* Paired: a session b starts tells a that b reaches it, and b that it reaches a; a's
   * device list carries both. */
  if (pair(a, b)) {
    uint64_t before = mc_mono_ms();
    mc_sleep_ms(5);
    int sa = a->ndone;
    CHECK(mc_sync_now(b->sync, a->uid) == MC_OK);
    uint64_t end = mc_mono_ms() + WAIT_MS;
    mc_mutex_lock(&a->m);
    while (a->ndone <= sa && mc_mono_ms() < end) mc_cond_wait_ms(&a->c, &a->m, 50);
    mc_mutex_unlock(&a->m);
    uint64_t in_ok = 0, out_ok = 0;
    char in_addr[MC_SYNC_ADDR_MAX] = "";
    mc_mutex_lock(&a->sync->lock);
    for (int i = 0; i < a->sync->npeers; i++)
      if (!strcmp(a->sync->peers[i].uid, b->uid)) {
        in_ok = a->sync->peers[i].in_ok_ms;
        memcpy(in_addr, a->sync->peers[i].in_addr, sizeof in_addr);
      }
    mc_mutex_unlock(&a->sync->lock);
    mc_mutex_lock(&b->sync->lock);
    for (int i = 0; i < b->sync->npeers; i++)
      if (!strcmp(b->sync->peers[i].uid, a->uid)) out_ok = b->sync->peers[i].out_ok_ms;
    mc_mutex_unlock(&b->sync->lock);
    CHECK(in_ok > before);
    CHECK(out_ok > before);
    CHECK(!strcmp(in_addr, "127.0.0.1"));

    mc_sync_device_list *list = NULL;
    CHECK(mc_sync_list_devices(a->sync, &list) == MC_OK);
    for (int i = 0; list && i < list->count; i++) {
      const mc_sync_device *dv = &list->items[i];
      if (dv->is_self) CHECK(dv->reach_us == MC_REACH_OK);
      else CHECK(dv->reach == MC_REACH_OK && dv->reach_us == MC_REACH_OK);
    }
    mc_sync_device_list_free(list);
  }

  dev_free(a);
  dev_free(b);
  remove_tree(root);
  return TEST_RESULT();
}
