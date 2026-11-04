/* A continuous mix of one long track: a single long enough is taken for a mix, worked out on
 * each device and never stored, unless the user has chosen; it's listed as an album and not
 * among the singles. A track with no album gets one to be a mix on: at import, once on
 * upgrading to v16, or by hand. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "search/search_internal.h"
#include "sync/rows.h"
#include "tagger/musicbrainz.h"
#include "test.h"
#include "util/platform.h"

#define LONG_MS (MC_MIX_MIN_MS + 60000)
#define SHORT_MS (MC_MIX_MIN_MS - 1)

static void remove_tree(const char *dir);

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

static mc_library *open_new(const char *scratch, const char *name, char *path, size_t cap) {
  snprintf(path, cap, "%s/test_mix_%s.d", scratch, name);
  remove_tree(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  return lib;
}

/* Writes as an edit would, so it's logged. */
static void logged(mc_library *lib, const char *sql) {
  CHECK(mc_library_begin_write(lib) == MC_OK);
  int rc = sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "%s: %s\n", sql, mc_library_errmsg(lib));
  CHECK(rc == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int in_albums(mc_library *lib, const char *title) {
  mc_album_list *list = NULL;
  int found = 0;
  CHECK(mc_library_list_albums(lib, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++) {
    if (strcmp(list->items[i].title, title)) continue;
    found++;
    CHECK(!list->items[i].is_singles);
  }
  mc_album_list_free(list);
  return found;
}

static int in_singles(mc_library *lib, const char *artist, const char *title) {
  mc_track_list *list = NULL;
  int found = 0;
  CHECK(mc_library_list_artist_singles(lib, artist, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++) found += !strcmp(list->items[i].title, title);
  mc_track_list_free(list);
  return found;
}

/* How many results of kind ('A' album, 'T' track) query finds titled title. */
static int found(mc_library *lib, const char *query, mc_search_kind kind, const char *title) {
  CHECK(mc_library_search_prepare(lib) == MC_OK);
  mc_search_wait(lib);
  mc_search_results *res = NULL;
  CHECK(mc_library_search(lib, query, 0, &res) == MC_OK);
  int n = 0;
  for (int i = 0; res && i < res->count; i++)
    n += res->items[i].kind == kind && !strcmp(res->items[i].name, title);
  mc_search_results_free(res);
  return n;
}

static void flags_are(mc_library *lib, const char *album, int mix, int by_hand) {
  mc_album_flags f;
  CHECK(mc_album_flags_get(lib, album, &f) == MC_OK);
  CHECK(f.mix == mix && f.mix_by_hand == by_hand);
}

/* Sets mix alone, as the dialog does: what the album says, with that changed. */
static void set_mix(mc_library *lib, const char *album, int mix) {
  mc_album_flags f;
  CHECK(mc_album_flags_get(lib, album, &f) == MC_OK);
  f.mix = mix;
  CHECK(mc_album_flags_set(lib, album, &f) == MC_OK);
}

#define SEED                                                                                   \
  "INSERT INTO artists (uid, name) VALUES ('ar1', 'Sasha Vale'), ('ar0', 'Aaron Earlier');"    \
  "INSERT INTO albums (uid, title, artist_uid, year) VALUES"                                   \
  " ('alb', 'Harbour Album', 'ar1', 2001), ('all', 'Warehouse Set', 'ar1', 2005),"             \
  " ('als', 'Short Single', 'ar1', 2006), ('ale', 'Edge Single', 'ar1', 2007),"                \
  " ('alz', 'Zero Length', 'ar1', 2008), ('ala', 'Aaron Set', 'ar0', 2020);"

static void seed(mc_library *lib) {
  char sql[1600];
  snprintf(sql, sizeof sql,
           SEED "INSERT INTO tracks (uid, album_uid, title, duration_ms) VALUES"
                " ('t1', 'alb', 'Bay', 200000), ('t2', 'alb', 'Quay', %d),"
                " ('tl', 'all', 'Warehouse Set', %d), ('ts', 'als', 'Short Single', 200000),"
                " ('te', 'ale', 'Edge Single', %d), ('tz', 'alz', 'Zero Length', NULL),"
                " ('ta', 'ala', 'Aaron Set', %d);",
           LONG_MS, LONG_MS, SHORT_MS, MC_MIX_MIN_MS);
  logged(lib, sql);
}

/* What a mix of one track is on every listing, and what stays a single. */
static void test_listing(const char *scratch) {
  char path[1024];
  mc_library *lib = open_new(scratch, "listing", path, sizeof path);
  if (!lib) return;
  seed(lib);

  CHECK(in_albums(lib, "Warehouse Set") == 1);
  CHECK(in_albums(lib, "Aaron Set") == 1); /* exactly the threshold */
  CHECK(in_albums(lib, "Harbour Album") == 1);
  CHECK(in_albums(lib, "Short Single") == 0);
  CHECK(in_albums(lib, "Edge Single") == 0); /* a millisecond under */
  CHECK(in_albums(lib, "Zero Length") == 0);

  /* In the albums' order: by artist, then year. */
  mc_album_list *list = NULL;
  CHECK(mc_library_list_albums(lib, &list) == MC_OK);
  if (list) {
    CHECK(list->count == 3);
    if (list->count == 3) {
      CHECK_STR(list->items[0].title, "Aaron Set");
      CHECK_STR(list->items[1].title, "Harbour Album");
      CHECK_STR(list->items[2].title, "Warehouse Set");
    }
  }
  mc_album_list_free(list);

  /* The artist's page: among the albums, of one track, and not the singles. */
  list = NULL;
  CHECK(mc_library_list_artist_albums(lib, "ar1", &list) == MC_OK);
  if (list) {
    CHECK(list->count == 2);
    for (int i = 0; i < list->count; i++)
      if (!strcmp(list->items[i].uid, "all")) CHECK(list->items[i].track_count == 1);
  }
  mc_album_list_free(list);
  CHECK(in_singles(lib, "ar1", "Warehouse Set") == 0);
  CHECK(in_singles(lib, "ar1", "Short Single") == 1);
  CHECK(in_singles(lib, "ar1", "Edge Single") == 1);

  /* And counted as one. */
  mc_artist_list *artists = NULL;
  CHECK(mc_library_list_artists(lib, &artists) == MC_OK);
  for (int i = 0; artists && i < artists->count; i++)
    if (!strcmp(artists->items[i].uid, "ar1")) CHECK(artists->items[i].album_count == 2);
  mc_artist_list_free(artists);

  /* Search has the album, not the track of the same name. */
  CHECK(found(lib, "warehouse", MC_SEARCH_ALBUM, "Warehouse Set") == 1);
  CHECK(found(lib, "warehouse", MC_SEARCH_TRACK, "Warehouse Set") == 0);
  CHECK(found(lib, "short single", MC_SEARCH_TRACK, "Short Single") == 1);
  CHECK(found(lib, "short single", MC_SEARCH_ALBUM, "Short Single") == 0);

  /* A smart album group's "isn't a single", as Random albums has, picks it; a smart playlist
   * doesn't pick its track. */
  mc_album_list *smart = NULL;
  CHECK(mc_smart_preview_albums(lib, "{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":false}}",
                                &smart) == MC_OK);
  int mixes = 0;
  for (int i = 0; smart && i < smart->count; i++) {
    mixes += !strcmp(smart->items[i].uid, "all");
    CHECK(strcmp(smart->items[i].uid, "als"));
  }
  CHECK(mixes == 1);
  mc_album_list_free(smart);
  mc_track_list *tracks = NULL;
  CHECK(mc_smart_preview_tracks(lib, "{\"v\":1,\"match\":{\"field\":\"duration\",\"op\":\">\",\"value\":1000}}",
                                &tracks) == MC_OK);
  int long_tracks = 0, quay = 0;
  for (int i = 0; tracks && i < tracks->count; i++) {
    long_tracks += !strcmp(tracks->items[i].uid, "tl");
    quay += !strcmp(tracks->items[i].uid, "t2");
  }
  CHECK(long_tracks == 0);
  CHECK(quay == 1); /* a long track of a longer album is just a track */
  mc_track_list_free(tracks);

  /* More of the release arrives: an album of two, long track and all, and no mix. */
  logged(lib, "INSERT INTO tracks (uid, album_uid, title, duration_ms) VALUES ('tl2', 'all', 'Encore', 100000)");
  flags_are(lib, "all", 0, 0);
  CHECK(in_albums(lib, "Warehouse Set") == 1);
  CHECK(found(lib, "warehouse", MC_SEARCH_TRACK, "Warehouse Set") == 1);
  mc_library_close(lib);
}

/* Length only decides until the user does, either way, and saving a form isn't deciding. */
static void test_by_hand(const char *scratch) {
  char path[1024];
  mc_library *lib = open_new(scratch, "hand", path, sizeof path);
  if (!lib) return;
  seed(lib);
  flags_are(lib, "all", 1, 0);
  flags_are(lib, "als", 0, 0);

  /* The dialog sends both flags: the other one saved leaves it to length. */
  mc_album_flags f;
  CHECK(mc_album_flags_get(lib, "all", &f) == MC_OK);
  f.no_scrobble = 1;
  CHECK(mc_album_flags_set(lib, "all", &f) == MC_OK);
  flags_are(lib, "all", 1, 0);
  CHECK(query_int(lib, "SELECT mix FROM albums WHERE uid = 'all'") == 0);

  /* Un-set by hand: a single again, for good. */
  set_mix(lib, "all", 0);
  flags_are(lib, "all", 0, 1);
  CHECK(in_albums(lib, "Warehouse Set") == 0);
  CHECK(in_singles(lib, "ar1", "Warehouse Set") == 1);
  CHECK(found(lib, "warehouse", MC_SEARCH_TRACK, "Warehouse Set") == 1);
  mc_library_close(lib);
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  flags_are(lib, "all", 0, 1);
  CHECK(in_albums(lib, "Warehouse Set") == 0);

  /* Set by hand on a short one: an album, whatever its length. */
  set_mix(lib, "als", 1);
  flags_are(lib, "als", 1, 1);
  CHECK(in_albums(lib, "Short Single") == 1);
  CHECK(in_singles(lib, "ar1", "Short Single") == 0);
  CHECK(found(lib, "short single", MC_SEARCH_ALBUM, "Short Single") == 1);

  /* And back: stays a hand choice, though length says the same. */
  set_mix(lib, "als", 0);
  flags_are(lib, "als", 0, 1);

  /* From the track: a single's its album's flag, a longer album's isn't the track's to set. */
  mc_track_mix tm;
  CHECK(mc_track_mix_get(lib, "ts", &tm) == MC_OK && tm.can_set && !tm.mix && tm.mix_by_hand);
  CHECK(mc_track_mix_set(lib, "ts", 1) == MC_OK);
  CHECK(mc_track_mix_get(lib, "ts", &tm) == MC_OK && tm.can_set && tm.mix && tm.mix_by_hand);
  flags_are(lib, "als", 1, 1);
  CHECK(mc_track_mix_get(lib, "te", &tm) == MC_OK && tm.can_set && !tm.mix && !tm.mix_by_hand);
  CHECK(mc_track_mix_get(lib, "t1", &tm) == MC_OK && !tm.can_set);
  CHECK(mc_track_mix_set(lib, "t1", 1) == MC_ERR_INVALID_ARG);
  CHECK(mc_track_mix_get(lib, "nope", &tm) == MC_ERR_NOT_FOUND);
  CHECK(mc_track_mix_set(lib, "nope", 1) == MC_ERR_NOT_FOUND);
  mc_library_close(lib);
}

/* Rows from one device to another, as a session moves them. */
static void send(mc_library *from, mc_library *to) {
  mc_origin_hlc *vv;
  int nvv;
  CHECK(mc_changelog_version_vector(mc_library_db(to), &vv, &nvv) == MC_OK);
  mc_rows_reader *rd = NULL;
  CHECK(mc_rows_reader_open(mc_library_db(from), vv, nvv, 0, &rd) == MC_OK);
  free(vv);
  if (!rd) return;
  mc_rows_target tg;
  mc_rows_target_library(to, &tg);
  mc_rows_applier *a = NULL;
  CHECK(mc_rows_applier_new(&tg, &a) == MC_OK);
  static unsigned char buf[1 << 16];
  mc_rows_writer w;
  mc_row r;
  int got = 1;
  while (got && a) {
    mc_rows_writer_init(&w, buf, sizeof buf, MC_ROWS_LIBRARY);
    while ((int)w.count < 100 && mc_rows_reader_next(rd, &r, &got) == MC_OK && got)
      CHECK(mc_rows_writer_add(&w, &r) == 1);
    size_t len = mc_rows_writer_finish(&w);
    int db, n;
    mc_row *rows = NULL;
    CHECK(mc_rows_decode(buf, len, &db, &rows, &n) == MC_OK);
    if (n) CHECK(mc_rows_apply(a, rows, n, NULL) == MC_OK);
    free(rows);
  }
  mc_rows_applier_free(a);
  mc_rows_reader_close(rd);
}

static int same_digest(mc_library *x, mc_library *y) {
  char dx[MC_DIGEST_STRLEN], dy[MC_DIGEST_STRLEN];
  CHECK(mc_rows_vv_digest(mc_library_db(x), dx) == MC_OK);
  CHECK(mc_rows_vv_digest(mc_library_db(y), dy) == MC_OK);
  return strcmp(dx, dy) == 0;
}

#define LOOSE_SEED                                                                        \
  "INSERT INTO artists (uid, name) VALUES ('ar1', 'Sasha Vale'), ('ar2', 'Guest');"       \
  "INSERT INTO albums (uid, title, artist_uid) VALUES ('all', 'Warehouse Set', 'ar1');"

/* A library of tracks an older version left: one long track on its album, one with none and
 * credited to two, one short with none, and a mix chosen by hand. */
static void seed_loose(mc_library *lib) {
  char sql[1400];
  snprintf(sql, sizeof sql,
           LOOSE_SEED "INSERT INTO tracks (uid, album_uid, artist_uid, artist_credit, title, year,"
                      " duration_ms) VALUES ('tl', 'all', NULL, NULL, 'Warehouse Set', NULL, %d),"
                      " ('tb', NULL, 'ar1', 'Sasha Vale & Guest', 'Boiler Room', 2019, %d),"
                      " ('ts', NULL, 'ar1', NULL, 'Short Demo', NULL, 200000);"
                      "INSERT INTO track_artists (uid, track_uid, artist_uid, position, joinphrase)"
                      " VALUES ('c0', 'tb', 'ar1', 0, ' & '), ('c1', 'tb', 'ar2', 1, '');",
           LONG_MS, LONG_MS);
  logged(lib, sql);
}

/* Winds lib back to before v16 and opens it again, as upgrading does. */
static mc_library *upgrade(mc_library *lib, const char *path) {
  CHECK(sqlite3_exec(mc_library_db(lib), "PRAGMA user_version = 15", NULL, NULL, NULL) == SQLITE_OK);
  mc_library_close(lib);
  mc_library *now = NULL;
  CHECK(mc_library_open(path, &now) == MC_OK);
  if (now) CHECK(mc_library_schema_version(now) >= 16);
  return now;
}

#define BOILER_ALBUM "(SELECT album_uid FROM tracks WHERE uid = 'tb')"

/* A long loose track has an album after upgrading, its own and logged; nothing else changes. */
static void test_migration(const char *scratch) {
  char path[1024];
  mc_library *lib = open_new(scratch, "migrate", path, sizeof path);
  if (!lib) return;
  seed_loose(lib);
  logged(lib, "UPDATE albums SET mix = 1 WHERE uid = 'all'");
  int before = query_int(lib, "SELECT count(*) FROM changelog");
  lib = upgrade(lib, path);
  if (!lib) return;

  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE uid = 'tb' AND album_uid IS NOT NULL") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE uid = 'ts' AND album_uid IS NULL") == 1);
  CHECK(in_albums(lib, "Boiler Room") == 1);
  CHECK(in_singles(lib, "ar1", "Boiler Room") == 0);
  CHECK(in_singles(lib, "ar1", "Short Demo") == 1);
  CHECK(found(lib, "boiler", MC_SEARCH_ALBUM, "Boiler Room") == 1);
  CHECK(found(lib, "boiler", MC_SEARCH_TRACK, "Boiler Room") == 0);
  /* The album says what the track did, credit and all. */
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE uid = " BOILER_ALBUM " AND artist_uid = 'ar1'"
                       " AND artist_credit = 'Sasha Vale & Guest' AND year = 2019 AND mix = 0") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM album_artists WHERE album_uid = " BOILER_ALBUM) == 2);
  /* What was chosen stays chosen. */
  flags_are(lib, "all", 1, 1);

  /* Its rows are logged, so a device paired later gets them; a detected flag isn't a row. */
  int after = query_int(lib, "SELECT count(*) FROM changelog");
  CHECK(after > before);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE field = 'mix' AND row_uid <> 'all'") == 0);

  /* Once: a track since taken off its album stays off it. */
  logged(lib, "UPDATE tracks SET album_uid = NULL WHERE uid = 'tb'");
  mc_library_close(lib);
  lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE uid = 'tb' AND album_uid IS NULL") == 1);
  mc_library_close(lib);
}

