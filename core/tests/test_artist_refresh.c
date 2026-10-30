/* The artist refresh after upgrading (tagger/refresh.h; "Refresh after upgrading" in
 * docs/tagging-rules.md). Each library is made at v12, with credits as older versions
 * imported them (one artists row for "Jay-Z feat. Rihanna"), and opened, which migrates it
 * and starts the refresh by itself. MusicBrainz is a fake (mb_set_fake), as in
 * test_import_*.c.
 *
 *   - online: albums with a release id are looked up (one that needs nothing isn't), the
 *     rest split by the string; files retagged in place with their rows following; the old
 *     whole-credit artists deleted, logged;
 *   - offline: the splitter only, the lookups done on a later open;
 *   - closed mid-pass: the close is prompt, and the next open goes on from there.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/dupes.h"
#include "tagger/musicbrainz.h"
#include "tagger/refresh.h"
#include "util/platform.h"
#include "util/uid.h"
#include "test.h"

#define JAY "f82bcf78-5b69-4622-a5ef-73800768d9ac"
#define RIRI "73e5e69d-3554-40d8-8516-00cb38737a1c"
#define KANYE "164f0d73-1234-4e2c-8743-d77bf2191051"
#define LOW "a74b1b7f-71a5-4011-9441-d0b5e4122711"
#define REL_ID "11111111-2222-3333-4444-555555555555"
#define SKIP_ID "77777777-2222-3333-4444-555555555555"
#define ANN "a1111111-1111-4111-8111-111111111111"
#define BOB "a2222222-2222-4222-8222-222222222222"
#define CAT "a3333333-3333-4333-8333-333333333333"

/* Takes a library back before v13: no artist credits (as test_library.c does). */
#define V12_CREDITS                                 \
  "DROP TRIGGER mc_log_ins_tracks_artist_credit;"   \
  "DROP TRIGGER mc_log_upd_tracks_artist_credit;"   \
  "DROP TRIGGER mc_log_ins_albums_artist_credit;"   \
  "DROP TRIGGER mc_log_upd_albums_artist_credit;"   \
  "DROP TABLE track_artists;"                       \
  "DROP TABLE album_artists;"                       \
  "ALTER TABLE tracks DROP COLUMN artist_credit;"   \
  "ALTER TABLE albums DROP COLUMN artist_credit;"   \
  "DELETE FROM meta WHERE key = 'artist_refresh';"

/* --- Files --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  char (*more)[512] = realloc(n->items, (size_t)(n->count + 1) * sizeof *more);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd') remove_tree(path);
    else mc_remove(path);
  }
  free(n.items);
  mc_rmdir(dir);
}

/* How many entries of dir start with prefix. */
static int count_prefixed(const char *dir, const char *prefix) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  int c = 0;
  for (int i = 0; i < n.count; i++) c += strncmp(n.items[i] + 1, prefix, strlen(prefix)) == 0;
  free(n.items);
  return c;
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A tenth of a second of silent 8 kHz mono WAV with these tags. */
static int write_track(const char *path, const mc_tags *tags) {
  enum { DATA_LEN = 1600 };
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + DATA_LEN);
  put_u32(hdr + 16, 16);
  hdr[20] = 1;
  hdr[22] = 1;
  put_u32(hdr + 24, 8000);
  put_u32(hdr + 28, 16000);
  hdr[32] = 2;
  hdr[34] = 16;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, DATA_LEN);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char silence[DATA_LEN];
  int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, sizeof silence, 1, f) == 1;
  if (fclose(f) != 0 || !ok) return 0;
  return mc_tags_write(path, tags) == MC_OK;
}

/* --- Fake MusicBrainz --- */

