/* Playlists and album groups, ordered by fractional index keys (see schema_v1.sql,
 * playlist_entries, and docs/schema.md). Every write goes through mc_library_begin_write(),
 * and the changelog triggers record it. */
#include "musiccore/playlist.h"

#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "util/fracindex.h"
#include "util/platform.h"

/* Prepares sql and binds up to four text parameters; a NULL one binds NULL. */
static sqlite3_stmt *prepare(mc_library *lib, const char *sql, const char *p1, const char *p2,
                             const char *p3, const char *p4) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
  const char *params[] = {p1, p2, p3, p4};
  for (int i = 0; i < 4; i++)
    if (params[i]) sqlite3_bind_text(stmt, i + 1, params[i], -1, SQLITE_STATIC);
  return stmt;
}

/* Runs one write. MC_ERR_NOT_FOUND if it changed no row, or broke a foreign key (a playlist
 * or track that doesn't exist). */
static mc_status write_one(mc_library *lib, const char *sql, const char *p1, const char *p2,
                           const char *p3, const char *p4) {
  sqlite3_stmt *stmt = prepare(lib, sql, p1, p2, p3, p4);
  if (!stmt) return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if ((rc & 0xff) == SQLITE_CONSTRAINT) return MC_ERR_NOT_FOUND;
  if (rc != SQLITE_DONE) return MC_ERR_DB;
  return sqlite3_changes(lib->db) ? MC_OK : MC_ERR_NOT_FOUND;
}

/* write_one() in a write transaction of its own. */
static mc_status write_txn(mc_library *lib, const char *sql, const char *p1, const char *p2) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  if ((st = write_one(lib, sql, p1, p2, NULL, NULL)) == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* Copies the first column of a query's first row into *out (malloc'd; NULL if there's no
 * row or the value is NULL). */
static mc_status query_text(mc_library *lib, const char *sql, const char *p1, const char *p2,
                            const char *p3, char **out) {
  *out = NULL;
  sqlite3_stmt *stmt = prepare(lib, sql, p1, p2, p3, NULL);
  if (!stmt) return MC_ERR_DB;
  mc_status st = MC_OK;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && !(*out = mc_strdup((const char *)v))) st = MC_ERR_NOMEM;
  } else if (rc != SQLITE_DONE) {
    st = MC_ERR_DB;
  }
  sqlite3_finalize(stmt);
  return st;
}

/* The position for an entry going right after after_entry_uid (NULL = at the start) of
 * playlist_uid, ignoring entry skip_uid (the one being moved, or NULL). It goes after every
 * entry sharing after's position, which two devices inserting at once can leave. */
static mc_status position_after(mc_library *lib, const char *playlist_uid, const char *after_entry_uid,
                                const char *skip_uid, char **out) {
  *out = NULL;
  char *before = NULL, *next = NULL;
  mc_status st = MC_OK;
  if (after_entry_uid) {
    st = query_text(lib, "SELECT position FROM playlist_entries WHERE uid = ?1 AND playlist_uid = ?2",
                    after_entry_uid, playlist_uid, NULL, &before);
    if (st == MC_OK && !before) st = MC_ERR_NOT_FOUND;
  }
  if (st == MC_OK)
    st = query_text(lib,
                    "SELECT min(position) FROM playlist_entries WHERE playlist_uid = ?1"
                    " AND (?2 IS NULL OR position > ?2) AND (?3 IS NULL OR uid != ?3)",
                    playlist_uid, before, skip_uid, &next);
  if (st == MC_OK && !(*out = mc_frac_key_between(before, next))) st = MC_ERR_FORMAT;
  free(before);
  free(next);
  return st;
}

mc_status mc_playlist_create(mc_library *lib, const char *name, char out_uid[MC_UID_STRLEN]) {
  if (!lib || !name || !*name || !out_uid) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st == MC_OK) st = write_txn(lib, "INSERT INTO playlists (uid, name) VALUES (?1, ?2)", uid, name);
  if (st == MC_OK) memcpy(out_uid, uid, MC_UID_STRLEN);
  return st;
}

