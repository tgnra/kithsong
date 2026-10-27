/* Sync profiles: reading and editing what each member holds (the profiles and
 * profile_items tables of sync.db), making a device an archive, and estimating a profile's
 * size. See "Profiles and what a device holds" and "Archives" in docs/sync-protocol.md.
 *
 * Edits go through the group changelog field by field, so a set writes only what differs
 * from the stored profile: changed columns, and items inserted or deleted one by one. Two
 * members editing one profile at once then both keep their changes.
 *
 * Confirming. A device's file phase runs on its own only while its profile has the
 * fingerprint it had when its user last pressed Sync (devices.confirmed_profile); see
 * "Confirming profiles" in docs/sync-protocol.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/merge.h"
#include "sync/plan.h"
#include "sync/sync_internal.h"

/* profile_items.kind, indexed by mc_profile_item_kind. */
static const char *const k_kinds[] = {"album", "artist", "playlist", "album_group"};
#define KIND_COUNT ((int)(sizeof k_kinds / sizeof k_kinds[0]))

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* A uid in a fixed-size field, which may not be terminated. */
static int is_uid_field(const char s[MC_UID_STRLEN]) {
  return memchr(s, '\0', MC_UID_STRLEN) == s + MC_UID_STRLEN - 1;
}

static int kind_parse(const char *s) {
  for (int i = 0; s && i < KIND_COUNT; i++)
    if (!strcmp(s, k_kinds[i])) return i;
  return -1;
}

static int valid_bitrate(int kbps) {
  for (int i = 0; i < MC_OPUS_BITRATE_COUNT; i++)
    if (mc_opus_bitrates[i] == kbps) return 1;
  return 0;
}

static int item_cmp(const void *x, const void *y) {
  const mc_profile_item *a = x, *b = y;
  if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
  return strcmp(a->ref_uid, b->ref_uid);
}

/* Sorts items and drops duplicates; returns the new count. */
static int sort_unique(mc_profile_item *items, int count) {
  if (count < 2) return count;
  qsort(items, (size_t)count, sizeof *items, item_cmp);
  int n = 1;
  for (int i = 1; i < count; i++)
    if (item_cmp(&items[i], &items[n - 1])) items[n++] = items[i];
  return n;
}

/* Under api_lock. MC_ERR_NOT_FOUND unless this device is in a group and uid is one of its
 * current members. */
static mc_status check_member(mc_sync_db *sdb, const char *uid) {
  if (!mc_sync_db_group_uid(sdb)) return MC_ERR_NOT_FOUND;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT 1 FROM devices WHERE uid = ?1 AND removed = 0", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
}

const char *mc_profile_kind_str(mc_profile_item_kind kind) {
  return (int)kind >= 0 && (int)kind < KIND_COUNT ? k_kinds[kind] : NULL;
}

/* uid's profiles row into p (items untouched), the defaults if there is none; *exists says
 * which. Formats other than 'opus' read as original, modes other than 'selected' as all, and
 * any format with budget_enabled as auto: the budget picks it (older versions stored a
 * format beside the budget, which then walked down from it). archive is whether it is one
 * (sync/archive.h): the stored flag, or a void demotion. The other fields are as stored,
 * even for an archive. */
static mc_status read_fields(sqlite3 *db, const char *uid, mc_sync_profile *p, int *exists) {
  p->name = NULL;
  p->archive = 0;
  p->sync_all = 1;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  p->budget_enabled = 0;
  p->budget_bytes = 0;
  *exists = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT archive, mode, format, bitrate_kbps, budget_bytes, budget_enabled,"
                         " name FROM profiles WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *exists = 1;
    const char *mode = (const char *)sqlite3_column_text(stmt, 1);
    const char *format = (const char *)sqlite3_column_text(stmt, 2);
    const char *name = (const char *)sqlite3_column_text(stmt, 6);
    p->archive = sqlite3_column_int(stmt, 0) != 0;
    p->sync_all = !mode || strcmp(mode, "selected") != 0;
    if (format && !strcmp(format, "opus")) {
      p->format.format = MC_FORMAT_OPUS;
      p->format.bitrate_kbps = sqlite3_column_int(stmt, 3);
    }
    p->budget_bytes = sqlite3_column_int64(stmt, 4);
    p->budget_enabled = sqlite3_column_int(stmt, 5) != 0;
    if (p->budget_enabled) {
      p->format.format = MC_FORMAT_AUTO;
      p->format.bitrate_kbps = 0;
    }
    if (name && *name && !(p->name = strdup(name))) st = MC_ERR_NOMEM;
  } else if (rc != SQLITE_DONE) {
    st = MC_ERR_DB;
  }
  sqlite3_finalize(stmt);
  if (st == MC_OK && !p->archive) {
    int a = mc_archive_is(db, uid);
    if (a < 0) st = MC_ERR_DB;
    p->archive = a > 0;
  }
  return st;
}

