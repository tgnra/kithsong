/* The artist refresh after upgrading (refresh.h; docs/tagging-rules.md, "Refresh after
 * upgrading").
 *
 * Three steps, each resumable from a cursor in meta (the last uid it finished, or 'done'):
 *
 *   artist_refresh_mb     albums with a release id whose credits need it: one release lookup
 *                         each, their credits rewritten from the release's. Skipped while
 *                         MusicBrainz can't be reached, and tried again on a later open.
 *   artist_refresh_split  albums whose credits have a separator and aren't split yet, but
 *                         those with a release id the first step has done (all of them once
 *                         it's 'done'): the string splitter.
 *   artist_refresh_loose  the same for tracks with no album.
 *
 * Each album (or loose track) is one write transaction: its credits, the artists that leaves
 * with nothing deleted, the cursor, and in artist_refresh_files the tracks whose credit
 * changed. Their files are then retagged, and the key cleared; an interrupted retag is done
 * again first thing next time. When every cursor is 'done', artist_refresh is 'done'. */
#include "tagger/refresh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/tagger.h"
#include "tagger/credit.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "util/uid.h"

#define KEY_STATE "artist_refresh"
#define KEY_MB "artist_refresh_mb"
#define KEY_SPLIT "artist_refresh_split"
#define KEY_LOOSE "artist_refresh_loose"
#define KEY_FILES "artist_refresh_files"

#define PROBE_TIMEOUT_MS 3000
#define BUSY_WAIT_MS 5000 /* as long as mc_library_open() waits for a lock */

struct mc_refresh {
  char *dir;
  mc_mutex lock; /* guards the rest */
  mc_cond wake;  /* broadcast on stop and when the thread ends */
  mc_thread thread;
  int stop;
  int running;
  int done, total;
  mc_refresh *next; /* in g_running */
};

/* One refresh per library folder in the process: two handles on one library would otherwise
 * retag the same files at once. */
static mc_mutex g_lock;
static int g_ready;
static mc_refresh *g_running;
static uint32_t g_delay_ms = 3000;

void mc_refresh_global_init(void) {
  if (g_ready) return;
  mc_mutex_init(&g_lock);
  g_ready = 1;
}

void mc_refresh_set_delay_ms(uint32_t ms) {
  g_delay_ms = ms;
}

static int stopping(mc_refresh *r) {
  mc_mutex_lock(&r->lock);
  int s = r->stop;
  mc_mutex_unlock(&r->lock);
  return s;
}

static int http_cancelled(void *user) {
  return stopping(user);
}

/* Instead of a busy timeout, so a stop isn't held up by another connection's lock. */
static int busy(void *user, int tries) {
  if (stopping(user) || tries >= BUSY_WAIT_MS / 20) return 0;
  mc_sleep_ms(20);
  return 1;
}

static void progress(mc_refresh *r) {
  mc_mutex_lock(&r->lock);
  r->done++;
  if (r->done > r->total) r->total = r->done;
  mc_mutex_unlock(&r->lock);
}

/* mc_credit_sep(text): whether a credit has a separator (mc_credit_has_separator()). */
static void sql_credit_sep(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  sqlite3_result_int(ctx, mc_credit_has_separator((const char *)sqlite3_value_text(argv[0])));
}

/* --- Small helpers --- */

static sqlite3_stmt *prepare(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
  return stmt;
}

static mc_status step_done(sqlite3_stmt *stmt) {
  if (!stmt) return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static char *col_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *v = sqlite3_column_text(stmt, col);
  return v ? mc_strdup((const char *)v) : NULL;
}

typedef struct uids {
  char (*items)[MC_UID_STRLEN];
  int count, cap;
} uids;

static int uids_add(uids *l, const char *uid) {
  if (!uid || strlen(uid) >= MC_UID_STRLEN) return 1;
  for (int i = 0; i < l->count; i++)
    if (!strcmp(l->items[i], uid)) return 1;
  if (l->count == l->cap) {
    int grown = l->cap ? l->cap * 2 : 16;
    char (*more)[MC_UID_STRLEN] = realloc(l->items, (size_t)grown * sizeof *more);
    if (!more) return 0;
    l->items = more;
    l->cap = grown;
  }
  strcpy(l->items[l->count++], uid);
  return 1;
}

