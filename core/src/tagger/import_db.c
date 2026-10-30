#include "tagger/import_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "util/platform.h"
#include "util/uid.h"

static void bind_text(sqlite3_stmt *stmt, int i, const char *s) {
  if (s && *s) sqlite3_bind_text(stmt, i, s, -1, SQLITE_STATIC);
  else sqlite3_bind_null(stmt, i);
}

/* 0 means unknown. */
static void bind_int(sqlite3_stmt *stmt, int i, int64_t v) {
  if (v) sqlite3_bind_int64(stmt, i, v);
  else sqlite3_bind_null(stmt, i);
}

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
  return stmt;
}

/* Steps a statement expected to return at most one row with a uid in column 0. */
static int select_uid(sqlite3_stmt *stmt, char out[MC_UID_STRLEN]) {
  int found = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && strlen((const char *)v) < MC_UID_STRLEN) {
      strcpy(out, (const char *)v);
      found = 1;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

static mc_status step_done(sqlite3_stmt *stmt) {
  if (!stmt) return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static mc_status upsert_artist(sqlite3 *db, const mc_db_artist *a, char uid[MC_UID_STRLEN]) {
  const char *name = a->name && *a->name ? a->name : "Unknown Artist";
  sqlite3_stmt *stmt;

  int found = 0;
  if (a->mb_artistid) {
    if (!(stmt = prepare(db, "SELECT uid FROM artists WHERE mb_artistid = ?1"))) return MC_ERR_DB;
    bind_text(stmt, 1, a->mb_artistid);
    found = select_uid(stmt, uid);
  }
  if (!found) {
    /* Same name, unless both sides have MusicBrainz ids and they differ. */
    if (!(stmt = prepare(db, "SELECT uid FROM artists WHERE name = ?1 COLLATE NOCASE"
                             " AND (mb_artistid IS NULL OR ?2 IS NULL) LIMIT 1")))
      return MC_ERR_DB;
    bind_text(stmt, 1, name);
    bind_text(stmt, 2, a->mb_artistid);
    found = select_uid(stmt, uid);
  }

  if (found) {
    /* Fill in what MusicBrainz knows; never erase what's there. */
    stmt = prepare(db, "UPDATE artists SET mb_artistid = coalesce(?2, mb_artistid),"
                       " sort_name = coalesce(?3, sort_name) WHERE uid = ?1");
    if (!stmt) return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    bind_text(stmt, 2, a->mb_artistid);
    bind_text(stmt, 3, a->sort_name);
    return step_done(stmt);
  }

  mc_status st = mc_uid_new(uid);
  if (st != MC_OK) return st;
  stmt = prepare(db, "INSERT INTO artists (uid, name, sort_name, mb_artistid) VALUES (?1, ?2, ?3, ?4)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  bind_text(stmt, 2, name);
  bind_text(stmt, 3, a->sort_name);
  bind_text(stmt, 4, a->mb_artistid);
  return step_done(stmt);
}

/* --- Artist credits (docs/schema.md) --- */

/* A credit resolved to library rows: each artist's uid and the join phrase after it, and
 * artist_credit (NULL for the first artist's own name). */
typedef struct credit_rows {
  char (*uids)[MC_UID_STRLEN];
  char **joins;
  int count;
  char *text;
} credit_rows;

static void credit_clear(credit_rows *c) {
  for (int i = 0; i < c->count; i++) free(c->joins[i]);
  free(c->uids);
  free(c->joins);
  free(c->text);
  memset(c, 0, sizeof *c);
}

static mc_status credit_alloc(credit_rows *c, int n) {
  memset(c, 0, sizeof *c);
  c->uids = calloc((size_t)(n ? n : 1), sizeof *c->uids);
  c->joins = calloc((size_t)(n ? n : 1), sizeof *c->joins);
  if (!c->uids || !c->joins) {
    credit_clear(c);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

/* Upserts each artist of a credit (whole, or the n parts), as upsert_artist() does one. With
 * no parts the credit is whole's artist alone. */
static mc_status resolve_credit(sqlite3 *db, const mc_db_artist *whole,
                                const mc_db_credit_part *parts, int n, credit_rows *out) {
  mc_status st = credit_alloc(out, n);
  if (st != MC_OK) return st;
  if (n <= 0 || !parts) {
    out->count = 1;
    if ((st = upsert_artist(db, whole, out->uids[0])) != MC_OK) credit_clear(out);
    return st;
  }
  size_t len = 1;
  for (int i = 0; i < n; i++) {
    const char *name = parts[i].artist.name && *parts[i].artist.name ? parts[i].artist.name : "Unknown Artist";
    const char *join = i < n - 1 && parts[i].join ? parts[i].join : "";
    len += strlen(name) + strlen(join);
    out->joins[out->count] = mc_strdup(join);
    if (!out->joins[out->count]) st = MC_ERR_NOMEM;
    out->count++;
    if (st == MC_OK) st = upsert_artist(db, &parts[i].artist, out->uids[i]);
    if (st != MC_OK) {
      credit_clear(out);
      return st;
    }
  }
  /* The credit as credited, else as its parts spell it; a lone artist under its own name is
   * just that artist. */
  if (whole && whole->name && *whole->name) {
    out->text = mc_strdup(whole->name);
  } else if ((out->text = malloc(len))) {
    out->text[0] = '\0';
    for (int i = 0; i < n; i++) {
      strcat(out->text, parts[i].artist.name && *parts[i].artist.name ? parts[i].artist.name : "Unknown Artist");
      strcat(out->text, out->joins[i]);
    }
  }
  if (!out->text) {
    credit_clear(out);
    return MC_ERR_NOMEM;
  }
  if (n == 1 && parts[0].artist.name && !strcmp(out->text, parts[0].artist.name)) {
    free(out->text);
    out->text = NULL;
  }
  return MC_OK;
}

/* Reads album_uid's credit as it is, for tracks that fall back to it. Its first artist is
 * artist_uid, as listings read it. 0 rows if it has no artist. */
static mc_status read_album_credit(sqlite3 *db, const char *album_uid, credit_rows *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt =
    prepare(db, "SELECT (SELECT count(*) FROM album_artists WHERE album_uid = ?1 AND position > 0),"
                " artist_credit FROM albums WHERE uid = ?1 AND artist_uid IS NOT NULL");
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, album_uid);
  int extra = -1;
  char *text = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    extra = sqlite3_column_int(stmt, 0);
    const char *v = (const char *)sqlite3_column_text(stmt, 1);
    text = v ? mc_strdup(v) : NULL;
  }
  sqlite3_finalize(stmt);
  if (extra < 0) return credit_alloc(out, 0);
  mc_status st = credit_alloc(out, extra + 1);
  if (st != MC_OK) {
    free(text);
    return st;
  }
  out->text = text;
  stmt = prepare(db, "SELECT artist_uid, joinphrase FROM ("
                     " SELECT 0 AS position, artist_uid, coalesce((SELECT joinphrase FROM album_artists"
                     "  WHERE album_uid = ?1 AND position = 0), '') AS joinphrase FROM albums WHERE uid = ?1"
                     " UNION ALL SELECT position, artist_uid, joinphrase FROM album_artists"
                     "  WHERE album_uid = ?1 AND position > 0) ORDER BY position");
  if (!stmt) {
    credit_clear(out);
    return MC_ERR_DB;
  }
  bind_text(stmt, 1, album_uid);
  while (st == MC_OK && out->count <= extra && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    const char *j = (const char *)sqlite3_column_text(stmt, 1);
    if (!u || strlen(u) >= MC_UID_STRLEN) continue;
    strcpy(out->uids[out->count], u);
    if (!(out->joins[out->count++] = mc_strdup(j ? j : ""))) st = MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) credit_clear(out);
  return st;
}

/* Writes owner's artist_credit and its credit rows (table "tracks" or "albums"), each row's
 * uid derived from owner and position, and deletes the rows past the credit's end. The
 * caller has set owner's artist_uid to the first artist. */
static mc_status write_credit(sqlite3 *db, const char *table, const char *owner, const credit_rows *c) {
  int album = !strcmp(table, "albums");
  char sql[320];
  snprintf(sql, sizeof sql, "UPDATE %s SET artist_credit = ?2 WHERE uid = ?1", table);
  sqlite3_stmt *stmt = prepare(db, sql);
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, owner);
  bind_text(stmt, 2, c->text);
  mc_status st = step_done(stmt);
  const char *rows = album ? "album_artists" : "track_artists";
  const char *key = album ? "album_uid" : "track_uid";
  for (int i = 0; st == MC_OK && i < c->count; i++) {
    char uid[MC_UID_STRLEN];
    if ((st = mc_uid_derive(owner, i, uid)) != MC_OK) break;
    snprintf(sql, sizeof sql,
             "INSERT INTO %s (uid, %s, artist_uid, position, joinphrase) VALUES (?1, ?2, ?3, ?4, ?5)"
             " ON CONFLICT (uid) DO UPDATE SET %s = excluded.%s, artist_uid = excluded.artist_uid,"
             " position = excluded.position, joinphrase = excluded.joinphrase",
             rows, key, key, key);
    if (!(stmt = prepare(db, sql))) return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    bind_text(stmt, 2, owner);
    sqlite3_bind_text(stmt, 3, c->uids[i], -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, i);
    sqlite3_bind_text(stmt, 5, c->joins[i] ? c->joins[i] : "", -1, SQLITE_STATIC);
    st = step_done(stmt);
  }
  if (st != MC_OK) return st;
  snprintf(sql, sizeof sql, "DELETE FROM %s WHERE %s = ?1 AND position >= ?2", rows, key);
  if (!(stmt = prepare(db, sql))) return MC_ERR_DB;
  bind_text(stmt, 1, owner);
  sqlite3_bind_int(stmt, 2, c->count);
  return step_done(stmt);
}

static mc_status upsert_album(sqlite3 *db, const mc_db_album *al, const char *artist_uid,
                              char uid[MC_UID_STRLEN]) {
  if (al->existing_uid) {
    if (strlen(al->existing_uid) >= MC_UID_STRLEN) return MC_ERR_INVALID_ARG;
    strcpy(uid, al->existing_uid);
  }
  if (al->existing_uid && !al->update_existing) {
    if (!al->art_hash) return MC_OK;
    sqlite3_stmt *stmt =
      prepare(db, "UPDATE albums SET art_hash = ?2 WHERE uid = ?1 AND art_hash IS NULL");
    if (!stmt) return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    bind_text(stmt, 2, al->art_hash);
    return step_done(stmt);
  }
  const char *title = al->title && *al->title ? al->title : "Unknown Album";
  sqlite3_stmt *stmt;
  int found = al->existing_uid != NULL;
  if (!found && al->mb_albumid) {
    if (!(stmt = prepare(db, "SELECT uid FROM albums WHERE mb_albumid = ?1 LIMIT 1"))) return MC_ERR_DB;
    bind_text(stmt, 1, al->mb_albumid);
    found = select_uid(stmt, uid);
  }
  if (!found) {
    if (!(stmt = prepare(db, "SELECT uid FROM albums WHERE artist_uid = ?1"
                             " AND title = ?2 COLLATE NOCASE AND (mb_albumid IS NULL OR ?3 IS NULL)"
                             " LIMIT 1")))
      return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, artist_uid, -1, SQLITE_STATIC);
    bind_text(stmt, 2, title);
    bind_text(stmt, 3, al->mb_albumid);
    found = select_uid(stmt, uid);
  }

  if (found) {
    stmt = prepare(db, "UPDATE albums SET title = ?2, artist_uid = ?3, year = ?4, original_year = ?5,"
                       " disc_total = ?6, mb_albumid = coalesce(?7, mb_albumid),"
                       " mb_releasegroupid = coalesce(?8, mb_releasegroupid),"
                       " art_hash = coalesce(?9, art_hash) WHERE uid = ?1");
  } else {
    mc_status st = mc_uid_new(uid);
    if (st != MC_OK) return st;
    stmt = prepare(db, "INSERT INTO albums (uid, title, artist_uid, year, original_year, disc_total,"
                       " mb_albumid, mb_releasegroupid, art_hash)"
                       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)");
  }
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  bind_text(stmt, 2, title);
  sqlite3_bind_text(stmt, 3, artist_uid, -1, SQLITE_STATIC);
  bind_int(stmt, 4, al->year);
  bind_int(stmt, 5, al->original_year);
  bind_int(stmt, 6, al->disc_total);
  bind_text(stmt, 7, al->mb_albumid);
  bind_text(stmt, 8, al->mb_releasegroupid);
  bind_text(stmt, 9, al->art_hash);
  return step_done(stmt);
}

/* Looks up the file row at path; returns 1 and fills the uids if there is one. */
static int find_file(sqlite3 *db, const char *path, char track_uid[MC_UID_STRLEN],
                     char file_uid[MC_UID_STRLEN], char album_uid[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt = prepare(db, "SELECT f.track_uid, f.uid, t.album_uid FROM files f"
                                   " JOIN tracks t ON t.uid = f.track_uid WHERE f.path = ?1");
  if (!stmt || !path) {
    sqlite3_finalize(stmt);
    return 0;
  }
  bind_text(stmt, 1, path);
  int found = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *tu = (const char *)sqlite3_column_text(stmt, 0);
    const char *fu = (const char *)sqlite3_column_text(stmt, 1);
    const char *au = (const char *)sqlite3_column_text(stmt, 2);
    if (tu && fu && strlen(tu) < MC_UID_STRLEN && strlen(fu) < MC_UID_STRLEN) {
      strcpy(track_uid, tu);
      strcpy(file_uid, fu);
      if (au && strlen(au) < MC_UID_STRLEN) strcpy(album_uid, au);
      found = 1;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

/* A file for a track already in the library (t->track_uid): swapped in for the files row
 * t->replace_file_uid, or added. The track's metadata stays as it is, unless
 * t->update_track (then its artists are credit). */
static mc_status attach_file(sqlite3 *db, const mc_db_track *t, const credit_rows *credit) {
  const char *artist_uid = credit ? credit->uids[0] : NULL;
  char file_uid[MC_UID_STRLEN], rev[MC_UID_STRLEN];
  mc_status st;
  if ((st = mc_uid_new(file_uid)) != MC_OK || (st = mc_uid_new(rev)) != MC_OK) return st;
  int swap = t->replace_file_uid != NULL;

  sqlite3_stmt *stmt;
  if (t->update_track) {
    stmt = prepare(db, "UPDATE tracks SET artist_uid = ?2, title = ?3, track_no = ?4, disc_no = ?5,"
                       " genre = coalesce(?6, genre), mb_trackid = ?7, mb_releasetrackid = ?8,"
                       " year = ?9, mb_albumid = ?10 WHERE uid = ?1");
    if (!stmt) return MC_ERR_DB;
    bind_text(stmt, 1, t->track_uid);
    bind_text(stmt, 2, artist_uid);
    bind_text(stmt, 3, t->title && *t->title ? t->title : "Untitled");
    bind_int(stmt, 4, t->track_no);
    bind_int(stmt, 5, t->disc_no);
    bind_text(stmt, 6, t->genre);
    bind_text(stmt, 7, t->mb_trackid);
    bind_text(stmt, 8, t->mb_releasetrackid);
    bind_int(stmt, 9, t->year);
    bind_text(stmt, 10, t->mb_albumid);
    if ((st = step_done(stmt)) != MC_OK) return st;
    if (sqlite3_changes(db) == 1 && (st = write_credit(db, "tracks", t->track_uid, credit)) != MC_OK)
      return st;
  }

  stmt = prepare(db, swap ? "UPDATE tracks SET duration_ms = coalesce(?2, duration_ms),"
                                          " file_rev = ?3 WHERE uid = ?1"
                                        : "UPDATE tracks SET duration_ms = coalesce(duration_ms, ?2)"
                                          " WHERE uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, t->track_uid);
  bind_int(stmt, 2, t->duration_ms);
  if (swap) sqlite3_bind_text(stmt, 3, rev, -1, SQLITE_STATIC);
  if ((st = step_done(stmt)) != MC_OK) return st;
  if (sqlite3_changes(db) != 1) return MC_ERR_NOT_FOUND;

  /* As in upsert_track: a row still claiming the destination path is stale. */
  stmt = prepare(db, "DELETE FROM files WHERE path = ?1 AND uid IS NOT ?2");
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, t->path);
  bind_text(stmt, 2, t->replace_file_uid);
  if ((st = step_done(stmt)) != MC_OK) return st;

  /* An imported original from now on, whatever the old row was; sha256 is unknown until
   * sync or a deep check records it. */
  stmt = prepare(db, swap ? "UPDATE files SET uid = ?1, format = ?3, bitrate_kbps = ?4,"
                            " size_bytes = ?5, mtime_ms = ?6, path = ?7, sha256 = NULL,"
                            " source_file_uid = NULL, source_peer_uid = NULL, file_rev = ?8,"
                            " source_sha256 = ?10 WHERE uid = ?2 AND track_uid = ?9"
                          : "INSERT INTO files (uid, track_uid, format, bitrate_kbps, size_bytes,"
                            " mtime_ms, path, file_rev, source_sha256)"
                            " VALUES (?1, ?9, ?3, ?4, ?5, ?6, ?7,"
                            "  (SELECT file_rev FROM tracks WHERE uid = ?9), ?10)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, file_uid, -1, SQLITE_STATIC);
  if (swap) bind_text(stmt, 2, t->replace_file_uid);
  bind_text(stmt, 3, t->format ? t->format : "unknown");
  bind_int(stmt, 4, t->bitrate_kbps);
  bind_int(stmt, 5, t->size_bytes);
  bind_int(stmt, 6, t->mtime_ms);
  bind_text(stmt, 7, t->path);
  if (swap) sqlite3_bind_text(stmt, 8, rev, -1, SQLITE_STATIC);
  bind_text(stmt, 9, t->track_uid);
  bind_text(stmt, 10, t->source_sha256);
  if ((st = step_done(stmt)) != MC_OK) return st;
  return sqlite3_changes(db) == 1 ? MC_OK : MC_ERR_NOT_FOUND;
}

/* Writes one track and its file row, in album_uid ("" for none). *old_album receives the
 * album the file's track was in before, if it was already in the library. */
static mc_status upsert_track(sqlite3 *db, const mc_db_track *t, const char *album_uid,
                              const credit_rows *credit, char old_album[MC_UID_STRLEN]) {
  const char *artist_uid = credit->uids[0];
  char track_uid[MC_UID_STRLEN], file_uid[MC_UID_STRLEN];
  old_album[0] = '\0';

  /* The row for where the file was, else the one for where it's going: if the files were
   * moved around outside the app, that row is stale but describes the same track. */
  int found = find_file(db, t->old_path, track_uid, file_uid, old_album) ||
              find_file(db, t->path, track_uid, file_uid, old_album);

  mc_status st;
  if (!found) {
    if ((st = mc_uid_new(track_uid)) != MC_OK || (st = mc_uid_new(file_uid)) != MC_OK) return st;
  }

  sqlite3_stmt *stmt = prepare(db, found
                       ? "UPDATE tracks SET album_uid = ?2, artist_uid = ?3, title = ?4, track_no = ?5,"
                         " disc_no = ?6, duration_ms = ?7, genre = coalesce(?8, genre),"
                         " mb_trackid = ?9, mb_releasetrackid = ?10, year = ?11, mb_albumid = ?12"
                         " WHERE uid = ?1"
                       : "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, disc_no,"
                         " duration_ms, genre, mb_trackid, mb_releasetrackid, year, mb_albumid, added_ms)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, track_uid, -1, SQLITE_STATIC);
  bind_text(stmt, 2, album_uid); /* "": no album */
  sqlite3_bind_text(stmt, 3, artist_uid, -1, SQLITE_STATIC);
  bind_text(stmt, 4, t->title && *t->title ? t->title : "Untitled");
  bind_int(stmt, 5, t->track_no);
  bind_int(stmt, 6, t->disc_no);
  bind_int(stmt, 7, t->duration_ms);
  bind_text(stmt, 8, t->genre);
  bind_text(stmt, 9, t->mb_trackid);
  bind_text(stmt, 10, t->mb_releasetrackid);
  bind_int(stmt, 11, t->year);
  bind_text(stmt, 12, t->mb_albumid);
  if (!found) sqlite3_bind_int64(stmt, 13, (sqlite3_int64)mc_wall_ms());
  if ((st = step_done(stmt)) != MC_OK) return st;
  if ((st = write_credit(db, "tracks", track_uid, credit)) != MC_OK) return st;

  /* A row still claiming the destination path is stale: the importer only writes to paths
   * that are free on disk (or hold this very track). */
  stmt = prepare(db, "DELETE FROM files WHERE path = ?1 AND uid <> ?2");
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, t->path);
  sqlite3_bind_text(stmt, 2, file_uid, -1, SQLITE_STATIC);
  if ((st = step_done(stmt)) != MC_OK) return st;

  stmt = prepare(db, found
                       ? "UPDATE files SET format = ?3, bitrate_kbps = ?4, size_bytes = ?5,"
                         " mtime_ms = ?6, path = ?7, source_sha256 = coalesce(?8, source_sha256)"
                         " WHERE uid = ?1"
                       : "INSERT INTO files (uid, track_uid, format, bitrate_kbps, size_bytes,"
                         " mtime_ms, path, source_sha256) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, file_uid, -1, SQLITE_STATIC);
  if (!found) sqlite3_bind_text(stmt, 2, track_uid, -1, SQLITE_STATIC);
  bind_text(stmt, 3, t->format ? t->format : "unknown");
  bind_int(stmt, 4, t->bitrate_kbps);
  bind_int(stmt, 5, t->size_bytes);
  bind_int(stmt, 6, t->mtime_ms);
  bind_text(stmt, 7, t->path);
  bind_text(stmt, 8, t->source_sha256);
  return step_done(stmt);
}

mc_status mc_import_db_move_file(mc_library *lib, const char *old_path, const char *new_path) {
  if (!lib || !old_path || !new_path) return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt = prepare(mc_library_db(lib), "UPDATE files SET path = ?2 WHERE path = ?1");
  if (stmt) {
    bind_text(stmt, 1, old_path);
    bind_text(stmt, 2, new_path);
  }
  if ((st = step_done(stmt)) == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

int mc_import_db_is_track_at(mc_library *lib, const char *path, const char *mb_releasetrackid) {
  if (!lib || !path || !mb_releasetrackid) return 0;
  sqlite3_stmt *stmt = prepare(mc_library_db(lib),
                               "SELECT 1 FROM files f JOIN tracks t ON t.uid = f.track_uid"
                               " WHERE f.path = ?1 AND t.mb_releasetrackid = ?2");
  if (!stmt) return 0;
  bind_text(stmt, 1, path);
  bind_text(stmt, 2, mb_releasetrackid);
  int found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

static mc_status delete_if_empty(sqlite3 *db, const char *album_uid) {
  sqlite3_stmt *stmt = prepare(db, "DELETE FROM albums WHERE uid = ?1"
                                   " AND NOT EXISTS (SELECT 1 FROM tracks WHERE album_uid = ?1)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC);
  return step_done(stmt);
}

mc_status mc_import_db_write(mc_library *lib, const mc_db_album *album, const mc_db_track *tracks,
                             int count) {
  if (!lib || !album || (count && !tracks)) return MC_ERR_INVALID_ARG;
  sqlite3 *db = mc_library_db(lib);
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;

  char album_uid[MC_UID_STRLEN] = "";
  credit_rows album_credit = {0}, track_credit = {0};
  /* An existing album keeps its credit, which new tracks without one fall back to. */
  const char *existing = album->no_album || album->update_existing ? NULL : album->existing_uid;
  if (existing && (st = read_album_credit(db, existing, &album_credit)) != MC_OK) goto fail;
  if (!album_credit.count) {
    credit_clear(&album_credit);
    if ((st = resolve_credit(db, &album->artist, album->credit, album->credit_count, &album_credit)) !=
        MC_OK)
      goto fail;
  }
  if (!album->no_album) {
    if ((st = upsert_album(db, album, album_credit.uids[0], album_uid)) != MC_OK) goto fail;
    if (!existing && (st = write_credit(db, "albums", album_uid, &album_credit)) != MC_OK) goto fail;
  }

  for (int i = 0; i < count; i++) {
    const mc_db_track *t = &tracks[i];
    char old_album[MC_UID_STRLEN];
    if (t->track_uid && !t->update_track) {
      if ((st = attach_file(db, t, NULL)) != MC_OK) goto fail;
      continue;
    }
    const credit_rows *credit = &album_credit;
    if ((t->artist.name && *t->artist.name) || t->credit_count > 0) {
      if ((st = resolve_credit(db, &t->artist, t->credit, t->credit_count, &track_credit)) != MC_OK)
        goto fail;
      credit = &track_credit;
    }
    if (t->track_uid)
      st = attach_file(db, t, credit);
    else if ((st = upsert_track(db, t, album_uid, credit, old_album)) == MC_OK && old_album[0] &&
             strcmp(old_album, album_uid) != 0)
      st = delete_if_empty(db, old_album);
    credit_clear(&track_credit);
    if (st != MC_OK) goto fail;
  }
  if ((st = mc_library_commit(lib)) != MC_OK) goto fail;
  credit_clear(&album_credit);
  return MC_OK;

fail:
  fprintf(stderr, "musiccore: import write failed: %s\n", sqlite3_errmsg(db));
  mc_library_rollback(lib);
  credit_clear(&album_credit);
  credit_clear(&track_credit);
  return st;
}

mc_status mc_import_db_set_credit_locked(mc_library *lib, int is_album, const char *uid,
                                         const mc_db_artist *artist,
                                         const mc_db_credit_part *credit, int credit_count) {
  if (!lib || !uid || !artist || (credit_count > 0 && !credit)) return MC_ERR_INVALID_ARG;
  sqlite3 *db = mc_library_db(lib);
  const char *table = is_album ? "albums" : "tracks";
  credit_rows c = {0};
  mc_status st = resolve_credit(db, artist, credit, credit_count, &c);
  if (st == MC_OK) {
    char sql[96];
    snprintf(sql, sizeof sql, "UPDATE %s SET artist_uid = ?2 WHERE uid = ?1", table);
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt) {
      bind_text(stmt, 1, uid);
      sqlite3_bind_text(stmt, 2, c.uids[0], -1, SQLITE_STATIC);
    }
    if ((st = step_done(stmt)) == MC_OK && sqlite3_changes(db) != 1) st = MC_ERR_NOT_FOUND;
  }
  if (st == MC_OK) st = write_credit(db, table, uid, &c);
  credit_clear(&c);
  return st;
}

mc_status mc_import_db_set_credit(mc_library *lib, int is_album, const char *uid,
                                  const mc_db_artist *artist, const mc_db_credit_part *credit,
                                  int credit_count) {
  if (!lib || !uid || !artist || (credit_count > 0 && !credit)) return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  st = mc_import_db_set_credit_locked(lib, is_album, uid, artist, credit, credit_count);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}