static const char k_release_json[] =
  "{\"id\":\"" REL_ID "\",\"title\":\"Watch the Throne\",\"date\":\"2011-08-08\","
  "\"artist-credit\":["
  "{\"name\":\"Jay-Z\",\"joinphrase\":\" & \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Kanye West\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" KANYE "\","
  "\"name\":\"Kanye West\",\"sort-name\":\"West, Kanye\"}}],"
  "\"release-group\":{\"id\":\"21111111-2222-3333-4444-555555555555\",\"primary-type\":\"Album\"},"
  "\"media\":[{\"position\":1,\"tracks\":["
  "{\"id\":\"31111111-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"Otis\","
  "\"artist-credit\":[{\"name\":\"Jay-Z\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" JAY "\","
  "\"name\":\"JAY-Z\",\"sort-name\":\"Jay-Z\"}}],"
  "\"recording\":{\"id\":\"41111111-2222-3333-4444-555555555555\"}},"
  "{\"id\":\"32222222-2222-3333-4444-555555555555\",\"position\":2,\"title\":\"Run This Town\","
  "\"artist-credit\":["
  "{\"name\":\"Jay-Z\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Rihanna\",\"artist\":{\"id\":\"" RIRI "\",\"name\":\"Rihanna\","
  "\"sort-name\":\"Rihanna\"}}],"
  "\"recording\":{\"id\":\"42222222-2222-3333-4444-555555555555\"}}]}]}";

/* Releases 55555555-...: "Ann & Bob", one track "Ann feat. Cat". */
static const char k_generic_json[] =
  "{\"id\":\"%s\",\"title\":\"Generic\",\"date\":\"2001\","
  "\"artist-credit\":["
  "{\"name\":\"Ann\",\"joinphrase\":\" & \",\"artist\":{\"id\":\"" ANN "\",\"name\":\"Ann\"}},"
  "{\"name\":\"Bob\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" BOB "\",\"name\":\"Bob\"}}],"
  "\"media\":[{\"position\":1,\"tracks\":["
  "{\"id\":\"6%s\",\"position\":1,\"title\":\"One\",\"artist-credit\":["
  "{\"name\":\"Ann\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" ANN "\",\"name\":\"Ann\"}},"
  "{\"name\":\"Cat\",\"artist\":{\"id\":\"" CAT "\",\"name\":\"Cat\"}}],"
  "\"recording\":{\"id\":\"4%s\"}}]}]}";

static mc_mutex g_mutex;
static int g_lookups, g_sleep_ms, g_skip_looked_up;

static int lookups(void) {
  mc_mutex_lock(&g_mutex);
  int n = g_lookups;
  mc_mutex_unlock(&g_mutex);
  return n;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  mc_mutex_lock(&g_mutex);
  g_lookups++;
  if (!strcmp(id, SKIP_ID)) g_skip_looked_up = 1;
  int sleep_ms = g_sleep_ms;
  mc_mutex_unlock(&g_mutex);
  if (sleep_ms) mc_sleep_ms((uint32_t)sleep_ms);
  if (!strcmp(id, REL_ID)) return mb_release_parse(k_release_json, sizeof k_release_json - 1, out);
  if (!strncmp(id, "55555555", 8)) {
    char json[2048];
    int n = snprintf(json, sizeof json, k_generic_json, id, id + 1, id + 1);
    return mb_release_parse(json, (size_t)n, out);
  }
  return MC_ERR_NOT_FOUND;
}

static const mb_fake k_fake = {NULL, fake_lookup, NULL, NULL, NULL};

/* --- Queries --- */

static int exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) fprintf(stderr, "sql error: %s\n", err ? err : "?");
  sqlite3_free(err);
  return rc;
}

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void query_text(sqlite3 *db, const char *sql, char *out, size_t cap) {
  sqlite3_stmt *stmt;
  out[0] = '\0';
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    snprintf(out, cap, "%s", (const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
}

/* A credit as "name<id>join|...", from its rows, the first artist being artist_uid. */
static void credit_of(mc_library *lib, const char *uid, char *out, size_t cap) {
  out[0] = '\0';
  mc_credit_list *c = NULL;
  if (mc_library_list_credit(lib, uid, &c) != MC_OK) return;
  sqlite3_stmt *stmt;
  sqlite3_prepare_v2(mc_library_db(lib), "SELECT coalesce(mb_artistid, '') FROM artists WHERE uid = ?1", -1,
                     &stmt, NULL);
  for (int i = 0; i < c->count; i++) {
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, c->items[i].uid, -1, SQLITE_STATIC);
    const char *id = sqlite3_step(stmt) == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : "?";
    size_t used = strlen(out);
    snprintf(out + used, cap - used, "%s%s<%s>%s", i ? "|" : "", c->items[i].name, id, c->items[i].join);
  }
  sqlite3_finalize(stmt);
  mc_credit_list_free(c);
}

static void check_credit(mc_library *lib, const char *uid, const char *want) {
  char got[1024];
  credit_of(lib, uid, got, sizeof got);
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "%s: credit %s, expected %s\n", uid, got, want);
    CHECK(0);
  }
}