/* Adds column 0 of every row of a query with ?1 bound to arg. */
static mc_status uids_query(mc_library *lib, uids *l, const char *sql, const char *arg) {
  sqlite3_stmt *stmt = prepare(lib, sql);
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    if (!uids_add(l, (const char *)sqlite3_column_text(stmt, 0))) st = MC_ERR_NOMEM;
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* A meta value, malloc'd; NULL if it isn't there (or on failure, then *st says so). */
static char *meta_dup(mc_library *lib, const char *key, mc_status *st) {
  *st = MC_OK;
  sqlite3_stmt *stmt = prepare(lib, "SELECT value FROM meta WHERE key = ?1");
  if (!stmt) {
    *st = MC_ERR_DB;
    return NULL;
  }
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  char *v = NULL;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW && !(v = col_dup(stmt, 0)) && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
    *st = MC_ERR_NOMEM;
  else if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    *st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return v;
}

/* A cursor: "" to start, a uid, or "done". */
static mc_status cursor_get(mc_library *lib, const char *key, char out[MC_UID_STRLEN]) {
  mc_status st = mc_library_meta_get(lib, key, out, MC_UID_STRLEN);
  if (st == MC_ERR_NOT_FOUND) {
    out[0] = '\0';
    st = MC_OK;
  }
  return st;
}

/* --- What's left to do --- */

/* The artists credited on owner o of a credit table: its artist_uid and its rows. */
#define ALBUM_ARTISTS(o) \
  "SELECT " o ".artist_uid UNION SELECT artist_uid FROM album_artists WHERE album_uid = " o ".uid"
#define TRACK_ARTISTS(o) \
  "SELECT " o ".artist_uid UNION SELECT artist_uid FROM track_artists WHERE track_uid = " o ".uid"
#define LACKS_ID(artists) \
  "EXISTS (SELECT 1 FROM artists x WHERE x.uid IN (" artists ") AND coalesce(x.mb_artistid, '') = '')"
#define SPLIT(table, key, o) \
  "EXISTS (SELECT 1 FROM " table " WHERE " key " = " o ".uid AND position > 0)"

/* An album a whose credits a release lookup would put right: its artist has no MusicBrainz
 * id, or its credit or a track's (with an artist of its own, tr) has a separator and an
 * artist with no id: not split, or split by the string splitter. Those split from
 * MusicBrainz (a v13 import) are skipped. */
#define NEEDS_LOOKUP                                                                       \
  "(coalesce(ar.mb_artistid, '') = ''"                                                     \
  " OR (mc_credit_sep(coalesce(a.artist_credit, ar.name)) AND " LACKS_ID(ALBUM_ARTISTS("a")) ")" \
  " OR EXISTS (SELECT 1 FROM tracks t JOIN artists tr ON tr.uid = t.artist_uid"            \
  "  WHERE t.album_uid = a.uid AND mc_credit_sep(coalesce(t.artist_credit, tr.name))"     \
  "  AND " LACKS_ID(TRACK_ARTISTS("t")) "))"
#define MB_ALBUMS                                                                  \
  " FROM albums a LEFT JOIN artists ar ON ar.uid = a.artist_uid"                   \
  " WHERE coalesce(a.mb_albumid, '') <> '' AND a.uid > ?1 AND " NEEDS_LOOKUP

/* An album a with a credit the splitter would split: its own, or a track's (with an artist
 * of its own), with a separator and not split yet. ?2 is the first step's cursor: albums
 * with a release id up to it are its, and all of them when it's 'done'. */
#define NEEDS_SPLIT                                                                          \
  "((mc_credit_sep(coalesce(a.artist_credit, ar.name)) AND NOT " SPLIT("album_artists", "album_uid", "a") ")" \
  " OR EXISTS (SELECT 1 FROM tracks t JOIN artists tr ON tr.uid = t.artist_uid"              \
  "  WHERE t.album_uid = a.uid AND mc_credit_sep(coalesce(t.artist_credit, tr.name))"       \
  "  AND NOT " SPLIT("track_artists", "track_uid", "t") "))"
#define SPLIT_ALBUMS                                                                 \
  " FROM albums a LEFT JOIN artists ar ON ar.uid = a.artist_uid WHERE a.uid > ?1"    \
  " AND (coalesce(a.mb_albumid, '') = '' OR (?2 <> 'done' AND a.uid > ?2)) AND " NEEDS_SPLIT
#define LOOSE_TRACKS                                                                    \
  " FROM tracks t JOIN artists tr ON tr.uid = t.artist_uid"                             \
  " WHERE t.album_uid IS NULL AND t.uid > ?1"                                           \
  " AND mc_credit_sep(coalesce(t.artist_credit, tr.name))"                              \
  " AND NOT " SPLIT("track_artists", "track_uid", "t")

static int count_left(mc_library *lib, const char *from, const char *c1, const char *c2) {
  char sql[4096];
  snprintf(sql, sizeof sql, "SELECT count(*)%s", from);
  sqlite3_stmt *stmt = prepare(lib, sql);
  if (!stmt) return 0;
  sqlite3_bind_text(stmt, 1, c1, -1, SQLITE_STATIC);
  if (sqlite3_bind_parameter_count(stmt) > 1) sqlite3_bind_text(stmt, 2, c2, -1, SQLITE_STATIC);
  int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return n;
}

/* The first row of "SELECT <cols><from>" after cursor c1 (c2: a second cursor, if from
 * takes one): its uid, and with second its column 1 too. *found 0 if there's none. */
static mc_status next_after(mc_library *lib, const char *cols, const char *from, const char *c1,
                            const char *c2, char out[MC_UID_STRLEN], char second[MC_UID_STRLEN],
                            int *found) {
  char sql[4096];
  snprintf(sql, sizeof sql, "SELECT %s%s ORDER BY 1 LIMIT 1", cols, from);
  sqlite3_stmt *stmt = prepare(lib, sql);
  *found = 0;
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, c1, -1, SQLITE_STATIC);
  if (sqlite3_bind_parameter_count(stmt) > 1) sqlite3_bind_text(stmt, 2, c2, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    const char *v = second ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
    if (u && strlen(u) < MC_UID_STRLEN && (!second || (v && strlen(v) < MC_UID_STRLEN))) {
      strcpy(out, u);
      if (second) strcpy(second, v);
      *found = 1;
    }
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* --- Changing one credit --- */

/* What happened in one transaction: the tracks whose files need new tags, and the artists
 * credits named before, which may be left with nothing. */
typedef struct change {
  uids retag;
  uids old_artists;
} change;

static void change_clear(change *c) {
  free(c->retag.items);
  free(c->old_artists.items);
  memset(c, 0, sizeof *c);
}

/* Everything about owner's credit its files' tags are made of: its artists in order, with
 * their names, ids and joins, and the credit as shown. */
static char *signature(mc_library *lib, int is_album, const char *uid) {
  static const char *const sql[] = {
    "SELECT coalesce(o.artist_uid, '') || '|' || coalesce(o.artist_credit, '') || '|'"
    " || coalesce((SELECT name || '=' || coalesce(mb_artistid, '') FROM artists"
    "  WHERE uid = o.artist_uid), '') || '|' || coalesce((SELECT group_concat(v, '|') FROM ("
    "  SELECT c.artist_uid || '=' || ar.name || '=' || coalesce(ar.mb_artistid, '') || '='"
    "  || c.joinphrase AS v FROM track_artists c JOIN artists ar ON ar.uid = c.artist_uid"
    "  WHERE c.track_uid = o.uid ORDER BY c.position)), '') FROM tracks o WHERE o.uid = ?1",
    "SELECT coalesce(o.artist_uid, '') || '|' || coalesce(o.artist_credit, '') || '|'"
    " || coalesce((SELECT name || '=' || coalesce(mb_artistid, '') FROM artists"
    "  WHERE uid = o.artist_uid), '') || '|' || coalesce((SELECT group_concat(v, '|') FROM ("
    "  SELECT c.artist_uid || '=' || ar.name || '=' || coalesce(ar.mb_artistid, '') || '='"
    "  || c.joinphrase AS v FROM album_artists c JOIN artists ar ON ar.uid = c.artist_uid"
    "  WHERE c.album_uid = o.uid ORDER BY c.position)), '') FROM albums o WHERE o.uid = ?1",
  };
  sqlite3_stmt *stmt = prepare(lib, sql[is_album ? 1 : 0]);
  if (!stmt) return NULL;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  char *s = sqlite3_step(stmt) == SQLITE_ROW ? col_dup(stmt, 0) : NULL;
  sqlite3_finalize(stmt);
  return s;
}

/* Replaces owner's credit (as mc_import_db_set_credit_locked()), noting in c what changed. */
static mc_status set_owner(mc_library *lib, int is_album, const char *uid, const mc_db_artist *whole,
                           const mc_db_credit_part *parts, int n, change *c) {
  char *before = signature(lib, is_album, uid);
  if (!before) return MC_ERR_DB;
  mc_status st = uids_query(lib, &c->old_artists,
                            is_album ? "SELECT artist_uid FROM albums WHERE uid = ?1"
                                       " UNION SELECT artist_uid FROM album_artists WHERE album_uid = ?1"
                                     : "SELECT artist_uid FROM tracks WHERE uid = ?1"
                                       " UNION SELECT artist_uid FROM track_artists WHERE track_uid = ?1",
                            uid);
  if (st == MC_OK) st = mc_import_db_set_credit_locked(lib, is_album, uid, whole, parts, n);
  char *after = st == MC_OK ? signature(lib, is_album, uid) : NULL;
  if (st == MC_OK && !after) st = MC_ERR_DB;
  if (st == MC_OK && strcmp(before, after) != 0) {
    /* An album's credit is in every one of its tracks' files (ALBUMARTISTS). */
    if (is_album)
      st = uids_query(lib, &c->retag, "SELECT uid FROM tracks WHERE album_uid = ?1", uid);
    else if (!uids_add(&c->retag, uid))
      st = MC_ERR_NOMEM;
  }
  free(before);
  free(after);
  return st;
}

/* A new artist the splitter names gets a uid derived from the name, so two devices
 * splitting the same credit make one row, not two that never merge (they've no MusicBrainz
 * id to merge by). An artist of that name already here is used as it is. */
static mc_status ensure_artist(mc_library *lib, const char *name) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM artists WHERE name = ?1 COLLATE NOCASE");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW) return MC_OK;
  if (rc != SQLITE_DONE) return MC_ERR_DB;
  size_t n = strlen(name);
  char *key = malloc(n + 8);
  if (!key) return MC_ERR_NOMEM;
  memcpy(key, "artist/", 7);
  for (size_t i = 0; i <= n; i++) {
    char ch = name[i];
    key[7 + i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
  }
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_derive(key, 0, uid);
  free(key);
  if (st != MC_OK) return st;
  /* Taken by an artist renamed since: the import upserts one of its own instead. */
  stmt = prepare(lib, "INSERT OR IGNORE INTO artists (uid, name) VALUES (?1, ?2)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
  return step_done(stmt);
}

/* Splits owner's credit text with the splitter, if it has a separator, isn't split yet
 * (one split from MusicBrainz names artists as credited, which the string can't), and
 * splits into more than one artist. */
static mc_status split_owner(mc_library *lib, int is_album, const char *uid, change *c) {
  static const char *const sql[] = {
    "SELECT coalesce(o.artist_credit, ar.name), " SPLIT("track_artists", "track_uid", "o")
    " FROM tracks o JOIN artists ar ON ar.uid = o.artist_uid WHERE o.uid = ?1",
    "SELECT coalesce(o.artist_credit, ar.name), " SPLIT("album_artists", "album_uid", "o")
    " FROM albums o JOIN artists ar ON ar.uid = o.artist_uid WHERE o.uid = ?1",
  };
  sqlite3_stmt *stmt = prepare(lib, sql[is_album ? 1 : 0]);
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  char *text = NULL;
  int split = 1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    text = col_dup(stmt, 0);
    split = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  if (!text || split || !mc_credit_has_separator(text)) {
    free(text);
    return MC_OK;
  }
  mc_credit_split sp = {0};
  mc_status st = mc_credit_split_text(text, mc_credit_known_in_library, lib, &sp);
  mc_db_credit_part *parts = st == MC_OK && sp.count > 1 ? calloc((size_t)sp.count, sizeof *parts) : NULL;
  if (st == MC_OK && sp.count > 1 && !parts) st = MC_ERR_NOMEM;
  for (int i = 0; parts && st == MC_OK && i < sp.count; i++) {
    parts[i].artist.name = sp.parts[i].name;
    parts[i].join = sp.parts[i].join;
    st = ensure_artist(lib, sp.parts[i].name);
  }
  if (parts && st == MC_OK) {
    mc_db_artist whole = {text, NULL, NULL};
    st = set_owner(lib, is_album, uid, &whole, parts, sp.count, c);
  }
  free(parts);
  mc_credit_split_clear(&sp);
  free(text);
  return st;
}

/* Deletes the artists c names that no track, album or credit row names any more, as
 * delete.c does (logged, so the deletes sync): old rows of whole credits ("A feat. B") go
 * once nothing is credited to them. */
static mc_status delete_empty_artists(mc_library *lib, const change *c) {
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < c->old_artists.count; i++) {
    sqlite3_stmt *stmt = prepare(lib, "DELETE FROM artists WHERE uid = ?1"
                                      " AND NOT EXISTS (SELECT 1 FROM tracks WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM albums WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM track_artists WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM album_artists WHERE artist_uid = ?1)");
    if (stmt) sqlite3_bind_text(stmt, 1, c->old_artists.items[i], -1, SQLITE_STATIC);
    st = step_done(stmt);
  }
  return st;
}

/* Ends a transaction that changed c: empty artists deleted, the cursor moved to uid, the
 * tracks to retag saved, committed. Rolls back on failure. */
static mc_status finish(mc_library *lib, const change *c, const char *cursor_key, const char *uid) {
  mc_status st = delete_empty_artists(lib, c);
  if (st == MC_OK) st = mc_library_meta_set(lib, cursor_key, uid);
  if (st == MC_OK && c->retag.count) {
    char *list = malloc((size_t)c->retag.count * MC_UID_STRLEN);
    if (!list) st = MC_ERR_NOMEM;
    for (int i = 0; list && i < c->retag.count; i++) {
      if (i) strcat(list, "\n");
      else list[0] = '\0';
      strcat(list, c->retag.items[i]);
    }
    if (list) st = mc_library_meta_set(lib, KEY_FILES, list);
    free(list);
  }
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* The splitter over album uid and its tracks with artists of their own. */
static mc_status split_album(mc_library *lib, const char *uid, const char *cursor_key) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  change c = {0};
  uids tracks = {0};
  st = split_owner(lib, 1, uid, &c);
  if (st == MC_OK)
    st = uids_query(lib, &tracks,
                    "SELECT uid FROM tracks WHERE album_uid = ?1 AND artist_uid IS NOT NULL", uid);
  for (int i = 0; st == MC_OK && i < tracks.count; i++) st = split_owner(lib, 0, tracks.items[i], &c);
  if (st == MC_OK) st = finish(lib, &c, cursor_key, uid);
  else mc_library_rollback(lib);
  free(tracks.items);
  change_clear(&c);
  return st;
}

static mc_status split_loose(mc_library *lib, const char *uid) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  change c = {0};
  st = split_owner(lib, 0, uid, &c);
  if (st == MC_OK) st = finish(lib, &c, KEY_LOOSE, uid);
  else mc_library_rollback(lib);
  change_clear(&c);
  return st;
}

/* A MusicBrainz credit as rows, as an import passes it. NULL with no parts. */
static mc_db_credit_part *mb_parts(const mb_credit *credit, mc_status *st) {
  *st = MC_OK;
  if (!credit->count) return NULL;
  mc_db_credit_part *parts = calloc((size_t)credit->count, sizeof *parts);
  if (!parts) {
    *st = MC_ERR_NOMEM;
    return NULL;
  }
  for (int i = 0; i < credit->count; i++)
    parts[i] = (mc_db_credit_part){{credit->parts[i].name, credit->parts[i].sort_name,
                                    credit->parts[i].id},
                                   credit->parts[i].join};
  return parts;
}

/* One of the album's tracks, to pair with the release's. */
typedef struct lib_track {
  char uid[MC_UID_STRLEN];
  char *releasetrackid;
  int disc, number;
} lib_track;

static const mb_track *pair_track(const mb_release *rel, const lib_track *t) {
  if (t->releasetrackid)
    for (int i = 0; i < rel->track_count; i++)
      if (rel->tracks[i].id && !strcmp(rel->tracks[i].id, t->releasetrackid)) return &rel->tracks[i];
  if (!t->number) return NULL;
  for (int i = 0; i < rel->track_count; i++) {
    const mb_track *tr = &rel->tracks[i];
    if ((tr->disc ? tr->disc : 1) == t->disc && tr->number == t->number) return tr;
  }
  return NULL;
}

/* Album uid's credit, and its tracks' (those with an artist of their own, paired by release
 * track id, else disc and number), from release rel, which is its mb_albumid (mbid). */
static mc_status apply_release(mc_library *lib, const char *uid, const char *mbid, const mb_release *rel) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  change c = {0};
  lib_track *tracks = NULL;
  int ntracks = 0;
  /* An import may have filed the album under another release meanwhile. */
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM albums WHERE uid = ?1 AND mb_albumid = ?2");
  if (!stmt) st = MC_ERR_DB;
  int same = 0;
  if (stmt) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mbid, -1, SQLITE_STATIC);
    same = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK && same && (rel->artist || rel->credit.count)) {
    mc_db_credit_part *parts = mb_parts(&rel->credit, &st);
    mc_db_artist whole = {rel->artist, rel->artist_sort, rel->artist_id};
    if (st == MC_OK) st = set_owner(lib, 1, uid, &whole, parts, parts ? rel->credit.count : 0, &c);
    free(parts);
  }
  if (st == MC_OK && same) {
    stmt = prepare(lib, "SELECT uid, mb_releasetrackid, coalesce(disc_no, 1), coalesce(track_no, 0)"
                        " FROM tracks WHERE album_uid = ?1 AND artist_uid IS NOT NULL");
    if (!stmt) st = MC_ERR_DB;
    else sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
      lib_track *more = realloc(tracks, (size_t)(ntracks + 1) * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      tracks = more;
      lib_track *t = &tracks[ntracks++];
      memset(t, 0, sizeof *t);
      const char *u = (const char *)sqlite3_column_text(stmt, 0);
      if (u && strlen(u) < MC_UID_STRLEN) strcpy(t->uid, u);
      t->releasetrackid = col_dup(stmt, 1);
      t->disc = sqlite3_column_int(stmt, 2);
      t->number = sqlite3_column_int(stmt, 3);
    }
    sqlite3_finalize(stmt);
  }
  for (int i = 0; st == MC_OK && i < ntracks; i++) {
    if (!tracks[i].uid[0]) continue;
    const mb_track *tr = pair_track(rel, &tracks[i]);
    if (!tr || (!tr->artist && !tr->credit.count)) {
      st = split_owner(lib, 0, tracks[i].uid, &c); /* not on the release: the string it is */
      continue;
    }
    mc_db_credit_part *parts = mb_parts(&tr->credit, &st);
    mc_db_artist whole = {tr->artist, NULL, tr->artist_id};
    if (st == MC_OK) st = set_owner(lib, 0, tracks[i].uid, &whole, parts, parts ? tr->credit.count : 0, &c);
    free(parts);
  }
  if (st == MC_OK) st = finish(lib, &c, KEY_MB, uid);
  else mc_library_rollback(lib);
  for (int i = 0; i < ntracks; i++) free(tracks[i].releasetrackid);
  free(tracks);
  change_clear(&c);
  return st;
}

