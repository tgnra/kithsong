#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"

#define NOW 1700000000000LL
#define DAY 86400000LL

static uint64_t fixed_clock(void) {
  return (uint64_t)NOW;
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

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static char g_out[1024];

/* The titles a track rule selects, joined with '|', or "ERR <status>". */
static const char *tracks(mc_library *lib, const char *rule) {
  mc_track_list *list = NULL;
  mc_status st = mc_smart_preview_tracks(lib, rule, &list);
  g_out[0] = '\0';
  if (st != MC_OK) {
    snprintf(g_out, sizeof g_out, "ERR %d", st);
    return g_out;
  }
  for (int i = 0; i < list->count; i++) {
    if (i) strncat(g_out, "|", sizeof g_out - strlen(g_out) - 1);
    strncat(g_out, list->items[i].title, sizeof g_out - strlen(g_out) - 1);
  }
  mc_track_list_free(list);
  return g_out;
}

static const char *albums(mc_library *lib, const char *rule) {
  mc_album_list *list = NULL;
  mc_status st = mc_smart_preview_albums(lib, rule, &list);
  g_out[0] = '\0';
  if (st != MC_OK) {
    snprintf(g_out, sizeof g_out, "ERR %d", st);
    return g_out;
  }
  for (int i = 0; i < list->count; i++) {
    if (i) strncat(g_out, "|", sizeof g_out - strlen(g_out) - 1);
    strncat(g_out, list->items[i].title, sizeof g_out - strlen(g_out) - 1);
  }
  mc_album_list_free(list);
  return g_out;
}

#define CHECK_TRACKS(rule, want) CHECK_STR(tracks(lib, rule), want)
#define CHECK_ALBUMS(rule, want) CHECK_STR(albums(lib, rule), want)

/* Abba and The Beatles (sorted as "Beatles, The") have an album each; Low has an old singles
 * album (kind 'singles', which means nothing now) of one track, so a single. Plays: Come
 * Together 7 over two devices, Dancing Queen 3, Money, Money 1. */
static void populate(sqlite3 *db) {
  char sql[4096];
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name, sort_name) VALUES ('ar1', 'The Beatles', 'Beatles, The'),"
           " ('ar2', 'Abba', NULL), ('ar3', 'Low', NULL);"
           "INSERT INTO albums (uid, title, artist_uid, year, kind) VALUES"
           " ('al1', 'Abbey Road', 'ar1', 1969, 'album'), ('al2', 'Arrival', 'ar2', 1976, 'album'),"
           " ('al3', 'Low Singles', 'ar3', NULL, 'singles');"
           "INSERT INTO tracks (uid, album_uid, title, track_no, duration_ms, genre, year, added_ms) VALUES"
           " ('t1', 'al1', 'Come Together', 1, 259000, 'Rock', NULL, 1000),"
           " ('t2', 'al1', 'Something', 2, 182000, 'Rock', NULL, 1000),"
           " ('t3', 'al2', 'Dancing Queen', 2, 230000, 'Pop', NULL, %lld),"
           " ('t4', 'al2', 'Money, Money', 3, 185000, 'Pop', NULL, %lld),"
           " ('t5', 'al3', 'Words', NULL, 200000, NULL, 2001, NULL);"
           "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES"
           " ('p1', 't1', 'nodeA', 5, %lld), ('p2', 't1', 'nodeB', 2, 1000),"
           " ('p3', 't3', 'nodeA', 3, %lld), ('p4', 't4', 'nodeA', 1, %lld);"
           "INSERT INTO files (uid, track_uid, path, format) VALUES"
           " ('f1', 't1', 'Beatles/Abbey Road/01.flac', 'flac'), ('f3', 't3', 'Abba/Arrival/02.opus', 'opus');"
           "INSERT INTO playlists (uid, name) VALUES ('pl1', 'Mix');"
           "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position) VALUES ('e1', 'pl1', 't2', 'a0');"
           "INSERT INTO album_groups (uid, name) VALUES ('g1', 'Swedes');"
           "INSERT INTO album_group_entries (uid, group_uid, album_uid, position) VALUES ('ge1', 'g1', 'al2', 'a0');",
           NOW - 5 * DAY, NOW - 40 * DAY, NOW - DAY, NOW - 200 * DAY, NOW - 10 * DAY);
  CHECK(exec(db, sql) == SQLITE_OK);
}