/* Two devices upgrade apart, each detecting and filing for itself. */
static void test_two_devices(const char *scratch) {
  char pa[1024], pb[1024], pc[1024];
  mc_library *a = open_new(scratch, "dev_a", pa, sizeof pa);
  mc_library *b = open_new(scratch, "dev_b", pb, sizeof pb);
  if (!a || !b) {
    mc_library_close(a);
    mc_library_close(b);
    return;
  }
  seed_loose(a);
  send(a, b);
  a = upgrade(a, pa);
  mc_sleep_ms(5);
  b = upgrade(b, pb);
  if (!a || !b) {
    mc_library_close(a);
    mc_library_close(b);
    return;
  }
  /* The one album on both, under the one uid, however the rows cross. */
  send(a, b);
  send(b, a);
  CHECK(same_digest(a, b));
  CHECK(query_int(a, "SELECT count(*) FROM albums") == 2 && query_int(b, "SELECT count(*) FROM albums") == 2);
  CHECK(query_int(a, "SELECT count(*) FROM albums WHERE title = 'Boiler Room'") == 1);
  CHECK(in_albums(a, "Boiler Room") == 1 && in_albums(b, "Boiler Room") == 1);
  CHECK(in_albums(a, "Warehouse Set") == 1 && in_albums(b, "Warehouse Set") == 1);
  CHECK(query_int(a, "SELECT count(*) FROM changelog WHERE field = 'mix'") == 0);

  /* A device paired later runs no migration, and the rows bring it all the same. */
  mc_library *c = open_new(scratch, "dev_c", pc, sizeof pc);
  if (c) {
    send(a, c);
    CHECK(in_albums(c, "Boiler Room") == 1);
    CHECK(in_albums(c, "Warehouse Set") == 1);
    CHECK(in_singles(c, "ar1", "Warehouse Set") == 0);
  }

  /* Un-set on one: it's a single on the other too, and stays one as that device opens,
   * detects and writes on. */
  set_mix(a, "all", 0);
  send(a, b);
  flags_are(b, "all", 0, 1);
  CHECK(in_albums(b, "Warehouse Set") == 0);
  CHECK(in_singles(b, "ar1", "Warehouse Set") == 1);
  mc_library_close(b);
  b = NULL;
  CHECK(mc_library_open(pb, &b) == MC_OK);
  if (b) {
    logged(b, "UPDATE tracks SET title = 'Warehouse Set' WHERE uid = 'tl'");
    flags_are(b, "all", 0, 1);
    send(b, a);
    flags_are(a, "all", 0, 1);
  }

  /* Chosen on both: the last choice wins on both. */
  if (b) {
    set_mix(b, "all", 1);
    mc_sleep_ms(5);
    set_mix(a, "all", 1);
    mc_sleep_ms(5);
    set_mix(a, "all", 0);
    send(b, a);
    send(a, b);
    flags_are(a, "all", 0, 1);
    flags_are(b, "all", 0, 1);
    CHECK(same_digest(a, b));
  }
  mc_library_close(a);
  mc_library_close(b);
  mc_library_close(c);
}

