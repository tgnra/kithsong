/* The rate limit on automatic sync (MC_SYNC_AUTO_INTERVAL_MS): after a complete session
 * with a member, this device starts no automatic one with it for an interval, but serves
 * the member's, still syncs on mc_sync_now() / mc_sync_confirm_now(), retries after a
 * failure, and syncs again once the interval is over. Two devices in one process over
 * loopback, discovery off, as in test_sync_timeout.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"
#include "sync/channel.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
#define MAX_EVENTS 64
/* How long the limited device is watched for a session it mustn't start: long enough for
 * a change (CHANGE_DELAY_MS) and a background request to be acted on if nothing held them. */
#define HOLD_MS 7000
/* A short interval, for the test that waits it out. */
#define SHORT_MS 3000
#define HOUR_MS ((uint64_t)MC_SYNC_AUTO_INTERVAL_MS)

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
  int codes;
  int results;
  mc_pair_reason reason;
  mc_status done[MAX_EVENTS]; /* the peer is always the other device */
  uint64_t done_ms[MAX_EVENTS];
  int ndone;
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
  mc_mutex_lock(&d->m);
  if (d->ndone < MAX_EVENTS) {
    d->done_ms[d->ndone] = mc_mono_ms();
    d->done[d->ndone++] = r->status;
  }
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done, NULL};

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

/* Tells x that y listens at port, as discovery would. */
static void point_at(dev *x, dev *y, uint16_t port) {
  mc_sync_test_peer(x->sync, y->uid, y->label, "desktop", "", "127.0.0.1", port, 1);
}

static int count_done(dev *d) {
  mc_mutex_lock(&d->m);
  int n = d->ndone;
  mc_mutex_unlock(&d->m);
  return n;
}

/* Waits for d's next session report after seen; MC_ERR_UNIMPLEMENTED if none came. *at
 * (may be NULL): when it came. */
static mc_status wait_done_at(dev *d, int seen, int wait_ms, uint64_t *at) {
  uint64_t end = mc_mono_ms() + (uint64_t)wait_ms;
  mc_mutex_lock(&d->m);
  while (d->ndone <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 50);
  mc_status st = d->ndone > seen ? d->done[seen] : MC_ERR_UNIMPLEMENTED;
  if (at) *at = d->ndone > seen ? d->done_ms[seen] : 0;
  mc_mutex_unlock(&d->m);
  return st;
}

static mc_status wait_done(dev *d, int seen, int wait_ms) {
  return wait_done_at(d, seen, wait_ms, NULL);
}

/* x pairs with y by address and they finish their first session. */
static int pair(dev *x, dev *y) {
  int codes = y->codes, sx = count_done(x), sy = count_done(y);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  char code[MC_PAIR_CODE_LEN + 1] = "";
  mc_mutex_lock(&y->m);
  while (y->codes == codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 50);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_mutex_lock(&x->m);
  while (!x->results && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 50);
  int paired = x->results && x->reason == MC_PAIR_PAIRED;
  mc_mutex_unlock(&x->m);
  CHECK(paired);
  return paired && wait_done(x, sx, WAIT_MS) == MC_OK && wait_done(y, sy, WAIT_MS) == MC_OK;
}

/* x's peer entry for y: its auto_after_ms (mono, 0 if none), backoff and busy. */
static uint64_t auto_after(dev *x, dev *y, uint32_t *backoff, int *busy) {
  uint64_t v = 0;
  mc_mutex_lock(&x->sync->lock);
  for (int i = 0; i < x->sync->npeers; i++) {
    mc_sync_peer *p = &x->sync->peers[i];
    if (strcmp(p->uid, y->uid) != 0) continue;
    v = p->auto_after_ms;
    if (backoff) *backoff = p->backoff_ms;
    if (busy) *busy = p->busy;
  }
  mc_mutex_unlock(&x->sync->lock);
  return v;
}

/* x syncs with y on mc_sync_now() until a session leaves the pair at rest: complete (both
 * hold the limit) and nothing running. The first after pairing or a press of Sync can have
 * a follow-up. */
static int settle(dev *x, dev *y) {
  for (int i = 0; i < 5; i++) {
    int sx = count_done(x), sy = count_done(y);
    CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
    if (wait_done(x, sx, WAIT_MS) != MC_OK || wait_done(y, sy, WAIT_MS) != MC_OK) continue;
    mc_sleep_ms(300);
    int bx = 0, by = 0;
    if (auto_after(x, y, NULL, &bx) && auto_after(y, x, NULL, &by) && !bx && !by) return 1;
  }
  return 0;
}

