#include "tethered_bridge.h"

#include <gio/gio.h>
#include <string.h>

enum { PROGRESS_MS = 150 };

static struct {
  mc_tethered *t;
  tethered_bridge_event_fn on_event; /* set before the mc_tethered exists, so its threads only read it */
  void *user;
  gint gen; /* bumped by every stop; events carry the one they were made in */
  GVolumeMonitor *monitor;
  gulong added_id, removed_id;
  /* Progress throttling: only the callbacks touch these, and core runs them one at a time. */
  gint64 progress_at;
  int progress_phase;
  char progress_uid[MC_UID_STRLEN];
} g_tb = {.progress_phase = -1};

typedef struct posted {
  gint gen;
  tethered_event ev;
} posted;

static void copy_str(char *dst, size_t cap, const char *src) {
  dst[0] = '\0';
  if (!src) return;
  g_strlcpy(dst, src, cap);
  const char *end = NULL;
  if (!g_utf8_validate(dst, -1, &end)) dst[end - dst] = '\0'; /* not mid-character */
}

static posted *new_event(void *user, tethered_event_kind kind) {
  posted *p = g_new0(posted, 1);
  p->gen = GPOINTER_TO_INT(user);
  p->ev.kind = kind;
  return p;
}

static gboolean deliver(gpointer data) {
  posted *p = data;
  if (g_tb.t && p->gen == g_atomic_int_get(&g_tb.gen) && g_tb.on_event) g_tb.on_event(g_tb.user, &p->ev);
  return G_SOURCE_REMOVE;
}

/* Never waits: the main thread may be waiting in mc_tethered_free() for this very thread. */
static void post(posted *p) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver, p, g_free);
}

static void on_devices(void *user) {
  post(new_event(user, TETHERED_EV_DEVICES));
}

static void on_progress(void *user, const mc_tethered_progress *pr) {
  gint64 now = g_get_monotonic_time();
  const char *uid = pr->device_uid ? pr->device_uid : "";
  gboolean same = (int)pr->phase == g_tb.progress_phase && !strcmp(uid, g_tb.progress_uid);
  if (same && now - g_tb.progress_at < PROGRESS_MS * 1000 && pr->done < pr->total) return;
  g_tb.progress_at = now;
  g_tb.progress_phase = (int)pr->phase;
  g_strlcpy(g_tb.progress_uid, uid, sizeof g_tb.progress_uid);
  posted *p = new_event(user, TETHERED_EV_PROGRESS);
  g_strlcpy(p->ev.uid, uid, sizeof p->ev.uid);
  p->ev.phase = pr->phase;
  p->ev.done = pr->done;
  p->ev.total = pr->total;
  p->ev.bytes_done = pr->bytes_done;
  p->ev.bytes_total = pr->bytes_total;
  copy_str(p->ev.title, sizeof p->ev.title, pr->title);
  post(p);
}

static void on_done(void *user, const mc_tethered_result *r) {
  g_tb.progress_phase = -1;
  posted *p = new_event(user, TETHERED_EV_DONE);
  if (r->device_uid) g_strlcpy(p->ev.uid, r->device_uid, sizeof p->ev.uid);
  p->ev.result = *r;
  p->ev.result.device_uid = NULL;
  post(p);
}

/* --- Volumes --- */

/* The mount's folder, if it's one worth probing; g_free it. */
static char *mount_dir(GMount *mount) {
  GFile *root = g_mount_get_root(mount);
  char *path = root ? g_file_get_path(root) : NULL;
  if (root) g_object_unref(root);
  if (path && strstr(path, "/gvfs/")) g_clear_pointer(&path, g_free);
  return path;
}

static void on_mount_added(GVolumeMonitor *monitor, GMount *mount, gpointer data) {
  (void)monitor, (void)data;
  char *path = g_tb.t ? mount_dir(mount) : NULL;
  if (path) mc_tethered_volume_added(g_tb.t, path, NULL);
  g_free(path);
}

static void on_mount_removed(GVolumeMonitor *monitor, GMount *mount, gpointer data) {
  (void)monitor, (void)data;
  char *path = g_tb.t ? mount_dir(mount) : NULL;
  if (path) mc_tethered_volume_removed(g_tb.t, path);
  g_free(path);
}

/* KITHSONG_TEST_VOLUMES: folders, separated by :, announced as volumes. */
static void announce_test_volumes(void) {
  const char *list = g_getenv("KITHSONG_TEST_VOLUMES");
  if (!list || !*list || !g_tb.t) return;
  char **dirs = g_strsplit(list, G_SEARCHPATH_SEPARATOR_S, -1);
  for (char **dir = dirs; *dir; dir++)
    if (**dir) mc_tethered_volume_added(g_tb.t, *dir, NULL);
  g_strfreev(dirs);
}