mc_status mc_playlist_rename(mc_library *lib, const char *playlist_uid, const char *name) {
  if (!lib || !playlist_uid || !name || !*name) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "UPDATE playlists SET name = ?2 WHERE uid = ?1", playlist_uid, name);
}

mc_status mc_playlist_delete(mc_library *lib, const char *playlist_uid) {
  if (!lib || !playlist_uid) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "DELETE FROM playlists WHERE uid = ?1", playlist_uid, NULL);
}

/* Adds an entry for track_uid after after_entry_uid, or at the end if at_end. */
static mc_status insert_entry(mc_library *lib, const char *playlist_uid, const char *track_uid,
                              const char *after_entry_uid, int at_end, char out_entry_uid[MC_UID_STRLEN]) {
  if (!lib || !playlist_uid || !track_uid) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st != MC_OK) return st;
  if ((st = mc_library_begin_write(lib)) != MC_OK) return st;

  char *position = NULL;
  if (at_end) {
    char *last = NULL;
    st = query_text(lib, "SELECT max(position) FROM playlist_entries WHERE playlist_uid = ?1", playlist_uid,
                    NULL, NULL, &last);
    if (st == MC_OK && !(position = mc_frac_key_between(last, NULL))) st = MC_ERR_FORMAT;
    free(last);
  } else {
    st = position_after(lib, playlist_uid, after_entry_uid, NULL, &position);
  }
  if (st == MC_OK)
    st = write_one(lib,
                   "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
                   " VALUES (?1, ?2, ?3, ?4)",
                   uid, playlist_uid, track_uid, position);
  free(position);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) {
    mc_library_rollback(lib);
    return st;
  }
  if (out_entry_uid) memcpy(out_entry_uid, uid, MC_UID_STRLEN);
  return MC_OK;
}

mc_status mc_playlist_insert(mc_library *lib, const char *playlist_uid, const char *track_uid,
                             const char *after_entry_uid, char out_entry_uid[MC_UID_STRLEN]) {
  return insert_entry(lib, playlist_uid, track_uid, after_entry_uid, 0, out_entry_uid);
}

mc_status mc_playlist_append(mc_library *lib, const char *playlist_uid, const char *track_uid,
                             char out_entry_uid[MC_UID_STRLEN]) {
  return insert_entry(lib, playlist_uid, track_uid, NULL, 1, out_entry_uid);
}

mc_status mc_playlist_move(mc_library *lib, const char *entry_uid, const char *after_entry_uid) {
  if (!lib || !entry_uid || (after_entry_uid && !strcmp(entry_uid, after_entry_uid))) return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  char *playlist = NULL, *position = NULL;
  st = query_text(lib, "SELECT playlist_uid FROM playlist_entries WHERE uid = ?1", entry_uid, NULL, NULL,
                  &playlist);
  if (st == MC_OK && !playlist) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK) st = position_after(lib, playlist, after_entry_uid, entry_uid, &position);
  if (st == MC_OK)
    st = write_one(lib, "UPDATE playlist_entries SET position = ?2 WHERE uid = ?1", entry_uid, position, NULL,
                   NULL);
  free(playlist);
  free(position);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

mc_status mc_playlist_remove(mc_library *lib, const char *entry_uid) {
  if (!lib || !entry_uid) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "DELETE FROM playlist_entries WHERE uid = ?1", entry_uid, NULL);
}

/* --- Album groups --- */

mc_status mc_album_group_create(mc_library *lib, const char *name, char out_uid[MC_UID_STRLEN]) {
  if (!lib || !name || !*name || !out_uid) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st == MC_OK) st = write_txn(lib, "INSERT INTO album_groups (uid, name) VALUES (?1, ?2)", uid, name);
  if (st == MC_OK) memcpy(out_uid, uid, MC_UID_STRLEN);
  return st;
}