/* Two devices import one long single apart, one un-setting it first: the albums merge, and
 * the later import's rows don't bring detection back. */
static void test_duplicates(const char *scratch) {
  char pa[1024], pb[1024], sql[900];
  mc_library *a = open_new(scratch, "dup_a", pa, sizeof pa);
  mc_library *b = open_new(scratch, "dup_b", pb, sizeof pb);
  if (!a || !b) {
    mc_library_close(a);
    mc_library_close(b);
    return;
  }
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name) VALUES ('ar-a', 'Sasha Vale');"
           "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES ('al-a', 'Warehouse Set', 'ar-a', 'REL');"
           "INSERT INTO tracks (uid, album_uid, title, mb_releasetrackid, duration_ms)"
           " VALUES ('t-a', 'al-a', 'Warehouse Set', 'RT', %d);", LONG_MS);
  logged(a, sql);
  set_mix(a, "al-a", 0);
  mc_sleep_ms(5); /* B's rows are all newer than A's choice */
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name) VALUES ('ar-b', 'Sasha Vale');"
           "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES ('al-b', 'Warehouse Set', 'ar-b', 'REL');"
           "INSERT INTO tracks (uid, album_uid, title, mb_releasetrackid, duration_ms)"
           " VALUES ('t-b', 'al-b', 'Warehouse Set', 'RT', %d);", LONG_MS);
  logged(b, sql);
  flags_are(b, "al-b", 1, 0);
  send(a, b);
  send(b, a);
  CHECK(same_digest(a, b));
  flags_are(a, "al-a", 0, 1);
  flags_are(b, "al-a", 0, 1);
  CHECK(in_albums(b, "Warehouse Set") == 0);
  mc_library_close(a);
  mc_library_close(b);
}