void tethered_bridge_scan(void) {
  if (!g_tb.t) return;
  if (g_tb.monitor) {
    GList *mounts = g_volume_monitor_get_mounts(g_tb.monitor);
    for (GList *m = mounts; m; m = m->next) on_mount_added(g_tb.monitor, m->data, NULL);
    g_list_free_full(mounts, g_object_unref);
  }
  announce_test_volumes();
}

/* --- Eject --- */

typedef struct eject_job {
  void (*done)(void *user, mc_status st, const char *message);
  void *user;
  gboolean ejecting; /* else unmounting */
} eject_job;

static void on_ejected(GObject *source, GAsyncResult *res, gpointer data) {
  eject_job *job = data;
  GError *error = NULL;
  gboolean ok = job->ejecting ? g_mount_eject_with_operation_finish(G_MOUNT(source), res, &error)
                              : g_mount_unmount_with_operation_finish(G_MOUNT(source), res, &error);
  if (job->done) job->done(job->user, ok ? MC_OK : MC_ERR_IO, error ? error->message : NULL);
  g_clear_error(&error);
  g_free(job);
}

void tethered_bridge_eject_volume(const char *mount_path, void (*done)(void *user, mc_status st, const char *message),
                                  void *user) {
  GMount *found = NULL;
  if (g_tb.monitor && mount_path) {
    GList *mounts = g_volume_monitor_get_mounts(g_tb.monitor);
    for (GList *m = mounts; m && !found; m = m->next) {
      char *path = mount_dir(m->data);
      if (path && !strcmp(path, mount_path)) found = g_object_ref(m->data);
      g_free(path);
    }
    g_list_free_full(mounts, g_object_unref);
  }
  if (!found || (!g_mount_can_eject(found) && !g_mount_can_unmount(found))) {
    if (found) g_object_unref(found);
    if (done) done(user, MC_OK, NULL);
    return;
  }
  eject_job *job = g_new0(eject_job, 1);
  job->done = done;
  job->user = user;
  job->ejecting = g_mount_can_eject(found);
  if (job->ejecting) g_mount_eject_with_operation(found, G_MOUNT_UNMOUNT_NONE, NULL, NULL, on_ejected, job);
  else g_mount_unmount_with_operation(found, G_MOUNT_UNMOUNT_NONE, NULL, NULL, on_ejected, job);
  g_object_unref(found);
}

/* --- Lifetime --- */

void tethered_bridge_start(mc_library *lib, const char *config_dir, tethered_bridge_event_fn on_event, void *user) {
  tethered_bridge_stop();
  if (!lib || !config_dir) return;
  g_tb.on_event = on_event;
  g_tb.user = user;
  g_tb.progress_phase = -1;
  static const mc_tethered_events events = {on_devices, on_progress, on_done};
  gint gen = g_atomic_int_get(&g_tb.gen);
  if (mc_tethered_new(lib, config_dir, &events, GINT_TO_POINTER(gen), &g_tb.t) != MC_OK) {
    g_tb.t = NULL;
    return;
  }
  g_tb.monitor = g_volume_monitor_get();
  g_tb.added_id = g_signal_connect(g_tb.monitor, "mount-added", G_CALLBACK(on_mount_added), NULL);
  g_tb.removed_id = g_signal_connect(g_tb.monitor, "mount-removed", G_CALLBACK(on_mount_removed), NULL);
  tethered_bridge_scan();
}

void tethered_bridge_stop(void) {
  if (g_tb.monitor) {
    g_signal_handler_disconnect(g_tb.monitor, g_tb.added_id);
    g_signal_handler_disconnect(g_tb.monitor, g_tb.removed_id);
    g_clear_object(&g_tb.monitor);
  }
  if (!g_tb.t) return;
  g_atomic_int_inc(&g_tb.gen); /* events still queued are dropped */
  mc_tethered_free(g_tb.t);
  g_tb.t = NULL;
}

mc_tethered *tethered_bridge_get(void) {
  return g_tb.t;
}

void tethered_bridge_set_scrobbler(mc_scrobbler *sc) {
  if (g_tb.t) mc_tethered_set_scrobbler(g_tb.t, sc);
}

gboolean tethered_bridge_has(const char *uid) {
  if (!g_tb.t || !uid) return FALSE;
  mc_tethered_device_list *list = NULL;
  gboolean has = FALSE;
  if (mc_tethered_list(g_tb.t, &list) == MC_OK && list) {
    for (int i = 0; i < list->count && !has; i++) has = !strcmp(list->items[i].uid, uid);
    mc_tethered_device_list_free(list);
  }
  return has;
}
