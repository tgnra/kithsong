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

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int has_column(sqlite3 *db, const char *table, const char *column) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM pragma_table_info('%s') WHERE name = '%s'",
           table, column);
  return query_int(db, sql) == 1;
}

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                               MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

/* Identity: library_uid, node uid rotation, adopting another library's uid. */
static void test_identity(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_identity.d", scratch);
  remove_db(path);

  mc_library *lib = NULL, *other = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  CHECK(mc_library_open(path, &other) == MC_OK);
  if (!lib || !other) {
    mc_library_close(lib);
    mc_library_close(other);
    return;
  }
  sqlite3 *db = mc_library_db(lib);
  char old_node[MC_UID_STRLEN], library_uid[MC_UID_STRLEN];
  strcpy(old_node, mc_library_node_uid(lib));
  strcpy(library_uid, mc_library_uid(lib));
  CHECK(strlen(library_uid) == MC_UID_STRLEN - 1);
  CHECK(strcmp(library_uid, old_node) != 0);
  CHECK(strcmp(mc_library_uid(other), library_uid) == 0); /* the same for every connection */

  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('a1', 'Low')") == SQLITE_OK);

  /* Rotating gives new changes a new origin; earlier ones keep the old one. */
  CHECK(mc_library_rotate_node_uid(lib) == MC_OK);
  CHECK(strcmp(mc_library_node_uid(lib), old_node) != 0);
  CHECK(strlen(mc_library_node_uid(lib)) == MC_UID_STRLEN - 1);
  CHECK(exec(db, "UPDATE artists SET name = 'Lower' WHERE uid = 'a1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'a1' AND origin_uid = mc_node_uid()") == 1);
  char where[128];
  snprintf(where, sizeof where, "row_uid = 'a1' AND origin_uid = '%s'", old_node);
  CHECK(changelog_count(db, where) == 1);

  /* Another open connection picks up the new uid on its next write. */
  CHECK(mc_library_begin_write(other) == MC_OK);
  CHECK_STR(mc_library_node_uid(other), mc_library_node_uid(lib));
  CHECK(exec(mc_library_db(other), "INSERT INTO artists (uid, name) VALUES ('a2', 'Duster')") ==
        SQLITE_OK);
  CHECK(mc_library_commit(other) == MC_OK);
  CHECK(changelog_count(db, "row_uid = 'a2' AND origin_uid = mc_node_uid()") == 1);
  mc_library_close(other);

  /* A library with tracks can't take on another library's uid; an empty one can. */
  char foreign[MC_UID_STRLEN];
  CHECK(mc_uid_new(foreign) == MC_OK);
  CHECK(mc_library_adopt_uid(lib, "not-a-uid") == MC_ERR_INVALID_ARG);
  CHECK(mc_library_adopt_uid(lib, "0192F1E4-7C3A-7B21-9D4E-1F2A3B4C5D6E") == MC_ERR_INVALID_ARG);
  CHECK(mc_library_adopt_uid(lib, foreign) == MC_OK); /* artists only, no tracks yet */
  CHECK(strcmp(mc_library_uid(lib), foreign) == 0);
  CHECK(exec(db, "INSERT INTO tracks (uid, title) VALUES ('t1', 'Starfire')") == SQLITE_OK);
  CHECK(mc_library_adopt_uid(lib, library_uid) == MC_ERR_LIBRARY_MISMATCH);
  CHECK(strcmp(mc_library_uid(lib), foreign) == 0);
  CHECK(mc_library_adopt_uid(lib, foreign) == MC_OK); /* already this library's uid */

  char node[MC_UID_STRLEN];
  strcpy(node, mc_library_node_uid(lib));
  mc_library_close(lib);

  /* Both survive a reopen. */
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (lib) {
    CHECK(strcmp(mc_library_uid(lib), foreign) == 0);
    CHECK(strcmp(mc_library_node_uid(lib), node) == 0);
    mc_library_close(lib);
  }
}

/* Migration v4 on a library made before it: peers goes, files gains sha256, and the library
 * gets a library_uid. The old state is recreated by hand from a current database. */