/* --- Importing --- */

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Silent 8-bit mono WAV at 1 kHz, so half an hour is under 2 MB. */
static int write_track(const char *path, const char *title, uint32_t ms) {
  uint32_t data_len = ms;
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + data_len);
  put_u32(hdr + 16, 16);
  hdr[20] = 1; /* PCM */
  hdr[22] = 1; /* mono */
  put_u32(hdr + 24, 1000);
  put_u32(hdr + 28, 1000);
  hdr[32] = 1;
  hdr[34] = 8;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, data_len);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  unsigned char *silence = malloc(data_len);
  if (silence) memset(silence, 0x80, data_len);
  int ok = silence && fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, data_len, 1, f) == 1;
  free(silence);
  if (fclose(f) != 0 || !ok) return 0;
  mc_tags tags = {0};
  tags.artist = "Sasha Vale";
  tags.title = (char *)title;
  return mc_tags_write(path, &tags) == MC_OK;
}

static mc_status no_lookup(const char *id, mb_release *out) {
  (void)id;
  memset(out, 0, sizeof *out);
  return MC_ERR_NOT_FOUND;
}

static mc_status no_recordings(const char *artist, const char *title, mb_release *out, int max, int *n) {
  (void)artist, (void)title, (void)out, (void)max;
  *n = 0;
  return MC_OK;
}

