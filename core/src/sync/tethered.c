/* Tethered devices: kithcore/tethered.h. See "Device targets" in docs/plugins.md.
 *
 * The device list is the tethered_devices rows of sync.db, plus a connection for each one
 * plugged in (its mount path, and the formats it said it plays). A sync is a thread of its
 * own with its own library and sync.db connections:
 *
 *   open the device, list() it -> take in its plays and playlists (once each, by token) ->
 *   the wanted set (plan.c, from the tethered profile) less what it holds as wanted ->
 *   remove what's no longer wanted -> for each track to write: encode through the cache's
 *   pool (a few ahead), put(), hash the file on the device against what was put, retry once
 *   -> commit() every 50 tracks or 2 minutes -> play counts and tags through update() ->
 *   the profile's playlists -> commit(), close().
 *
 * Only tracks list() reports with a track_uid are Kithsong's: nothing else on the device is
 * ever changed or removed. */
#include "kithcore/tethered.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "library/library_internal.h"
#include "plugin/plugins_internal.h"
#include "sync/encode.h"
#include "sync/plan.h"
#include "sync/sync_db.h"
#include "sync/transfer.h"
#include "transcode/transcode_internal.h"
#include "util/platform.h"

#define COMMIT_EVERY 50                 /* tracks */
#define COMMIT_MS (2 * 60 * 1000)
#define AHEAD 4                         /* encodes queued ahead of the track being written */
#define SCROBBLE_MAX_AGE_MS ((int64_t)14 * 24 * 60 * 60 * 1000) /* Last.fm's limit */
#define PLAY_GAP_MS 30000               /* between plays placed before a device's last */
#define CLOCK_SLACK_MS ((int64_t)24 * 60 * 60 * 1000) /* a device's time zone mistaken, at most */
#define PLACED_PLAY_MS ((int64_t)4 * 60 * 1000)       /* a play placed by placed_plays() */
#define MAX_FORMATS 16

/* A device plugged in. */
typedef struct conn {
  char uid[MC_UID_STRLEN];
  char *mount;
  char plugin_id[128], target_id[64];
  int connected;
  int syncing;
  volatile int cancel;
  int thread_live;
  mc_thread thread;
  /* What it plays, as open() said. */
  char formats[MAX_FORMATS][MC_ENCODER_ID_LEN];
  int nformats;
  struct mc_tethered *owner;
} conn;

struct mc_tethered {
  char *lib_dir, *config_dir, *cache_dir;
  mc_tethered_events ev;
  void *user;
  mc_sync_db *sdb; /* the UI thread's */
  mc_mutex lock;   /* conns, sc */
  mc_mutex event_lock;
  conn **conns;
  int nconns, cap;
  mc_scrobbler *sc;
  mc_encode_pool *pool;
};

/* --- Small helpers --- */

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

static void copy(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src ? src : "");
}

static int valid_id(const char *s) {
  size_t n = strlen(s);
  if (n >= MC_ENCODER_ID_LEN) return 0;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
  return 1;
}

static int same_ci(const char *a, const char *b) {
  if (!a || !b) return 0;
  for (; *a && *b; a++, b++) {
    int x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
    if (x != y) return 0;
  }
  return *a == *b;
}

static mc_status exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

static void bind_text(sqlite3_stmt *stmt, int i, const char *s) {
  if (s) sqlite3_bind_text(stmt, i, s, -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, i);
}

static char *col_dup(sqlite3_stmt *stmt, int i) {
  const unsigned char *v = sqlite3_column_text(stmt, i);
  return v ? mc_strdup((const char *)v) : NULL;
}

static void col_copy(sqlite3_stmt *stmt, int i, char *dst, size_t cap) {
  copy(dst, cap, (const char *)sqlite3_column_text(stmt, i));
}

/* hex SHA-256 of a file. */
static mc_status hash_file(const char *path, char out[65]) {
  out[0] = '\0';
  FILE *f = mc_fopen(path, "rb");
  if (!f) return MC_ERR_IO;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  unsigned char *buf = malloc(1 << 16), digest[32];
  int ok = buf && mbedtls_sha256_starts(&ctx, 0) == 0;
  size_t n;
  while (ok && (n = fread(buf, 1, 1 << 16, f)) > 0) ok = mbedtls_sha256_update(&ctx, buf, n) == 0;
  if (ferror(f)) ok = 0;
  fclose(f);
  free(buf);
  if (ok) ok = mbedtls_sha256_finish(&ctx, digest) == 0;
  mbedtls_sha256_free(&ctx);
  if (!ok) return buf ? MC_ERR_IO : MC_ERR_NOMEM;
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
  return MC_OK;
}

/* Whether status is one of a plugin's: kith_status values match mc_status's where they
 * mean the same; KITH_ERR_FULL has no mc_status, so it's an I/O error. */
static mc_status from_kith(int rc) {
  return rc == KITH_OK ? MC_OK : rc == KITH_ERR_FULL ? MC_ERR_IO : (mc_status)rc;
}

/* --- Events --- */

static void emit_devices(struct mc_tethered *t) {
  if (!t->ev.on_devices) return;
  mc_mutex_lock(&t->event_lock);
  t->ev.on_devices(t->user);
  mc_mutex_unlock(&t->event_lock);
}

static void emit_progress(struct mc_tethered *t, const mc_tethered_progress *p) {
  if (!t->ev.on_progress) return;
  mc_mutex_lock(&t->event_lock);
  t->ev.on_progress(t->user, p);
  mc_mutex_unlock(&t->event_lock);
}

static void emit_done(struct mc_tethered *t, const mc_tethered_result *r) {
  if (!t->ev.on_done) return;
  mc_mutex_lock(&t->event_lock);
  t->ev.on_done(t->user, r);
  mc_mutex_unlock(&t->event_lock);
}

/* --- The object --- */

static char *join(const char *a, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c%s", a, MC_PATH_SEP, b);
  return p;
}

mc_status mc_tethered_new(mc_library *lib, const char *config_dir, const mc_tethered_events *events,
                          void *user, mc_tethered **out) {
  if (out) *out = NULL;
  if (!lib || !config_dir || !out) return MC_ERR_INVALID_ARG;
  mc_tethered *t = calloc(1, sizeof *t);
  if (!t) return MC_ERR_NOMEM;
  mc_mutex_init(&t->lock);
  mc_mutex_init(&t->event_lock);
  if (events) t->ev = *events;
  t->user = user;
  t->lib_dir = mc_strdup(mc_library_dir(lib));
  t->config_dir = mc_strdup(config_dir);
  t->cache_dir = join(config_dir, MC_TRANSCODE_CACHE_DIR);
  mc_status st = t->lib_dir && t->config_dir && t->cache_dir ? MC_OK : MC_ERR_NOMEM;
  if (st == MC_OK) st = mc_sync_db_open(config_dir, &t->sdb);
  if (st == MC_OK) st = mc_encode_pool_new(t->cache_dir, MC_TRANSCODE_CACHE_MAX, &t->pool);
  if (st == MC_OK) {
    int cores = mc_cpu_count();
    mc_encode_pool_set_size(t->pool, cores > 2 ? cores - 1 : 1);
  }
  if (st != MC_OK) {
    mc_tethered_free(t);
    return st;
  }
  *out = t;
  return MC_OK;
}

static void join_conn(conn *c) {
  if (c->thread_live) {
    mc_thread_join(&c->thread);
    c->thread_live = 0;
  }
}

void mc_tethered_free(mc_tethered *t) {
  if (!t) return;
  mc_mutex_lock(&t->lock);
  for (int i = 0; i < t->nconns; i++) t->conns[i]->cancel = 1;
  mc_mutex_unlock(&t->lock);
  for (int i = 0; i < t->nconns; i++) {
    join_conn(t->conns[i]);
    free(t->conns[i]->mount);
    free(t->conns[i]);
  }
  free(t->conns);
  mc_encode_pool_free(t->pool);
  mc_sync_db_close(t->sdb);
  mc_mutex_destroy(&t->event_lock);
  mc_mutex_destroy(&t->lock);
  free(t->lib_dir);
  free(t->config_dir);
  free(t->cache_dir);
  free(t);
}

void mc_tethered_set_scrobbler(mc_tethered *t, mc_scrobbler *sc) {
  if (!t) return;
  mc_mutex_lock(&t->lock);
  t->sc = sc; /* the lock is held around each use, so the old one is free to go now */
  mc_mutex_unlock(&t->lock);
}

/* The connection for uid. Under lock. */
static conn *find_conn(mc_tethered *t, const char *uid) {
  for (int i = 0; i < t->nconns; i++)
    if (!strcmp(t->conns[i]->uid, uid)) return t->conns[i];
  return NULL;
}

/* --- Volumes --- */

/* The row for (plugin, target, device id), made with the target's defaults if there is
 * none; its uid into uid. Its name, model and sizes are brought up to date either way. */