/* --- Retagging files --- */

/* The tags a credit change touches, for track uid: ARTIST and ALBUMARTIST (the credits as
 * shown), ARTISTS and ALBUMARTISTS, and their ids when every artist has one. The album tags
 * of a track with no album are left alone. */
static mc_status credit_tags(mc_library *lib, const char *uid, mc_tags *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt =
    prepare(lib, "SELECT coalesce(t.artist_credit, ta.name, al.artist_credit, aa.name), al.uid,"
                 " coalesce(al.artist_credit, aa.name)"
                 " FROM tracks t LEFT JOIN artists ta ON ta.uid = t.artist_uid"
                 " LEFT JOIN albums al ON al.uid = t.album_uid"
                 " LEFT JOIN artists aa ON aa.uid = al.artist_uid WHERE t.uid = ?1");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return MC_ERR_NOT_FOUND;
  }
  out->artist = col_dup(stmt, 0);
  char *album = col_dup(stmt, 1);
  out->album_artist = album ? col_dup(stmt, 2) : NULL;
  sqlite3_finalize(stmt);
  mc_status st = mc_dupes_credit_tags(lib, uid, &out->artists, &out->mb_artistids);
  if (st == MC_OK && album)
    st = mc_dupes_credit_tags(lib, album, &out->album_artists, &out->mb_albumartistids);
  free(album);
  if (st != MC_OK) mc_tags_clear(out);
  return st;
}