static const mb_fake k_fake = {NULL, no_lookup, no_recordings, NULL, NULL};

static mc_import_state item_state(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return MC_IMPORT_FAILED;
  mc_import_state s = it->state;
  mc_import_item_free(it);
  return s;
}

static int settled(mc_import_state s) {
  return s != MC_IMPORT_PENDING && s != MC_IMPORT_MATCHING && s != MC_IMPORT_WRITING;
}

/* Imports dir's two files as-is, nothing having matched. */
static void import_as_is(mc_library *lib, const char *dir) {
  mc_import *imp = NULL;
  const char *paths[] = {dir};
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};
  CHECK(mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (!imp) return;
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), ready = 0;
    for (int i = 0; i < n; i++) ready += settled(item_state(imp, i));
    if (n == 2 && ready == 2) break;
    mc_sleep_ms(20);
  }
  CHECK(mc_import_item_count(imp) == 2);
  for (int i = 0; i < 2; i++) {
    CHECK(item_state(imp, i) == MC_IMPORT_REVIEW);
    CHECK(mc_import_accept_as_is(imp, i) == MC_OK);
  }
  deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline &&
         (item_state(imp, 0) != MC_IMPORT_DONE || item_state(imp, 1) != MC_IMPORT_DONE))
    mc_sleep_ms(20);
  CHECK(item_state(imp, 0) == MC_IMPORT_DONE && item_state(imp, 1) == MC_IMPORT_DONE);
  mc_import_free(imp);
}