/* What an archive's profile is fixed at: everything, originals, no budget. */
static int archive_shape(const mc_sync_profile *p) {
  return p->sync_all && p->format.format == MC_FORMAT_ORIGINAL && !p->budget_enabled;
}

/* An album or artist item names the uid its duplicates were merged into (see "Duplicates" in
 * docs/sync-protocol.md), whichever copy it was made for. The stored row keeps the uid it
 * was written with, so sync.db stays what its changelog says. lib may be NULL. */
static mc_status resolve_item(mc_library *lib, mc_profile_item *item) {
  if (item->kind != MC_PROFILE_ALBUM && item->kind != MC_PROFILE_ARTIST) return MC_OK;
  return mc_merge_resolve(lib, item->ref_uid, item->ref_uid);
}

/* uid's items of known kinds, resolved, sorted, each (kind, ref_uid) once. *out is malloc'd
 * (NULL if there are none). */
static mc_status read_items(sqlite3 *db, mc_library *lib, const char *uid, mc_profile_item **out,
                            int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT DISTINCT kind, ref_uid FROM profile_items WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  mc_profile_item *items = NULL;
  int n = 0, cap = 0, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int kind = kind_parse((const char *)sqlite3_column_text(stmt, 0));
    const char *ref = (const char *)sqlite3_column_text(stmt, 1);
    if (kind < 0 || !is_uid(ref)) continue; /* a kind this version doesn't know */
    if (n == cap) {
      int grown = cap ? cap * 2 : 16;
      mc_profile_item *more = realloc(items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      items = more;
      cap = grown;
    }
    items[n].kind = (mc_profile_item_kind)kind;
    memcpy(items[n].ref_uid, ref, MC_UID_STRLEN);
    if ((st = resolve_item(lib, &items[n])) != MC_OK) break;
    n++;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    free(items);
    return st;
  }
  *out = items;
  *count = sort_unique(items, n);
  return MC_OK;
}

mc_status mc_sync_get_profile(mc_sync *sync, const char *device_uid, mc_sync_profile **out) {
  if (out) *out = NULL;
  if (!sync || !is_uid(device_uid) || !out) return MC_ERR_INVALID_ARG;
  mc_sync_profile *p = calloc(1, sizeof *p);
  if (!p) return MC_ERR_NOMEM;
  mc_mutex_lock(&sync->api_lock);
  sqlite3 *db = mc_sync_db_handle(sync->api.sdb);
  int exists;
  mc_status st = check_member(sync->api.sdb, device_uid);
  if (st == MC_OK) st = read_fields(db, device_uid, p, &exists);
  if (st == MC_OK) st = read_items(db, sync->api.lib, device_uid, &p->items, &p->count);
  mc_mutex_unlock(&sync->api_lock);
  if (st != MC_OK) {
    mc_sync_profile_free(p);
    return st;
  }
  if (p->archive) {
    /* Shown as what it holds. The stored mode, format and budget flag stay as they are, and
     * apply again if it stops being an archive; budget_bytes and the items are kept. */
    p->sync_all = 1;
    p->format.format = MC_FORMAT_ORIGINAL;
    p->format.bitrate_kbps = 0;
    p->budget_enabled = 0;
  }
  *out = p;
  return MC_OK;
}

void mc_sync_profile_free(mc_sync_profile *profile) {
  if (!profile) return;
  free(profile->name);
  free(profile->items);
  free(profile);
}

/* --- Setting --- */