static void test_track_rules(mc_library *lib) {
  /* No match: everything, as the album grid orders it. */
  CHECK_TRACKS("{\"v\":1}", "Dancing Queen|Money, Money|Come Together|Something|Words");

  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"contains\",\"value\":\"RO\"}}",
               "Come Together|Something");
  /* is_not and not_contains include tracks with no genre. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"is_not\",\"value\":\"rock\"}}",
               "Dancing Queen|Money, Money|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"not_contains\",\"value\":\"o\"}}", "Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"empty\"}}", "Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"starts_with\",\"value\":\"so\"}}", "Something");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"artist\",\"op\":\"is\",\"value\":\"the beatles\"}}",
               "Come Together|Something");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"album\",\"op\":\"is\",\"value\":\"Arrival\"}}",
               "Dancing Queen|Money, Money");

  /* Numbers; duration is in seconds. != includes tracks with no year. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"<\",\"value\":3}}", "Money, Money|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\">=\",\"value\":7}}", "Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"duration\",\"op\":\">\",\"value\":200}}",
               "Dancing Queen|Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"duration\",\"op\":\"between\",\"value\":[180,190]}}",
               "Money, Money|Something");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"year\",\"op\":\"=\",\"value\":2001}}", "Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"year\",\"op\":\"!=\",\"value\":1969}}",
               "Dancing Queen|Money, Money|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"track_no\",\"op\":\"empty\"}}", "Words");

  /* Dates, against the fixed clock. Never played counts as not played within. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"last_played\",\"op\":\"within\",\"value\":{\"days\":30}}}",
               "Money, Money|Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":30}}}",
               "Dancing Queen|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"not\":{\"field\":\"last_played\",\"op\":\"within\",\"value\":{\"days\":30}}}}",
               "Dancing Queen|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}}",
               "Dancing Queen");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"after\",\"value\":500}}",
               "Dancing Queen|Money, Money|Come Together|Something");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"before\",\"value\":5000}}", "Come Together|Something");

  /* Conditions on the track's album, files, playlists and groups. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"on_device\",\"op\":\"is\",\"value\":false}}",
               "Money, Money|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"format\",\"op\":\"is\",\"value\":\"FLAC\"}}", "Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"format\",\"op\":\"is_not\",\"value\":\"flac\"}}",
               "Dancing Queen|Money, Money|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"playlist\",\"op\":\"is\",\"value\":\"pl1\"}}", "Something");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"group\",\"op\":\"is\",\"value\":\"g1\"}}",
               "Dancing Queen|Money, Money");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"group\",\"op\":\"is\",\"value\":\"gone\"}}", "");

  /* Groups. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"any\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"pop\"},"
               "{\"not\":{\"field\":\"on_device\",\"op\":\"is\",\"value\":false}}]}}",
               "Dancing Queen|Money, Money|Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"all\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"pop\"},"
               "{\"field\":\"plays\",\"op\":\">\",\"value\":1}]}}",
               "Dancing Queen");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"all\":[]}}", "Dancing Queen|Money, Money|Come Together|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"any\":[]}}", "");

  /* Values are bound, never spliced into the SQL. */
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"x' OR 1=1 --\"}}", "");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"contains\",\"value\":\"%\"}}", "");
}

static void test_order(mc_library *lib) {
  /* The 2 least played: ties fall back to album order. */
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"}],\"limit\":2}", "Something|Words");
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"plays\",\"dir\":\"desc\"}],\"limit\":1}", "Come Together");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"is\",\"value\":\"pop\"},"
               "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"}],\"limit\":10}",
               "Money, Money|Dancing Queen");

  /* Artist sorts by sort name: "Beatles, The" before Low. */
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"artist\",\"dir\":\"desc\"}]}",
               "Words|Come Together|Something|Dancing Queen|Money, Money");
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"title\"}]}",
               "Come Together|Dancing Queen|Money, Money|Something|Words");
  /* Missing values go last either way. */
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}",
               "Dancing Queen|Money, Money|Come Together|Something|Words");
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"added\",\"dir\":\"asc\"}]}",
               "Come Together|Something|Money, Money|Dancing Queen|Words");
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"genre\"},{\"field\":\"plays\",\"dir\":\"desc\"}]}",
               "Dancing Queen|Money, Money|Come Together|Something|Words");

  /* Random: the same seed gives the same order; every track is still there. */
  char first[1024];
  snprintf(first, sizeof first, "%s", tracks(lib, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":42}]}"));
  CHECK(strcmp(tracks(lib, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":42}]}"), first) == 0);
  CHECK(strlen(first) == strlen("Dancing Queen|Money, Money|Come Together|Something|Words"));
  int differs = 0;
  for (int seed = 1; seed < 20 && !differs; seed++) {
    char rule[128];
    snprintf(rule, sizeof rule, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":%d}]}", seed);
    differs = strcmp(tracks(lib, rule), first) != 0;
  }
  CHECK(differs);
  CHECK_TRACKS("{\"v\":1,\"order\":[{\"field\":\"plays\",\"dir\":\"desc\"},{\"field\":\"random\",\"seed\":7}],"
               "\"limit\":1}",
               "Come Together");
}

