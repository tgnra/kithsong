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

/* track_plays as v3 to v5 had it, to take a library back before v6. Its last_played_ms
 * can't simply be dropped while the changelog triggers name it. */
#define V5_TRACK_PLAYS \
  "CREATE TABLE track_plays (" \
  "  uid        TEXT PRIMARY KEY," \
  "  track_uid  TEXT NOT NULL REFERENCES tracks (uid) ON DELETE CASCADE," \
  "  node_uid   TEXT NOT NULL," \
  "  play_count INTEGER NOT NULL DEFAULT 0," \
  "  UNIQUE (track_uid, node_uid)" \
  ") WITHOUT ROWID;"

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
                 "DROP TABLE recents;"
                 "DROP TABLE aliases;"
                 "DROP TABLE merge_keys;"
                 "DROP TABLE smart_lists;"
                 "DROP TABLE track_plays;" V5_TRACK_PLAYS
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
  CHECK(mc_library_schema_version(lib) == 8);
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
                 "DROP TABLE recents;"
                 "DROP TABLE aliases;"
                 "DROP TABLE merge_keys;"
                 "DROP TABLE smart_lists;"
                 "DROP TABLE track_plays;" V5_TRACK_PLAYS
                 "PRAGMA user_version = 4;") == SQLITE_OK);
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  db = mc_library_db(lib);
  CHECK(mc_library_schema_version(lib) == 8);
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

/* Migration v6: smart lists and last played, both logged. Plays from an existing v5 row
 * carry on counting from where they were. */
static void test_migration_v6(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_v6.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "DROP TABLE recents;"
                 "DROP TABLE aliases;"
                 "DROP TABLE merge_keys;"
                 "DROP TABLE smart_lists;"
                 "DROP TABLE track_plays;" V5_TRACK_PLAYS
                 "PRAGMA user_version = 5;") == SQLITE_OK);
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  db = mc_library_db(lib);
  CHECK(mc_library_schema_version(lib) == 8);
  CHECK(has_column(db, "smart_lists", "rule"));
  CHECK(has_column(db, "track_plays", "last_played_ms"));

  CHECK(exec(db, "INSERT INTO tracks (uid, title) VALUES ('t1', 'Starfire'), ('t2', 'Lion');"
                 "INSERT INTO track_plays (uid, track_uid, node_uid, play_count)"
                 "  VALUES ('p1', 't1', mc_node_uid(), 4), ('p2', 't1', 'other-node', 9);") == SQLITE_OK);
  CHECK(mc_library_record_play(lib, "t1") == MC_OK);
  CHECK(mc_library_record_play(lib, "t2") == MC_OK);
  CHECK(mc_library_record_play(lib, "t2") == MC_OK);
  CHECK(mc_library_record_play(lib, "nope") == MC_ERR_NOT_FOUND);
  /* Only this device's row moves; the other device's is left for it to write. */
  CHECK(query_int(db, "SELECT play_count FROM track_plays WHERE uid = 'p1'") == 5);
  CHECK(query_int(db, "SELECT play_count FROM track_plays WHERE uid = 'p2'") == 9);
  CHECK(query_int(db, "SELECT last_played_ms IS NULL FROM track_plays WHERE uid = 'p2'") == 1);
  CHECK(query_int(db, "SELECT play_count FROM track_plays WHERE track_uid = 't2'") == 2);
  CHECK(query_int(db, "SELECT count(*) FROM track_plays WHERE last_played_ms > 0"
                      " AND node_uid = mc_node_uid()") == 2);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'track_plays'"
                      " AND row_uid = 'p1' AND field = 'last_played_ms'") == 1);
  mc_library_close(lib);
}

/* Uids of the recents in order, joined with spaces, into out. */
static void recent_uids(mc_library *lib, int limit, char *out, size_t n) {
  out[0] = '\0';
  mc_recent_list *list = NULL;
  if (mc_library_list_recents(lib, limit, 0, &list) != MC_OK || !list) return;
  for (int i = 0; i < list->count; i++) {
    if (i) strncat(out, " ", n - strlen(out) - 1);
    strncat(out, list->items[i].uid, n - strlen(out) - 1);
  }
  mc_recent_list_free(list);
}

/* Migration v7 and recents: one entry per item, newest open first, logged for sync, and
 * items that are gone left out. */