/* What x's mc_sync_list_devices() says of y's auto_after_ms; -1 if y isn't listed. */
static int64_t listed_auto_after(dev *x, dev *y) {
  mc_sync_device_list *list = NULL;
  int64_t v = -1;
  if (mc_sync_list_devices(x->sync, &list) != MC_OK) return -1;
  for (int i = 0; i < list->count; i++)
    if (!strcmp(list->items[i].uid, y->uid)) v = list->items[i].auto_after_ms;
  mc_sync_device_list_free(list);
  return v;
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_rate.d", scratch);
  remove_tree(root);

  static dev d1, d2;
  if (!dev_open(&d1, root, "one") || !dev_open(&d2, root, "two")) return TEST_RESULT();
  /* a is the lower uid, the preferred initiator, so its retries aren't left to b. b runs
   * manually throughout: it starts sessions only when told to. */
  dev *a = strcmp(d1.uid, d2.uid) < 0 ? &d1 : &d2, *b = a == &d1 ? &d2 : &d1;
  if (!dev_start(a) || !dev_start(b)) return TEST_RESULT();
  if (!pair(a, b)) return TEST_RESULT();
  CHECK(settle(a, b));

  /* A complete session sets the limit on both ends: the initiator's and the server's. */
  uint64_t now = mc_mono_ms();
  uint64_t a_after = auto_after(a, b, NULL, NULL);
  CHECK(a_after > now + HOUR_MS - 60000 && a_after <= now + HOUR_MS);
  uint64_t b_after = auto_after(b, a, NULL, NULL);
  CHECK(b_after > now + HOUR_MS - 60000 && b_after <= now + HOUR_MS);
  int64_t wall = mc_wall_ms(), listed = listed_auto_after(a, b);
  CHECK(listed > wall + (int64_t)HOUR_MS - 60000 && listed <= wall + (int64_t)HOUR_MS + 1000);

  /* Within the hour, a schedules nothing with b: not periodically, not for a change here,
   * not for a background request, not when b is seen online again. */
  {
    mc_sync_set_test_manual(a->sync, 0);
    int sa = count_done(a), sb = count_done(b);
    point_at(a, b, mc_sync_port(b->sync));
    mc_sync_notify_changed(a->sync);
    CHECK(mc_sync_background(a->sync, b->uid) == MC_OK);
    CHECK(mc_sync_background(a->sync, NULL) == MC_OK);
    mc_sleep_ms(HOLD_MS);
    CHECK(count_done(a) == sa);
    CHECK(count_done(b) == sb);
    CHECK(auto_after(a, b, NULL, NULL) == a_after);
  }

  /* b may start one with a at any time, which a serves; it counts as a complete session
   * for a's schedule too. */
  {
    int sa = count_done(a), sb = count_done(b);
    CHECK(mc_sync_now(b->sync, a->uid) == MC_OK);
    CHECK(wait_done(b, sb, WAIT_MS) == MC_OK);
    CHECK(wait_done(a, sa, WAIT_MS) == MC_OK);
    mc_sleep_ms(300);
    CHECK(auto_after(a, b, NULL, NULL) > a_after);
  }

  /* The Sync button isn't held back. Its session confirms the profiles, which needs a
   * follow-up; that isn't held back either, since the session wasn't complete. */
  {
    int sa = count_done(a);
    CHECK(mc_sync_confirm_now(a->sync, b->uid) == MC_OK);
    CHECK(wait_done(a, sa, WAIT_MS) == MC_OK);
    /* Nor is mc_sync_now(). */
    CHECK(settle(a, b));
  }

  /* A failed session lifts the limit: the retry goes ahead after the backoff, not in an
   * hour. b's address answers TCP and nothing more (a connects there first, so tries no
   * other); with a short timeout a's session fails. */
  {
    mc_listener *quiet = NULL;
    CHECK(mc_listener_open(0, &quiet) == MC_OK);
    if (!quiet) return TEST_RESULT();
    mc_channel_set_default_timeout(1000);
    point_at(a, b, mc_listener_port(quiet));
    int sa = count_done(a);
    CHECK(mc_sync_now(a->sync, b->uid) == MC_OK);
    CHECK(wait_done(a, sa, WAIT_MS) == MC_ERR_NET);
    mc_channel_set_default_timeout(MC_CHANNEL_TIMEOUT_MS);
    uint32_t backoff = 0;
    CHECK(auto_after(a, b, &backoff, NULL) == 0);
    CHECK(backoff > 0);
    CHECK(listed_auto_after(a, b) == 0);
    /* Back on the network: a retries by itself (it isn't manual). Failed attempts at the
     * quiet address before the switch would show as MC_ERR_NET; wait for the success. */
    point_at(a, b, mc_sync_port(b->sync));
    uint64_t end = mc_mono_ms() + backoff + WAIT_MS;
    int seen = sa + 1, ok = 0;
    while (!ok && mc_mono_ms() < end) {
      mc_status st = wait_done(a, seen, (int)(end - mc_mono_ms()));
      if (st == MC_ERR_UNIMPLEMENTED) break;
      ok = st == MC_OK;
      seen++;
    }
    CHECK(ok);
    mc_listener_close(quiet);
  }

  /* With a short interval: a held background request goes ahead once it runs out. */
  {
    mc_sync_set_test_auto_interval(a->sync, SHORT_MS);
    mc_sync_set_test_auto_interval(b->sync, SHORT_MS);
    CHECK(settle(a, b));
    uint64_t after = auto_after(a, b, NULL, NULL);
    CHECK(after > mc_mono_ms());
    int sa = count_done(a);
    CHECK(mc_sync_background(a->sync, b->uid) == MC_OK);
    uint64_t at = 0;
    CHECK(wait_done_at(a, sa, WAIT_MS, &at) == MC_OK);
    CHECK(at >= after);
    mc_sync_set_test_manual(a->sync, 1);
    mc_sleep_ms(1000); /* whatever a started on its own before going manual is over */
  }

  /* The limit survives a restart: it is seeded from the last session in sync.db. */
  {
    CHECK(settle(a, b));
    mc_sync *again = NULL;
    CHECK(mc_sync_new(a->lib, a->config, "again", MC_DEVICE_DESKTOP, NULL, NULL, &again) == MC_OK);
    if (again) {
      uint64_t seeded = 0;
      now = mc_mono_ms();
      mc_mutex_lock(&again->lock);
      for (int i = 0; i < again->npeers; i++)
        if (!strcmp(again->peers[i].uid, b->uid)) seeded = again->peers[i].auto_after_ms;
      mc_mutex_unlock(&again->lock);
      CHECK(seeded > now + HOUR_MS - 60000 && seeded <= now + HOUR_MS);
      mc_sync_free(again);
    }
  }

  dev_free(&d1);
  dev_free(&d2);
  remove_tree(root);
  return TEST_RESULT();
}