static void test_album_rules(mc_library *lib) {
  CHECK_ALBUMS("{\"v\":1}", "Arrival|Abbey Road|Low Singles");
  CHECK_ALBUMS("{\"v\":1,\"order\":[{\"field\":\"plays\",\"dir\":\"desc\"}],\"limit\":2}", "Abbey Road|Arrival");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}}", "Arrival");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"is\",\"value\":\"pop\"}}", "Arrival");
  /* No track of the album is rock. */
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"is_not\",\"value\":\"rock\"}}", "Arrival|Low Singles");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"track_count\",\"op\":\"=\",\"value\":2}}", "Arrival|Abbey Road");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"duration\",\"op\":\">\",\"value\":430}}", "Abbey Road");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"group\",\"op\":\"is\",\"value\":\"g1\"}}", "Arrival");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}}", "Arrival|Abbey Road");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"last_played\",\"op\":\"empty\"}}", "Low Singles");
  CHECK_ALBUMS("{\"v\":1,\"order\":[{\"field\":\"artist\"}]}", "Arrival|Abbey Road|Low Singles");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "Low Singles");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":false}}", "Arrival|Abbey Road");
}

/* A single is a track alone on its album, or one with no album; an album is one when it has
 * one track. Kinds don't count. */
static void test_singles(mc_library *lib) {
  sqlite3 *db = mc_library_db(lib);
  CHECK(exec(db, "INSERT INTO tracks (uid, album_uid, artist_uid, title) VALUES ('t6', NULL, 'ar3', 'Loose');"
                 "INSERT INTO albums (uid, title, artist_uid, kind) VALUES ('al4', 'Two Singles', 'ar3', 'singles');"
                 "INSERT INTO tracks (uid, album_uid, title) VALUES ('t7', 'al4', 'One'), ('t8', 'al4', 'Two');") ==
        SQLITE_OK);
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "Loose|Words");
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":false}}",
               "Dancing Queen|Money, Money|Come Together|Something|One|Two");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "Low Singles");
  /* Once Words has company, Low Singles is an album. */
  CHECK(exec(db, "UPDATE tracks SET album_uid = 'al3' WHERE uid = 't6'") == SQLITE_OK);
  CHECK_TRACKS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "");
  CHECK_ALBUMS("{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":true}}", "");
  CHECK(exec(db, "DELETE FROM tracks WHERE uid IN ('t6', 't7', 't8'); DELETE FROM albums WHERE uid = 'al4';") ==
        SQLITE_OK);
}

static void test_invalid(void) {
  static const char *const bad[] = {
    "",
    "not json",
    "[]",
    "{}",
    "{\"v\":2}",
    "{\"v\":\"1\"}",
    "{\"v\":1,\"extra\":1}",
    "{\"v\":1,\"limit\":0}",
    "{\"v\":1,\"limit\":\"10\"}",
    "{\"v\":1,\"match\":[]}",
    "{\"v\":1,\"match\":{\"field\":\"nope\",\"op\":\"is\",\"value\":\"x\"}}",
    "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"nope\",\"value\":\"x\"}}",
    "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":3}}",
    "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\"}}",
    "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"empty\",\"value\":\"x\"}}",
    "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"x\",\"typo\":1}}",
    "{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"<\",\"value\":\"3\"}}",
    "{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"<\",\"value\":1.5}}",
    "{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"between\",\"value\":[1]}}",
    "{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":30}}",
    "{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":-1}}}",
    "{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":{\"weeks\":1}}}",
    "{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":1}}",
    "{\"v\":1,\"match\":{\"field\":\"playlist\",\"op\":\"contains\",\"value\":\"x\"}}",
    "{\"v\":1,\"match\":{\"field\":\"track_count\",\"op\":\"=\",\"value\":1}}", /* albums only */
    "{\"v\":1,\"match\":{\"all\":{}}}",
    "{\"v\":1,\"match\":{\"all\":[],\"any\":[]}}",
    "{\"v\":1,\"match\":{\"not\":[]}}",
    "{\"v\":1,\"order\":{}}",
    "{\"v\":1,\"order\":[{\"field\":\"singles\"}]}",
    "{\"v\":1,\"order\":[{\"field\":\"title\",\"dir\":\"up\"}]}",
    "{\"v\":1,\"order\":[{\"field\":\"random\"}]}",
    "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":1},{\"field\":\"title\"}]}",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    mc_status st = mc_smart_validate(MC_SMART_TRACKS, bad[i]);
    if (st != MC_ERR_FORMAT) fprintf(stderr, "accepted: %s\n", bad[i]);
    CHECK(st == MC_ERR_FORMAT);
  }
  CHECK(mc_smart_validate(MC_SMART_TRACKS, NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_smart_validate(MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"album\",\"op\":\"empty\"}}") ==
        MC_ERR_FORMAT);
  CHECK(mc_smart_validate(MC_SMART_ALBUMS, "{\"v\":1,\"order\":[{\"field\":\"genre\"}]}") == MC_ERR_FORMAT);

  /* Nesting is capped. */
  char deep[1024] = "{\"v\":1,\"match\":";
  for (int i = 0; i < 40; i++) strcat(deep, "{\"not\":");
  strcat(deep, "{\"field\":\"title\",\"op\":\"empty\"}");
  for (int i = 0; i < 40; i++) strcat(deep, "}");
  strcat(deep, "}");
  CHECK(mc_smart_validate(MC_SMART_TRACKS, deep) == MC_ERR_FORMAT);
}

