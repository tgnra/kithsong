#include "tagger/import_db.h"

#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "util/platform.h"

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

static mc_status upsert_singles_album(sqlite3 *db, const char *artist_uid, const char *artist_name,
                                      char uid[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt =
    prepare(db, "SELECT uid FROM albums WHERE kind = 'singles' AND artist_uid = ?1 LIMIT 1");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, artist_uid, -1, SQLITE_STATIC);
  if (select_uid(stmt, uid)) return MC_OK;

  char title[512];
  snprintf(title, sizeof title, "%s Singles", artist_name && *artist_name ? artist_name : "Unknown Artist");
  mc_status st = mc_uid_new(uid);
  if (st != MC_OK) return st;
  stmt = prepare(db, "INSERT INTO albums (uid, title, artist_uid, kind) VALUES (?1, ?2, ?3, 'singles')");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, title, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, artist_uid, -1, SQLITE_STATIC);
  return step_done(stmt);
}

static mc_status upsert_album(sqlite3 *db, const mc_db_album *al, const char *artist_uid,
                              char uid[MC_UID_STRLEN]) {
  const char *title = al->title && *al->title ? al->title : "Unknown Album";
  sqlite3_stmt *stmt;
  int found = 0;
  if (al->mb_albumid) {
    if (!(stmt = prepare(db, "SELECT uid FROM albums WHERE mb_albumid = ?1 LIMIT 1"))) return MC_ERR_DB;
    bind_text(stmt, 1, al->mb_albumid);
    found = select_uid(stmt, uid);
  }
  if (!found) {
    if (!(stmt = prepare(db, "SELECT uid FROM albums WHERE kind = 'album' AND artist_uid = ?1"
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
                       " mb_albumid, mb_releasegroupid, art_hash, kind)"
                       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 'album')");
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

/* Writes one track and its file row. *old_album receives the album the file's track was in
 * before, if it was already in the library. */
static mc_status upsert_track(sqlite3 *db, const mc_db_track *t, const char *album_uid,
                              const char *artist_uid, char old_album[MC_UID_STRLEN]) {
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
  sqlite3_bind_text(stmt, 2, album_uid, -1, SQLITE_STATIC);
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

  /* A row still claiming the destination path is stale: the importer only writes to paths
   * that are free on disk (or hold this very track). */
  stmt = prepare(db, "DELETE FROM files WHERE path = ?1 AND uid <> ?2");
  if (!stmt) return MC_ERR_DB;
  bind_text(stmt, 1, t->path);
  sqlite3_bind_text(stmt, 2, file_uid, -1, SQLITE_STATIC);
  if ((st = step_done(stmt)) != MC_OK) return st;

  stmt = prepare(db, found
                       ? "UPDATE files SET format = ?3, bitrate_kbps = ?4, size_bytes = ?5,"
                         " mtime_ms = ?6, path = ?7 WHERE uid = ?1"
                       : "INSERT INTO files (uid, track_uid, format, bitrate_kbps, size_bytes,"
                         " mtime_ms, path) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, file_uid, -1, SQLITE_STATIC);
  if (!found) sqlite3_bind_text(stmt, 2, track_uid, -1, SQLITE_STATIC);
  bind_text(stmt, 3, t->format ? t->format : "unknown");
  bind_int(stmt, 4, t->bitrate_kbps);
  bind_int(stmt, 5, t->size_bytes);
  bind_int(stmt, 6, t->mtime_ms);
  bind_text(stmt, 7, t->path);
  return step_done(stmt);
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

  char album_artist[MC_UID_STRLEN], album_uid[MC_UID_STRLEN];
  if ((st = upsert_artist(db, &album->artist, album_artist)) != MC_OK) goto fail;
  st = album->singles ? upsert_singles_album(db, album_artist, album->artist.name, album_uid)
                      : upsert_album(db, album, album_artist, album_uid);
  if (st != MC_OK) goto fail;

  for (int i = 0; i < count; i++) {
    const mc_db_track *t = &tracks[i];
    char artist_uid[MC_UID_STRLEN], old_album[MC_UID_STRLEN];
    if (t->artist.name && *t->artist.name) {
      if ((st = upsert_artist(db, &t->artist, artist_uid)) != MC_OK) goto fail;
    } else {
      strcpy(artist_uid, album_artist);
    }
    if ((st = upsert_track(db, t, album_uid, artist_uid, old_album)) != MC_OK) goto fail;
    if (old_album[0] && strcmp(old_album, album_uid) != 0 &&
        (st = delete_if_empty(db, old_album)) != MC_OK)
      goto fail;
  }
  if ((st = mc_library_commit(lib)) != MC_OK) goto fail;
  return MC_OK;

fail:
  fprintf(stderr, "musiccore: import write failed: %s\n", sqlite3_errmsg(db));
  mc_library_rollback(lib);
  return st;
}