typedef struct file_row {
  char uid[MC_UID_STRLEN];
  char *path;
  int64_t size, mtime;
  int has_mtime, has_sha;
} file_row;

/* "<dir>/.kithsong-<random>-<name>", beside abs, keeping its extension for TagLib. */
static char *temp_beside(const char *abs) {
  const char *name = abs + strlen(abs);
  while (name > abs && name[-1] != '/' && name[-1] != '\\') name--;
  unsigned char rnd[4];
  if (mc_random_bytes(rnd, sizeof rnd) != MC_OK) return NULL;
  size_t n = strlen(abs) + 32;
  char *tmp = malloc(n);
  if (tmp)
    snprintf(tmp, n, "%.*s.kithsong-%02x%02x%02x%02x-%s", (int)(name - abs), abs, rnd[0], rnd[1],
             rnd[2], rnd[3], name);
  return tmp;
}

/* Rewrites one file's tags in place, as far as anyone can tell: a tagged copy is made beside
 * it and renamed over it, so a reader (sync sending it, the player) sees either the old
 * bytes or the new, and a crash leaves the old file whole. Its files row follows (size,
 * mtime, and sha256 when it had one; source_sha256 stays, being what it was imported from),
 * in the same transaction as the rename, so sync and the import's duplicate check never see
 * a changed or unknown file. Same path, same file_rev. A file that's missing, read-only, or
 * not what its row says (changed outside the library, or by an import just now) is left as
 * it is. */