static void test_fields(void) {
  int n = mc_smart_field_count(MC_SMART_TRACKS);
  CHECK(n > 10);
  int has_album = 0, has_count = 0;
  mc_smart_field f;
  for (int i = 0; i < n; i++) {
    CHECK(mc_smart_field_get(MC_SMART_TRACKS, i, &f));
    CHECK(f.name && f.type && f.ops && f.ops[0]);
    has_album |= strcmp(f.name, "album") == 0;
    has_count |= strcmp(f.name, "track_count") == 0;
    /* Every field and its first op make a rule that's accepted, or is refused only for its value. */
    char rule[256];
    snprintf(rule, sizeof rule, "{\"v\":1,\"match\":{\"field\":\"%s\",\"op\":\"%s\",\"value\":null}}", f.name,
             f.ops[0]);
    CHECK(mc_smart_validate(MC_SMART_TRACKS, rule) == MC_ERR_FORMAT);
    if (f.sortable) {
      snprintf(rule, sizeof rule, "{\"v\":1,\"order\":[{\"field\":\"%s\"}]}", f.name);
      CHECK(mc_smart_validate(MC_SMART_TRACKS, rule) == MC_OK);
    }
  }
  CHECK(has_album && !has_count);
  CHECK(!mc_smart_field_get(MC_SMART_TRACKS, n, &f));
  for (int i = 0; mc_smart_field_get(MC_SMART_ALBUMS, i, &f); i++)
    if (strcmp(f.name, "genre") == 0) CHECK(!f.sortable && strcmp(f.type, "text") == 0);
}

/* The titles saved smart list uid holds now, joined with '|', or "ERR <status>". */
static const char *saved(mc_library *lib, const char *uid, mc_smart_subject subject) {
  g_out[0] = '\0';
  mc_track_list *t = NULL;
  mc_album_list *a = NULL;
  mc_status st = subject == MC_SMART_TRACKS ? mc_library_list_smart_tracks(lib, uid, &t)
                                            : mc_library_list_smart_albums(lib, uid, &a);
  if (st != MC_OK) {
    snprintf(g_out, sizeof g_out, "ERR %d", st);
    return g_out;
  }
  for (int i = 0, n = t ? t->count : a->count; i < n; i++) {
    if (i) strncat(g_out, "|", sizeof g_out - strlen(g_out) - 1);
    strncat(g_out, t ? t->items[i].title : a->items[i].title, sizeof g_out - strlen(g_out) - 1);
  }
  mc_track_list_free(t);
  mc_album_list_free(a);
  return g_out;
}

/* v10's example lists (library.c), then gone so the rest start from none. Random ones are
 * compared by length, as their order depends on the seed. */
static void test_examples(mc_library *lib) {
  char uid[MC_UID_STRLEN];
#define EXAMPLE(n) (snprintf(uid, sizeof uid, "5e1ec7ed-0000-4000-8000-00000000000%d", n), uid)
  CHECK_STR(saved(lib, EXAMPLE(1), MC_SMART_TRACKS), "Dancing Queen|Money, Money|Come Together|Something");
  CHECK_STR(saved(lib, EXAMPLE(2), MC_SMART_TRACKS), "Come Together|Dancing Queen|Money, Money");
  CHECK(strlen(saved(lib, EXAMPLE(3), MC_SMART_TRACKS)) == strlen("Something|Words"));
  CHECK_STR(saved(lib, EXAMPLE(4), MC_SMART_ALBUMS), "Arrival|Abbey Road");
  CHECK(strlen(saved(lib, EXAMPLE(5), MC_SMART_ALBUMS)) == strlen("Abbey Road|Arrival"));
#undef EXAMPLE
  lib->applying_remote = 1;
  CHECK(exec(mc_library_db(lib), "DELETE FROM smart_lists") == SQLITE_OK);
  lib->applying_remote = 0;
}