mc_status mc_album_group_rename(mc_library *lib, const char *group_uid, const char *name) {
  if (!lib || !group_uid || !name || !*name) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "UPDATE album_groups SET name = ?2 WHERE uid = ?1", group_uid, name);
}

mc_status mc_album_group_delete(mc_library *lib, const char *group_uid) {
  if (!lib || !group_uid) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "DELETE FROM album_groups WHERE uid = ?1", group_uid, NULL);
}

mc_status mc_album_group_add(mc_library *lib, const char *group_uid, const char *album_uid) {
  if (!lib || !group_uid || !album_uid) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st != MC_OK) return st;
  if ((st = mc_library_begin_write(lib)) != MC_OK) return st;

  char *there = NULL, *last = NULL, *position = NULL;
  st = query_text(lib, "SELECT uid FROM album_group_entries WHERE group_uid = ?1 AND album_uid = ?2", group_uid,
                  album_uid, NULL, &there);
  if (st == MC_OK && there) { /* already in it */
    free(there);
    mc_library_rollback(lib);
    return MC_OK;
  }
  if (st == MC_OK)
    st = query_text(lib, "SELECT max(position) FROM album_group_entries WHERE group_uid = ?1", group_uid, NULL,
                    NULL, &last);
  if (st == MC_OK && !(position = mc_frac_key_between(last, NULL))) st = MC_ERR_FORMAT;
  if (st == MC_OK)
    st = write_one(lib,
                   "INSERT INTO album_group_entries (uid, group_uid, album_uid, position) VALUES (?1, ?2, ?3, ?4)",
                   uid, group_uid, album_uid, position);
  free(last);
  free(position);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

mc_status mc_album_group_remove(mc_library *lib, const char *group_uid, const char *album_uid) {
  if (!lib || !group_uid || !album_uid) return MC_ERR_INVALID_ARG;
  return write_txn(lib, "DELETE FROM album_group_entries WHERE group_uid = ?1 AND album_uid = ?2", group_uid,
                   album_uid);
}

mc_status mc_album_group_move(mc_library *lib, const char *group_uid, const char *album_uid,
                              const char *after_album_uid) {
  if (!lib || !group_uid || !album_uid || (after_album_uid && !strcmp(album_uid, after_album_uid)))
    return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  /* As position_after(), but by album: an album is listed at its first entry, so it goes
   * after after_album_uid's first entry and before the next album's, and every entry it has
   * moves (should sync have left two). */
  static const char first[] = "SELECT min(position) FROM album_group_entries WHERE group_uid = ?1 AND album_uid = ?2";
  char *there = NULL, *before = NULL, *next = NULL, *position = NULL;
  st = query_text(lib, first, group_uid, album_uid, NULL, &there);
  if (st == MC_OK && !there) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK && after_album_uid) {
    st = query_text(lib, first, group_uid, after_album_uid, NULL, &before);
    if (st == MC_OK && !before) st = MC_ERR_NOT_FOUND;
  }
  if (st == MC_OK)
    st = query_text(lib,
                    "SELECT min(position) FROM album_group_entries WHERE group_uid = ?1"
                    " AND (?2 IS NULL OR position > ?2) AND album_uid != ?3"
                    " AND album_uid NOT IN (SELECT album_uid FROM album_group_entries"
                    "  WHERE group_uid = ?1 AND position = ?2)",
                    group_uid, before, album_uid, &next);
  if (st == MC_OK && !(position = mc_frac_key_between(before, next))) st = MC_ERR_FORMAT;
  if (st == MC_OK)
    st = write_one(lib, "UPDATE album_group_entries SET position = ?3 WHERE group_uid = ?1 AND album_uid = ?2",
                   group_uid, album_uid, position, NULL);
  free(there);
  free(before);
  free(next);
  free(position);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}