static mc_status run(sqlite3_stmt *stmt) {
  int rc = sqlite3_step(stmt);
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static int same_text(const char *a, const char *b) {
  return a == b || (a && b && !strcmp(a, b));
}

/* A bare profiles row holding the defaults, which every version has anyway: unlogged, so
 * the insert trigger doesn't log each default as an edit that could beat a member's
 * concurrent change to that field. The caller's update then logs what it changes. */
static mc_status insert_bare(mc_sync_db *sdb, const char *uid) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), "INSERT INTO profiles (device_uid) VALUES (?1)",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int was = sdb->applying_remote;
  sdb->applying_remote = 1;
  mc_status st = run(stmt);
  sdb->applying_remote = was;
  sqlite3_finalize(stmt);
  return st;
}

/* The profiles row: the fields of p that differ from the stored ones (old, which exists or
 * is the defaults). *changed is set if anything was written. Auto is stored as originals
 * with budget_enabled, which versions without auto read the same way: the walk then starts
 * at originals. */
static mc_status write_fields(mc_sync_db *sdb, const char *uid, const mc_sync_profile *old,
                              int exists, const mc_sync_profile *p, const char *name,
                              int *changed) {
  int opus = p->format.format == MC_FORMAT_OPUS;
  if (p->sync_all == old->sync_all && p->format.format == old->format.format &&
      (!opus || p->format.bitrate_kbps == old->format.bitrate_kbps) &&
      !p->budget_enabled == !old->budget_enabled && p->budget_bytes == old->budget_bytes &&
      same_text(name, old->name))
    return MC_OK;
  sqlite3 *db = mc_sync_db_handle(sdb);
  sqlite3_stmt *stmt;
  if (!exists) {
    mc_status st = insert_bare(sdb, uid);
    if (st != MC_OK) return st;
  }
  /* The update triggers log only the columns whose value changes. */
  if (sqlite3_prepare_v2(db,
                         "UPDATE profiles SET mode = ?2, format = ?3, bitrate_kbps = ?4,"
                         " budget_bytes = ?5, budget_enabled = ?6, name = ?7"
                         " WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, p->sync_all ? "all" : "selected", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, opus ? "opus" : "original", -1, SQLITE_STATIC);
  if (opus) sqlite3_bind_int(stmt, 4, p->format.bitrate_kbps);
  if (p->budget_bytes) sqlite3_bind_int64(stmt, 5, p->budget_bytes);
  sqlite3_bind_int(stmt, 6, p->budget_enabled ? 1 : 0);
  if (name) sqlite3_bind_text(stmt, 7, name, -1, SQLITE_STATIC);
  mc_status st = run(stmt);
  sqlite3_finalize(stmt);
  if (st == MC_OK) *changed = 1;
  return st;
}

/* Deletes uid's stored rows that resolve to item: every copy, whichever duplicate's uid it
 * was written with. */
static mc_status delete_item(sqlite3 *db, mc_library *lib, const char *uid,
                             const mc_profile_item *item) {
  sqlite3_stmt *sel, *del = NULL;
  if (sqlite3_prepare_v2(db, "SELECT uid, ref_uid FROM profile_items WHERE device_uid = ?1"
                             " AND kind = ?2", -1, &sel, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = sqlite3_prepare_v2(db, "DELETE FROM profile_items WHERE uid = ?1", -1, &del,
                                    NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  sqlite3_bind_text(sel, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(sel, 2, k_kinds[item->kind], -1, SQLITE_STATIC);
  /* Collected first: deleting while stepping the same table's rows is best avoided. */
  char (*rows)[MC_UID_STRLEN] = NULL;
  int n = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(sel)) == SQLITE_ROW) {
    const char *row = (const char *)sqlite3_column_text(sel, 0);
    mc_profile_item it = {.kind = item->kind};
    const char *ref = (const char *)sqlite3_column_text(sel, 1);
    if (!is_uid(row) || !is_uid(ref)) continue;
    memcpy(it.ref_uid, ref, MC_UID_STRLEN);
    if ((st = resolve_item(lib, &it)) != MC_OK || strcmp(it.ref_uid, item->ref_uid)) continue;
    char (*more)[MC_UID_STRLEN] = realloc(rows, (size_t)(n + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    rows = more;
    memcpy(rows[n++], row, MC_UID_STRLEN);
  }
  if (st == MC_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) st = MC_ERR_DB;
  sqlite3_finalize(sel);
  for (int i = 0; st == MC_OK && i < n; i++) {
    sqlite3_bind_text(del, 1, rows[i], -1, SQLITE_STATIC);
    st = run(del);
  }
  sqlite3_finalize(del);
  free(rows);
  return st;
}

/* profile_items: inserts the wanted items not stored, and deletes the stored ones not
 * wanted (with any duplicates of them). want is sorted and unique; it is resolved here, so a
 * caller holding a merged-away uid still means its album. Rows of kinds this version
 * doesn't know are left as they are. */
static mc_status write_items(mc_sync_db *sdb, mc_library *lib, const char *uid,
                             mc_profile_item *want, int nwant, int *changed) {
  sqlite3 *db = mc_sync_db_handle(sdb);
  mc_profile_item *have;
  int nhave;
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < nwant; i++) st = resolve_item(lib, &want[i]);
  if (st != MC_OK) return st;
  nwant = sort_unique(want, nwant);
  st = read_items(db, lib, uid, &have, &nhave);
  if (st != MC_OK) return st;
  sqlite3_stmt *ins = NULL;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO profile_items (uid, device_uid, kind, ref_uid)"
                         " VALUES (?1, ?2, ?3, ?4)",
                         -1, &ins, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int i = 0, j = 0;
  while (st == MC_OK && (i < nwant || j < nhave)) {
    int c = i == nwant ? 1 : j == nhave ? -1 : item_cmp(&want[i], &have[j]);
    if (c < 0) {
      char item_uid[MC_UID_STRLEN];
      st = mc_uid_new(item_uid);
      if (st != MC_OK) break;
      sqlite3_bind_text(ins, 1, item_uid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, uid, -1, SQLITE_STATIC);
      sqlite3_bind_text(ins, 3, k_kinds[want[i].kind], -1, SQLITE_STATIC);
      sqlite3_bind_text(ins, 4, want[i].ref_uid, -1, SQLITE_STATIC);
      st = run(ins);
      i++;
    } else if (c > 0) {
      st = delete_item(db, lib, uid, &have[j]);
      j++;
    } else {
      i++;
      j++;
      continue;
    }
    if (st == MC_OK) *changed = 1;
  }
  sqlite3_finalize(ins);
  free(have);
  return st;
}

/* MC_ERR_INVALID_ARG for a profile no device could have. */
static mc_status check_profile(const mc_sync_profile *profile) {
  if (!profile || profile->count < 0 || (profile->count && !profile->items) ||
      profile->budget_bytes < 0)
    return MC_ERR_INVALID_ARG;
  if (profile->format.format == MC_FORMAT_OPUS) {
    if (!valid_bitrate(profile->format.bitrate_kbps)) return MC_ERR_INVALID_ARG;
  } else if (profile->format.format != MC_FORMAT_ORIGINAL &&
             profile->format.format != MC_FORMAT_AUTO) {
    return MC_ERR_INVALID_ARG;
  }
  /* The budget picks the format, so the two come together or not at all. */
  if (!profile->budget_enabled != (profile->format.format != MC_FORMAT_AUTO))
    return MC_ERR_INVALID_ARG;
  for (int i = 0; i < profile->count; i++)
    if ((int)profile->items[i].kind < 0 || (int)profile->items[i].kind >= KIND_COUNT ||
        !is_uid_field(profile->items[i].ref_uid))
      return MC_ERR_INVALID_ARG;
  return MC_OK;
}

mc_status mc_sync_set_profile(mc_sync *sync, const char *device_uid, const mc_sync_profile *profile) {
  if (!sync || !is_uid(device_uid) || check_profile(profile) != MC_OK) return MC_ERR_INVALID_ARG;
  mc_profile_item *want = NULL;
  int nwant = 0;
  if (profile->count) {
    want = malloc((size_t)profile->count * sizeof *want);
    if (!want) return MC_ERR_NOMEM;
    memcpy(want, profile->items, (size_t)profile->count * sizeof *want);
    nwant = sort_unique(want, profile->count);
  }
  const char *name = profile->name && *profile->name ? profile->name : NULL;

  mc_mutex_lock(&sync->api_lock);
  mc_sync_db *sdb = sync->api.sdb;
  mc_sync_profile old = {0};
  int exists = 0, changed = 0;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st == MC_OK) {
    /* Compared against what's stored, inside the write so nothing changes it meanwhile. */
    st = check_member(sdb, device_uid);
    if (st == MC_OK) st = read_fields(mc_sync_db_handle(sdb), device_uid, &old, &exists);
    mc_sync_profile want_fields = *profile;
    if (st == MC_OK && old.archive) {
      if (!archive_shape(profile)) st = MC_ERR_LOCKED;
      /* The fixed fields aren't written: what's stored stays for when it stops being one. */
      want_fields.sync_all = old.sync_all;
      want_fields.format = old.format;
      want_fields.budget_enabled = old.budget_enabled;
    }
    if (st == MC_OK)
      st = write_fields(sdb, device_uid, &old, exists, &want_fields, name, &changed);
    if (st == MC_OK) st = write_items(sdb, sync->api.lib, device_uid, want, nwant, &changed);
    if (st == MC_OK) st = mc_sync_db_commit(sdb);
    if (st != MC_OK) mc_sync_db_rollback(sdb);
  }
  /* A new name for this device takes effect now, not at the next session. */
  if (st == MC_OK && changed && strcmp(device_uid, mc_sync_device_uid(sync)) == 0)
    mc_sync_take_group_name(sync, &sync->api);
  mc_mutex_unlock(&sync->api_lock);
  free(old.name);
  free(want);
  if (st == MC_OK && changed) mc_sync_notify_changed(sync);
  return st;
}

/* --- Archives --- */

/* The wall time (Unix ms) of the library's latest change, 0 if it has none; -1 on error. */
static int64_t library_latest_ms(mc_library *lib) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT max(hlc) FROM changelog", -1, &stmt,
                         NULL) != SQLITE_OK)
    return -1;
  int64_t ms = -1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    ms = (int64_t)((uint64_t)sqlite3_column_int64(stmt, 0) >> 16); /* its wall ms */
  sqlite3_finalize(stmt);
  return ms;
}

/* Whether a current member other than uid is an archive whose sync_status shows it
 * verified complete: nothing missing or corrupt, checked after latest_ms. 1, 0 or -1. */
static int other_archive_verified(sqlite3 *db, const char *uid, int64_t latest_ms) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM profiles p JOIN devices d ON d.uid = p.device_uid"
                         " JOIN sync_status s ON s.device_uid = p.device_uid"
                         " WHERE p.archive <> 0 AND d.removed = 0 AND p.device_uid <> ?1"
                         " AND s.missing = 0 AND s.corrupt = 0 AND s.verified_ms > ?2",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, latest_ms);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* Logs profiles.archive = value for uid, adding a bare row first if it has none. */