static void test_recents(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_recents.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "DROP TABLE recents; DROP TABLE aliases; DROP TABLE merge_keys;"
                 "PRAGMA user_version = 6;") == SQLITE_OK);
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  db = mc_library_db(lib);
  CHECK(mc_library_schema_version(lib) == 8);
  CHECK(has_column(db, "recents", "opened_ms"));

  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('ar1', 'Low');"
                 "INSERT INTO albums (uid, title, artist_uid) VALUES ('al1', 'Things', 'ar1'),"
                 " ('al2', 'Untitled', NULL);"
                 "INSERT INTO playlists (uid, name) VALUES ('p1', 'Mix');"
                 "INSERT INTO album_groups (uid, name) VALUES ('g1', 'Late');"
                 "INSERT INTO smart_lists (uid, name, subject, rule) VALUES"
                 " ('s1', 'Loved', 'tracks', '{}'), ('s2', 'New', 'albums', '{}');") == SQLITE_OK);

  /* Each open is a millisecond or two apart, so the order is by time alone. */
  const struct {
    mc_recent_kind kind;
    const char *uid;
  } opens[] = {{MC_RECENT_ALBUM, "al1"},    {MC_RECENT_ARTIST, "ar1"},      {MC_RECENT_PLAYLIST, "p1"},
               {MC_RECENT_PLAYLIST, "s1"},  {MC_RECENT_ALBUM_GROUP, "g1"},  {MC_RECENT_ALBUM_GROUP, "s2"},
               {MC_RECENT_ALBUM, "al2"},    {MC_RECENT_ALBUM, "al1"}};
  for (size_t i = 0; i < sizeof opens / sizeof opens[0]; i++) {
    mc_sleep_ms(2);
    CHECK(mc_library_record_opened(lib, opens[i].kind, opens[i].uid) == MC_OK);
  }
  char uids[256];
  recent_uids(lib, 0, uids, sizeof uids);
  CHECK(strcmp(uids, "al1 al2 s2 g1 s1 p1 ar1") == 0);
  recent_uids(lib, 3, uids, sizeof uids);
  CHECK(strcmp(uids, "al1 al2 s2") == 0);

  mc_recent_list *list = NULL;
  CHECK(mc_library_list_recents(lib, 0, 0, &list) == MC_OK && list && list->count == 7);
  if (list && list->count == 7) {
    CHECK(list->items[0].kind == MC_RECENT_ALBUM);
    CHECK_STR(list->items[0].name, "Things");
    CHECK_STR(list->items[0].artist, "Low");
    CHECK(list->items[1].artist == NULL);
    CHECK(list->items[2].kind == MC_RECENT_ALBUM_GROUP && list->items[2].is_smart);
    CHECK_STR(list->items[2].name, "New");
    CHECK(list->items[3].kind == MC_RECENT_ALBUM_GROUP && !list->items[3].is_smart);
    CHECK(list->items[4].kind == MC_RECENT_PLAYLIST && list->items[4].is_smart);
    CHECK(list->items[6].kind == MC_RECENT_ARTIST);
    CHECK_STR(list->items[6].name, "Low");
    CHECK(list->items[0].opened_ms > list->items[1].opened_ms);
    CHECK(list->items[0].opened_ms >= (int64_t)mc_wall_ms() - 60000);
  }
  mc_recent_list_free(list);

  /* Only items that exist, as the kind given. */
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM, "nope") == MC_ERR_NOT_FOUND);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ARTIST, "al1") == MC_ERR_NOT_FOUND);
  CHECK(mc_library_record_opened(lib, MC_RECENT_PLAYLIST, "s2") == MC_ERR_NOT_FOUND);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM_GROUP, "s1") == MC_ERR_NOT_FOUND);
  CHECK(mc_library_record_opened(lib, (mc_recent_kind)9, "al1") == MC_ERR_INVALID_ARG);
  CHECK(query_int(db, "SELECT count(*) FROM recents") == 7);

  /* Reopening only moves the time, and only that is logged. */
  CHECK(changelog_count(db, "entity = 'recents' AND row_uid = 'al1' AND field = 'kind'") == 1);
  CHECK(changelog_count(db, "entity = 'recents' AND row_uid = 'al1' AND field = 'opened_ms'") == 2);

  /* An item that's gone is left out, and one of an unknown kind (from a newer version) too. */
  CHECK(exec(db, "DELETE FROM albums WHERE uid = 'al2';"
                 "DELETE FROM smart_lists WHERE uid = 's1';"
                 "INSERT INTO recents (uid, kind, opened_ms) VALUES ('x1', 'radio', 1)") == SQLITE_OK);
  recent_uids(lib, 0, uids, sizeof uids);
  CHECK(strcmp(uids, "al1 s2 g1 p1 ar1") == 0);

  CHECK(mc_library_remove_recent(lib, "g1") == MC_OK);
  CHECK(mc_library_remove_recent(lib, "g1") == MC_ERR_NOT_FOUND);
  CHECK(changelog_count(db, "entity = 'recents' AND row_uid = 'g1' AND field = '_deleted'") == 1);
  recent_uids(lib, 0, uids, sizeof uids);
  CHECK(strcmp(uids, "al1 s2 p1 ar1") == 0);
  /* Opening it again brings it back, first. */
  mc_sleep_ms(2);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM_GROUP, "g1") == MC_OK);
  recent_uids(lib, 2, uids, sizeof uids);
  CHECK(strcmp(uids, "g1 al1") == 0);

  CHECK(mc_library_clear_recents(lib) == MC_OK);
  CHECK(mc_library_clear_recents(lib) == MC_OK);
  CHECK(query_int(db, "SELECT count(*) FROM recents") == 0);
  recent_uids(lib, 0, uids, sizeof uids);
  CHECK(strcmp(uids, "") == 0);
  mc_library_close(lib);
}