static void meta(sqlite3 *db, const char *key, char *out, size_t cap) {
  char sql[128];
  snprintf(sql, sizeof sql, "SELECT value FROM meta WHERE key = '%s'", key);
  query_text(db, sql, out, cap);
}

/* --- A library from before v13 --- */

/* Artists, albums and tracks as v12 had them: whole credits as single artists. */
static const char k_rows[] =
  "INSERT INTO artists (uid, name, mb_artistid) VALUES"
  " ('ar-throne', 'Jay-Z & Kanye West', NULL),"
  " ('ar-jay', 'Jay-Z', '" JAY "'),"
  " ('ar-riri', 'Jay-Z feat. Rihanna', NULL),"
  " ('ar-low', 'Low', '" LOW "'),"
  " ('ar-pq', 'Pat & Quinn', NULL),"
  " ('ar-pat', 'Pat', NULL),"
  " ('ar-quinn', 'Quinn', NULL),"
  " ('ar-duet', 'Xavier feat. Yolanda', NULL);"
  "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES"
  " ('al-throne', 'Watch the Throne', 'ar-throne', '" REL_ID "'),"
  " ('al-low', 'Things We Lost', 'ar-low', '" SKIP_ID "'),"
  " ('al-pq', 'Together', 'ar-pq', NULL);"
  "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, disc_no, mb_releasetrackid) VALUES"
  " ('t-otis', 'al-throne', 'ar-jay', 'Otis', 1, 1, '31111111-2222-3333-4444-555555555555'),"
  " ('t-run', 'al-throne', 'ar-riri', 'Run This Town', 2, NULL, NULL),"
  " ('t-low', 'al-low', 'ar-low', 'Sunflower', 1, 1, NULL),"
  " ('t-pq', 'al-pq', 'ar-pq', 'Both', 1, 1, NULL),"
  " ('t-duet', NULL, 'ar-duet', 'Duet', NULL, NULL, NULL);";

typedef struct lib_file {
  const char *uid, *track, *rel, *artist, *album_artist;
  int sha; /* record sha256 */
} lib_file;

static const lib_file k_files[] = {
  {"f-otis", "t-otis", "Throne/01 Otis.wav", "Jay-Z", "Jay-Z & Kanye West", 0},
  {"f-run", "t-run", "Throne/02 Run This Town.wav", "Jay-Z feat. Rihanna", "Jay-Z & Kanye West", 1},
  {"f-low", "t-low", "Low/01 Sunflower.wav", "Low", "Low", 0},
  {"f-pq", "t-pq", "PQ/01 Both.wav", "Pat & Quinn", "Pat & Quinn", 0},
  {"f-duet", "t-duet", "Duet.wav", "Xavier feat. Yolanda", NULL, 0},
};
#define FILE_COUNT ((int)(sizeof k_files / sizeof k_files[0]))
#define SOURCE_SHA "0000000000000000000000000000000000000000000000000000000000000abc"