static mc_status write_archive(mc_sync_db *sdb, const char *uid, int exists, int value) {
  mc_status st = exists ? MC_OK : insert_bare(sdb, uid);
  sqlite3_stmt *stmt;
  if (st == MC_OK &&
      sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "UPDATE profiles SET archive = ?2 WHERE device_uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, value);
    st = run(stmt);
    sqlite3_finalize(stmt);
  } else if (st == MC_OK) {
    st = MC_ERR_DB;
  }
  return st;
}

mc_status mc_sync_set_archive(mc_sync *sync, const char *device_uid, int archive) {
  if (!sync || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  int self = !strcmp(device_uid, mc_sync_device_uid(sync));
  /* Clearing judges the other archives' checks by the library's latest change, which only
   * the group's library can say. */
  mc_sync_lib_state state = MC_SYNC_LIBRARY_NO_GROUP;
  if (!archive && self) {
    mc_status st = mc_sync_library_state(sync, &state);
    if (st != MC_OK) return st;
  }

  mc_mutex_lock(&sync->api_lock);
  mc_sync_db *sdb = sync->api.sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  int changed = 0;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st == MC_OK) {
    st = check_member(sdb, device_uid);
    mc_sync_profile old = {0};
    int exists = 0;
    if (st == MC_OK) st = read_fields(db, device_uid, &old, &exists);
    free(old.name);
    /* old.archive counts a void demotion: such a device is one, but its row says 0. */
    int stored = 0;
    if (st == MC_OK && exists) {
      sqlite3_stmt *stmt;
      if (sqlite3_prepare_v2(db, "SELECT archive <> 0 FROM profiles WHERE device_uid = ?1", -1,
                             &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, device_uid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) stored = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
      } else {
        st = MC_ERR_DB;
      }
    }
    if (st == MC_OK && archive && !stored) {
      /* Any member, any device. Only the flag is written: the rest of the profile stays as
       * it is, unused while it is an archive. */
      st = write_archive(sdb, device_uid, exists, 1);
      changed = st == MC_OK;
    } else if (st == MC_OK && !archive && old.archive) {
      int64_t latest = 0;
      int verified = 0;
      if (!self) {
        st = MC_ERR_LOCKED; /* only the device itself */
      } else if (state != MC_SYNC_LIBRARY_GROUP) {
        st = MC_ERR_LIBRARY_MISMATCH;
      } else if ((latest = library_latest_ms(sync->api.lib)) < 0 ||
                 (verified = other_archive_verified(db, device_uid, latest)) < 0) {
        st = MC_ERR_DB;
      } else if (!verified) {
        st = MC_ERR_LOCKED;
      } else {
        st = write_archive(sdb, device_uid, exists, 0);
        changed = st == MC_OK;
      }
    }
    if (st == MC_OK) st = mc_sync_db_commit(sdb);
    if (st != MC_OK) {
      mc_sync_db_rollback(sdb);
      changed = 0;
    }
  }
  mc_mutex_unlock(&sync->api_lock);
  if (changed) mc_sync_notify_changed(sync);
  return st;
}