/* Titles of a playlist's tracks in order, joined with spaces, into out. */
static void playlist_titles(mc_library *lib, const char *playlist, char *out, size_t n) {
  out[0] = '\0';
  mc_track_list *tracks = NULL;
  if (mc_library_list_playlist_tracks(lib, playlist, &tracks) != MC_OK || !tracks) return;
  for (int i = 0; i < tracks->count; i++) {
    if (i) strncat(out, " ", n - strlen(out) - 1);
    strncat(out, tracks->items[i].title, n - strlen(out) - 1);
  }
  mc_track_list_free(tracks);
}

/* Playlists: entries keep the order they were put in, a track can be in twice, and every
 * change is logged. */
static void test_playlists(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_playlists.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('ar1', 'Low');"
                 "INSERT INTO albums (uid, title, artist_uid, art_hash) VALUES ('al1', 'Secret Name', 'ar1', 'h');"
                 "INSERT INTO tracks (uid, album_uid, title) VALUES ('t1', 'al1', 'One'), ('t2', 'al1', 'Two'),"
                 " ('t3', NULL, 'Three');"
                 "INSERT INTO files (uid, track_uid, path, format) VALUES"
                 " ('f1', 't1', 'Low/Secret Name/01 One.flac', 'flac');") == SQLITE_OK);

  char mix[MC_UID_STRLEN], other[MC_UID_STRLEN];
  CHECK(mc_playlist_create(lib, "", mix) == MC_ERR_INVALID_ARG);
  CHECK(mc_playlist_create(lib, "Mix", mix) == MC_OK);
  CHECK(mc_playlist_create(lib, "another", other) == MC_OK);
  CHECK(changelog_count(db, "entity = 'playlists' AND field = 'name'") == 2);

  char e1[MC_UID_STRLEN], e2[MC_UID_STRLEN], e3[MC_UID_STRLEN];
  CHECK(mc_playlist_append(lib, mix, "t2", e1) == MC_OK);
  CHECK(mc_playlist_append(lib, mix, "t1", e2) == MC_OK);
  CHECK(mc_playlist_append(lib, mix, "t2", NULL) == MC_OK); /* again */
  CHECK(mc_playlist_insert(lib, mix, "t3", NULL, e3) == MC_OK); /* at the start */
  CHECK(mc_playlist_append(lib, mix, "nope", NULL) == MC_ERR_NOT_FOUND);
  CHECK(mc_playlist_append(lib, "nope", "t1", NULL) == MC_ERR_NOT_FOUND);
  char titles[256];
  playlist_titles(lib, mix, titles, sizeof titles);
  CHECK(strcmp(titles, "Three Two One Two") == 0);

  CHECK(mc_playlist_insert(lib, mix, "t3", e1, NULL) == MC_OK); /* between e1 and e2 */
  playlist_titles(lib, mix, titles, sizeof titles);
  CHECK(strcmp(titles, "Three Two Three One Two") == 0);
  CHECK(mc_playlist_move(lib, e3, e2) == MC_OK);
  playlist_titles(lib, mix, titles, sizeof titles);
  CHECK(strcmp(titles, "Two Three One Three Two") == 0);
  CHECK(mc_playlist_move(lib, e2, NULL) == MC_OK);
  playlist_titles(lib, mix, titles, sizeof titles);
  CHECK(strcmp(titles, "One Two Three Three Two") == 0);
  CHECK(mc_playlist_remove(lib, e1) == MC_OK);
  CHECK(mc_playlist_remove(lib, e1) == MC_ERR_NOT_FOUND);
  playlist_titles(lib, mix, titles, sizeof titles);
  CHECK(strcmp(titles, "One Three Three Two") == 0);
  CHECK(changelog_count(db, "entity = 'playlist_entries' AND field = 'position'") == 5 + 2);

  /* Tracks carry their artist, album and cover folder, for the play queue. */
  mc_track_list *tracks = NULL;
  CHECK(mc_library_list_playlist_tracks(lib, mix, &tracks) == MC_OK && tracks && tracks->count == 4);
  if (tracks && tracks->count == 4) {
    CHECK_STR(tracks->items[0].artist, "Low");
    CHECK_STR(tracks->items[0].album, "Secret Name");
    CHECK(tracks->items[0].cover_dir && strstr(tracks->items[0].cover_dir, "Secret Name") &&
          !strstr(tracks->items[0].cover_dir, "One.flac"));
    CHECK(tracks->items[1].artist == NULL && tracks->items[1].album == NULL);
    CHECK(tracks->items[3].cover_dir == NULL); /* no file here */
    CHECK(strcmp(tracks->items[0].entry_uid, e2) == 0); /* the entries, to move and remove */
    CHECK(tracks->items[1].entry_uid[0] && strcmp(tracks->items[1].entry_uid, tracks->items[2].entry_uid));
  }
  mc_track_list_free(tracks);

  mc_playlist_list *lists = NULL;
  CHECK(mc_library_list_playlists(lib, 0, &lists) == MC_OK && lists && lists->count == 2);
  if (lists && lists->count == 2) {
    CHECK_STR(lists->items[0].name, "another"); /* by name, ignoring case */
    CHECK(lists->items[0].track_count == 0);
    CHECK_STR(lists->items[1].name, "Mix");
    CHECK(lists->items[1].track_count == 4);
  }
  mc_playlist_list_free(lists);

  CHECK(mc_playlist_rename(lib, other, "Zzz") == MC_OK);
  CHECK(mc_playlist_rename(lib, "nope", "Zzz") == MC_ERR_NOT_FOUND);
  CHECK(mc_playlist_delete(lib, mix) == MC_OK);
  CHECK(query_int(db, "SELECT count(*) FROM playlist_entries") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM tracks") == 3);
  CHECK(changelog_count(db, "entity = 'playlist_entries' AND field = '_deleted'") == 5);
  mc_library_close(lib);
}