static void make_v12(const char *dir, const char *rows, int files) {
  remove_tree(dir);
  CHECK(mc_mkdirs(dir) == MC_OK);
  mc_library *lib = NULL;
  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  lib->applying_remote = 1;
  CHECK(exec(db, V12_CREDITS) == SQLITE_OK);
  CHECK(exec(db, rows) == SQLITE_OK);
  for (int i = 0; i < (files ? FILE_COUNT : 0); i++) {
    const lib_file *f = &k_files[i];
    char path[1400], sub[1400];
    snprintf(path, sizeof path, "%s/%s", dir, f->rel);
    snprintf(sub, sizeof sub, "%s", path);
    char *slash = strrchr(sub, '/');
    if (slash) *slash = '\0';
    CHECK(mc_mkdirs(sub) == MC_OK);
    mc_tags t = {0};
    t.title = "x";
    t.artist = (char *)f->artist;
    t.album_artist = (char *)f->album_artist;
    CHECK(write_track(path, &t));
    int64_t size = 0, mtime = 0;
    CHECK(mc_file_info(path, &size, &mtime) == MC_OK);
    char sha[MC_SHA256_STRLEN];
    CHECK(mc_sha256_file(path, sha) == MC_OK);
    sqlite3_stmt *stmt;
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms,"
                                 " sha256, source_sha256) VALUES (?1, ?2, ?3, 'wav', ?4, ?5, ?6, ?7)",
                             -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, f->uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, f->track, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, f->rel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, size);
    sqlite3_bind_int64(stmt, 5, mtime);
    if (f->sha) sqlite3_bind_text(stmt, 6, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, SOURCE_SHA, -1, SQLITE_STATIC);
    CHECK(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
  }
  /* A second file of Run This Town that isn't here: skipped. */
  if (files)
    CHECK(exec(db, "INSERT INTO files (uid, track_uid, path, format, size_bytes)"
                   " VALUES ('f-gone', 't-run', 'Throne/gone.wav', 'wav', 10)") == SQLITE_OK);
  CHECK(exec(db, "PRAGMA user_version = 12") == SQLITE_OK);
  lib->applying_remote = 0;
  mc_library_close(lib);
}

/* The files row of f matches the file on disk; its source_sha256 and file_rev stay. */
static void check_file_row(mc_library *lib, const lib_file *f) {
  char path[1400], sql[256];
  snprintf(path, sizeof path, "%s/%s", mc_library_dir(lib), f->rel);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(path, &size, &mtime) == MC_OK);
  sqlite3 *db = mc_library_db(lib);
  snprintf(sql, sizeof sql,
           "SELECT size_bytes = %lld AND mtime_ms = %lld AND path = '%s' AND source_sha256 = '" SOURCE_SHA
           "' AND file_rev IS NULL FROM files WHERE uid = '%s'",
           (long long)size, (long long)mtime, f->rel, f->uid);
  if (query_int(db, sql) != 1) {
    fprintf(stderr, "%s: files row doesn't match the file\n", f->uid);
    CHECK(0);
  }
  char sha[MC_SHA256_STRLEN], recorded[80];
  snprintf(sql, sizeof sql, "SELECT coalesce(sha256, '') FROM files WHERE uid = '%s'", f->uid);
  query_text(db, sql, recorded, sizeof recorded);
  if (f->sha) CHECK(mc_sha256_file(path, sha) == MC_OK && strcmp(sha, recorded) == 0);
  else CHECK_STR(recorded, "");
}

static void read_tags(mc_library *lib, const char *rel, mc_tags *t) {
  char path[1400];
  snprintf(path, sizeof path, "%s/%s", mc_library_dir(lib), rel);
  memset(t, 0, sizeof *t);
  CHECK(mc_tags_read(path, t) == MC_OK);
}

static int artist_gone(sqlite3 *db, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql,
           "SELECT NOT EXISTS (SELECT 1 FROM artists WHERE uid = '%s') AND EXISTS (SELECT 1 FROM changelog"
           " WHERE entity = 'artists' AND row_uid = '%s' AND field = '_deleted')",
           uid, uid);
  return query_int(db, sql) == 1;
}

static void derived_artist(const char *name_lower, char out[MC_UID_STRLEN]) {
  char key[128];
  snprintf(key, sizeof key, "artist/%s", name_lower);
  CHECK(mc_uid_derive(key, 0, out) == MC_OK);
}

/* Points the offline check at value (a closed port: offline), or back at MusicBrainz. */
static void set_probe(const char *value) {
#ifdef _WIN32
  _putenv_s("KITHSONG_MUSICBRAINZ_PROBE", value ? value : "");
#else
  if (value) setenv("KITHSONG_MUSICBRAINZ_PROBE", value, 1);
  else unsetenv("KITHSONG_MUSICBRAINZ_PROBE");
#endif
}

/* --- Online --- */