static void retag_file(mc_library *lib, const file_row *f, const mc_tags *tags) {
  char *abs = mc_library_abspath(lib, f->path);
  char *tmp = NULL;
  int64_t size, mtime, new_size, new_mtime;
  char sha[MC_SHA256_STRLEN] = "";
  int renamed = 0;
  FILE *probe;
  if (!abs || mc_file_info(abs, &size, &mtime) != MC_OK || size != f->size ||
      (f->has_mtime && mtime != f->mtime))
    goto done;
  if (!(probe = mc_fopen(abs, "r+b"))) goto done; /* read-only */
  fclose(probe);
  if (!(tmp = temp_beside(abs)) || mc_copy_file(abs, tmp, 1) != MC_OK) goto done;
  if (mc_tags_write(tmp, tags) != MC_OK || mc_file_info(tmp, &new_size, &new_mtime) != MC_OK ||
      (f->has_sha && mc_sha256_file(tmp, sha) != MC_OK))
    goto done;
  if (mc_library_begin_write(lib) != MC_OK) goto done;
  sqlite3_stmt *stmt = prepare(lib, "UPDATE files SET size_bytes = ?2, mtime_ms = ?3,"
                                    " sha256 = CASE WHEN sha256 IS NULL THEN NULL ELSE ?4 END"
                                    " WHERE uid = ?1 AND path = ?5 AND size_bytes = ?6"
                                    " AND (mtime_ms IS NULL OR mtime_ms = ?7)");
  if (stmt) {
    sqlite3_bind_text(stmt, 1, f->uid, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, new_size);
    sqlite3_bind_int64(stmt, 3, new_mtime);
    sqlite3_bind_text(stmt, 4, sha, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, f->path, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, f->size);
    sqlite3_bind_int64(stmt, 7, f->mtime);
  }
  int64_t now_size, now_mtime;
  int ok = step_done(stmt) == MC_OK && sqlite3_changes(mc_library_db(lib)) == 1 &&
           mc_file_info(abs, &now_size, &now_mtime) == MC_OK && now_size == size && now_mtime == mtime &&
           mc_rename(tmp, abs, 1) == MC_OK;
  if (ok && mc_library_commit(lib) == MC_OK) {
    renamed = 1;
  } else {
    if (ok) fprintf(stderr, "musiccore: artist refresh: couldn't record %s's new tags\n", abs);
    mc_library_rollback(lib);
  }
done:
  if (tmp && !renamed) mc_remove(tmp);
  free(tmp);
  free(abs);
}

