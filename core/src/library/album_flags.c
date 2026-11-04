/* See musiccore/edit.h. */
#include "musiccore/edit.h"

#include <string.h>

#include "library/library_internal.h"

mc_status mc_album_flags_get(mc_library *lib, const char *album_uid, mc_album_flags *out) {
  if (!lib || !album_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT no_scrobble, mix FROM albums WHERE uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    out->no_scrobble = sqlite3_column_int(stmt, 0);
    out->mix = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
}

mc_status mc_album_flags_set(mc_library *lib, const char *album_uid, const mc_album_flags *flags) {
  if (!lib || !album_uid || !flags) return MC_ERR_INVALID_ARG;
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "UPDATE albums SET no_scrobble = ?2, mix = ?3 WHERE uid = ?1", -1,
                         &stmt, NULL) != SQLITE_OK) {
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