static void test_online(const char *root) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/online", root);
  make_v12(dir, k_rows, 1);
  mb_set_fake(&k_fake);
  g_lookups = g_skip_looked_up = 0;

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  CHECK(mc_library_schema_version(lib) == 13);
  mc_refresh_wait(lib);
  CHECK(mc_library_refresh_progress(lib, NULL, NULL) == 0);
  sqlite3 *db = mc_library_db(lib);
  char s[256];
  meta(db, "artist_refresh", s, sizeof s);
  CHECK_STR(s, "done");
  CHECK(query_int(db, "SELECT count(*) FROM meta WHERE key LIKE 'artist_refresh_%'") == 0);
  CHECK(lookups() == 1); /* Watch the Throne; Things We Lost needed none */
  CHECK(!g_skip_looked_up);

  /* From the release: names as credited, ids, joins; Jay-Z's own row is used. */
  check_credit(lib, "al-throne", "Jay-Z<" JAY "> & |Kanye West<" KANYE ">");
  check_credit(lib, "t-otis", "Jay-Z<" JAY ">");
  check_credit(lib, "t-run", "Jay-Z<" JAY "> feat. |Rihanna<" RIRI ">");
  query_text(db, "SELECT artist_credit FROM tracks WHERE uid = 't-run'", s, sizeof s);
  CHECK_STR(s, "Jay-Z feat. Rihanna");
  query_text(db, "SELECT artist_uid FROM tracks WHERE uid = 't-run'", s, sizeof s);
  CHECK_STR(s, "ar-jay");
  /* Untouched. */
  check_credit(lib, "al-low", "Low<" LOW ">");
  /* No release id: the splitter, against the library's artists. */
  check_credit(lib, "al-pq", "Pat<> & |Quinn<>");
  check_credit(lib, "t-pq", "Pat<> & |Quinn<>");
  check_credit(lib, "t-duet", "Xavier<> feat. |Yolanda<>");
  /* New artists the splitter names get uids from their names. */
  char yolanda[MC_UID_STRLEN], sql[256];
  derived_artist("yolanda", yolanda);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM artists WHERE uid = '%s' AND name = 'Yolanda'", yolanda);
  CHECK(query_int(db, sql) == 1);

  /* The whole-credit rows are gone, the deletes logged; the credit changes are logged too. */
  CHECK(artist_gone(db, "ar-throne"));
  CHECK(artist_gone(db, "ar-riri"));
  CHECK(artist_gone(db, "ar-pq"));
  CHECK(artist_gone(db, "ar-duet"));
  CHECK(query_int(db, "SELECT count(*) FROM artists WHERE uid IN ('ar-jay', 'ar-low', 'ar-pat', 'ar-quinn')") == 4);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'tracks' AND row_uid = 't-run'"
                      " AND field = 'artist_credit' AND value = 'Jay-Z feat. Rihanna'") == 1);
  CHECK(query_int(db, "SELECT count(*) FROM changelog WHERE entity = 'track_artists'"
                      " AND field = 'artist_uid'") >= 3);

  /* Files: retagged where the credit changed, ARTIST and ALBUMARTIST still the credits. */
  mc_tags t;
  read_tags(lib, "Throne/02 Run This Town.wav", &t);
  CHECK_STR(t.artist, "Jay-Z feat. Rihanna");
  CHECK_STR(t.artists, "Jay-Z\nRihanna");
  CHECK_STR(t.mb_artistids, JAY "\n" RIRI);
  CHECK_STR(t.album_artist, "Jay-Z & Kanye West");
  CHECK_STR(t.album_artists, "Jay-Z\nKanye West");
  CHECK_STR(t.mb_albumartistids, JAY "\n" KANYE);
  CHECK_STR(t.title, "x"); /* nothing else touched */
  mc_tags_clear(&t);
  read_tags(lib, "Throne/01 Otis.wav", &t); /* its album's credit changed */
  CHECK_STR(t.artists, "Jay-Z");
  CHECK_STR(t.album_artists, "Jay-Z\nKanye West");
  mc_tags_clear(&t);
  read_tags(lib, "PQ/01 Both.wav", &t);
  CHECK_STR(t.artist, "Pat & Quinn");
  CHECK_STR(t.artists, "Pat\nQuinn");
  CHECK(t.mb_artistids == NULL); /* no ids to write */
  CHECK_STR(t.album_artists, "Pat\nQuinn");
  mc_tags_clear(&t);
  read_tags(lib, "Duet.wav", &t);
  CHECK_STR(t.artists, "Xavier\nYolanda");
  CHECK(t.album_artists == NULL);
  mc_tags_clear(&t);
  read_tags(lib, "Low/01 Sunflower.wav", &t); /* left alone */
  CHECK(t.artists == NULL);
  mc_tags_clear(&t);
  for (int i = 0; i < FILE_COUNT; i++) check_file_row(lib, &k_files[i]);
  char folder[1400];
  snprintf(folder, sizeof folder, "%s/Throne", dir);
  CHECK(count_prefixed(folder, ".kithsong-") == 0);
  CHECK(query_int(db, "SELECT size_bytes FROM files WHERE uid = 'f-gone'") == 10);
  mc_library_close(lib);

  /* Done once: opening again starts nothing. */
  lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (lib) CHECK(lib->refresh == NULL);
  mc_library_close(lib);
  mb_set_fake(NULL);
}

