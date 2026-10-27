/* Sessions with a peer that goes silent, as a phone does once Android blocks its network:
 * its packets are dropped, so nothing resets the connection. The session must fail within
 * the (shortened) inactivity timeout and back off, not hang, and stopping must not wait on
 * it. Two devices in one process over loopback, discovery off, as in test_session.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"
#include "sync/channel.h"
#include "sync/frame.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
/* The shortened timeout, and how long a test allows for silence to be noticed. */
#define SILENT_MS 1000
#define NOTICE_MS 10000
#define MAX_EVENTS 32

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
  if (d->ndone < MAX_EVENTS) d->done[d->ndone++] = r->status;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done};

static int dev_start(dev *d, const char *root, const char *label) {
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

/* Waits for d's next session report after seen; MC_ERR_UNIMPLEMENTED if none came. */
static mc_status wait_done(dev *d, int seen, int wait_ms) {
  uint64_t end = mc_mono_ms() + (uint64_t)wait_ms;
  mc_mutex_lock(&d->m);
  while (d->ndone <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 50);
  mc_status st = d->ndone > seen ? d->done[seen] : MC_ERR_UNIMPLEMENTED;
  mc_mutex_unlock(&d->m);
  return st;
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

/* x's backoff for y, in ms. */
static uint32_t backoff(dev *x, dev *y) {
  uint32_t ms = 0;
  mc_mutex_lock(&x->sync->lock);
  for (int i = 0; i < x->sync->npeers; i++)
    if (!strcmp(x->sync->peers[i].uid, y->uid)) ms = x->sync->peers[i].backoff_ms;
  mc_mutex_unlock(&x->sync->lock);
  return ms;
}

/* --- A proxy that stops forwarding --- */

/* Forwards frames between a client and a server until the server has sent freeze_after of
 * them, then drops everything both ways, keeping both connections open. */
typedef struct proxy {
  mc_listener *l;
  uint16_t to_port;
  int freeze_after;
  mc_mutex m;
  int frames, frozen;
  mc_socket *client, *server;
  mc_thread main, back;
} proxy;

typedef struct pipe_arg {
  proxy *p;
  mc_socket *from, *to;
  int counts; /* the server's direction */
} pipe_arg;

static void pipe_frames(pipe_arg *a) {
  static unsigned char bufs[2][MC_FRAME_MAX];
  unsigned char *buf = bufs[a->counts];
  size_t n;
  while (mc_frame_recv(a->from, buf, MC_FRAME_MAX, &n, MC_SOCKET_FOREVER) == MC_OK) {
    mc_mutex_lock(&a->p->m);
    int frozen = a->p->frozen;
    if (a->counts && !frozen && ++a->p->frames >= a->p->freeze_after) a->p->frozen = 1;
    mc_mutex_unlock(&a->p->m);
    if (!frozen && mc_frame_send(a->to, buf, n, WAIT_MS) != MC_OK) break;
  }
}

static void pipe_back(void *arg) {
  pipe_frames(arg);
}

static void proxy_main(void *arg) {
  proxy *p = arg;
  if (mc_listener_accept(p->l, WAIT_MS, &p->client) != MC_OK || !p->client) return;
  unsigned char pre[MC_PREAMBLE_LEN];
  if (mc_socket_connect("127.0.0.1", p->to_port, WAIT_MS, &p->server) != MC_OK ||
      mc_socket_recv_exact(p->client, pre, sizeof pre, WAIT_MS) != MC_OK ||
      mc_socket_send_all(p->server, pre, sizeof pre, WAIT_MS) != MC_OK)
    return;
  pipe_arg up = {p, p->client, p->server, 0}, down = {p, p->server, p->client, 1};
  if (mc_thread_start(&p->back, pipe_back, &down) != MC_OK) return;
  pipe_frames(&up);
  mc_thread_join(&p->back);
}

static int proxy_start(proxy *p, uint16_t to_port, int freeze_after) {
  memset(p, 0, sizeof *p);
  p->to_port = to_port;
  p->freeze_after = freeze_after;
  mc_mutex_init(&p->m);
  CHECK(mc_listener_open(0, &p->l) == MC_OK);
  if (!p->l) return 0;
  CHECK(mc_thread_start(&p->main, proxy_main, p) == MC_OK);
  return 1;
}

static void proxy_stop(proxy *p) {
  mc_listener_stop(p->l);
  /* Its threads wait forever; ending the connections ends them. */
  if (p->client) mc_socket_shutdown(p->client);
  if (p->server) mc_socket_shutdown(p->server);
  mc_thread_join(&p->main);
  mc_socket_close(p->client);
  mc_socket_close(p->server);
  mc_listener_close(p->l);
  mc_mutex_destroy(&p->m);
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_timeout.d", scratch);
  remove_tree(root);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  if (!pair(&a, &b)) return TEST_RESULT();
  /* Nothing is connected now; every channel from here on gets the short timeout. */
  mc_channel_set_default_timeout(SILENT_MS);

  /* B's address answers TCP but nothing more: a listener nobody accepts from, whose
   * backlog completes the connection. A's session fails and backs off, doubling. */
  mc_listener *quiet = NULL;
  CHECK(mc_listener_open(0, &quiet) == MC_OK);
  if (!quiet) return TEST_RESULT();
  point_at(&a, &b, mc_listener_port(quiet));
  for (uint32_t want = 10000; want <= 20000; want *= 2) {
    int seen = count_done(&a);
    uint64_t t0 = mc_mono_ms();
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    CHECK(wait_done(&a, seen, NOTICE_MS) == MC_ERR_NET);
    uint64_t took = mc_mono_ms() - t0;
    CHECK(took >= SILENT_MS - 100 && took < NOTICE_MS);
    CHECK(backoff(&a, &b) == want);
  }

  /* Mid-session: through a proxy that goes quiet once B has sent two frames (its half of
   * the handshake, then HELLO). Both sides notice. */
  proxy p;
  if (proxy_start(&p, mc_sync_port(b.sync), 2)) {
    point_at(&a, &b, mc_listener_port(p.l));
    int sa = count_done(&a), sb = count_done(&b);
    uint64_t t0 = mc_mono_ms();
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    CHECK(wait_done(&a, sa, NOTICE_MS) == MC_ERR_NET);
    CHECK(wait_done(&b, sb, NOTICE_MS) == MC_ERR_NET);
    CHECK(mc_mono_ms() - t0 < NOTICE_MS);
    CHECK(backoff(&a, &b) == 40000);
    CHECK(backoff(&b, &a) == 10000);
    proxy_stop(&p);
  }

  /* Back on the network: the next session works and resets the backoff. */
  point_at(&a, &b, mc_sync_port(b.sync));
  int seen = count_done(&a);
  CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
  CHECK(wait_done(&a, seen, WAIT_MS) == MC_OK);
  CHECK(backoff(&a, &b) == 0);

  /* With the real timeout: a connection to A that never sends its preamble doesn't hold up
   * A's listener, and neither it nor a session stuck on the quiet address holds up
   * stopping. */
  mc_channel_set_default_timeout(MC_CHANNEL_TIMEOUT_MS);
  mc_socket *mute = NULL;
  CHECK(mc_socket_connect("127.0.0.1", mc_sync_port(a.sync), WAIT_MS, &mute) == MC_OK);
  point_at(&b, &a, mc_sync_port(a.sync));
  seen = count_done(&b);
  uint64_t t0 = mc_mono_ms();
  CHECK(mc_sync_now(b.sync, a.uid) == MC_OK);
  CHECK(wait_done(&b, seen, WAIT_MS) == MC_OK);
  CHECK(mc_mono_ms() - t0 < MC_CHANNEL_TIMEOUT_MS / 2);
  point_at(&a, &b, mc_listener_port(quiet));
  CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
  mc_sleep_ms(500); /* connected, and waiting on the handshake */
  t0 = mc_mono_ms();
  mc_sync_stop(a.sync);
  CHECK(mc_mono_ms() - t0 < 3000);
  mc_socket_close(mute);
  mc_listener_close(quiet);

  dev_free(&a);
  dev_free(&b);
  remove_tree(root);
  return TEST_RESULT();
}