static void uid_of(mc_library *lib, const char *title, char uid[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt;
  uid[0] = '\0';
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT uid FROM tracks WHERE title = ?1", -1, &stmt, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(stmt, 1, title, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) snprintf(uid, MC_UID_STRLEN, "%s", sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
}

/* A long file with no album tag gets an album of its title, a short one doesn't, until it's
 * made a mix by hand, when its file follows. */
static void test_import(const char *scratch) {
  char root[1024], src[1100], lib_dir[1100], path[1300], uid[MC_UID_STRLEN];
  snprintf(root, sizeof root, "%s/test_mix_import.d", scratch);
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(src) == MC_OK && mc_mkdirs(lib_dir) == MC_OK);
  snprintf(path, sizeof path, "%s/1.wav", src);
  CHECK(write_track(path, "Qwxzzy Long Set", MC_MIX_MIN_MS + 5000));
  snprintf(path, sizeof path, "%s/2.wav", src);
  CHECK(write_track(path, "Qwxzzy Demo", 5000));

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  if (!lib) return;
  import_as_is(lib, src);

  CHECK(in_albums(lib, "Qwxzzy Long Set") == 1);
  CHECK(in_albums(lib, "Qwxzzy Demo") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE title = 'Qwxzzy Demo' AND album_uid IS NULL") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE mix <> 0") == 0); /* detected, not stored */
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE field = 'mix'") == 0);
  snprintf(path, sizeof path, "%s/Sasha Vale/Qwxzzy Long Set/Qwxzzy Long Set.wav", lib_dir);
  CHECK(mc_file_exists(path));
  snprintf(path, sizeof path, "%s/Sasha Vale/Qwxzzy Demo.wav", lib_dir);
  CHECK(mc_file_exists(path));

  /* A loose track made a mix by hand. */
  uid_of(lib, "Qwxzzy Demo", uid);
  mc_track_mix tm;
  CHECK(mc_track_mix_get(lib, uid, &tm) == MC_OK && tm.can_set && !tm.mix && !tm.mix_by_hand);
  CHECK(mc_track_mix_set(lib, uid, 0) == MC_OK); /* nothing to un-set: no album made */
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE title = 'Qwxzzy Demo' AND album_uid IS NULL") == 1);
  CHECK(mc_track_mix_set(lib, uid, 1) == MC_OK);
  CHECK(mc_track_mix_get(lib, uid, &tm) == MC_OK && tm.can_set && tm.mix && tm.mix_by_hand);
  CHECK(in_albums(lib, "Qwxzzy Demo") == 1);
  CHECK(!mc_file_exists(path));
  snprintf(path, sizeof path, "%s/Sasha Vale/Qwxzzy Demo/Qwxzzy Demo.wav", lib_dir);
  CHECK(mc_file_exists(path));
  mc_tags tags;
  CHECK(mc_tags_read(path, &tags) == MC_OK);
  CHECK_STR(tags.album, "Qwxzzy Demo");
  mc_tags_clear(&tags);

  /* Un-set, it's a single on its album, not back to no album. */
  CHECK(mc_track_mix_set(lib, uid, 0) == MC_OK);
  CHECK(in_albums(lib, "Qwxzzy Demo") == 0);
  mc_track_list *singles = NULL;
  mc_artist_list *artists = NULL;
  CHECK(mc_library_list_artists(lib, &artists) == MC_OK);
  if (artists && artists->count == 1) {
    CHECK(mc_library_list_artist_singles(lib, artists->items[0].uid, &singles) == MC_OK);
    CHECK(singles && singles->count == 1);
  }
  mc_track_list_free(singles);
  mc_artist_list_free(artists);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  test_listing(scratch);
  test_by_hand(scratch);
  test_migration(scratch);
  test_two_devices(scratch);
  test_duplicates(scratch);
  test_import(scratch);
  return TEST_RESULT();
}