/* --- Offline, then online --- */

static void test_offline(const char *root) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/offline", root);
  make_v12(dir, k_rows, 1);
  mb_set_fake(NULL);
  set_probe("127.0.0.1:1");

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  mc_refresh_wait(lib);
  sqlite3 *db = mc_library_db(lib);
  char s[256];
  meta(db, "artist_refresh", s, sizeof s);
  CHECK_STR(s, "pending");
  meta(db, "artist_refresh_mb", s, sizeof s);
  CHECK_STR(s, "");
  meta(db, "artist_refresh_split", s, sizeof s);
  CHECK_STR(s, "done");
  meta(db, "artist_refresh_loose", s, sizeof s);
  CHECK_STR(s, "done");
  /* The splitter did what it could: "feat." splits, "Jay-Z & Kanye West" doesn't (Kanye West
   * isn't known). Rihanna's uid comes from her name, so another device splitting this
   * offline makes the same row. */
  char rihanna[MC_UID_STRLEN], want[512];
  derived_artist("rihanna", rihanna);
  snprintf(want, sizeof want, "SELECT count(*) FROM track_artists WHERE track_uid = 't-run'"
                              " AND position = 1 AND artist_uid = '%s'", rihanna);
  CHECK(query_int(db, want) == 1);
  check_credit(lib, "t-run", "Jay-Z<" JAY "> feat. |Rihanna<>");
  check_credit(lib, "al-throne", "Jay-Z & Kanye West<>");
  check_credit(lib, "t-duet", "Xavier<> feat. |Yolanda<>");
  CHECK(artist_gone(db, "ar-riri"));
  mc_tags t;
  read_tags(lib, "Throne/02 Run This Town.wav", &t);
  CHECK_STR(t.artists, "Jay-Z\nRihanna");
  mc_tags_clear(&t);
  for (int i = 0; i < FILE_COUNT; i++) check_file_row(lib, &k_files[i]);
  mc_library_close(lib);

  /* Online again: the lookups are done now, over what the splitter did. */
  set_probe(NULL);
  mb_set_fake(&k_fake);
  g_lookups = 0;
  lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  mc_refresh_wait(lib);
  db = mc_library_db(lib);
  meta(db, "artist_refresh", s, sizeof s);
  CHECK_STR(s, "done");
  CHECK(lookups() == 1);
  check_credit(lib, "al-throne", "Jay-Z<" JAY "> & |Kanye West<" KANYE ">");
  check_credit(lib, "t-run", "Jay-Z<" JAY "> feat. |Rihanna<" RIRI ">");
  snprintf(want, sizeof want, "SELECT count(*) FROM artists WHERE uid = '%s' AND mb_artistid = '" RIRI "'", rihanna);
  CHECK(query_int(db, want) == 1); /* the same row, now with her id */
  CHECK(artist_gone(db, "ar-throne"));
  read_tags(lib, "Throne/02 Run This Town.wav", &t);
  CHECK_STR(t.mb_artistids, JAY "\n" RIRI);
  CHECK_STR(t.album_artists, "Jay-Z\nKanye West");
  mc_tags_clear(&t);
  for (int i = 0; i < FILE_COUNT; i++) check_file_row(lib, &k_files[i]);
  mc_library_close(lib);
  mb_set_fake(NULL);
}