/* --- Size estimate --- */

/* device_uid's free space: measured here for this device, else from its capabilities row.
 * -1 if unknown. */
static int64_t free_space(mc_sync *sync, const char *device_uid) {
  int64_t free_bytes = -1;
  if (!strcmp(device_uid, mc_sync_device_uid(sync))) {
    if (mc_free_space(mc_library_dir(sync->api.lib), &free_bytes) != MC_OK) free_bytes = -1;
    return free_bytes;
  }
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sync->api.sdb),
                         "SELECT free_bytes FROM capabilities WHERE device_uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, device_uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    free_bytes = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return free_bytes;
}

mc_status mc_sync_estimate_profile(mc_sync *sync, const char *device_uid,
                                   const mc_sync_profile *profile, mc_sync_estimate *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!sync || !is_uid(device_uid) || !out || check_profile(profile) != MC_OK)
    return MC_ERR_INVALID_ARG;
  mc_sync_lib_state state;
  mc_status st = mc_sync_library_state(sync, &state);
  if (st != MC_OK) return st;
  if (state == MC_SYNC_LIBRARY_NO_GROUP) return MC_ERR_NOT_FOUND;
  if (state != MC_SYNC_LIBRARY_GROUP) return MC_ERR_LIBRARY_MISMATCH;

  mc_mutex_lock(&sync->api_lock);
  mc_plan *plan = NULL;
  int64_t free_bytes = -1;
  st = check_member(sync->api.sdb, device_uid);
  if (st == MC_OK) {
    free_bytes = free_space(sync, device_uid);
    st = mc_plan_compute_profile(sync->api.sdb, sync->api.lib, device_uid, profile, free_bytes,
                                 MC_PLAN_SIZES, &plan);
  }
  mc_mutex_unlock(&sync->api_lock);
  if (st != MC_OK) return st;
  out->archive = plan->archive;
  out->tracks = plan->wanted;
  out->total_bytes = plan->total_bytes;
  out->kbps = plan->kbps;
  out->over_budget = plan->status == MC_PLAN_OVER_BUDGET;
  out->shortfall_bytes = plan->shortfall_bytes;
  out->limit_bytes = plan->limit_bytes;
  out->free_bytes = free_bytes;
  for (int i = 0; i < MC_SYNC_ESTIMATE_STEPS; i++) out->step_bytes[i] = plan->step_bytes[i];
  mc_plan_free(plan);
  return MC_OK;
}