/* Names of the playlists listed with flags, joined with '|', into out; *hidden gets the
 * number left out. */
static void playlist_names(mc_library *lib, unsigned flags, char *out, size_t n, int *hidden) {
  out[0] = '\0';
  *hidden = -1;
  mc_playlist_list *list = NULL;
  if (mc_library_list_playlists(lib, flags, &list) != MC_OK || !list) return;
  for (int i = 0; i < list->count; i++) {
    if (i) strncat(out, "|", n - strlen(out) - 1);
    strncat(out, list->items[i].name, n - strlen(out) - 1);
  }
  *hidden = list->hidden;
  mc_playlist_list_free(list);
}

static void smart_names(mc_library *lib, unsigned flags, char *out, size_t n, int *hidden) {
  out[0] = '\0';
  *hidden = -1;
  mc_smart_list *list = NULL;
  if (mc_library_list_smart(lib, flags, &list) != MC_OK || !list) return;
  for (int i = 0; i < list->count; i++) {
    if (i) strncat(out, "|", n - strlen(out) - 1);
    strncat(out, list->items[i].name, n - strlen(out) - 1);
  }
  *hidden = list->hidden;
  mc_smart_list_free(list);
}

/* MC_LIST_PLAYABLE: playlists, smart lists and recents with nothing that has a file on this
 * device are left out, by the same test that gives a track its path; and they come back as
 * soon as a file does. */