static void test_saved(mc_library *lib) {
  sqlite3 *db = mc_library_db(lib);
  char least[MC_UID_STRLEN], recent[MC_UID_STRLEN], shuffled[MC_UID_STRLEN];
  CHECK(mc_smart_create(lib, "", MC_SMART_TRACKS, "{\"v\":1}", least) == MC_ERR_INVALID_ARG);
  CHECK(mc_smart_create(lib, "Bad", MC_SMART_TRACKS, "{\"v\":9}", least) == MC_ERR_FORMAT);
  CHECK(query_int(db, "SELECT count(*) FROM smart_lists") == 0);
  CHECK(mc_smart_create(lib, "Least played", MC_SMART_TRACKS,
                        "{\"v\":1,\"order\":[{\"field\":\"plays\"}],\"limit\":2}", least) == MC_OK);
  CHECK(mc_smart_create(lib, "recently added", MC_SMART_ALBUMS,
                        "{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}}",
                        recent) == MC_OK);
  CHECK(mc_smart_create(lib, "Shuffle", MC_SMART_TRACKS, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":1}]}",
                        shuffled) == MC_OK);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'smart_lists' AND field = 'rule'") == 3);

  mc_track_list *t = NULL;
  CHECK(mc_library_list_smart_tracks(lib, least, &t) == MC_OK && t && t->count == 2);
  if (t && t->count == 2) CHECK_STR(t->items[0].title, "Something");
  mc_track_list_free(t);
  mc_album_list *a = NULL;
  CHECK(mc_library_list_smart_albums(lib, recent, &a) == MC_OK && a && a->count == 1);
  if (a && a->count == 1) CHECK_STR(a->items[0].title, "Arrival");
  mc_album_list_free(a);
  CHECK(mc_library_list_smart_albums(lib, least, &a) == MC_ERR_INVALID_ARG && !a);
  CHECK(mc_library_list_smart_tracks(lib, "nope", &t) == MC_ERR_NOT_FOUND && !t);

  /* A bad rule leaves the old one alone. */
  CHECK(mc_smart_set_rule(lib, least, "{\"v\":1,\"limit\":-1}") == MC_ERR_FORMAT);
  CHECK(mc_smart_set_rule(lib, recent, "{\"v\":1,\"match\":{\"field\":\"track_no\",\"op\":\"empty\"}}") ==
        MC_ERR_FORMAT); /* tracks only */
  CHECK(mc_smart_set_rule(lib, least, "{\"v\":1,\"order\":[{\"field\":\"plays\"}],\"limit\":1}") == MC_OK);
  CHECK(mc_smart_set_rule(lib, "nope", "{\"v\":1}") == MC_ERR_NOT_FOUND);
  CHECK(mc_library_list_smart_tracks(lib, least, &t) == MC_OK && t && t->count == 1);
  mc_track_list_free(t);

  CHECK(mc_smart_reshuffle(lib, least) == MC_ERR_INVALID_ARG);
  int before = query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'smart_lists' AND field = 'rule'");
  CHECK(mc_smart_reshuffle(lib, shuffled) == MC_OK);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'smart_lists' AND field = 'rule'") ==
        before + 1);
  CHECK(query_int(db, "SELECT count(*) FROM smart_lists WHERE rule LIKE '%\"seed\":1}%'") == 0);
  CHECK(mc_library_list_smart_tracks(lib, shuffled, &t) == MC_OK && t && t->count == 5);
  mc_track_list_free(t);

  CHECK(mc_smart_rename(lib, recent, "New albums") == MC_OK);
  CHECK(mc_smart_rename(lib, "nope", "x") == MC_ERR_NOT_FOUND);
  /* A kind of list from a newer app isn't listed. */
  CHECK(exec(db, "INSERT INTO smart_lists (uid, name, subject, rule) VALUES ('s9', 'Future', 'artists', '{}')") ==
        SQLITE_OK);
  mc_smart_list *lists = NULL;
  CHECK(mc_library_list_smart(lib, 0, &lists) == MC_OK && lists && lists->count == 3);
  if (lists && lists->count == 3) {
    CHECK_STR(lists->items[0].name, "Least played");
    CHECK_STR(lists->items[1].name, "New albums");
    CHECK(lists->items[1].subject == MC_SMART_ALBUMS);
    CHECK(strstr(lists->items[1].rule, "within") != NULL);
    CHECK_STR(lists->items[2].name, "Shuffle");
  }
  mc_smart_list_free(lists);
  CHECK(mc_library_list_smart_tracks(lib, "s9", &t) == MC_ERR_FORMAT);

  CHECK(mc_smart_delete(lib, least) == MC_OK);
  CHECK(mc_smart_delete(lib, least) == MC_ERR_NOT_FOUND);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'smart_lists' AND field = '_deleted'") == 1);
}

/* A rule as mc_smart_describe() puts it, or "ERR <status>". */
static const char *describe(mc_library *lib, mc_smart_subject subject, const char *rule) {
  char *s = NULL;
  mc_status st = mc_smart_describe(lib, subject, rule, &s);
  if (st != MC_OK) snprintf(g_out, sizeof g_out, "ERR %d", st);
  else snprintf(g_out, sizeof g_out, "%s", s);
  mc_free(s);
  return g_out;
}

/* A track rule matching cond, described. */
static const char *describe_cond(mc_library *lib, const char *cond) {
  char rule[1024];
  snprintf(rule, sizeof rule, "{\"v\":1,\"match\":%s}", cond);
  return describe(lib, MC_SMART_TRACKS, rule);
}