/* --- Confirming --- */

static void sha_line(mbedtls_sha256_context *ctx, const char *s) {
  mbedtls_sha256_update(ctx, (const unsigned char *)s, strlen(s));
  mbedtls_sha256_update(ctx, (const unsigned char *)"\n", 1);
}

mc_status mc_profile_fingerprint(sqlite3 *db, const char *uid, char out[MC_PROFILE_FP_STRLEN]) {
  out[0] = '\0';
  int archive = mc_archive_is(db, uid);
  if (archive < 0) return MC_ERR_DB;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mc_status st = MC_OK;
  /* Only what decides what the device holds. An archive's row and items don't, nor does the
   * selection while it syncs everything, a bitrate with originals, a switched-off budget's
   * size, or the name. */
  if (archive) {
    sha_line(&ctx, "archive");
  } else {
    sqlite3_stmt *stmt;
    int all = 1, rc = SQLITE_DONE;
    if (sqlite3_prepare_v2(db,
                           "SELECT mode, format, bitrate_kbps, budget_enabled, budget_bytes"
                           " FROM profiles WHERE device_uid = ?1",
                           -1, &stmt, NULL) != SQLITE_OK) {
      st = MC_ERR_DB;
    } else {
      sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
      rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const char *mode = (const char *)sqlite3_column_text(stmt, 0);
        const char *format = (const char *)sqlite3_column_text(stmt, 1);
        int opus = format && !strcmp(format, "opus");
        int budget = sqlite3_column_int(stmt, 3) != 0;
        all = !mode || strcmp(mode, "selected") != 0;
        char line[128];
        snprintf(line, sizeof line, "%s %s %d %s %lld", all ? "all" : "selected",
                 opus ? "opus" : "original", opus ? sqlite3_column_int(stmt, 2) : 0,
                 budget ? "budget" : "-", budget ? (long long)sqlite3_column_int64(stmt, 4) : 0LL);
        sha_line(&ctx, line);
      } else if (rc == SQLITE_DONE) {
        sha_line(&ctx, "none");
      } else {
        st = MC_ERR_DB;
      }
      sqlite3_finalize(stmt);
    }
    /* The stored rows as they are, of every kind, so devices of any version agree. */
    if (st == MC_OK && rc == SQLITE_ROW && !all) {
      if (sqlite3_prepare_v2(db,
                             "SELECT DISTINCT kind, ref_uid FROM profile_items"
                             " WHERE device_uid = ?1 ORDER BY kind, ref_uid",
                             -1, &stmt, NULL) != SQLITE_OK) {
        st = MC_ERR_DB;
      } else {
        sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
          const char *kind = (const char *)sqlite3_column_text(stmt, 0);
          const char *ref = (const char *)sqlite3_column_text(stmt, 1);
          char line[160];
          snprintf(line, sizeof line, "%s %s", kind ? kind : "", ref ? ref : "");
          sha_line(&ctx, line);
        }
        if (rc != SQLITE_DONE) st = MC_ERR_DB;
        sqlite3_finalize(stmt);
      }
    }
  }
  unsigned char digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (st != MC_OK) return st;
  static const char hexd[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i] = hexd[digest[i] >> 4];
    out[2 * i + 1] = hexd[digest[i] & 15];
  }
  out[64] = '\0';
  return MC_OK;
}