static void test_playable(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_playable.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  /* t1 has its original here, t3 only a transcode sync brought; t2 and t4 have no file here
   * (an archive device has them, say). al2 has nothing here. */
  CHECK(exec(db, "INSERT INTO albums (uid, title) VALUES ('al1', 'Here'), ('al2', 'Away');"
                 "INSERT INTO tracks (uid, album_uid, title) VALUES ('t1', 'al1', 'One'), ('t2', 'al1', 'Two'),"
                 " ('t3', 'al1', 'Three'), ('t4', 'al2', 'Four');"
                 "INSERT INTO files (uid, track_uid, path, format, source_file_uid, source_peer_uid) VALUES"
                 " ('f1', 't1', 'Here/01 One.flac', 'flac', NULL, NULL),"
                 " ('f3', 't3', 'Here/03 Three.opus', 'opus', 'orig3', 'peer');") == SQLITE_OK);

  char all[MC_UID_STRLEN], partly[MC_UID_STRLEN], away[MC_UID_STRLEN], empty[MC_UID_STRLEN];
  CHECK(mc_playlist_create(lib, "All here", all) == MC_OK);
  CHECK(mc_playlist_create(lib, "Partly", partly) == MC_OK);
  CHECK(mc_playlist_create(lib, "Away", away) == MC_OK);
  CHECK(mc_playlist_create(lib, "Empty", empty) == MC_OK);
  CHECK(mc_playlist_append(lib, all, "t1", NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, partly, "t2", NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, partly, "t3", NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, away, "t2", NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, away, "t4", NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, away, "t2", NULL) == MC_OK);

  /* Without the flag, every playlist, with how many of its entries can play here. */
  mc_playlist_list *lists = NULL;
  CHECK(mc_library_list_playlists(lib, 0, &lists) == MC_OK && lists && lists->count == 4 && lists->hidden == 0);
  if (lists && lists->count == 4) {
    CHECK_STR(lists->items[0].name, "All here");
    CHECK(lists->items[0].track_count == 1 && lists->items[0].playable_count == 1);
    CHECK_STR(lists->items[1].name, "Away");
    CHECK(lists->items[1].track_count == 3 && lists->items[1].playable_count == 0);
    CHECK_STR(lists->items[2].name, "Empty");
    CHECK(lists->items[2].track_count == 0 && lists->items[2].playable_count == 0);
    CHECK_STR(lists->items[3].name, "Partly");
    CHECK(lists->items[3].track_count == 2 && lists->items[3].playable_count == 1);
  }
  mc_playlist_list_free(lists);

  char names[256];
  int hidden;
  playlist_names(lib, MC_LIST_PLAYABLE, names, sizeof names, &hidden);
  CHECK(strcmp(names, "All here|Partly") == 0);
  CHECK(hidden == 2);
  /* The same notion as the track list's: the hidden one's tracks all have no path. */
  mc_track_list *tracks = NULL;
  CHECK(mc_library_list_playlist_tracks(lib, away, &tracks) == MC_OK && tracks && tracks->count == 3);
  for (int i = 0; tracks && i < tracks->count; i++) CHECK(tracks->items[i].path == NULL);
  mc_track_list_free(tracks);

  /* Smart lists: a smart playlist by whether its tracks have files here, a smart album group
   * by whether its albums do, and one whose rule can't be evaluated is left out too. */
  char s_uid[MC_UID_STRLEN];
  CHECK(mc_smart_create(lib, "Ones", MC_SMART_TRACKS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"One\"}}", s_uid) == MC_OK);
  char s_away[MC_UID_STRLEN];
  CHECK(mc_smart_create(lib, "Fours", MC_SMART_TRACKS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"Four\"}}", s_away) == MC_OK);
  CHECK(mc_smart_create(lib, "Nothing", MC_SMART_TRACKS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"Zero\"}}", s_uid) == MC_OK);
  CHECK(mc_smart_create(lib, "Here albums", MC_SMART_ALBUMS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"Here\"}}", s_uid) == MC_OK);
  CHECK(mc_smart_create(lib, "Away albums", MC_SMART_ALBUMS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"Away\"}}", s_uid) == MC_OK);
  CHECK(exec(db, "INSERT INTO smart_lists (uid, name, subject, rule) VALUES ('sx', 'Future', 'tracks', '{\"v\":9}')") ==
        SQLITE_OK);
  smart_names(lib, 0, names, sizeof names, &hidden);
  CHECK(strcmp(names, "Away albums|Fours|Future|Here albums|Nothing|Ones") == 0);
  CHECK(hidden == 0);
  smart_names(lib, MC_LIST_PLAYABLE, names, sizeof names, &hidden);
  CHECK(strcmp(names, "Here albums|Ones") == 0);
  CHECK(hidden == 4);

  /* Recents leave the same ones out with the flag, and still fill the limit. */
  const struct {
    mc_recent_kind kind;
    const char *uid;
  } opens[] = {{MC_RECENT_ALBUM, "al2"}, {MC_RECENT_PLAYLIST, all}, {MC_RECENT_PLAYLIST, s_away},
               {MC_RECENT_PLAYLIST, partly}, {MC_RECENT_PLAYLIST, empty}, {MC_RECENT_PLAYLIST, away}};
  for (size_t i = 0; i < sizeof opens / sizeof opens[0]; i++) {
    mc_sleep_ms(2);
    CHECK(mc_library_record_opened(lib, opens[i].kind, opens[i].uid) == MC_OK);
  }
  mc_recent_list *recents = NULL;
  CHECK(mc_library_list_recents(lib, 0, 0, &recents) == MC_OK && recents && recents->count == 6);
  mc_recent_list_free(recents);
  recents = NULL;
  CHECK(mc_library_list_recents(lib, 0, MC_LIST_PLAYABLE, &recents) == MC_OK && recents && recents->count == 3);
  if (recents && recents->count == 3) {
    CHECK(strcmp(recents->items[0].uid, partly) == 0);
    CHECK(strcmp(recents->items[1].uid, all) == 0);
    CHECK_STR(recents->items[2].uid, "al2"); /* other kinds are kept */
  }
  mc_recent_list_free(recents);
  recents = NULL;
  CHECK(mc_library_list_recents(lib, 2, MC_LIST_PLAYABLE, &recents) == MC_OK && recents && recents->count == 2);
  if (recents && recents->count == 2) CHECK(strcmp(recents->items[1].uid, all) == 0);
  mc_recent_list_free(recents);

  /* A file arriving (sync, an import) brings a playlist back; one going (a delete, sync
   * removing it) takes it away. Listing again is all it takes. */
  CHECK(exec(db, "INSERT INTO files (uid, track_uid, path, format) VALUES ('f4', 't4', 'Away/04 Four.flac', 'flac')") ==
        SQLITE_OK);
  CHECK(exec(db, "DELETE FROM files WHERE uid = 'f1'") == SQLITE_OK);
  playlist_names(lib, MC_LIST_PLAYABLE, names, sizeof names, &hidden);
  CHECK(strcmp(names, "Away|Partly") == 0);
  CHECK(hidden == 2);
  smart_names(lib, MC_LIST_PLAYABLE, names, sizeof names, &hidden);
  CHECK(strcmp(names, "Away albums|Fours|Here albums") == 0);
  /* A track that's gone takes its entries with it. */
  CHECK(exec(db, "DELETE FROM tracks WHERE uid = 't3'") == SQLITE_OK);
  playlist_names(lib, MC_LIST_PLAYABLE, names, sizeof names, &hidden);
  CHECK(strcmp(names, "Away") == 0);
  CHECK(hidden == 3);

  CHECK(mc_library_list_playlists(NULL, MC_LIST_PLAYABLE, &lists) == MC_ERR_INVALID_ARG);
  mc_library_close(lib);
}