static void test_migration_v4(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_v4.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  CHECK(query_int(db, "SELECT count(*) FROM sqlite_master WHERE name = 'peers'") == 0);
  CHECK(has_column(db, "files", "sha256"));
  CHECK(exec(db, "CREATE TABLE peers (uid TEXT PRIMARY KEY) WITHOUT ROWID;"
                 "ALTER TABLE files DROP COLUMN sha256;"
                 "DELETE FROM meta WHERE key = 'library_uid';"
                 "DROP TABLE album_group_entries;"
                 "DROP TABLE album_groups;"
                 "PRAGMA user_version = 3;") == SQLITE_OK);
  char node[MC_UID_STRLEN];
  strcpy(node, mc_library_node_uid(lib));
  char library_uid[MC_UID_STRLEN];
  strcpy(library_uid, mc_library_uid(lib));
  CHECK(strlen(library_uid) == MC_UID_STRLEN - 1);
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  db = mc_library_db(lib);
  CHECK(mc_library_schema_version(lib) == 5);
  CHECK(query_int(db, "SELECT count(*) FROM sqlite_master WHERE name = 'peers'") == 0);
  CHECK(has_column(db, "files", "sha256"));
  CHECK(strlen(mc_library_uid(lib)) == MC_UID_STRLEN - 1);
  CHECK(query_int(db, "SELECT count(*) FROM meta WHERE key = 'library_uid'") == 1);
  CHECK(strcmp(mc_library_node_uid(lib), node) == 0);
  mc_library_close(lib);
}

/* Migration v5: album groups. Their changes are logged like playlists', and deleting a group
 * or an album takes its entries with it. */
