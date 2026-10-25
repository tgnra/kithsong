#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"

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
  char path[1024], extra[1100];
  snprintf(path, sizeof path, "%s/test_library.db", argc > 1 ? argv[1] : ".");
  remove(path);
  snprintf(extra, sizeof extra, "%s-wal", path);
  remove(extra);
  snprintf(extra, sizeof extra, "%s-shm", path);
  remove(extra);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  CHECK(mc_library_schema_version(lib) == 1);
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

  mc_library_close(lib);

  /* Reopening keeps the node identity and doesn't re-run migrations. */
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (lib) {
    CHECK_STR(mc_library_node_uid(lib), node);
    CHECK(mc_library_schema_version(lib) == 1);
    CHECK(mc_library_count(lib, MC_ENTITY_ARTISTS, &n) == MC_OK && n == 1);
    mc_library_close(lib);
  }

  return TEST_RESULT();
}