/* --- Closed mid-pass, then resumed --- */

#define ALBUMS 6

static void test_interrupted(const char *root) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/interrupted", root);
  char rows[4096] = "INSERT INTO artists (uid, name) VALUES ('ar-ab', 'Ann & Bob'), ('ar-ac', 'Ann feat. Cat');";
  for (int i = 0; i < ALBUMS; i++) {
    size_t used = strlen(rows);
    snprintf(rows + used, sizeof rows - used,
             "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES"
             " ('al-%d', 'Album %d', 'ar-ab', '55555555-0000-4000-8000-00000000000%d');"
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, disc_no) VALUES"
             " ('t-%d', 'al-%d', 'ar-ac', 'One', 1, 1);",
             i, i, i, i, i);
  }
  make_v12(dir, rows, 0);
  mb_set_fake(&k_fake);
  g_lookups = 0;
  g_sleep_ms = 300;

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  uint64_t deadline = mc_mono_ms() + 10000;
  while (lookups() < 2 && mc_mono_ms() < deadline) mc_sleep_ms(10);
  int done = -1, total = -1;
  CHECK(mc_library_refresh_progress(lib, &done, &total) == 1);
  if (!(total == ALBUMS && done >= 1 && done < ALBUMS)) fprintf(stderr, "progress %d of %d\n", done, total);
  CHECK(total == ALBUMS && done >= 1 && done < ALBUMS);
  uint64_t t0 = mc_mono_ms();
  mc_library_close(lib);
  CHECK(mc_mono_ms() - t0 < 1500); /* waits for the lookup under way at most */
  int first = lookups();

  lib = NULL;
  CHECK(mc_library_open_internal(dir, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  char s[256];
  meta(db, "artist_refresh", s, sizeof s);
  CHECK_STR(s, "pending");
  int credited = query_int(db, "SELECT count(*) FROM albums WHERE artist_credit = 'Ann & Bob'");
  CHECK(credited >= 1 && credited < ALBUMS);
  meta(db, "artist_refresh_mb", s, sizeof s);
  char last[64];
  snprintf(last, sizeof last, "al-%d", credited - 1);
  CHECK_STR(s, last); /* albums are done in uid order, the cursor at the last */
  mc_library_close(lib);

  g_sleep_ms = 0;
  lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  mc_refresh_wait(lib);
  db = mc_library_db(lib);
  meta(db, "artist_refresh", s, sizeof s);
  CHECK_STR(s, "done");
  /* Each album looked up once, but the one under way when it was closed. */
  CHECK(lookups() - first == ALBUMS - credited);
  CHECK(lookups() <= ALBUMS + 1);
  CHECK(query_int(db, "SELECT count(*) FROM albums WHERE artist_credit = 'Ann & Bob'") == ALBUMS);
  CHECK(query_int(db, "SELECT count(*) FROM album_artists") == 2 * ALBUMS);
  CHECK(query_int(db, "SELECT count(*) FROM track_artists") == 2 * ALBUMS);
  for (int i = 0; i < ALBUMS; i++) {
    char uid[16];
    snprintf(uid, sizeof uid, "t-%d", i);
    check_credit(lib, uid, "Ann<" ANN "> feat. |Cat<" CAT ">");
  }
  CHECK(artist_gone(db, "ar-ab"));
  CHECK(artist_gone(db, "ar-ac"));
  CHECK(query_int(db, "SELECT count(*) FROM artists") == 3);
  mc_library_close(lib);
  mb_set_fake(NULL);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mc_mutex_init(&g_mutex);
  mc_refresh_set_delay_ms(0);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_artist_refresh.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  test_online(root);
  test_offline(root);
  test_interrupted(root);

  if (!test_failures) remove_tree(root);
  mc_mutex_destroy(&g_mutex);
  mc_shutdown();
  return TEST_RESULT();
}