static void test_migration_v5(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_v5.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "DROP TABLE album_group_entries;"
                 "DROP TABLE album_groups;"
                 "PRAGMA user_version = 4;") == SQLITE_OK);
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  db = mc_library_db(lib);
  CHECK(mc_library_schema_version(lib) == 5);
  CHECK(has_column(db, "album_groups", "name"));
  CHECK(has_column(db, "album_group_entries", "position"));

  CHECK(exec(db, "INSERT INTO albums (uid, title) VALUES ('a1', 'Avalon'), ('a2', 'Siren');"
                 "INSERT INTO album_groups (uid, name) VALUES ('g1', 'Roxy Music');"
                 "INSERT INTO album_group_entries (uid, group_uid, album_uid, position)"
                 "  VALUES ('e1', 'g1', 'a1', 'a0'), ('e2', 'g1', 'a2', 'a1');") == SQLITE_OK);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'album_groups'"
                      " AND row_uid = 'g1' AND field = 'name'") == 1);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'album_group_entries'"
                      " AND row_uid = 'e1'") == 3);

  /* Deleting an album removes it from its groups, and the removal is logged. */
  CHECK(exec(db, "DELETE FROM albums WHERE uid = 'a1'") == SQLITE_OK);
  CHECK(query_int(db, "SELECT count(*) FROM album_group_entries") == 1);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'album_group_entries'"
                      " AND row_uid = 'e1' AND field = '_deleted'") == 1);
  CHECK(exec(db, "DELETE FROM album_groups WHERE uid = 'g1'") == SQLITE_OK);
  CHECK(query_int(db, "SELECT count(*) FROM album_group_entries") == 0);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  /* The library is a folder; the database is library.db inside it. */
  char path[1024], file[1100];
  snprintf(path, sizeof path, "%s/test_library.d", argc > 1 ? argv[1] : ".");
  const char *const leftovers[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                                   MC_LIBRARY_DB_NAME "-shm"};
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", path, leftovers[i]);
    remove(file);
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  CHECK(mc_library_schema_version(lib) == 5);
  char node[MC_UID_STRLEN];
  strcpy(node, mc_library_node_uid(lib));
  char library_uid[MC_UID_STRLEN];
  strcpy(library_uid, mc_library_uid(lib));
  CHECK(strlen(library_uid) == MC_UID_STRLEN - 1);
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

  /* An album's tracks come back by disc, then number (unnumbered last), with an absolute
   * path to the original file rather than a transcode. */
  CHECK(exec(db, "INSERT INTO albums (uid, title) VALUES ('al1', 'Secret Name')") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO tracks (uid, album_uid, title, track_no, disc_no, duration_ms) VALUES"
                 " ('t2', 'al1', 'Bright', 2, 1, 120000), ('t3', 'al1', 'Starfire', 1, 1, 200500),"
                 " ('t4', 'al1', 'Hidden', NULL, 1, 1000), ('t5', 'al1', 'Disc Two', 1, 2, 5000),"
                 " ('t6', NULL, 'Elsewhere', 1, 1, 1)") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO files (uid, track_uid, path, format, source_file_uid) VALUES"
                 " ('f1', 't3', 'Low/Secret Name/01 Starfire.opus', 'opus', 'orig'),"
                 " ('f2', 't3', 'Low/Secret Name/01 Starfire.flac', 'flac', NULL)") == SQLITE_OK);
  mc_track_list *tracks = NULL;
  CHECK(mc_library_list_album_tracks(lib, "al1", &tracks) == MC_OK && tracks && tracks->count == 4);
  if (tracks && tracks->count == 4) {
    CHECK_STR(tracks->items[0].title, "Starfire");
    CHECK_STR(tracks->items[1].title, "Bright");
    CHECK_STR(tracks->items[2].title, "Hidden");
    CHECK_STR(tracks->items[3].title, "Disc Two");
    CHECK(tracks->items[0].track_no == 1 && tracks->items[0].disc_no == 1);
    CHECK(tracks->items[0].duration_ms == 200500);
    CHECK(tracks->items[2].track_no == 0);
    CHECK(tracks->items[0].path && strstr(tracks->items[0].path, "Starfire.flac"));
    CHECK(tracks->items[1].path == NULL);
  }
  mc_track_list_free(tracks);
  tracks = NULL;
  CHECK(mc_library_list_album_tracks(lib, "nope", &tracks) == MC_OK && tracks && tracks->count == 0);
  mc_track_list_free(tracks);

  mc_library_close(lib);

  /* Reopening keeps the node identity and doesn't re-run migrations. */
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (lib) {
    CHECK(strcmp(mc_library_node_uid(lib), node) == 0);
    CHECK(strcmp(mc_library_uid(lib), library_uid) == 0);
    CHECK(mc_library_schema_version(lib) == 5);
    CHECK(mc_library_count(lib, MC_ENTITY_ARTISTS, &n) == MC_OK && n == 1);
    mc_library_close(lib);
  }

  /* File paths are stored relative to the library folder, with '/'. A trailing separator
   * on the folder doesn't matter. */
  snprintf(file, sizeof file, "%s/", path);
  lib = NULL;
  CHECK(mc_library_open(file, &lib) == MC_OK);
  if (lib) {
    CHECK(strcmp(mc_library_dir(lib), path) == 0);
    char abs[1200];
    snprintf(abs, sizeof abs, "%s%cLow%cSecret Name (1999)%c01 Starfire.flac", path, MC_PATH_SEP,
             MC_PATH_SEP, MC_PATH_SEP);
    char *rel = mc_library_relpath(lib, abs);
    CHECK_STR(rel, "Low/Secret Name (1999)/01 Starfire.flac");
    char *back = rel ? mc_library_abspath(lib, rel) : NULL;
    CHECK(back && strcmp(back, abs) == 0);
    mc_free(rel);
    mc_free(back);

    CHECK(mc_library_relpath(lib, "/somewhere/else/song.flac") == NULL);
    CHECK(mc_library_relpath(lib, path) == NULL);
    snprintf(abs, sizeof abs, "%s-other%csong.flac", path, MC_PATH_SEP); /* sibling, same prefix */
    CHECK(mc_library_relpath(lib, abs) == NULL);
    mc_library_close(lib);
  }

  test_identity(argc > 1 ? argv[1] : ".");
  test_migration_v4(argc > 1 ? argv[1] : ".");
  test_migration_v5(argc > 1 ? argv[1] : ".");
  return TEST_RESULT();
}