#define CHECK_DESC(cond, want) CHECK_STR(describe_cond(lib, cond), "Tracks where " want)
#define GENRE(op, v) "{\"field\":\"genre\",\"op\":\"" op "\",\"value\":\"" v "\"}"
#define PLAYS(op, v) "{\"field\":\"plays\",\"op\":\"" op "\",\"value\":" #v "}"
#define DEVICE "{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}"

static void test_describe(mc_library *lib) {
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "{\"v\":1}"), "All tracks");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1}"), "All albums");
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"all\":[]}}"), "All tracks");
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"any\":[]}}"), "No tracks");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"not\":{\"all\":[{\"all\":[]}]}}}"), "No albums");

  /* docs/schema.md's example. */
  CHECK_STR(describe(lib, MC_SMART_TRACKS,
                     "{\"v\":1,\"match\":{\"all\":[" GENRE("contains", "jazz") ",{\"any\":[" PLAYS("<", 3)
                     ",{\"not\":{\"field\":\"last_played\",\"op\":\"within\",\"value\":{\"days\":180}}}]}]},"
                     "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"added\",\"dir\":\"desc\"}],"
                     "\"limit\":10}"),
            "Tracks where genre contains “jazz” and (play count is less than 3 or last played isn't in the last"
            " 180 days) · least played first, then newest added first · 10 tracks");

  /* Text, and "not" turning into the negative op. */
  CHECK_DESC(GENRE("is", "Rock"), "genre is “Rock”");
  CHECK_DESC(GENRE("is_not", "Rock"), "genre isn't “Rock”");
  CHECK_DESC("{\"not\":" GENRE("is", "Rock") "}", "genre isn't “Rock”");
  CHECK_DESC(GENRE("contains", "o"), "genre contains “o”");
  CHECK_DESC(GENRE("not_contains", "o"), "genre doesn't contain “o”");
  CHECK_DESC("{\"not\":" GENRE("not_contains", "o") "}", "genre contains “o”");
  CHECK_DESC("{\"field\":\"title\",\"op\":\"starts_with\",\"value\":\"So\"}", "title starts with “So”");
  CHECK_DESC("{\"not\":{\"field\":\"album_artist\",\"op\":\"starts_with\",\"value\":\"The\"}}",
             "album artist doesn't start with “The”");
  CHECK_DESC("{\"field\":\"album\",\"op\":\"empty\"}", "album is empty");
  CHECK_DESC("{\"field\":\"artist\",\"op\":\"not_empty\"}", "artist isn't empty");
  CHECK_DESC("{\"not\":{\"not\":{\"field\":\"artist\",\"op\":\"empty\"}}}", "artist is empty");

  /* Numbers and durations. */
  CHECK_DESC("{\"field\":\"year\",\"op\":\"=\",\"value\":1969}", "year is 1969");
  CHECK_DESC("{\"field\":\"year\",\"op\":\"!=\",\"value\":1969}", "year isn't 1969");
  CHECK_DESC(PLAYS("<", 3), "play count is less than 3");
  CHECK_DESC("{\"not\":" PLAYS("<", 3) "}", "play count isn't less than 3");
  CHECK_DESC(PLAYS("<=", 3), "play count is at most 3");
  CHECK_DESC(PLAYS(">", 3), "play count is more than 3");
  CHECK_DESC(PLAYS(">=", 3), "play count is at least 3");
  CHECK_DESC("{\"not\":" PLAYS(">=", 3) "}", "play count isn't at least 3");
  CHECK_DESC("{\"field\":\"track_no\",\"op\":\"between\",\"value\":[1,5]}", "track number is between 1 and 5");
  CHECK_DESC("{\"not\":{\"field\":\"disc_no\",\"op\":\"between\",\"value\":[1,2]}}",
             "disc number isn't between 1 and 2");
  CHECK_DESC("{\"field\":\"year\",\"op\":\"empty\"}", "year is empty");
  CHECK_DESC("{\"field\":\"duration\",\"op\":\">\",\"value\":300}", "duration is more than 5:00");
  CHECK_DESC("{\"field\":\"duration\",\"op\":\"between\",\"value\":[90,3725]}",
             "duration is between 1:30 and 1:02:05");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"track_count\",\"op\":\">\",\"value\":9}}"),
            "Albums where number of tracks is more than 9");

  /* Dates: 12 March 2025 at noon UTC, which is the 12th in every time zone within 11 hours. */
  CHECK_DESC("{\"field\":\"added\",\"op\":\"before\",\"value\":1741780800000}", "date added is before March 12, 2025");
  CHECK_DESC("{\"not\":{\"field\":\"added\",\"op\":\"after\",\"value\":1741780800000}}",
             "date added isn't after March 12, 2025");
  CHECK_DESC("{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}", "date added is in the last 30 days");
  CHECK_DESC("{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":14}}", "date added is in the last 2 weeks");
  CHECK_DESC("{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":1}}", "date added is in the last 1 day");
  CHECK_DESC("{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":7}}",
             "last played isn't in the last 7 days");
  CHECK_DESC("{\"field\":\"last_played\",\"op\":\"empty\"}", "it's never been played");
  CHECK_DESC("{\"not\":{\"field\":\"last_played\",\"op\":\"empty\"}}", "it's been played");
  CHECK_DESC("{\"field\":\"last_played\",\"op\":\"not_empty\"}", "it's been played");
  CHECK_DESC("{\"field\":\"added\",\"op\":\"empty\"}", "date added is empty");

  /* Yes-or-no fields, formats, playlists and groups. */
  CHECK_DESC(DEVICE, "it's on this device");
  CHECK_DESC("{\"field\":\"on_device\",\"op\":\"is\",\"value\":false}", "it isn't on this device");
  CHECK_DESC("{\"not\":" DEVICE "}", "it isn't on this device");
  CHECK_DESC("{\"field\":\"singles\",\"op\":\"is\",\"value\":true}", "it's a single");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":false}}"),
            "Albums where it isn't a single");
  CHECK_DESC("{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}", "it has a FLAC file");
  CHECK_DESC("{\"field\":\"format\",\"op\":\"is_not\",\"value\":\"opus\"}", "it has no OPUS file");
  CHECK_DESC("{\"field\":\"playlist\",\"op\":\"is\",\"value\":\"pl1\"}", "it's in the playlist “Mix”");
  CHECK_DESC("{\"not\":{\"field\":\"playlist\",\"op\":\"is\",\"value\":\"pl1\"}}", "it isn't in the playlist “Mix”");
  CHECK_DESC("{\"field\":\"playlist\",\"op\":\"is\",\"value\":\"gone\"}", "it's in a deleted playlist");
  CHECK_DESC("{\"field\":\"playlist\",\"op\":\"is_not\",\"value\":\"gone\"}", "it isn't in a deleted playlist");
  CHECK_DESC("{\"field\":\"group\",\"op\":\"is\",\"value\":\"g1\"}", "it's in the album group “Swedes”");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"group\",\"op\":\"is_not\",\"value\":\"x\"}}"),
            "Albums where it isn't in a deleted album group");

  /* Groups: lists, parentheses for nested ones, "none of" and "not all of". */
  CHECK_DESC("{\"all\":[" GENRE("is", "a") "," GENRE("is", "b") "," GENRE("is", "c") "]}",
             "genre is “a”, genre is “b”, and genre is “c”");
  CHECK_DESC("{\"any\":[" GENRE("is", "a") "," GENRE("is", "b") "," GENRE("is", "c") "]}",
             "genre is “a”, genre is “b”, or genre is “c”");
  CHECK_DESC("{\"all\":[" GENRE("is", "a") "]}", "genre is “a”");
  CHECK_DESC("{\"not\":{\"any\":[" GENRE("is", "a") "]}}", "genre isn't “a”");
  CHECK_DESC("{\"not\":{\"any\":[" GENRE("is", "a") "," DEVICE "]}}", "none of (genre is “a” or it's on this device)");
  CHECK_DESC("{\"not\":{\"all\":[" GENRE("is", "a") "," DEVICE "]}}",
             "not all of (genre is “a” and it's on this device)");
  CHECK_DESC("{\"any\":[" DEVICE ",{\"all\":[" GENRE("is", "a") ",{\"not\":{\"any\":[" PLAYS("=", 1) "," PLAYS("=", 2)
             "]}}]}]}",
             "it's on this device or (genre is “a” and none of (play count is 1 or play count is 2))");
  CHECK_DESC("{\"all\":[" DEVICE ",{\"any\":[]}]}", "it's on this device and nothing matches");

  /* Order and limit. */
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":7}]}"),
            "All tracks · shuffled");
  CHECK_STR(describe(lib, MC_SMART_TRACKS,
                     "{\"v\":1,\"order\":[{\"field\":\"plays\",\"dir\":\"desc\"},{\"field\":\"random\",\"seed\":7}],"
                     "\"limit\":1}"),
            "All tracks · most played first, then shuffled · 1 track");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"order\":[{\"field\":\"title\"}],\"limit\":5}"),
            "All albums · title A to Z · 5 albums");
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":" DEVICE ",\"limit\":1}"),
            "Albums where it's on this device · 1 album");

  /* Invalid rules. */
  char err_buf[32];
  snprintf(err_buf, sizeof err_buf, "ERR %d", MC_ERR_FORMAT);
  const char *err = err_buf;
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"field\":\"nope\",\"op\":\"is\",\"value\":1}}"),
            err);
  CHECK_STR(describe(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"album\",\"op\":\"is\",\"value\":\"x\"}}"),
            err);
  CHECK_STR(describe(lib, MC_SMART_TRACKS, "not json"), err);
  char *s = NULL;
  CHECK(mc_smart_describe(NULL, MC_SMART_TRACKS, "{\"v\":1}", &s) == MC_ERR_INVALID_ARG && !s);

  /* Labels for an editor. */
  CHECK_STR(mc_smart_field_label("last_played"), "Last played");
  CHECK_STR(mc_smart_field_label("random"), "Random");
  CHECK(mc_smart_field_label("nope") == NULL);
  CHECK_STR(mc_smart_op_label("int", "<"), "is less than");
  CHECK_STR(mc_smart_op_label("date", "not_within"), "isn't in the last");
  CHECK_STR(mc_smart_op_label("playlist", "is_not"), "isn't");
  CHECK(mc_smart_op_label("text", "<") == NULL);
  CHECK(mc_smart_op_label("nope", "is") == NULL);
  CHECK_STR(mc_smart_order_label("plays", 0), "least played first");
  CHECK_STR(mc_smart_order_label("added", 1), "newest added first");
  CHECK_STR(mc_smart_order_label("random", 0), "shuffled");
  CHECK(mc_smart_order_label("singles", 0) == NULL);
  /* Every field and operator has words. */
  for (int subject = MC_SMART_TRACKS; subject <= MC_SMART_ALBUMS; subject++) {
    mc_smart_field f;
    for (int i = 0; mc_smart_field_get((mc_smart_subject)subject, i, &f); i++) {
      CHECK(mc_smart_field_label(f.name) && *mc_smart_field_label(f.name));
      CHECK(!f.sortable || mc_smart_order_label(f.name, 0));
      for (const char *const *op = f.ops; *op; op++) CHECK(mc_smart_op_label(f.type, *op));
    }
  }
}

