/* Duplicate merging. See merge.h and "Duplicates" in docs/sync-protocol.md.
 *
 * aliases is a union-find kept flat: each merged-away uid points straight at its class's
 * canonical (smallest) uid, and the canonical uid has no row. merge_keys records every key
 * value the changelog has ever shown for a uid, so a later edit that changes or clears a
 * MusicBrainz id never splits a class: merges are permanent, which is what keeps the
 * relation the same on every device whatever order rows arrive in. */
#include "sync/merge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "util/platform.h"

/* The field whose shared value makes two rows of a table one. */
typedef struct merge_key {
  const char *table, *field;
} merge_key;

static const merge_key k_keys[] = {
  {"artists", "mb_artistid"},
  {"albums", "mb_albumid"},
  {"tracks", "mb_releasetrackid"},
};

/* Columns that hold a mergeable uid. unique_with: the column is part of a UNIQUE key with
 * it, so two rows that would collide keep only the smaller uid. */
typedef struct merge_ref {
  const char *table, *col, *parent, *unique_with;
} merge_ref;

static const merge_ref k_refs[] = {
  {"albums", "artist_uid", "artists", NULL},
  {"tracks", "artist_uid", "artists", NULL},
  {"tracks", "album_uid", "albums", NULL},
  {"album_group_entries", "album_uid", "albums", NULL},
  {"files", "track_uid", "tracks", NULL},
  {"playlist_entries", "track_uid", "tracks", NULL},
  {"track_plays", "track_uid", "tracks", "node_uid"},
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

int mc_merge_table(const char *table) {
  for (size_t i = 0; i < COUNT(k_keys); i++)
    if (!strcmp(k_keys[i].table, table)) return 1;
  return !strcmp(table, "recents");
}

/* --- Helpers --- */

typedef struct uid_list {
  char (*items)[MC_UID_STRLEN];
  int count, cap;
} uid_list;

static int list_add(uid_list *l, const char *uid) {
  if (!uid || strlen(uid) != MC_UID_STRLEN - 1) return 1; /* not a uid: nothing to merge */
  if (l->count == l->cap) {
    int grown = l->cap ? l->cap * 2 : 8;
    char (*more)[MC_UID_STRLEN] = realloc(l->items, (size_t)grown * sizeof *more);
    if (!more) return 0;
    l->items = more;
    l->cap = grown;
  }
  memcpy(l->items[l->count++], uid, MC_UID_STRLEN);
  return 1;
}

/* Runs a prepared statement with text parameters to completion; its first column, a uid,
 * goes into out if given. Finalizes stmt. */
static mc_status run(sqlite3 *db, const char *sql, const char *p1, const char *p2,
                     uid_list *out) {
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return MC_ERR_DB;
  if (p1) sqlite3_bind_text(s, 1, p1, -1, SQLITE_STATIC);
  if (p2) sqlite3_bind_text(s, 2, p2, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW)
    if (out && !list_add(out, (const char *)sqlite3_column_text(s, 0))) {
      st = MC_ERR_NOMEM;
      break;
    }
  if (st == MC_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) st = MC_ERR_DB;
  sqlite3_finalize(s);
  return st;
}

/* The first uid sql returns, or "" for none. */
static mc_status one_uid(sqlite3 *db, const char *sql, const char *p1, const char *p2,
                         char out[MC_UID_STRLEN]) {
  uid_list l = {0};
  mc_status st = run(db, sql, p1, p2, &l);
  out[0] = '\0';
  if (st == MC_OK && l.count) memcpy(out, l.items[0], MC_UID_STRLEN);
  free(l.items);
  return st;
}

static mc_status canonical_of(sqlite3 *db, const char *uid, char out[MC_UID_STRLEN]) {
  char c[MC_UID_STRLEN];
  mc_status st = one_uid(db, "SELECT canonical FROM aliases WHERE uid = ?1", uid, NULL, c);
  if (st != MC_OK) return st;
  memmove(out, c[0] ? c : uid, MC_UID_STRLEN);
  return MC_OK;
}

static int row_exists(sqlite3 *db, const char *table, const char *uid, mc_status *st) {
  char sql[128], found[MC_UID_STRLEN];
  snprintf(sql, sizeof sql, "SELECT uid FROM %s WHERE uid = ?1", table);
  *st = one_uid(db, sql, uid, NULL, found);
  return found[0] != '\0';
}

/* --- Scanning --- */

typedef struct scan {
  sqlite3 *db;
  mc_merge_pair *pairs;
  int count, cap;
} scan;

/* Joins the classes of x and y in table. */
static mc_status join(scan *sc, const char *table, const char *x, const char *y) {
  char cx[MC_UID_STRLEN], cy[MC_UID_STRLEN];
  mc_status st = canonical_of(sc->db, x, cx);
  if (st == MC_OK) st = canonical_of(sc->db, y, cy);
  if (st != MC_OK || !strcmp(cx, cy)) return st;
  const char *c = strcmp(cx, cy) < 0 ? cx : cy, *l = c == cx ? cy : cx;

  if ((st = run(sc->db, "UPDATE aliases SET canonical = ?2 WHERE canonical = ?1", l, c, NULL)) !=
      MC_OK)
    return st;
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(sc->db, "INSERT INTO aliases (uid, entity, canonical) VALUES (?1, ?2, ?3)",
                         -1, &s, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(s, 1, l, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, c, -1, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE) return MC_ERR_DB;

  if (sc->count == sc->cap) {
    int grown = sc->cap ? sc->cap * 2 : 8;
    mc_merge_pair *more = realloc(sc->pairs, (size_t)grown * sizeof *more);
    if (!more) return MC_ERR_NOMEM;
    sc->pairs = more;
    sc->cap = grown;
  }
  mc_merge_pair *p = &sc->pairs[sc->count++];
  snprintf(p->table, sizeof p->table, "%s", table);
  memcpy(p->loser, l, MC_UID_STRLEN);
  return MC_OK;
}

/* One changelog row: records its key, and joins the uids that share it. */
static mc_status scan_row(scan *sc, const char *table, const char *uid, const char *field,
                          const char *value) {
  int key = 0;
  for (size_t i = 0; i < COUNT(k_keys); i++)
    if (!strcmp(k_keys[i].table, table) && !strcmp(k_keys[i].field, field)) key = 1;
  if (!key) return MC_OK;

  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(sc->db,
                         "INSERT OR IGNORE INTO merge_keys (entity, field, value, uid)"
                         " VALUES (?1, ?2, ?3, ?4)",
                         -1, &s, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(s, 1, table, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, field, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 3, value, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 4, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(s);
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE) return MC_ERR_DB;
  if (!sqlite3_changes(sc->db)) return MC_OK; /* seen before: nothing new */

  uid_list same = {0};
  char sql[200];
  snprintf(sql, sizeof sql,
           "SELECT uid FROM merge_keys WHERE entity = '%s' AND field = '%s' AND value = ?1"
           " AND uid <> ?2",
           table, field);
  mc_status st = run(sc->db, sql, value, uid, &same);
  for (int i = 0; st == MC_OK && i < same.count; i++) st = join(sc, table, uid, same.items[i]);
  free(same.items);
  return st;
}

mc_status mc_merge_scan(sqlite3 *db, mc_merge_pair **pairs, int *count) {
  *pairs = NULL;
  *count = 0;
  scan sc = {db, NULL, 0, 0};
  sqlite3_int64 from = 0, last;
  sqlite3_stmt *s;
  if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'merge_seq'", -1, &s, NULL) !=
      SQLITE_OK)
    return MC_ERR_DB;
  if (sqlite3_step(s) == SQLITE_ROW) from = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  last = from;

  if (sqlite3_prepare_v2(db,
                         "SELECT seq, entity, row_uid, field, value FROM changelog WHERE seq > ?1"
                         " AND entity IN ('artists', 'albums', 'tracks')"
                         " AND field IN ('mb_artistid', 'mb_albumid', 'mb_releasetrackid')"
                         " ORDER BY seq",
                         -1, &s, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_int64(s, 1, from);
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(s)) == SQLITE_ROW) {
    last = sqlite3_column_int64(s, 0);
    const char *table = (const char *)sqlite3_column_text(s, 1);
    const char *uid = (const char *)sqlite3_column_text(s, 2);
    const char *field = (const char *)sqlite3_column_text(s, 3);
    const char *value = (const char *)sqlite3_column_text(s, 4);
    /* Taggers write an empty id for "none". */
    if (!table || !uid || !field || !value || !*value || strlen(uid) != MC_UID_STRLEN - 1)
      continue;
    char t[16], u[MC_UID_STRLEN], f[32];
    snprintf(t, sizeof t, "%s", table);
    memcpy(u, uid, MC_UID_STRLEN);
    snprintf(f, sizeof f, "%s", field);
    char *v = mc_strdup(value);
    if (!v) {
      st = MC_ERR_NOMEM;
      break;
    }
    st = scan_row(&sc, t, u, f, v);
    free(v);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(s);

  if (st == MC_OK && last != from) {
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta (key, value) VALUES ('merge_seq', ?1)",
                           -1, &s, NULL) != SQLITE_OK)
      st = MC_ERR_DB;
    else {
      sqlite3_bind_int64(s, 1, last);
      if (sqlite3_step(s) != SQLITE_DONE) st = MC_ERR_DB;
      sqlite3_finalize(s);
    }
  }
  if (st != MC_OK) {
    free(sc.pairs);
    return st;
  }
  *pairs = sc.pairs;
  *count = sc.count;
  return MC_OK;
}

/* --- Rewriting --- */

mc_status mc_merge_canonical(sqlite3 *db, const char *uid, char out[MC_UID_STRLEN], int *merged) {
  mc_status st = canonical_of(db, uid, out);
  if (st != MC_OK) return st;
  *merged = strcmp(out, uid) != 0;
  if (!*merged) {
    char any[MC_UID_STRLEN];
    st = one_uid(db, "SELECT uid FROM aliases WHERE canonical = ?1 LIMIT 1", uid, NULL, any);
    *merged = any[0] != '\0';
  }
  return st;
}

/* Two rows that would share a UNIQUE key once from becomes to: the larger uid goes. */
static mc_status drop_collisions(sqlite3 *db, const merge_ref *r, const char *from, const char *to) {
  char sql[400];
  snprintf(sql, sizeof sql,
           "DELETE FROM %s WHERE uid IN (SELECT max(a.uid, b.uid) FROM %s a JOIN %s b"
           " ON b.%s = a.%s WHERE a.%s = ?1 AND b.%s = ?2)",
           r->table, r->table, r->table, r->unique_with, r->unique_with, r->col, r->col);
  return run(db, sql, from, to, NULL);
}

mc_status mc_merge_rewrite(sqlite3 *db, const mc_merge_pair *p) {
  char c[MC_UID_STRLEN];
  mc_status st = canonical_of(db, p->loser, c);
  if (st != MC_OK) return st;
  int exists = strcmp(c, p->loser) && row_exists(db, p->table, c, &st);
  char sql[200];
  for (size_t i = 0; st == MC_OK && exists && i < COUNT(k_refs); i++) {
    const merge_ref *r = &k_refs[i];
    if (strcmp(r->parent, p->table)) continue;
    if (r->unique_with && (st = drop_collisions(db, r, p->loser, c)) != MC_OK) break;
    snprintf(sql, sizeof sql, "UPDATE %s SET %s = ?2 WHERE %s = ?1", r->table, r->col, r->col);
    st = run(db, sql, p->loser, c, NULL);
  }
  /* What still points at the loser (nothing, unless the class is deleted) goes the way ON
   * DELETE takes it. */
  snprintf(sql, sizeof sql, "DELETE FROM %s WHERE uid = ?1", p->table);
  if (st == MC_OK) st = run(db, sql, p->loser, NULL, NULL);
  if (st == MC_OK) st = run(db, "DELETE FROM recents WHERE uid = ?1", p->loser, NULL, NULL);
  return st;
}

mc_status mc_merge_map_row(sqlite3 *db, const char *table, const char *uid) {
  mc_status st = MC_OK;
  for (size_t i = 0; st == MC_OK && i < COUNT(k_refs); i++) {
    const merge_ref *r = &k_refs[i];
    if (strcmp(r->table, table)) continue;
    char sql[300], v[MC_UID_STRLEN], c[MC_UID_STRLEN];
    snprintf(sql, sizeof sql, "SELECT %s FROM %s WHERE uid = ?1", r->col, r->table);
    if ((st = one_uid(db, sql, uid, NULL, v)) != MC_OK || !v[0]) continue;
    if ((st = canonical_of(db, v, c)) != MC_OK || !strcmp(c, v)) continue;
    if (!row_exists(db, r->parent, c, &st)) continue; /* left to the reference check */
    if (r->unique_with) {
      char other[MC_UID_STRLEN];
      snprintf(sql, sizeof sql,
               "SELECT o.uid FROM %s o JOIN %s me ON me.uid = ?1 AND o.%s = me.%s"
               " WHERE o.%s = ?2 AND o.uid <> ?1",
               r->table, r->table, r->unique_with, r->unique_with, r->col);
      if ((st = one_uid(db, sql, uid, c, other)) != MC_OK) break;
      if (other[0]) {
        snprintf(sql, sizeof sql, "DELETE FROM %s WHERE uid = ?1", r->table);
        int keep_me = strcmp(uid, other) < 0;
        if ((st = run(db, sql, keep_me ? other : uid, NULL, NULL)) != MC_OK || !keep_me) break;
      }
    }
    snprintf(sql, sizeof sql, "UPDATE %s SET %s = ?2 WHERE uid = ?1", r->table, r->col);
    st = run(db, sql, uid, c, NULL);
  }
  return st;
}

mc_status mc_merge_resolve(mc_library *lib, const char *uid, char out[MC_UID_STRLEN]) {
  if (!lib) {
    memmove(out, uid, MC_UID_STRLEN);
    return MC_OK;
  }
  return canonical_of(lib->db, uid, out);
}