static mc_status remember(mc_sync_db *sdb, const char *plugin_id, const char *target_id,
                          const kith_device_info *info, char uid[MC_UID_STRLEN]) {
  sqlite3 *db = mc_sync_db_handle(sdb);
  sqlite3_stmt *stmt;
  uid[0] = '\0';
  if (sqlite3_prepare_v2(db,
                         "SELECT uid FROM tethered_devices WHERE plugin_id = ?1 AND target_id = ?2"
                         " AND device_id = ?3",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  bind_text(stmt, 1, plugin_id);
  bind_text(stmt, 2, target_id);
  bind_text(stmt, 3, info->id);
  if (sqlite3_step(stmt) == SQLITE_ROW) col_copy(stmt, 0, uid, MC_UID_STRLEN);
  sqlite3_finalize(stmt);
  mc_status st;
  if (!uid[0]) {
    if ((st = mc_uid_new(uid)) != MC_OK) return st;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO tethered_devices (uid, plugin_id, target_id, device_id,"
                           " lossless_format, lossless_setting, lossy_format, lossy_setting)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                           -1, &stmt, NULL) != SQLITE_OK)
      return MC_ERR_DB;
    bind_text(stmt, 1, uid);
    bind_text(stmt, 2, plugin_id);
    bind_text(stmt, 3, target_id);
    bind_text(stmt, 4, info->id);
    bind_text(stmt, 5, info->lossless_format);
    bind_text(stmt, 6, info->lossless_setting ? info->lossless_setting : "");
    bind_text(stmt, 7, info->lossy_format);
    bind_text(stmt, 8, info->lossy_setting ? info->lossy_setting : "");
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return MC_ERR_DB;
  }
  if (sqlite3_prepare_v2(db,
                         "UPDATE tethered_devices SET name = ?2, model = ?3, capacity_bytes = ?4,"
                         " free_bytes = ?5 WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  bind_text(stmt, 1, uid);
  bind_text(stmt, 2, info->name);
  bind_text(stmt, 3, info->model);
  sqlite3_bind_int64(stmt, 4, info->capacity_bytes);
  sqlite3_bind_int64(stmt, 5, info->free_bytes);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static int valid_device_id(const char *id) {
  if (!id || !*id || strlen(id) > 63) return 0;
  for (const char *p = id; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
          *p == '.' || *p == '_' || *p == '-'))
      return 0;
  return 1;
}

mc_status mc_tethered_volume_added(mc_tethered *t, const char *mount_path,
                                   char out_uid[MC_UID_STRLEN]) {
  if (out_uid) out_uid[0] = '\0';
  if (!t || !mount_path || !*mount_path) return MC_ERR_INVALID_ARG;
  mc_plugin_target targets[32];
  int n = mc_plugins_targets(targets, 32);
  if (n > 32) n = 32;
  for (int i = 0; i < n; i++) {
    const kith_device_target *dt = targets[i].target;
    if (!dt->probe || !dt->open || !dt->close || !dt->probe(mount_path)) continue;
    kith_target *dev = NULL;
    kith_device_info info;
    memset(&info, 0, sizeof info);
    if (dt->open(mount_path, &dev, &info) != KITH_OK || !dev) continue;
    char uid[MC_UID_STRLEN];
    mc_status st = valid_device_id(info.id)
                     ? remember(t->sdb, targets[i].plugin->id, dt->id, &info, uid)
                     : MC_ERR_FORMAT;
    conn *c = NULL;
    if (st == MC_OK) {
      mc_mutex_lock(&t->lock);
      c = find_conn(t, uid);
      if (!c) {
        if (t->nconns == t->cap) {
          int cap = t->cap ? t->cap * 2 : 4;
          conn **more = realloc(t->conns, (size_t)cap * sizeof *more);
          if (more) {
            t->conns = more;
            t->cap = cap;
          }
        }
        if (t->nconns < t->cap && (c = calloc(1, sizeof *c))) {
          memcpy(c->uid, uid, MC_UID_STRLEN);
          c->owner = t;
          t->conns[t->nconns++] = c;
        }
      }
      if (c) {
        char *mount = mc_strdup(mount_path);
        if (mount) {
          free(c->mount);
          c->mount = mount;
          c->connected = 1;
          copy(c->plugin_id, sizeof c->plugin_id, targets[i].plugin->id);
          copy(c->target_id, sizeof c->target_id, dt->id);
          c->nformats = 0;
          for (int f = 0; f < info.format_count && c->nformats < MAX_FORMATS; f++)
            if (info.formats[f].format)
              copy(c->formats[c->nformats++], MC_ENCODER_ID_LEN, info.formats[f].format);
        } else {
          st = MC_ERR_NOMEM;
        }
      } else {
        st = MC_ERR_NOMEM;
      }
      mc_mutex_unlock(&t->lock);
    }
    dt->close(dev);
    if (st != MC_OK) return st;
    if (out_uid) memcpy(out_uid, uid, MC_UID_STRLEN);
    emit_devices(t);
    return MC_OK;
  }
  return MC_ERR_NOT_FOUND;
}

mc_status mc_tethered_volume_removed(mc_tethered *t, const char *mount_path) {
  if (!t || !mount_path) return MC_ERR_INVALID_ARG;
  int changed = 0;
  mc_mutex_lock(&t->lock);
  for (int i = 0; i < t->nconns; i++) {
    conn *c = t->conns[i];
    if (c->connected && c->mount && mc_same_path(c->mount, mount_path)) {
      c->connected = 0;
      c->cancel = 1;
      changed = 1;
    }
  }
  mc_mutex_unlock(&t->lock);
  if (changed) emit_devices(t);
  return changed ? MC_OK : MC_ERR_NOT_FOUND;
}

/* --- The list --- */

void mc_tethered_device_list_free(mc_tethered_device_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    mc_tethered_device *d = &list->items[i];
    free(d->name);
    free(d->model);
    free(d->kind);
    free(d->plugin_id);
    free(d->mount_path);
  }
  free(list->items);
  free(list);
}

static int by_connected_name(const void *a, const void *b) {
  const mc_tethered_device *x = a, *y = b;
  if (x->connected != y->connected) return y->connected - x->connected;
  int c = strcmp(x->name ? x->name : "", y->name ? y->name : "");
  return c ? c : strcmp(x->uid, y->uid);
}

mc_status mc_tethered_list(mc_tethered *t, mc_tethered_device_list **out) {
  if (out) *out = NULL;
  if (!t || !out) return MC_ERR_INVALID_ARG;
  mc_tethered_device_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(t->sdb),
                         "SELECT uid, name, model, plugin_id, target_id, capacity_bytes, free_bytes,"
                         " last_sync_ms FROM tethered_devices",
                         -1, &stmt, NULL) != SQLITE_OK) {
    free(list);
    return MC_ERR_DB;
  }
  mc_status st = MC_OK;
  int cap = 0, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (list->count == cap) {
      int grown = cap ? cap * 2 : 4;
      mc_tethered_device *more = realloc(list->items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      list->items = more;
      cap = grown;
    }
    mc_tethered_device *d = &list->items[list->count++];
    memset(d, 0, sizeof *d);
    col_copy(stmt, 0, d->uid, sizeof d->uid);
    d->name = col_dup(stmt, 1);
    d->model = col_dup(stmt, 2);
    d->plugin_id = col_dup(stmt, 3);
    d->capacity_bytes = sqlite3_column_int64(stmt, 5);
    d->free_bytes = sqlite3_column_int64(stmt, 6);
    d->last_sync_ms = sqlite3_column_int64(stmt, 7);
    mc_plugin_target target;
    const char *plugin = (const char *)sqlite3_column_text(stmt, 3);
    const char *tid = (const char *)sqlite3_column_text(stmt, 4);
    if (plugin && tid && mc_plugins_find_target(plugin, tid, &target) == MC_OK) {
      d->usable = 1;
      d->kind = mc_strdup(target.target->name ? target.target->name : tid);
    } else {
      d->kind = tid ? mc_strdup(tid) : NULL;
    }
    mc_mutex_lock(&t->lock);
    conn *c = find_conn(t, d->uid);
    if (c) {
      d->connected = c->connected;
      d->syncing = c->syncing;
      if (c->connected && c->mount) d->mount_path = mc_strdup(c->mount);
    }
    mc_mutex_unlock(&t->lock);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_tethered_device_list_free(list);
    return st;
  }
  if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof *list->items, by_connected_name);
  *out = list;
  return MC_OK;
}

/* --- Profiles --- */

static const char *const k_kinds[] = {"album", "artist", "playlist", "album_group"};

static mc_status read_profile(sqlite3 *db, const char *uid, mc_sync_profile **out,
                              mc_tethered_policy *policy) {
  *out = NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT mode, budget_bytes, budget_enabled, lossless_format,"
                         " lossless_setting, lossy_format, lossy_setting, lossy_cap_kbps"
                         " FROM tethered_devices WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  bind_text(stmt, 1, uid);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  }
  mc_sync_profile *p = calloc(1, sizeof *p);
  if (!p) {
    sqlite3_finalize(stmt);
    return MC_ERR_NOMEM;
  }
  const char *mode = (const char *)sqlite3_column_text(stmt, 0);
  p->sync_all = !mode || strcmp(mode, "selected") != 0;
  p->budget_bytes = sqlite3_column_int64(stmt, 1);
  p->budget_enabled = sqlite3_column_int(stmt, 2) != 0;
  p->format.format = p->budget_enabled ? MC_FORMAT_AUTO : MC_FORMAT_ORIGINAL;
  if (policy) {
    memset(policy, 0, sizeof *policy);
    col_copy(stmt, 3, policy->lossless_format, sizeof policy->lossless_format);
    col_copy(stmt, 4, policy->lossless_setting, sizeof policy->lossless_setting);
    col_copy(stmt, 5, policy->lossy_format, sizeof policy->lossy_format);
    col_copy(stmt, 6, policy->lossy_setting, sizeof policy->lossy_setting);
    policy->lossy_cap_kbps = sqlite3_column_int(stmt, 7);
  }
  sqlite3_finalize(stmt);
  mc_status st = MC_OK;
  if (sqlite3_prepare_v2(db,
                         "SELECT kind, ref_uid FROM tethered_profile_items WHERE device_uid = ?1"
                         " ORDER BY kind, ref_uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int cap = 0;
  if (st == MC_OK) bind_text(stmt, 1, uid);
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *kind = (const char *)sqlite3_column_text(stmt, 0);
    const char *ref = (const char *)sqlite3_column_text(stmt, 1);
    int k = -1;
    for (int i = 0; kind && i < 4; i++)
      if (!strcmp(kind, k_kinds[i])) k = i;
    if (k < 0 || !is_uid(ref)) continue;
    if (p->count == cap) {
      int grown = cap ? cap * 2 : 8;
      mc_profile_item *more = realloc(p->items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      p->items = more;
      cap = grown;
    }
    p->items[p->count].kind = (mc_profile_item_kind)k;
    memcpy(p->items[p->count].ref_uid, ref, MC_UID_STRLEN);
    p->count++;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  if (stmt) sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_sync_profile_free(p);
    return st;
  }
  *out = p;
  return MC_OK;
}

mc_status mc_tethered_get_profile(mc_tethered *t, const char *device_uid, mc_sync_profile **out,
                                  mc_tethered_policy *policy) {
  if (out) *out = NULL;
  if (!t || !is_uid(device_uid) || !out) return MC_ERR_INVALID_ARG;
  return read_profile(mc_sync_db_handle(t->sdb), device_uid, out, policy);
}

mc_status mc_tethered_set_profile(mc_tethered *t, const char *device_uid,
                                  const mc_sync_profile *profile, const mc_tethered_policy *policy) {
  if (!t || !is_uid(device_uid) || !profile || profile->count < 0 ||
      (profile->count && !profile->items) || profile->budget_bytes < 0)
    return MC_ERR_INVALID_ARG;
  for (int i = 0; i < profile->count; i++)
    if ((int)profile->items[i].kind < 0 || (int)profile->items[i].kind >= 4 ||
        !memchr(profile->items[i].ref_uid, '\0', MC_UID_STRLEN) ||
        !is_uid(profile->items[i].ref_uid))
      return MC_ERR_INVALID_ARG;
  if (policy && (!memchr(policy->lossless_format, '\0', MC_ENCODER_ID_LEN) ||
                 !memchr(policy->lossless_setting, '\0', MC_ENCODER_ID_LEN) ||
                 !memchr(policy->lossy_format, '\0', MC_ENCODER_ID_LEN) ||
                 !memchr(policy->lossy_setting, '\0', MC_ENCODER_ID_LEN) ||
                 !valid_id(policy->lossless_format) || !valid_id(policy->lossless_setting) ||
                 !valid_id(policy->lossy_format) || !valid_id(policy->lossy_setting) ||
                 policy->lossy_cap_kbps < 0))
    return MC_ERR_INVALID_ARG;
  sqlite3 *db = mc_sync_db_handle(t->sdb);
  if (exec(db, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  sqlite3_stmt *stmt;
  mc_status st = MC_ERR_DB;
  if (sqlite3_prepare_v2(db,
                         "UPDATE tethered_devices SET mode = ?2, budget_bytes = ?3,"
                         " budget_enabled = ?4 WHERE uid = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    bind_text(stmt, 1, device_uid);
    bind_text(stmt, 2, profile->sync_all ? "all" : "selected");
    sqlite3_bind_int64(stmt, 3, profile->budget_bytes);
    sqlite3_bind_int(stmt, 4, profile->budget_enabled != 0);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = sqlite3_changes(db) ? MC_OK : MC_ERR_NOT_FOUND;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK && policy) {
    st = MC_ERR_DB;
    if (sqlite3_prepare_v2(db,
                           "UPDATE tethered_devices SET lossless_format = ?2, lossless_setting = ?3,"
                           " lossy_format = ?4, lossy_setting = ?5, lossy_cap_kbps = ?6"
                           " WHERE uid = ?1",
                           -1, &stmt, NULL) == SQLITE_OK) {
      bind_text(stmt, 1, device_uid);
      bind_text(stmt, 2, policy->lossless_format);
      bind_text(stmt, 3, policy->lossless_setting);
      bind_text(stmt, 4, policy->lossy_format);
      bind_text(stmt, 5, policy->lossy_setting);
      sqlite3_bind_int(stmt, 6, policy->lossy_cap_kbps);
      if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
      sqlite3_finalize(stmt);
    }
  }
  if (st == MC_OK && sqlite3_prepare_v2(db, "DELETE FROM tethered_profile_items WHERE device_uid = ?1",
                                        -1, &stmt, NULL) == SQLITE_OK) {
    bind_text(stmt, 1, device_uid);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK &&
      sqlite3_prepare_v2(db,
                         "INSERT OR IGNORE INTO tethered_profile_items (device_uid, kind, ref_uid)"
                         " VALUES (?1, ?2, ?3)",
                         -1, &stmt, NULL) == SQLITE_OK) {
    for (int i = 0; st == MC_OK && i < profile->count; i++) {
      sqlite3_reset(stmt);
      bind_text(stmt, 1, device_uid);
      bind_text(stmt, 2, k_kinds[profile->items[i].kind]);
      bind_text(stmt, 3, profile->items[i].ref_uid);
      if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    }
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = exec(db, "COMMIT");
  if (st != MC_OK) exec(db, "ROLLBACK");
  return st;
}

void mc_tethered_choice_list_free(mc_tethered_choice_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) free(list->items[i].label);
  free(list->items);
  free(list);
}

mc_status mc_tethered_choices(mc_tethered *t, const char *device_uid, mc_tethered_choice_list **out) {
  if (out) *out = NULL;
  if (!t || !is_uid(device_uid) || !out) return MC_ERR_INVALID_ARG;
  char formats[MAX_FORMATS][MC_ENCODER_ID_LEN];
  int nformats = 0;
  mc_mutex_lock(&t->lock);
  conn *c = find_conn(t, device_uid);
  if (c) {
    nformats = c->nformats;
    memcpy(formats, c->formats, sizeof formats);
  }
  mc_mutex_unlock(&t->lock);
  mc_tethered_choice_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  int cap = 0;
  mc_status st = MC_OK;
  /* Each format the device plays (as it was last seen) that an encoder writes. */
  for (int f = 0; st == MC_OK && f < nformats; f++) {
    mc_plugin_encoder enc;
    if (mc_plugins_find_encoder(formats[f], &enc) != MC_OK) continue;
    const kith_encoder_format *ef = enc.format;
    int n = ef->setting_count ? ef->setting_count : 1;
    for (int s = 0; st == MC_OK && s < n; s++) {
      if (list->count == cap) {
        int grown = cap ? cap * 2 : 8;
        mc_tethered_choice *more = realloc(list->items, (size_t)grown * sizeof *more);
        if (!more) {
          st = MC_ERR_NOMEM;
          break;
        }
        list->items = more;
        cap = grown;
      }
      mc_tethered_choice *ch = &list->items[list->count++];
      memset(ch, 0, sizeof *ch);
      copy(ch->format, sizeof ch->format, ef->id);
      ch->lossless = ef->lossless;
      const kith_encoder_setting *set = ef->setting_count ? &ef->settings[s] : NULL;
      if (set) {
        copy(ch->setting, sizeof ch->setting, set->id);
        ch->kbps = set->kbps;
      }
      size_t len = strlen(ef->name ? ef->name : ef->id) + (set && set->label ? strlen(set->label) : 0) + 8;
      ch->label = malloc(len);
      if (!ch->label) {
        st = MC_ERR_NOMEM;
        break;
      }
      if (set && set->label) snprintf(ch->label, len, "%s \xc2\xb7 %s", ef->name ? ef->name : ef->id, set->label);
      else snprintf(ch->label, len, "%s", ef->name ? ef->name : ef->id);
    }
  }
  if (st != MC_OK) {
    mc_tethered_choice_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

/* --- A sync --- */

/* A track on the device, copied out of list()'s visitor call. */
typedef struct dtrack {
  char key[KITH_KEY_LEN];
  char track_uid[MC_UID_STRLEN], file_uid[MC_UID_STRLEN];
  char setting[MC_ENCODER_ID_LEN], format[MC_ENCODER_ID_LEN];
  char sha[65];
  int kbps;
  int64_t size;
  char *title, *artist, *album;
  int duration_ms, play_count;
  int64_t last_played_ms;
  int kept;    /* already what the profile wants */
  int replace; /* a wanted track's, to be replaced */
} dtrack;

/* A track to write. */
typedef struct want {
  char track_uid[MC_UID_STRLEN];
  char file_uid[MC_UID_STRLEN];
  char *src;                 /* the original's absolute path */
  char src_format[16];
  char src_sha[65];
  int src_kbps;
  int64_t src_size;
  int copy;                  /* as it is, else encoded with opts */
  mc_transcode_opts opts;
  int kbps;                  /* nominal, for the device */
  int64_t est_bytes;
  int on_device;             /* index into the device's tracks it replaces, or -1 */
  int duration_ms;
} want;

typedef struct job {
  mc_tethered *t;
  conn *c;
  char uid[MC_UID_STRLEN];
  char *mount;
  mc_library *lib;
  mc_sync_db *sdb;
  mc_plugin_target pt;
  const kith_device_target *dt;
  kith_target *dev;
  kith_device_info info;
  char device_name[256];
  dtrack *tracks;  /* sorted by key */
  int ntracks, cap;
  int oom;
  kith_job kjob;
  mc_tethered_result r;
  mc_tethered_progress p;
  int since_commit;
  uint64_t last_commit;
} job;

static int stop_job(void *user) {
  job *j = user;
  return j->c->cancel;
}

static void progress(job *j, mc_tethered_phase phase, const char *title) {
  j->p.device_uid = j->uid;
  j->p.phase = phase;
  j->p.title = title;
  emit_progress(j->t, &j->p);
}

static int on_track(void *ctx, const kith_device_track *d) {
  job *j = ctx;
  if (!d || !d->key || strlen(d->key) >= KITH_KEY_LEN) return 0;
  if (j->ntracks == j->cap) {
    int cap = j->cap ? j->cap * 2 : 64;
    dtrack *more = realloc(j->tracks, (size_t)cap * sizeof *more);
    if (!more) {
      j->oom = 1;
      return 1;
    }
    j->tracks = more;
    j->cap = cap;
  }
  dtrack *t = &j->tracks[j->ntracks++];
  memset(t, 0, sizeof *t);
  copy(t->key, sizeof t->key, d->key);
  if (is_uid(d->track_uid)) copy(t->track_uid, sizeof t->track_uid, d->track_uid);
  if (is_uid(d->file_uid)) copy(t->file_uid, sizeof t->file_uid, d->file_uid);
  copy(t->setting, sizeof t->setting, d->setting);
  copy(t->format, sizeof t->format, d->format);
  copy(t->sha, sizeof t->sha, d->sha256);
  t->kbps = d->bitrate_kbps;
  t->size = d->size_bytes;
  t->title = d->title ? mc_strdup(d->title) : NULL;
  t->artist = d->artist ? mc_strdup(d->artist) : NULL;
  t->album = d->album ? mc_strdup(d->album) : NULL;
  t->duration_ms = d->duration_ms;
  t->play_count = d->play_count;
  t->last_played_ms = d->last_played_ms;
  return 0;
}

static int dtrack_cmp(const void *a, const void *b) {
  return strcmp(((const dtrack *)a)->key, ((const dtrack *)b)->key);
}

static dtrack *find_key(job *j, const char *key) {
  dtrack probe;
  copy(probe.key, sizeof probe.key, key);
  return key ? bsearch(&probe, j->tracks, (size_t)j->ntracks, sizeof *j->tracks, dtrack_cmp) : NULL;
}

/* A library track for the device's track at key: its own track_uid if Kithsong wrote it
 * and the library still has it, else one with the same artist, album and title within 2
 * seconds. *duration_ms is the library's length. 0 if none. */
static int match_key(job *j, const char *key, char uid[MC_UID_STRLEN], int *duration_ms) {
  uid[0] = '\0';
  *duration_ms = 0;
  dtrack *d = find_key(j, key);
  if (!d) return 0;
  sqlite3 *db = mc_library_db(j->lib);
  sqlite3_stmt *stmt;
  if (d->track_uid[0] &&
      sqlite3_prepare_v2(db, "SELECT uid, duration_ms FROM tracks WHERE uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    bind_text(stmt, 1, d->track_uid);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      col_copy(stmt, 0, uid, MC_UID_STRLEN);
      *duration_ms = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    if (uid[0]) return 1;
  }
  if (!d->title || !*d->title) return 0;
  static const char sql[] =
    "SELECT t.uid, t.duration_ms FROM tracks t" TRACK_JOINS
    " WHERE t.title = ?1 COLLATE NOCASE AND ifnull(al.title, '') = ?2 COLLATE NOCASE"
    " AND (" TRACK_ARTIST_NAME " = ?3 COLLATE NOCASE OR ar.name = ?3 COLLATE NOCASE"
    "  OR aar.name = ?3 COLLATE NOCASE)"
    " AND abs(ifnull(t.duration_ms, 0) - ?4) <= 2000"
    " ORDER BY abs(ifnull(t.duration_ms, 0) - ?4), t.uid LIMIT 1";
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  bind_text(stmt, 1, d->title);
  bind_text(stmt, 2, d->album ? d->album : "");
  bind_text(stmt, 3, d->artist ? d->artist : "");
  sqlite3_bind_int(stmt, 4, d->duration_ms);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    col_copy(stmt, 0, uid, MC_UID_STRLEN);
    *duration_ms = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return uid[0] != 0;
}

/* Whether a set of what ("plays", "playlists") with token was taken in from this device. */
static int imported(job *j, const char *what, const char *token) {
  sqlite3_stmt *stmt;
  int found = 0;
  if (sqlite3_prepare_v2(mc_sync_db_handle(j->sdb),
                         "SELECT 1 FROM tethered_imports WHERE device_uid = ?1 AND what = ?2"
                         " AND token = ?3",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  bind_text(stmt, 1, j->uid);
  bind_text(stmt, 2, what);
  bind_text(stmt, 3, token);
  int rc = sqlite3_step(stmt);
  found = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
  sqlite3_finalize(stmt);
  return found;
}

static mc_status record_import(job *j, const char *what, const char *token) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(j->sdb),
                         "INSERT OR IGNORE INTO tethered_imports (device_uid, what, token, imported_ms)"
                         " VALUES (?1, ?2, ?3, ?4)",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  bind_text(stmt, 1, j->uid);
  bind_text(stmt, 2, what);
  bind_text(stmt, 3, token);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)mc_wall_ms());
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* --- Plays --- */

typedef struct play_in {
  char key[KITH_KEY_LEN];
  int count;
  int64_t last_ms;
} play_in;

typedef struct plays {
  play_in *items;
  int count, cap, oom;
} plays;

static int on_play(void *ctx, const kith_device_play *p) {
  plays *ps = ctx;
  if (!p || !p->key || p->count <= 0 || strlen(p->key) >= KITH_KEY_LEN) return 0;
  if (ps->count == ps->cap) {
    int cap = ps->cap ? ps->cap * 2 : 64;
    play_in *more = realloc(ps->items, (size_t)cap * sizeof *more);
    if (!more) {
      ps->oom = 1;
      return 1;
    }
    ps->items = more;
    ps->cap = cap;
  }
  play_in *in = &ps->items[ps->count++];
  copy(in->key, sizeof in->key, p->key);
  in->count = p->count;
  in->last_ms = p->last_played_ms;
  return 0;
}

/* When this device was last synced, ms; 0 if never. */
static int64_t last_sync(job *j) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(j->sdb), "SELECT last_sync_ms FROM tethered_devices WHERE uid = ?1", -1,
                         &stmt, NULL) != SQLITE_OK)
    return 0;
  bind_text(stmt, 1, j->uid);
  int64_t ms = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return ms > 0 ? ms : 0;
}

static int by_last(const void *a, const void *b) {
  const play_in *x = *(play_in *const *)a, *y = *(play_in *const *)b;
  return x->last_ms < y->last_ms ? -1 : x->last_ms > y->last_ms;
}

/* Plays whose time can't be true, by a device clock the plugin couldn't put right (a flat
 * battery sets an iPod back to 2000): before the last sync, as a device's plays are cleared
 * by it, or in the future, each by more than a time zone could make it. They're placed in
 * their order just after the last sync, ahead of any play with a believable time, as they
 * came before the clock was set right; with no sync before, or for ones in the future, they
 * end at now. Each takes PLACED_PLAY_MS; import_plays() places a track's earlier plays
 * before its last as ever. */
static void placed_plays(play_in *items, int count, int64_t prev, int64_t now) {
  play_in **wrong = malloc((size_t)(count ? count : 1) * sizeof *wrong);
  if (!wrong) return;
  int n = 0;
  int64_t span = 0;
  for (int i = 0; i < count; i++) {
    int64_t t = items[i].last_ms;
    if (t <= 0) continue;
    if ((prev > 0 && t < prev - CLOCK_SLACK_MS) || t > now + CLOCK_SLACK_MS) {
      wrong[n++] = &items[i];
      span += (int64_t)items[i].count * PLACED_PLAY_MS;
    }
  }
  qsort(wrong, (size_t)n, sizeof *wrong, by_last);
  int64_t t = prev > 0 ? prev : now - span;
  for (int i = 0; i < n; i++) {
    t += (int64_t)wrong[i]->count * PLACED_PLAY_MS;
    wrong[i]->last_ms = t < now ? t : now;
  }
  free(wrong);
}

/* The plays made on the device: logged, counted and scrobbled, once. */
static mc_status import_plays(job *j) {
  if (!j->dt->plays_read) return MC_OK;
  char token[KITH_SHA256_LEN] = "";
  int rc = j->dt->plays_read(j->dev, token, NULL, NULL);
  if (rc != KITH_OK) return from_kith(rc);
  token[KITH_SHA256_LEN - 1] = '\0';
  if (!token[0]) return MC_OK;
  int seen = imported(j, "plays", token);
  if (seen < 0) return MC_ERR_DB;
  if (!seen) {
    plays ps = {0};
    rc = j->dt->plays_read(j->dev, token, on_play, &ps);
    mc_status st = ps.oom ? MC_ERR_NOMEM : from_kith(rc);
    int64_t now = (int64_t)mc_wall_ms();
    if (st == MC_OK) placed_plays(ps.items, ps.count, last_sync(j), now);
    for (int i = 0; st == MC_OK && i < ps.count; i++) {
      char uid[MC_UID_STRLEN];
      int length = 0;
      if (!match_key(j, ps.items[i].key, uid, &length)) {
        j->r.plays_unmatched += ps.items[i].count;
        continue;
      }
      dtrack *d = find_key(j, ps.items[i].key);
      if (length <= 0 && d) length = d->duration_ms;
      int64_t last = ps.items[i].last_ms > 0 ? ps.items[i].last_ms : now;
      int n = ps.items[i].count;
      /* The device keeps only the last play's time: the earlier ones are placed a track
       * (and a gap) apart before it, so they're in order and believable. */
      for (int k = 0; st == MC_OK && k < n; k++) {
        mc_play_event e;
        memset(&e, 0, sizeof e);
        memcpy(e.track_uid, uid, MC_UID_STRLEN);
        e.started_ms = last - (int64_t)(n - 1 - k) * ((int64_t)length + PLAY_GAP_MS);
        e.listened_ms = length;
        e.end = MC_PLAY_END_FINISHED;
        e.context = MC_PLAY_CONTEXT_DEVICE;
        memcpy(e.context_uid, j->uid, MC_UID_STRLEN);
        st = mc_library_log_play(j->lib, &e);
        if (st == MC_ERR_NOT_FOUND) { /* gone meanwhile */
          st = MC_OK;
          continue;
        }
        if (st != MC_OK) break;
        j->r.plays_imported++;
        mc_mutex_lock(&j->t->lock);
        if (j->t->sc) {
          if (now - e.started_ms > SCROBBLE_MAX_AGE_MS) j->r.plays_not_scrobbled++;
          else mc_scrobbler_scrobble(j->t->sc, uid, e.started_ms);
        }
        mc_mutex_unlock(&j->t->lock);
      }
      if (st == MC_OK) {
        mc_status counted = mc_library_add_plays(j->lib, uid, n, last);
        if (counted != MC_OK && counted != MC_ERR_NOT_FOUND) st = counted;
      }
    }
    free(ps.items);
    if (st != MC_OK) return st;
    /* Recorded before the device is told, so an unplug in between never doubles anything. */
    if ((st = record_import(j, "plays", token)) != MC_OK) return st;
  }
  if (j->dt->device_done) {
    rc = j->dt->device_done(j->dev, KITH_DONE_PLAYS);
    if (rc != KITH_OK) return from_kith(rc);
  }
  return MC_OK;
}

/* --- Playlists from the device --- */

typedef struct list_in {
  char **keys;
  int count;
} list_in;

typedef struct lists {
  list_in *items;
  int count, cap, oom;
} lists;

static int on_list(void *ctx, const kith_device_playlist *pl) {
  lists *ls = ctx;
  if (!pl || pl->count < 0 || (pl->count && !pl->keys)) return 0;
  if (ls->count == ls->cap) {
    int cap = ls->cap ? ls->cap * 2 : 4;
    list_in *more = realloc(ls->items, (size_t)cap * sizeof *more);
    if (!more) {
      ls->oom = 1;
      return 1;
    }
    ls->items = more;
    ls->cap = cap;
  }
  list_in *in = &ls->items[ls->count++];
  in->count = 0;
  in->keys = pl->count ? calloc((size_t)pl->count, sizeof *in->keys) : NULL;
  if (pl->count && !in->keys) {
    ls->oom = 1;
    return 1;
  }
  for (int i = 0; i < pl->count; i++)
    if (pl->keys[i] && (in->keys[in->count] = mc_strdup(pl->keys[i]))) in->count++;
  return 0;
}

static int playlist_named(mc_library *lib, const char *name) {
  sqlite3_stmt *stmt;
  int found = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT 1 FROM playlists WHERE name = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return 0;
  bind_text(stmt, 1, name);
  found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

/* The device's On-The-Go playlists, made into the library's manual playlists, once. */
static mc_status import_playlists(job *j) {
  if (!j->dt->playlists_read) return MC_OK;
  char token[KITH_SHA256_LEN] = "";
  int rc = j->dt->playlists_read(j->dev, token, NULL, NULL);
  if (rc != KITH_OK) return from_kith(rc);
  token[KITH_SHA256_LEN - 1] = '\0';
  if (!token[0]) return MC_OK;
  int seen = imported(j, "playlists", token);
  if (seen < 0) return MC_ERR_DB;
  if (!seen) {
    lists ls = {0};
    rc = j->dt->playlists_read(j->dev, token, on_list, &ls);
    mc_status st = ls.oom ? MC_ERR_NOMEM : from_kith(rc);
    for (int i = 0; st == MC_OK && i < ls.count; i++) {
      /* "On-The-Go 1", or "On-The-Go 1 (Walkman)" if the library has one by that name
       * (another iPod's), counting on past any taken both ways. */
      char name[512];
      for (int n = i + 1;; n++) {
        mc_format(name, sizeof name, mc_str(MC_STR_TETHERED_OTG_PLAYLIST), MC_D(n));
        if (!playlist_named(j->lib, name)) break;
        mc_format(name, sizeof name, mc_str(MC_STR_TETHERED_OTG_PLAYLIST_DEVICE), MC_D(n),
                  MC_S(j->device_name));
        if (!playlist_named(j->lib, name) || n > i + 1000) break;
      }
      char puid[MC_UID_STRLEN];
      if ((st = mc_playlist_create(j->lib, name, puid)) != MC_OK) break;
      for (int k = 0; st == MC_OK && k < ls.items[i].count; k++) {
        char uid[MC_UID_STRLEN];
        int length;
        if (!match_key(j, ls.items[i].keys[k], uid, &length)) continue;
        st = mc_playlist_append(j->lib, puid, uid, NULL);
        if (st == MC_ERR_NOT_FOUND) st = MC_OK;
      }
      if (st == MC_OK) j->r.playlists_imported++;
    }
    for (int i = 0; i < ls.count; i++) {
      for (int k = 0; k < ls.items[i].count; k++) free(ls.items[i].keys[k]);
      free(ls.items[i].keys);
    }
    free(ls.items);
    if (st != MC_OK) return st;
    if ((st = record_import(j, "playlists", token)) != MC_OK) return st;
  }
  if (j->dt->device_done) {
    rc = j->dt->device_done(j->dev, KITH_DONE_PLAYLISTS);
    if (rc != KITH_OK) return from_kith(rc);
  }
  return MC_OK;
}

/* --- What to write --- */

static const kith_device_format *device_format(job *j, const char *format) {
  for (int i = 0; i < j->info.format_count; i++)
    if (j->info.formats[i].format && same_ci(j->info.formats[i].format, format))
      return &j->info.formats[i];
  return NULL;
}

/* opts for encoding to format at setting, with the device's limits for it. */
static void encode_as(job *j, want *w, const char *format, const char *setting) {
  memset(&w->opts, 0, sizeof w->opts);
  w->opts.format = MC_FORMAT_ENCODER;
  copy(w->opts.encoder, sizeof w->opts.encoder, format);
  copy(w->opts.setting, sizeof w->opts.setting, setting);
  const kith_device_format *df = device_format(j, format);
  if (df) {
    w->opts.max_sample_rate = df->max_sample_rate > 0 ? df->max_sample_rate : 0;
    w->opts.max_bits = df->max_bits > 0 ? df->max_bits : 0;
    w->opts.max_channels = df->max_channels > 0 ? df->max_channels : 0;
  }
  w->copy = 0;
  w->kbps = 0;
  mc_plugin_encoder enc;
  if (mc_plugins_find_encoder(format, &enc) == MC_OK && !enc.format->lossless)
    for (int i = 0; i < enc.format->setting_count; i++)
      if (enc.format->settings[i].id && !strcmp(enc.format->settings[i].id, setting))
        w->kbps = enc.format->settings[i].kbps;
  if (w->kbps > 0 && mc_transcode_estimate_bytes(w->duration_ms, w->kbps, &w->est_bytes) == MC_OK) return;
  w->est_bytes = w->src_size; /* lossless: about the original's size */
}

/* The format policy for one original. */
static void choose(job *j, const mc_tethered_policy *pol, want *w) {
  int lossy = mc_plan_is_lossy(w->src_format, w->src_kbps);
  if (!lossy) {
    const char *f = pol->lossless_format[0] ? pol->lossless_format : j->info.lossless_format;
    const char *s = pol->lossless_format[0] ? pol->lossless_setting : j->info.lossless_setting;
    const kith_device_format *df = f ? device_format(j, f) : NULL;
    mc_plugin_encoder enc;
    /* Limits the original may be over (sample rate, bits, channels aren't in the library):
     * through the encoder, which copies sample for sample what's within them. */
    int limited = df && (df->max_sample_rate > 0 || df->max_bits > 0 || df->max_channels > 0) &&
                  mc_plugins_find_encoder(f, &enc) == MC_OK;
    if (f && same_ci(f, w->src_format) && df && !limited) {
      w->copy = 1;
    } else if (f) {
      encode_as(j, w, f, s ? s : "");
      return;
    } else {
      w->copy = 1;
    }
  } else {
    const kith_device_format *df = device_format(j, w->src_format);
    int over = (pol->lossy_cap_kbps > 0 && w->src_kbps > pol->lossy_cap_kbps) ||
               (df && df->max_kbps > 0 && w->src_kbps > df->max_kbps);
    if (df && !over) {
      w->copy = 1;
    } else {
      const char *f = pol->lossy_format[0] ? pol->lossy_format : j->info.lossy_format;
      const char *s = pol->lossy_format[0] ? pol->lossy_setting : j->info.lossy_setting;
      if (f) {
        encode_as(j, w, f, s ? s : "");
        return;
      }
      w->copy = 1;
    }
  }
  memset(&w->opts, 0, sizeof w->opts);
  w->opts.format = MC_FORMAT_ORIGINAL;
  w->kbps = w->src_kbps;
  w->est_bytes = w->src_size;
}

/* This computer's original of track_uid into w; 0 if it has none. */
static int original(job *j, const char *track_uid, want *w) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(j->lib),
                         "SELECT uid, path, format, bitrate_kbps, size_bytes, sha256 FROM files"
                         " WHERE track_uid = ?1 AND source_file_uid IS NULL"
                         " ORDER BY source_peer_uid IS NOT NULL, uid LIMIT 1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  bind_text(stmt, 1, track_uid);
  int ok = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 1);
    char *abs = rel ? mc_library_abspath(j->lib, rel) : NULL;
    if (abs && mc_file_exists(abs)) {
      col_copy(stmt, 0, w->file_uid, sizeof w->file_uid);
      w->src = abs;
      abs = NULL;
      col_copy(stmt, 2, w->src_format, sizeof w->src_format);
      w->src_kbps = sqlite3_column_int(stmt, 3);
      w->src_size = sqlite3_column_int64(stmt, 4);
      col_copy(stmt, 5, w->src_sha, sizeof w->src_sha);
      ok = is_uid(w->file_uid);
    }
    free(abs);
  }
  sqlite3_finalize(stmt);
  return ok;
}

/* --- Tracks for the plugin --- */

/* A kith_track with the library's tags and counts, and the strings it points at. */
typedef struct track_out {
  kith_track k;
  char *title, *artist, *album, *album_artist, *genre, *mb_trackid, *mb_albumid;
  unsigned char *cover;
} track_out;

static void track_out_clear(track_out *o) {
  free(o->title);
  free(o->artist);
  free(o->album);
  free(o->album_artist);
  free(o->genre);
  free(o->mb_trackid);
  free(o->mb_albumid);
  free(o->cover);
  memset(o, 0, sizeof *o);
}

/* The library's play count and last play of track_uid. */
static void counts(job *j, const char *track_uid, int *count, int64_t *last_ms) {
  *count = 0;
  *last_ms = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(j->lib),
                         "SELECT ifnull(sum(play_count), 0), ifnull(max(last_played_ms), 0)"
                         " FROM track_plays WHERE track_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  bind_text(stmt, 1, track_uid);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    *count = sqlite3_column_int(stmt, 0);
    *last_ms = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);
}

/* The album's cover beside the original at src, read whole if it's a sensible size. */
static void read_cover(const char *src, track_out *o) {
  if (!src) return;
  const char *a = strrchr(src, '/'), *b = strrchr(src, MC_PATH_SEP);
  const char *sep = a > b ? a : b;
  if (!sep) return;
  size_t len = (size_t)(sep - src);
  char *dir = malloc(len + 1);
  if (!dir) return;
  memcpy(dir, src, len);
  dir[len] = '\0';
  char *path = mc_coverart_find(dir);
  free(dir);
  int64_t size = 0, mtime = 0;
  if (path && mc_file_info(path, &size, &mtime) == MC_OK && size > 0 && size < ((int64_t)32 << 20)) {
    FILE *f = mc_fopen(path, "rb");
    unsigned char *buf = f ? malloc((size_t)size) : NULL;
    if (buf && fread(buf, 1, (size_t)size, f) == (size_t)size) {
      o->cover = buf;
      o->k.cover.data = buf;
      o->k.cover.size = (size_t)size;
      o->k.cover.mime = size >= 2 && buf[0] == 0x89 && buf[1] == 'P' ? "image/png" : "image/jpeg";
      buf = NULL;
    }
    free(buf);
    if (f) fclose(f);
  }
  mc_free(path);
}

static mc_status fill_track(job *j, const char *track_uid, track_out *o) {
  memset(o, 0, sizeof *o);
  static const char sql[] =
    "SELECT t.title, " TRACK_ARTIST_NAME ", al.title, coalesce(al.artist_credit, aar.name),"
    " t.genre, coalesce(t.year, al.year), t.track_no, t.disc_no, al.disc_total, t.mb_trackid,"
    " al.mb_albumid, t.duration_ms,"
    " (SELECT count(*) FROM tracks t2 WHERE t2.album_uid = t.album_uid"
    "  AND ifnull(t2.disc_no, 1) = ifnull(t.disc_no, 1))"
    " FROM tracks t" TRACK_JOINS " WHERE t.uid = ?1";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(j->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  bind_text(stmt, 1, track_uid);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  }
  copy(o->k.track_uid, sizeof o->k.track_uid, track_uid);
  o->title = col_dup(stmt, 0);
  o->artist = col_dup(stmt, 1);
  o->album = col_dup(stmt, 2);
  o->album_artist = col_dup(stmt, 3);
  o->genre = col_dup(stmt, 4);
  o->mb_trackid = col_dup(stmt, 9);
  o->mb_albumid = col_dup(stmt, 10);
  o->k.tags.title = o->title;
  o->k.tags.artist = o->artist;
  o->k.tags.album = o->album;
  o->k.tags.album_artist = o->album_artist;
  o->k.tags.genre = o->genre;
  o->k.tags.mb_trackid = o->mb_trackid;
  o->k.tags.mb_albumid = o->mb_albumid;
  o->k.tags.year = sqlite3_column_int(stmt, 5);
  o->k.tags.track_no = sqlite3_column_int(stmt, 6);
  o->k.tags.disc_no = sqlite3_column_int(stmt, 7);
  o->k.tags.disc_total = sqlite3_column_int(stmt, 8);
  o->k.duration_ms = sqlite3_column_int(stmt, 11);
  o->k.tags.track_total = o->k.tags.track_no ? sqlite3_column_int(stmt, 12) : 0;
  sqlite3_finalize(stmt);
  counts(j, track_uid, &o->k.play_count, &o->k.last_played_ms);
  o->k.format = "";
  o->k.setting = "";
  return MC_OK;
}

/* --- Writing --- */

static void maybe_commit(job *j, int force) {
  if (!force && j->since_commit < COMMIT_EVERY && mc_mono_ms() - j->last_commit < COMMIT_MS) return;
  if (j->since_commit || force) j->dt->commit(j->dev);
  j->since_commit = 0;
  j->last_commit = mc_mono_ms();
}

static int pool_idle(void *user) {
  return stop_job(user);
}

/* The file to put for w (*path malloc'd) and its hash. */
static mc_status prepare(job *j, want *w, char **path, char sha[65]) {
  *path = NULL;
  sha[0] = '\0';
  if (w->copy) {
    int64_t size = 0, mtime = 0;
    if (!(*path = mc_strdup(w->src))) return MC_ERR_NOMEM;
    if (strlen(w->src_sha) == 64 && mc_file_info(w->src, &size, &mtime) == MC_OK && size == w->src_size)
      copy(sha, 65, w->src_sha);
    else if (hash_file(w->src, sha) != MC_OK)
      return MC_ERR_IO;
    return MC_OK;
  }
  mc_status st = mc_encode_pool_encode_as(j->t->pool, w->file_uid, w->src, &w->opts, pool_idle, j, path);
  if (st != MC_OK) return st;
  int64_t size = 0, mtime = 0, side_size = 0, side_mtime = 0;
  if (mc_transcode_sha_read(*path, sha, &side_size, &side_mtime) == MC_OK &&
      mc_file_info(*path, &size, &mtime) == MC_OK && size == side_size && mtime == side_mtime)
    return MC_OK;
  return hash_file(*path, sha);
}

/* Whether the file under key on the device is sha. 1 if the plugin can't say where it is. */
static int verified(job *j, const char *key, const char *sha) {
  char *path = j->dt->file_path ? j->dt->file_path(j->dev, key) : NULL;
  if (!path) return 1;
  char got[65];
  int ok = hash_file(path, got) == MC_OK && !strcmp(got, sha);
  j->pt.plugin->free(path);
  return ok;
}

/* Puts one track, verified; *full set if the device has no room. */
static mc_status write_one(job *j, want *w, int *full) {
  char *path = NULL, sha[65];
  mc_status st = prepare(j, w, &path, sha);
  if (st != MC_OK) {
    free(path);
    return st;
  }
  track_out o;
  st = fill_track(j, w->track_uid, &o);
  if (st == MC_OK) {
    read_cover(w->src, &o);
    copy(o.k.file_uid, sizeof o.k.file_uid, w->file_uid);
    o.k.format = w->copy ? w->src_format : w->opts.encoder;
    o.k.setting = w->copy ? "" : w->opts.setting;
    copy(o.k.sha256, sizeof o.k.sha256, sha);
    int64_t mtime = 0;
    mc_file_info(path, &o.k.size_bytes, &mtime);
    o.k.bitrate_kbps = w->kbps;
    char key[KITH_KEY_LEN] = "";
    if (w->on_device >= 0) copy(key, sizeof key, j->tracks[w->on_device].key);
    mc_encode_pool_hold(j->t->pool); /* the trim mustn't take the file while it's copied */
    st = MC_ERR_IO;
    for (int attempt = 0; attempt < 2; attempt++) {
      int rc = j->dt->put(j->dev, &o.k, path, key, &j->kjob);
      if (rc == KITH_ERR_FULL) *full = 1;
      if (rc != KITH_OK) {
        st = rc == KITH_ERR_FULL ? MC_ERR_IO : from_kith(rc);
        break;
      }
      key[KITH_KEY_LEN - 1] = '\0';
      if (verified(j, key, sha)) {
        st = MC_OK;
        break;
      }
      fprintf(stderr, "kithcore: tethered: %s didn't verify on the device%s\n", w->track_uid,
              attempt ? "" : ", writing it again");
      if (attempt) {
        j->dt->remove(j->dev, key); /* a bad copy goes, so the next sync tries again */
        st = MC_ERR_IO;
      }
    }
    mc_encode_pool_release(j->t->pool);
  }
  track_out_clear(&o);
  free(path);
  return st;
}

/* Rewrites a kept track's tags and counts where they differ from the library's. */
static void update_kept(job *j, dtrack *d) {
  track_out o;
  if (fill_track(j, d->track_uid, &o) != MC_OK) return;
  int differs = o.k.play_count != d->play_count ||
                o.k.last_played_ms / 1000 != d->last_played_ms / 1000 ||
                strcmp(o.title ? o.title : "", d->title ? d->title : "") ||
                strcmp(o.artist ? o.artist : "", d->artist ? d->artist : "") ||
                strcmp(o.album ? o.album : "", d->album ? d->album : "");
  if (differs) {
    copy(o.k.file_uid, sizeof o.k.file_uid, d->file_uid);
    o.k.format = d->format;
    o.k.setting = d->setting;
    o.k.bitrate_kbps = d->kbps;
    if (j->dt->update(j->dev, d->key, &o.k) == KITH_OK) {
      j->r.updated++;
      j->since_commit++;
    }
  }
  track_out_clear(&o);
}

/* The profile's playlists, as far as their tracks are on the device. */
static void write_playlists(job *j, const mc_sync_profile *profile) {
  if (!j->dt->playlists_write || !j->info.has_playlists) return;
  int n = 0;
  for (int i = 0; i < profile->count; i++)
    if (profile->items[i].kind == MC_PROFILE_PLAYLIST && !profile->sync_all) n++;
  kith_playlist *pls = n ? calloc((size_t)n, sizeof *pls) : NULL;
  char **names = n ? calloc((size_t)n, sizeof *names) : NULL;
  const char ***keys = n ? calloc((size_t)n, sizeof *keys) : NULL;
  if (n && (!pls || !names || !keys)) goto done;
  int k = 0;
  for (int i = 0; i < profile->count && k < n; i++) {
    if (profile->items[i].kind != MC_PROFILE_PLAYLIST || profile->sync_all) continue;
    const char *puid = profile->items[i].ref_uid;
    mc_track_list *tl = NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(mc_library_db(j->lib), "SELECT name FROM playlists WHERE uid = ?1", -1,
                           &stmt, NULL) != SQLITE_OK)
      continue;
    bind_text(stmt, 1, puid);
    if (sqlite3_step(stmt) == SQLITE_ROW) names[k] = col_dup(stmt, 0);
    sqlite3_finalize(stmt);
    if (!names[k] || mc_library_list_playlist_tracks(j->lib, puid, &tl) != MC_OK) {
      free(names[k]);
      names[k] = NULL;
      continue;
    }
    keys[k] = tl->count ? calloc((size_t)tl->count, sizeof **keys) : NULL;
    int m = 0;
    for (int e = 0; keys[k] && e < tl->count; e++)
      for (int d = 0; d < j->ntracks; d++)
        if (j->tracks[d].track_uid[0] && !strcmp(j->tracks[d].track_uid, tl->items[e].uid)) {
          keys[k][m++] = j->tracks[d].key;
          break;
        }
    mc_track_list_free(tl);
    pls[k].uid = puid;
    pls[k].name = names[k];
    pls[k].keys = keys[k];
    pls[k].count = m;
    k++;
  }
  j->dt->playlists_write(j->dev, pls, k);
done:
  for (int i = 0; i < n; i++) {
    if (names) free(names[i]);
    if (keys) free((void *)keys[i]);
  }
  free(names);
  free(keys);
  free(pls);
}

static int want_cmp(const void *a, const void *b) {
  return strcmp(((const want *)a)->track_uid, ((const want *)b)->track_uid);
}

/* Reads the device again after writing, for list()'s view of keys (playlists). */
static void relist(job *j) {
  for (int i = 0; i < j->ntracks; i++) {
    free(j->tracks[i].title);
    free(j->tracks[i].artist);
    free(j->tracks[i].album);
  }
  j->ntracks = 0;
  j->dt->list(j->dev, on_track, j);
  if (j->ntracks > 1) qsort(j->tracks, (size_t)j->ntracks, sizeof *j->tracks, dtrack_cmp);
}

static mc_status sync_files(job *j) {
  mc_sync_profile *profile = NULL;
  mc_tethered_policy pol;
  mc_status st = read_profile(mc_sync_db_handle(j->sdb), j->uid, &profile, &pol);
  if (st != MC_OK) return st;
  /* The wanted set, as plan.c works it out for any device, from this profile in hand: its
   * format and budget are this file's business, so it's planned as originals with none. */
  mc_sync_profile as_planned = *profile;
  as_planned.format.format = MC_FORMAT_ORIGINAL;
  as_planned.format.bitrate_kbps = 0;
  as_planned.budget_enabled = 0;
  as_planned.archive = 0;
  mc_plan *plan = NULL;
  st = mc_plan_compute_profile(j->sdb, j->lib, j->uid, &as_planned, -1, 0, &plan);
  want *wants = NULL;
  int nwants = 0;
  if (st == MC_OK && plan->count && !(wants = calloc((size_t)plan->count, sizeof *wants))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < plan->count; i++) {
    want *w = &wants[nwants];
    memcpy(w->track_uid, plan->tracks[i].track_uid, MC_UID_STRLEN);
    w->duration_ms = plan->tracks[i].duration_ms;
    w->on_device = -1;
    if (!original(j, w->track_uid, w)) {
      j->r.missing++;
      continue;
    }
    choose(j, &pol, w);
    nwants++;
  }
  mc_plan_free(plan);

  /* What the device holds already: Kithsong's tracks in the wanted format stay. */
  int64_t held = 0;
  for (int d = 0; d < j->ntracks; d++) {
    dtrack *t = &j->tracks[d];
    if (!t->track_uid[0]) {
      j->r.foreign++;
      continue;
    }
    held += t->size;
    want probe;
    memcpy(probe.track_uid, t->track_uid, MC_UID_STRLEN);
    want *w = nwants ? bsearch(&probe, wants, (size_t)nwants, sizeof *wants, want_cmp) : NULL;
    if (!w) continue;
    const char *format = w->copy ? w->src_format : w->opts.encoder;
    const char *setting = w->copy ? "" : w->opts.setting;
    int same = same_ci(t->format, format) && !strcmp(t->setting, setting) &&
               !strcmp(t->file_uid, w->file_uid);
    if (same && w->on_device != -2) {
      t->kept = 1;
      w->on_device = -2; /* held: nothing to write */
    } else if (!same && w->on_device == -1) {
      t->replace = 1;
      w->on_device = d;
    }
  }
  int64_t total = 0, to_write = 0;
  int nwrite = 0;
  for (int i = 0; i < nwants; i++) {
    total += wants[i].est_bytes;
    if (wants[i].on_device != -2) {
      nwrite++;
      to_write += wants[i].est_bytes;
    }
  }

  /* A budget: everything wanted must fit, or nothing changes. */
  if (st == MC_OK && profile->budget_enabled) {
    int64_t limit = profile->budget_bytes > 0 ? profile->budget_bytes : INT64_MAX;
    int64_t room = j->info.free_bytes + held;
    if (j->info.free_bytes >= 0 && room < limit) limit = room;
    if (total > limit) {
      j->r.over_budget = 1;
      j->r.shortfall_bytes = total - limit;
    }
  }

  if (st == MC_OK && !j->r.over_budget) {
    /* Removals first, for the room. */
    for (int d = 0; d < j->ntracks && !j->c->cancel; d++) {
      dtrack *t = &j->tracks[d];
      if (!t->track_uid[0] || t->kept || t->replace) continue;
      if (j->dt->remove(j->dev, t->key) == KITH_OK) {
        j->r.removed++;
        j->since_commit++;
        maybe_commit(j, 0);
      }
    }
    /* Then writing, a few encodes ahead. */
    j->p.total = nwrite;
    j->p.bytes_total = to_write;
    int full = 0, next_ahead = 0, done = 0;
    for (int i = 0; i < nwants && !j->c->cancel && !full; i++) {
      want *w = &wants[i];
      if (w->on_device == -2) continue;
      for (; next_ahead < nwants && next_ahead <= i + AHEAD; next_ahead++)
        if (wants[next_ahead].on_device != -2 && !wants[next_ahead].copy)
          mc_encode_pool_queue_as(j->t->pool, wants[next_ahead].file_uid, wants[next_ahead].src,
                                  &wants[next_ahead].opts, MC_ENCODE_AHEAD, j);
      track_out o;
      const char *title = NULL;
      if (fill_track(j, w->track_uid, &o) == MC_OK) title = o.title;
      progress(j, MC_TETHERED_PHASE_WRITING, title);
      track_out_clear(&o);
      mc_status ws = write_one(j, w, &full);
      if (ws == MC_OK) {
        j->r.written++;
        j->since_commit++;
      } else if (ws != MC_ERR_CANCELLED && !full) {
        j->r.failed++;
        fprintf(stderr, "kithcore: tethered: writing %s failed: %s\n", w->track_uid, mc_status_str(ws));
      }
      j->p.done = ++done;
      j->p.bytes_done += w->est_bytes;
      maybe_commit(j, 0);
    }
    mc_encode_pool_drop(j->t->pool, j);
    if (full) j->r.full = 1;
  }

  /* Counts and tags of what was already there, and the profile's playlists. */
  if (st == MC_OK && !j->c->cancel) {
    progress(j, MC_TETHERED_PHASE_FINISHING, NULL);
    for (int d = 0; d < j->ntracks && !j->c->cancel; d++)
      if (j->tracks[d].kept) update_kept(j, &j->tracks[d]);
    if (!j->r.over_budget && !j->c->cancel) {
      relist(j);
      write_playlists(j, profile);
    }
  }
  for (int i = 0; i < nwants; i++) free(wants[i].src);
  free(wants);
  mc_sync_profile_free(profile);
  return st;
}

static void store_after(job *j) {
  int64_t used = 0;
  for (int d = 0; d < j->ntracks; d++) used += j->tracks[d].size;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(j->sdb),
                         "UPDATE tethered_devices SET last_sync_ms = ?2,"
                         " free_bytes = CASE WHEN capacity_bytes > 0 THEN max(capacity_bytes - ?3, 0)"
                         " ELSE free_bytes END WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  bind_text(stmt, 1, j->uid);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)mc_wall_ms());
  sqlite3_bind_int64(stmt, 3, used);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void run(void *arg) {
  job *j = arg;
  mc_status st = mc_library_open_internal(j->t->lib_dir, &j->lib);
  if (st == MC_OK) st = mc_sync_db_open(j->t->config_dir, &j->sdb);
  if (st == MC_OK) st = mc_plugins_find_target(j->c->plugin_id, j->c->target_id, &j->pt);
  if (st == MC_OK) j->dt = j->pt.target;
  progress(j, MC_TETHERED_PHASE_READING, NULL);
  if (st == MC_OK) {
    memset(&j->info, 0, sizeof j->info);
    int rc = j->dt->open(j->mount, &j->dev, &j->info);
    st = rc == KITH_OK && j->dev ? MC_OK : MC_ERR_IO;
  }
  if (st == MC_OK) {
    copy(j->device_name, sizeof j->device_name, j->info.name);
    int rc = j->dt->list(j->dev, on_track, j);
    st = j->oom ? MC_ERR_NOMEM : from_kith(rc);
    if (j->ntracks > 1) qsort(j->tracks, (size_t)j->ntracks, sizeof *j->tracks, dtrack_cmp);
  }
  j->last_commit = mc_mono_ms();
  if (st == MC_OK) {
    progress(j, MC_TETHERED_PHASE_IMPORTING, NULL);
    st = import_plays(j);
    if (st == MC_OK) st = import_playlists(j);
  }
  if (st == MC_OK && !j->c->cancel) st = sync_files(j);
  if (j->dev) {
    maybe_commit(j, 1);
    j->dt->close(j->dev);
    store_after(j);
  }
  if (st == MC_OK && j->c->cancel) st = MC_ERR_CANCELLED;
  if (st == MC_OK && !j->c->connected) st = MC_ERR_IO;
  j->r.status = st;
  j->r.device_uid = j->uid;
  progress(j, MC_TETHERED_PHASE_DONE, NULL);
  mc_mutex_lock(&j->t->lock);
  j->c->syncing = 0;
  mc_mutex_unlock(&j->t->lock);
  emit_done(j->t, &j->r);
  emit_devices(j->t);
  for (int i = 0; i < j->ntracks; i++) {
    free(j->tracks[i].title);
    free(j->tracks[i].artist);
    free(j->tracks[i].album);
  }
  free(j->tracks);
  mc_sync_db_close(j->sdb);
  mc_library_close(j->lib);
  free(j->mount);
  free(j);
}

mc_status mc_tethered_sync(mc_tethered *t, const char *device_uid) {
  if (!t || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&t->lock);
  conn *c = find_conn(t, device_uid);
  mc_status st = !c || !c->connected ? MC_ERR_NOT_FOUND : c->syncing ? MC_ERR_IN_USE : MC_OK;
  mc_plugin_target pt;
  if (st == MC_OK && mc_plugins_find_target(c->plugin_id, c->target_id, &pt) != MC_OK)
    st = MC_ERR_UNIMPLEMENTED;
  if (st == MC_OK) c->syncing = 1; /* claimed, so no second sync starts meanwhile */
  mc_mutex_unlock(&t->lock);
  if (st != MC_OK) return st;
  join_conn(c); /* the last one's thread, finished */
  job *j = calloc(1, sizeof *j);
  mc_mutex_lock(&t->lock);
  char *mount = c->mount ? mc_strdup(c->mount) : NULL;
  mc_mutex_unlock(&t->lock);
  if (!j || !mount) {
    free(j);
    free(mount);
    mc_mutex_lock(&t->lock);
    c->syncing = 0;
    mc_mutex_unlock(&t->lock);
    return MC_ERR_NOMEM;
  }
  j->t = t;
  j->c = c;
  j->mount = mount;
  memcpy(j->uid, device_uid, MC_UID_STRLEN);
  j->kjob.stop = stop_job;
  j->kjob.user = j;
  c->cancel = 0;
  if ((st = mc_thread_start(&c->thread, run, j)) != MC_OK) {
    free(j->mount);
    free(j);
    mc_mutex_lock(&t->lock);
    c->syncing = 0;
    mc_mutex_unlock(&t->lock);
    return st;
  }
  c->thread_live = 1;
  emit_devices(t);
  return MC_OK;
}

mc_status mc_tethered_cancel(mc_tethered *t, const char *device_uid) {
  if (!t || !device_uid) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&t->lock);
  conn *c = find_conn(t, device_uid);
  if (c && c->syncing) c->cancel = 1;
  mc_mutex_unlock(&t->lock);
  return MC_OK;
}

mc_status mc_tethered_eject(mc_tethered *t, const char *device_uid) {
  if (!t || !device_uid) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&t->lock);
  conn *c = find_conn(t, device_uid);
  mc_status st = !c || !c->connected ? MC_ERR_NOT_FOUND : c->syncing ? MC_ERR_IN_USE : MC_OK;
  char *mount = st == MC_OK && c->mount ? mc_strdup(c->mount) : NULL;
  char plugin[128] = "", target[64] = "";
  if (c) {
    copy(plugin, sizeof plugin, c->plugin_id);
    copy(target, sizeof target, c->target_id);
  }
  mc_mutex_unlock(&t->lock);
  if (st != MC_OK) return st;
  mc_plugin_target pt;
  if (mount && mc_plugins_find_target(plugin, target, &pt) == MC_OK && pt.target->eject)
    st = from_kith(pt.target->eject(mount));
  free(mount);
  return st;
}

mc_status mc_tethered_forget(mc_tethered *t, const char *device_uid) {
  if (!t || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&t->lock);
  conn *c = find_conn(t, device_uid);
  mc_status st = c && (c->connected || c->syncing) ? MC_ERR_IN_USE : MC_OK;
  mc_mutex_unlock(&t->lock);
  if (st != MC_OK) return st;
  sqlite3 *db = mc_sync_db_handle(t->sdb);
  static const char *const sql[] = {
    "DELETE FROM tethered_profile_items WHERE device_uid = ?1",
    "DELETE FROM tethered_imports WHERE device_uid = ?1",
    "DELETE FROM tethered_devices WHERE uid = ?1",
  };
  if (exec(db, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  for (int i = 0; st == MC_OK && i < 3; i++) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql[i], -1, &stmt, NULL) != SQLITE_OK) {
      st = MC_ERR_DB;
      break;
    }
    bind_text(stmt, 1, device_uid);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = exec(db, "COMMIT");
  if (st != MC_OK) exec(db, "ROLLBACK");
  if (st == MC_OK) emit_devices(t);
  return st;
}