/* How many a rule picks, or -1 - status. */
static int count(mc_library *lib, mc_smart_subject subject, const char *rule) {
  int n = -99;
  mc_status st = mc_smart_count(lib, subject, rule, &n);
  return st == MC_OK ? n : -1 - (int)st;
}

static void test_count(mc_library *lib) {
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1}") == 5);
  CHECK(count(lib, MC_SMART_ALBUMS, "{\"v\":1}") == 3);
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"field\":\"genre\",\"op\":\"contains\",\"value\":\"RO\"}}") ==
        2);
  CHECK(count(lib, MC_SMART_TRACKS,
              "{\"v\":1,\"match\":{\"all\":[{\"field\":\"genre\",\"op\":\"is\",\"value\":\"pop\"},"
              "{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]}}") == 1);
  CHECK(count(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"group\",\"op\":\"is\",\"value\":\"g1\"}}") == 1);
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"any\":[]}}") == 0);
  /* The limit caps it, whatever the order; the order is still checked. */
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1,\"order\":[{\"field\":\"plays\"}],\"limit\":3}") == 3);
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1,\"limit\":30}") == 5);
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":1,\"order\":[{\"field\":\"random\",\"seed\":4}],\"limit\":2}") == 2);
  CHECK(count(lib, MC_SMART_ALBUMS, "{\"v\":1,\"order\":[{\"field\":\"genre\"}]}") == -1 - MC_ERR_FORMAT);
  CHECK(count(lib, MC_SMART_ALBUMS, "{\"v\":1,\"match\":{\"field\":\"album\",\"op\":\"empty\"}}") ==
        -1 - MC_ERR_FORMAT);
  CHECK(count(lib, MC_SMART_TRACKS, "{\"v\":2}") == -1 - MC_ERR_FORMAT);
  int n;
  CHECK(mc_smart_count(lib, MC_SMART_TRACKS, NULL, &n) == MC_ERR_INVALID_ARG);
  /* The same as listing them, for every rule in the track tests above. */
  const char *rules[] = {"{\"v\":1,\"match\":{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":30}}}",
                         "{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"<\",\"value\":3}}",
                         "{\"v\":1,\"match\":{\"field\":\"playlist\",\"op\":\"is_not\",\"value\":\"pl1\"}}"};
  for (size_t i = 0; i < sizeof rules / sizeof rules[0]; i++) {
    mc_track_list *list = NULL;
    CHECK(mc_smart_preview_tracks(lib, rules[i], &list) == MC_OK && list);
    CHECK(list && count(lib, MC_SMART_TRACKS, rules[i]) == list->count);
    mc_track_list_free(list);
  }
}

int main(int argc, char **argv) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_smart.d", argc > 1 ? argv[1] : ".");
  remove_db(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();
  lib->hlc.wall_ms = fixed_clock;
  populate(mc_library_db(lib));

  test_track_rules(lib);
  test_count(lib);
  test_order(lib);
  test_album_rules(lib);
  test_singles(lib);
  test_invalid();
  test_fields();
  test_examples(lib);
  test_saved(lib);
  test_describe(lib);

  mc_library_close(lib);
  return TEST_RESULT();
}
