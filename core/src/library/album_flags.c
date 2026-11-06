/* See musiccore/edit.h. */
#include "kithcore/edit.h"

#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"

static mc_status mc_album_flags_write(mc_library *lib, const char *album_uid, const mc_album_flags *flags);

mc_status mc_album_flags_get(mc_library *lib, const char *album_uid, mc_album_flags *out) {
  if (!lib || !album_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT a.no_scrobble, " MC_ALBUM_IS_MIX("a") ", a.mix <> 0,"
                         " a.art_chosen IS NOT NULL FROM albums a WHERE a.uid = ?1", -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    out->no_scrobble = sqlite3_column_int(stmt, 0);
    out->mix = sqlite3_column_int(stmt, 1);
    out->mix_by_hand = sqlite3_column_int(stmt, 2);
    out->art_chosen = sqlite3_column_int(stmt, 3);
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
}

mc_status mc_album_flags_set(mc_library *lib, const char *album_uid, const mc_album_flags *flags) {
  if (!lib || !album_uid || !flags) return MC_ERR_INVALID_ARG;
  mc_status st = mc_album_flags_write(lib, album_uid, flags);
  /* A mix no more, and nothing else for the album it was given to hold. */
  if (st == MC_OK && !flags->mix && !flags->no_scrobble) st = mc_mix_unfile(lib, album_uid);
  return st;
}

static mc_status mc_album_flags_write(mc_library *lib, const char *album_uid, const mc_album_flags *flags) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  /* Only a change is a choice: a form saved for its other flag leaves a detected mix detected. */
  if (sqlite3_prepare_v2(lib->db, "UPDATE albums AS a SET no_scrobble = ?2,"
                         " mix = CASE WHEN ?3 = " MC_ALBUM_IS_MIX("a") " THEN a.mix"
                         " WHEN ?3 THEN 1 ELSE -1 END WHERE a.uid = ?1", -1, &stmt, NULL) != SQLITE_OK) {
    st = MC_ERR_DB;
  } else {
    sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, !!flags->no_scrobble);
    sqlite3_bind_int(stmt, 3, !!flags->mix);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    else if (!sqlite3_changes(lib->db)) st = MC_ERR_NOT_FOUND;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* A filed track's album: derived from the track, so two devices filing it make the one album
 * and not two to merge. A number no credit's position reaches, as a track's credit rows derive
 * their uids from it too. */
#define MIX_ALBUM_POSITION 7170424
#define MIX_ALBUM "mc_credit_uid(t.uid, " MC_SQL_NUM(MIX_ALBUM_POSITION) ")"

int mc_mix_album_of_track(mc_library *lib, const char *album_uid, char track_uid[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT t.uid FROM tracks t WHERE t.album_uid = ?1"
                         " AND " MIX_ALBUM " = ?1 AND (SELECT count(*) FROM tracks st"
                         "  WHERE st.album_uid = ?1) = 1", -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  if (yes) snprintf(track_uid, MC_UID_STRLEN, "%s", sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return yes;
}

static mc_status file_step(mc_library *lib, const char *head, const char *where, const char *arg) {
  char sql[768];
  snprintf(sql, sizeof sql, "%s t.album_uid IS NULL AND %s", head, where);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  if (arg) sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_mix_file_tracks(mc_library *lib, const char *where, const char *arg) {
  /* The track last: each step finds its tracks by having no album. OR IGNORE: two devices
   * filing the same track, or a track filed once before, find the rows there. */
  static const char *const steps[] = {
    "INSERT OR IGNORE INTO albums (uid, title, artist_uid, artist_credit, year)"
    " SELECT " MIX_ALBUM ", t.title, t.artist_uid, t.artist_credit, t.year FROM tracks t WHERE",
    "INSERT OR IGNORE INTO album_artists (uid, album_uid, artist_uid, position, joinphrase)"
    " SELECT mc_credit_uid(" MIX_ALBUM ", c.position), " MIX_ALBUM ", c.artist_uid, c.position,"
    " c.joinphrase FROM tracks t JOIN track_artists c ON c.track_uid = t.uid WHERE",
    "UPDATE tracks AS t SET album_uid = " MIX_ALBUM " WHERE",
  };
  mc_status st = MC_OK;
  for (size_t i = 0; st == MC_OK && i < sizeof steps / sizeof steps[0]; i++)
    st = file_step(lib, steps[i], where, arg);
  return st;
}