int mc_profile_confirm_state(sqlite3 *db, const char *uid) {
  char fp[MC_PROFILE_FP_STRLEN];
  if (mc_profile_fingerprint(db, uid, fp) != MC_OK) return -1;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT confirmed_profile FROM devices WHERE uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int state = MC_SYNC_UNCONFIRMED, rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    if (v && *v) state = !strcmp(v, fp) ? MC_SYNC_CONFIRMED : MC_SYNC_PROFILE_CHANGED;
  } else if (rc != SQLITE_DONE) {
    state = -1;
  }
  sqlite3_finalize(stmt);
  return state;
}

mc_status mc_profile_confirm(mc_sync_db *sdb, int *wrote) {
  if (wrote) *wrote = 0;
  if (!mc_sync_db_group_uid(sdb)) return MC_ERR_NOT_FOUND;
  sqlite3 *db = mc_sync_db_handle(sdb);
  const char *me = mc_sync_db_device_uid(sdb);
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  /* Inside the write, so the fingerprint is of what's stored when it's recorded. */
  char fp[MC_PROFILE_FP_STRLEN];
  st = mc_profile_fingerprint(db, me, fp);
  sqlite3_stmt *stmt = NULL;
  if (st == MC_OK &&
      sqlite3_prepare_v2(db,
                         "UPDATE devices SET confirmed_profile = ?1 WHERE uid = ?2"
                         " AND confirmed_profile IS NOT ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  if (st == MC_OK) {
    sqlite3_bind_text(stmt, 1, fp, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, me, -1, SQLITE_STATIC);
    st = run(stmt);
    if (st == MC_OK && wrote) *wrote = sqlite3_changes(db) > 0;
  }
  sqlite3_finalize(stmt);
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}