static mc_status retag_track(mc_library *lib, const char *uid) {
  mc_tags tags;
  mc_status st = credit_tags(lib, uid, &tags);
  if (st == MC_ERR_NOT_FOUND) return MC_OK; /* deleted meanwhile */
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt = prepare(lib, "SELECT uid, path, size_bytes, mtime_ms, mtime_ms IS NOT NULL,"
                                    " sha256 IS NOT NULL FROM files WHERE track_uid = ?1");
  file_row *rows = NULL;
  int n = 0;
  if (!stmt) st = MC_ERR_DB;
  else sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    file_row *more = realloc(rows, (size_t)(n + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    rows = more;
    file_row *f = &rows[n];
    memset(f, 0, sizeof *f);
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    if (!u || strlen(u) >= MC_UID_STRLEN || !(f->path = col_dup(stmt, 1))) continue;
    strcpy(f->uid, u);
    f->size = sqlite3_column_int64(stmt, 2);
    f->mtime = sqlite3_column_int64(stmt, 3);
    f->has_mtime = sqlite3_column_int(stmt, 4);
    f->has_sha = sqlite3_column_int(stmt, 5);
    n++;
  }
  sqlite3_finalize(stmt);
  for (int i = 0; st == MC_OK && i < n; i++) retag_file(lib, &rows[i], &tags);
  for (int i = 0; i < n; i++) free(rows[i].path);
  free(rows);
  mc_tags_clear(&tags);
  return st;
}

/* The tracks a finished transaction listed in artist_refresh_files. */
static mc_status retag_pending(mc_refresh *r, mc_library *lib) {
  mc_status st;
  char *list = meta_dup(lib, KEY_FILES, &st);
  if (!list) return st;
  for (char *p = list; *p && st == MC_OK;) {
    if (stopping(r)) {
      free(list);
      return MC_OK; /* done again from the start next time; tagging twice is harmless */
    }
    char *nl = strchr(p, '\n');
    if (nl) *nl = '\0';
    st = retag_track(lib, p);
    p = nl ? nl + 1 : p + strlen(p);
  }
  free(list);
  return st == MC_OK ? mc_library_meta_set(lib, KEY_FILES, NULL) : st;
}

/* --- The steps --- */

/* Albums looked up on MusicBrainz, from cursor on. *offline is set if it couldn't be
 * reached, and the rest wait for a later open. */
static mc_status step_mb(mc_refresh *r, mc_library *lib, char cursor[MC_UID_STRLEN], int *offline) {
  int probed = 0;
  mc_http_cancel cancel = {http_cancelled, r};
  for (;;) {
    if (stopping(r)) return MC_OK;
    char uid[MC_UID_STRLEN], mbid[MC_UID_STRLEN];
    int found;
    mc_status st = next_after(lib, "a.uid, a.mb_albumid", MB_ALBUMS, cursor, NULL, uid, mbid, &found);
    if (st != MC_OK) return st;
    if (!found) {
      if ((st = mc_library_meta_set(lib, KEY_MB, "done")) == MC_OK) strcpy(cursor, "done");
      return st;
    }
    if (!probed) {
      probed = 1;
      if (mc_musicbrainz_reachable(PROBE_TIMEOUT_MS) != MC_OK) {
        *offline = 1;
        return MC_OK;
      }
    }
    mb_release rel;
    st = mb_lookup_release_cancel(mbid, &rel, &cancel);
    if (st == MC_ERR_NET) {
      mb_release_clear(&rel);
      if (!stopping(r)) *offline = 1;
      return MC_OK;
    }
    if (st == MC_ERR_NOMEM) {
      mb_release_clear(&rel);
      return st;
    }
    /* Not on MusicBrainz (any more), or an answer that can't be read: the splitter. */
    st = st == MC_OK ? apply_release(lib, uid, mbid, &rel) : split_album(lib, uid, KEY_MB);
    mb_release_clear(&rel);
    if (st != MC_OK) return st;
    strcpy(cursor, uid);
    progress(r);
    if ((st = retag_pending(r, lib)) != MC_OK) return st;
  }
}

/* The splitter, from cursor on: albums (mb is the first step's cursor), or tracks with no
 * album. */
static mc_status step_split(mc_refresh *r, mc_library *lib, int loose, char cursor[MC_UID_STRLEN],
                            const char *mb) {
  for (;;) {
    if (stopping(r)) return MC_OK;
    char uid[MC_UID_STRLEN];
    int found;
    mc_status st = next_after(lib, loose ? "t.uid" : "a.uid", loose ? LOOSE_TRACKS : SPLIT_ALBUMS, cursor,
                              mb, uid, NULL, &found);
    if (st != MC_OK) return st;
    const char *key = loose ? KEY_LOOSE : KEY_SPLIT;
    if (!found) {
      if ((st = mc_library_meta_set(lib, key, "done")) == MC_OK) strcpy(cursor, "done");
      return st;
    }
    if ((st = loose ? split_loose(lib, uid) : split_album(lib, uid, key)) != MC_OK) return st;
    strcpy(cursor, uid);
    progress(r);
    if ((st = retag_pending(r, lib)) != MC_OK) return st;
  }
}

static mc_status refresh(mc_refresh *r, mc_library *lib) {
  char state[16];
  if (mc_library_meta_get(lib, KEY_STATE, state, sizeof state) != MC_OK || strcmp(state, "pending"))
    return MC_OK;
  char mb[MC_UID_STRLEN], split[MC_UID_STRLEN], loose[MC_UID_STRLEN];
  mc_status st = retag_pending(r, lib);
  if (st == MC_OK) st = cursor_get(lib, KEY_MB, mb);
  if (st == MC_OK) st = cursor_get(lib, KEY_SPLIT, split);
  if (st == MC_OK) st = cursor_get(lib, KEY_LOOSE, loose);
  if (st != MC_OK) return st;

  /* Albums with a release id count once, as the first step's (online, as is likely). */
  int total = (strcmp(mb, "done") ? count_left(lib, MB_ALBUMS, mb, NULL) : 0) +
              (strcmp(split, "done") ? count_left(lib, SPLIT_ALBUMS, split, "done") : 0) +
              (strcmp(loose, "done") ? count_left(lib, LOOSE_TRACKS, loose, NULL) : 0);
  mc_mutex_lock(&r->lock);
  r->total = total;
  mc_mutex_unlock(&r->lock);

  int offline = 0;
  if (strcmp(mb, "done")) st = step_mb(r, lib, mb, &offline);
  /* Offline, the splitter does the albums the first step hasn't reached too; it looks them
   * up when it can. */
  if (st == MC_OK && strcmp(split, "done")) st = step_split(r, lib, 0, split, mb);
  if (st == MC_OK && strcmp(loose, "done")) st = step_split(r, lib, 1, loose, NULL);
  if (st != MC_OK || stopping(r)) return st;
  if (strcmp(mb, "done") || strcmp(split, "done") || strcmp(loose, "done")) {
    if (offline) fprintf(stderr, "musiccore: artist refresh: MusicBrainz unreachable; trying again next time\n");
    return MC_OK;
  }
  if ((st = mc_library_meta_set(lib, KEY_STATE, "done")) == MC_OK) {
    mc_library_meta_set(lib, KEY_MB, NULL);
    mc_library_meta_set(lib, KEY_SPLIT, NULL);
    mc_library_meta_set(lib, KEY_LOOSE, NULL);
  }
  return st;
}

/* --- The thread --- */

static void run(void *arg) {
  mc_refresh *r = arg;
  mc_mutex_lock(&r->lock);
  uint64_t until = mc_mono_ms() + g_delay_ms;
  for (uint64_t now; !r->stop && (now = mc_mono_ms()) < until;)
    mc_cond_wait_ms(&r->wake, &r->lock, (uint32_t)(until - now));
  int stop = r->stop;
  mc_mutex_unlock(&r->lock);

  mc_library *lib = NULL;
  if (!stop && mc_library_open_internal(r->dir, &lib) == MC_OK) {
    sqlite3 *db = mc_library_db(lib);
    sqlite3_busy_handler(db, busy, r);
    mc_status st = sqlite3_create_function(db, "mc_credit_sep", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                                           NULL, sql_credit_sep, NULL, NULL) == SQLITE_OK
                     ? refresh(r, lib)
                     : MC_ERR_DB;
    if (st != MC_OK && !stopping(r))
      fprintf(stderr, "musiccore: artist refresh stopped (%s); it goes on next time\n",
              sqlite3_errmsg(db));
  }
  mc_library_close(lib);

  mc_mutex_lock(&r->lock);
  r->running = 0;
  mc_cond_broadcast(&r->wake);
  mc_mutex_unlock(&r->lock);
}

mc_refresh *mc_refresh_start(mc_library *lib) {
  char state[16];
  if (!g_ready || mc_library_meta_get(lib, KEY_STATE, state, sizeof state) != MC_OK ||
      strcmp(state, "pending"))
    return NULL;
  mc_refresh *r = calloc(1, sizeof *r);
  if (!r || !(r->dir = mc_strdup(mc_library_dir(lib)))) {
    free(r);
    return NULL;
  }
  mc_mutex_init(&r->lock);
  mc_cond_init(&r->wake);
  r->running = 1;

  mc_mutex_lock(&g_lock);
  int taken = 0;
  for (mc_refresh *o = g_running; o && !taken; o = o->next) taken = mc_same_path(o->dir, r->dir);
  if (!taken && mc_thread_start(&r->thread, run, r) == MC_OK) {
    r->next = g_running;
    g_running = r;
  } else {
    taken = 1;
  }
  mc_mutex_unlock(&g_lock);
  if (taken) {
    mc_cond_destroy(&r->wake);
    mc_mutex_destroy(&r->lock);
    free(r->dir);
    free(r);
    return NULL;
  }
  return r;
}

void mc_refresh_stop(mc_refresh *r) {
  if (!r) return;
  mc_mutex_lock(&r->lock);
  r->stop = 1;
  mc_cond_broadcast(&r->wake);
  mc_mutex_unlock(&r->lock);
  mc_thread_join(&r->thread);

  mc_mutex_lock(&g_lock);
  for (mc_refresh **p = &g_running; *p; p = &(*p)->next)
    if (*p == r) {
      *p = r->next;
      break;
    }
  mc_mutex_unlock(&g_lock);
  mc_cond_destroy(&r->wake);
  mc_mutex_destroy(&r->lock);
  free(r->dir);
  free(r);
}

int mc_refresh_progress(mc_refresh *r, int *done, int *total) {
  int running = 0, d = 0, t = 0;
  if (r) {
    mc_mutex_lock(&r->lock);
    running = r->running;
    d = r->done;
    t = r->total;
    mc_mutex_unlock(&r->lock);
  }
  if (done) *done = running ? d : 0;
  if (total) *total = running ? t : 0;
  return running;
}

void mc_refresh_wait(mc_library *lib) {
  mc_refresh *r = lib ? lib->refresh : NULL;
  if (!r) return;
  mc_mutex_lock(&r->lock);
  while (r->running) mc_cond_wait(&r->wake, &r->lock);
  mc_mutex_unlock(&r->lock);
}