/* Artists and album groups for the library's browse views. */
static void test_artists_and_groups(const char *scratch) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_library_artists.d", scratch);
  remove_db(path);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);

  /* An empty library has neither. */
  mc_artist_list *artists = NULL;
  CHECK(mc_library_list_artists(lib, &artists) == MC_OK && artists && artists->count == 0);
  mc_artist_list_free(artists);
  mc_album_group_list *groups = NULL;
  CHECK(mc_library_list_album_groups(lib, &groups) == MC_OK && groups && groups->count == 0);
  mc_album_group_list_free(groups);
  mc_artist_list_free(NULL);
  mc_album_group_list_free(NULL);

  /* The Beatles sort under B, Low's singles count as an album, Guest is only on a track and
   * Nobody is on nothing at all. */
  CHECK(exec(db, "INSERT INTO artists (uid, name, sort_name) VALUES"
                 " ('ar1', 'The Beatles', 'Beatles, The'), ('ar2', 'Low', NULL),"
                 " ('ar3', 'Guest', NULL), ('ar4', 'abba', NULL), ('ar5', 'Nobody', NULL);"
                 "INSERT INTO albums (uid, title, artist_uid, kind) VALUES"
                 " ('al1', 'Abbey Road', 'ar1', 'album'), ('al2', 'Help!', 'ar1', 'album'),"
                 " ('al3', 'Secret Name', 'ar2', 'album'), ('al4', 'Low Singles', 'ar2', 'singles'),"
                 " ('al5', 'Arrival', 'ar4', 'album');"
                 "INSERT INTO tracks (uid, album_uid, artist_uid, title) VALUES"
                 " ('t1', 'al3', 'ar3', 'Duet');") == SQLITE_OK);
  artists = NULL;
  CHECK(mc_library_list_artists(lib, &artists) == MC_OK && artists && artists->count == 4);
  if (artists && artists->count == 4) {
    CHECK_STR(artists->items[0].name, "abba"); /* by sort name, ignoring case */
    CHECK(artists->items[0].album_count == 1);
    CHECK_STR(artists->items[1].name, "The Beatles");
    CHECK(strcmp(artists->items[1].uid, "ar1") == 0);
    CHECK(artists->items[1].album_count == 2);
    CHECK_STR(artists->items[2].name, "Guest");
    CHECK(artists->items[2].album_count == 0);
    CHECK_STR(artists->items[3].name, "Low");
    CHECK(artists->items[3].album_count == 2);
  }
  mc_artist_list_free(artists);

  /* Groups by name, ignoring case, each with its number of albums. */
  CHECK(exec(db, "INSERT INTO album_groups (uid, name) VALUES ('g1', 'road trip'), ('g2', 'Empty'),"
                 " ('g3', 'Beatles');"
                 "INSERT INTO album_group_entries (uid, group_uid, album_uid, position) VALUES"
                 " ('e1', 'g1', 'al1', 'a0'), ('e2', 'g1', 'al3', 'a1'), ('e3', 'g1', 'al5', 'a2'),"
                 " ('e4', 'g3', 'al1', 'a0'), ('e5', 'g3', 'al2', 'a1');"
                 "INSERT INTO smart_lists (uid, name, subject, rule) VALUES"
                 " ('s1', 'Recent', 'albums', '{}');") == SQLITE_OK);
  groups = NULL;
  CHECK(mc_library_list_album_groups(lib, &groups) == MC_OK && groups && groups->count == 3);
  if (groups && groups->count == 3) {
    CHECK_STR(groups->items[0].name, "Beatles");
    CHECK(groups->items[0].album_count == 2);
    CHECK_STR(groups->items[1].name, "Empty");
    CHECK(groups->items[1].album_count == 0);
    CHECK_STR(groups->items[2].name, "road trip");
    CHECK(strcmp(groups->items[2].uid, "g1") == 0);
    CHECK(groups->items[2].album_count == 3);
  }
  mc_album_group_list_free(groups);
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

  CHECK(mc_library_schema_version(lib) == 8);
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
    CHECK(tracks->items[0].entry_uid[0] == '\0');
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
    CHECK(mc_library_schema_version(lib) == 8);
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
  test_migration_v6(argc > 1 ? argv[1] : ".");
  test_recents(argc > 1 ? argv[1] : ".");
  test_playlists(argc > 1 ? argv[1] : ".");
  test_playable(argc > 1 ? argv[1] : ".");
  test_artists_and_groups(argc > 1 ? argv[1] : ".");
  return TEST_RESULT();
}
