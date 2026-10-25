#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

static int changelog_count(sqlite3 *db, const char *where) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE %s", where);
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

int main(int argc, char **argv) {
  /* The library is a folder; the database is library.db inside it. */
  char path[1024], file[1100];
  snprintf(path, sizeof path, "%s/test_library", argc > 1 ? argv[1] : ".");
  const char *const leftovers[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                                   MC_LIBRARY_DB_NAME "-shm"};
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", path, leftovers[i]);
    remove(file);
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  CHECK(mc_library_schema_version(lib) == 3);
  char node[MC_UID_STRLEN];
  strcpy(node, mc_library_node_uid(lib));
  CHECK(strlen(node) == MC_UID_STRLEN - 1);

  int64_t n = -1;
  CHECK(mc_library_count(lib, MC_ENTITY_ALBUMS, &n) == MC_OK && n == 0);

  /* Local edits are logged field by field. */
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('a1', 'Low')") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'a1' AND field = 'name'") == 1);
  CHECK(changelog_count(db, "row_uid = 'a1' AND field = 'sort_name'") == 0); /* NULL: skipped */

  CHECK(exec(db, "UPDATE artists SET name = 'Low' WHERE uid = 'a1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'a1'") == 1); /* unchanged value: nothing logged */
  CHECK(exec(db, "UPDATE artists SET sort_name = 'Low' WHERE uid = 'a1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'a1'") == 2);

  CHECK(exec(db, "DELETE FROM artists WHERE uid = 'a1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'a1' AND field = '_deleted'") == 1);

  /* Timestamps are strictly increasing and carry this node as origin. */
  CHECK(changelog_count(db, "origin_uid <> mc_node_uid()") == 0);
  CHECK(changelog_count(db, "hlc <= (SELECT max(hlc) FROM changelog c2 WHERE c2.seq < changelog.seq)") == 0);

  /* Changes being applied from a peer are not re-logged by the triggers. */
  lib->applying_remote = 1;
  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('a2', 'Duster')") == SQLITE_OK);
  lib->applying_remote = 0;
  CHECK(changelog_count(db, "row_uid = 'a2'") == 0);

  /* Play counts are logged like any other tracked table and go when the track does. */
  CHECK(exec(db, "INSERT INTO tracks (uid, title) VALUES ('t1', 'Starfire')") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO track_plays (uid, track_uid, node_uid, play_count)"
                 " VALUES ('p1', 't1', mc_node_uid(), 1)") == SQLITE_OK);
  CHECK(exec(db, "UPDATE track_plays SET play_count = play_count + 1 WHERE uid = 'p1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'p1' AND field = 'play_count' AND value = 2") == 1);
  CHECK(exec(db, "DELETE FROM tracks WHERE uid = 't1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'p1' AND field = '_deleted'") == 1);

  mc_library_close(lib);

  /* Reopening keeps the node identity and doesn't re-run migrations. */
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (lib) {
    CHECK_STR(mc_library_node_uid(lib), node);
    CHECK(mc_library_schema_version(lib) == 3);
    CHECK(mc_library_count(lib, MC_ENTITY_ARTISTS, &n) == MC_OK && n == 1);
    mc_library_close(lib);
  }

  /* File paths are stored relative to the library folder, with '/'. A trailing separator
   * on the folder doesn't matter. */
  snprintf(file, sizeof file, "%s/", path);
  lib = NULL;
  CHECK(mc_library_open(file, &lib) == MC_OK);
  if (lib) {
    CHECK_STR(mc_library_dir(lib), path);
    char abs[1200];
    snprintf(abs, sizeof abs, "%s%cLow%cSecret Name (1999)%c01 Starfire.flac", path, MC_PATH_SEP,
             MC_PATH_SEP, MC_PATH_SEP);
    char *rel = mc_library_relpath(lib, abs);
    CHECK_STR(rel, "Low/Secret Name (1999)/01 Starfire.flac");
    char *back = rel ? mc_library_abspath(lib, rel) : NULL;
    CHECK_STR(back, abs);
    mc_free(rel);
    mc_free(back);

    CHECK(mc_library_relpath(lib, "/somewhere/else/song.flac") == NULL);
    CHECK(mc_library_relpath(lib, path) == NULL);
    snprintf(abs, sizeof abs, "%s-other%csong.flac", path, MC_PATH_SEP); /* sibling, same prefix */
    CHECK(mc_library_relpath(lib, abs) == NULL);
    mc_library_close(lib);
  }

  return TEST_RESULT();
}
