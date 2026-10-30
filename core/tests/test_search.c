#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "search/search_internal.h"
#include "test.h"
#include "util/platform.h"

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static char g_out[4096];

/* The hits for query as "<kind>:<name>" joined with '|', best first; the kind is R(artist),
 * A(lbum), T(rack), P(laylist) or G(roup), and a smart list's name ends in '*'. */
static const char *search(mc_library *lib, const char *query, int limit) {
  static const char kinds[] = "RATPG";
  mc_search_results *res = NULL;
  mc_status st = mc_library_search(lib, query, limit, &res);
  g_out[0] = '\0';
  if (st != MC_OK) {
    snprintf(g_out, sizeof g_out, "ERR %d", st);
    return g_out;
  }
  for (int i = 0; i < res->count; i++) {
    const mc_search_hit *h = &res->items[i];
    size_t n = strlen(g_out);
    snprintf(g_out + n, sizeof g_out - n, "%s%c:%s%s", i ? "|" : "", kinds[h->kind], h->name,
             h->is_smart ? "*" : "");
  }
  mc_search_results_free(res);
  return g_out;
}

/* Whether hit (e.g. "A:Abbey Road") is among query's hits. */
static int has(mc_library *lib, const char *query, const char *hit) {
  const char *out = search(lib, query, 0);
  size_t n = strlen(hit);
  for (const char *p = out; (p = strstr(p, hit)) != NULL; p++)
    if ((p == out || p[-1] == '|') && (p[n] == '|' || p[n] == '\0')) return 1;
  return 0;
}

/* The first hit for query, cut at its '|'. */
static const char *first(mc_library *lib, const char *query) {
  char *out = (char *)search(lib, query, 0);
  char *bar = strchr(out, '|');
  if (bar) *bar = '\0';
  return out;
}

/* Where hit is among query's hits, or -1. */
static int rank(mc_library *lib, const char *query, const char *hit) {
  const char *out = search(lib, query, 0);
  int i = 0;
  size_t n = strlen(hit);
  for (const char *p = out; *p; i++) {
    if (strncmp(p, hit, n) == 0 && (p[n] == '|' || p[n] == '\0')) return i;
    const char *bar = strchr(p, '|');
    if (!bar) break;
    p = bar + 1;
  }
  return -1;
}

static int count_kind(mc_library *lib, const char *query, int limit, char kind) {
  const char *out = search(lib, query, limit);
  int n = 0;
  for (const char *p = out; *p; p++)
    if (*p == kind && p[1] == ':' && (p == out || p[-1] == '|')) n++;
  return n;
}

static void populate(sqlite3 *db) {
  CHECK(exec(db,
             "INSERT INTO artists (uid, name, sort_name) VALUES ('ar1', 'The Beatles', 'Beatles, The'),"
             " ('ar2', 'Björk', NULL), ('ar3', 'AC/DC', NULL), ('ar4', 'Simon & Garfunkel', NULL),"
             " ('ar5', 'Radiohead', NULL), ('ar6', 'ABBA', NULL);"
             "INSERT INTO albums (uid, title, artist_uid) VALUES ('al1', 'Abbey Road', 'ar1'),"
             " ('al2', 'Homogenic', 'ar2'), ('al3', 'Back in Black', 'ar3'),"
             " ('al4', 'Bridge over Troubled Water', 'ar4'), ('al5', 'Pablo Honey', 'ar5'),"
             " ('al6', 'Help!', 'ar1'), ('al7', 'Arrival', 'ar6');"
             "INSERT INTO tracks (uid, album_uid, artist_uid, title) VALUES"
             " ('t1', 'al1', 'ar1', 'Come Together'), ('t2', 'al1', 'ar1', 'Something'),"
             " ('t3', 'al5', 'ar5', 'Creep'), ('t4', 'al6', 'ar1', 'Help!'),"
             " ('t5', 'al6', 'ar1', 'Help Me'), ('t6', 'al2', NULL, 'Jóga'),"
             " ('t7', 'al7', 'ar6', 'Dancing Queen');"
             "INSERT INTO playlists (uid, name) VALUES ('pl1', 'Road Trip');"
             "INSERT INTO album_groups (uid, name) VALUES ('g1', 'Swedish Pop');"
             "INSERT INTO smart_lists (uid, name, subject, rule) VALUES"
             " ('s1', 'Least Played', 'tracks', '{}'), ('s2', 'New Albums', 'albums', '{}'),"
             " ('s3', 'Weird Future', 'artists', '{}');") == SQLITE_OK);
}

static void test_typos(mc_library *lib) {
  CHECK_STR(first(lib, "beatels"), "R:The Beatles"); /* a swap */
  CHECK_STR(first(lib, "abey raod"), "A:Abbey Road"); /* a letter missing, then a swap */
  CHECK(has(lib, "abbye", "A:Abbey Road"));
  CHECK(has(lib, "garfunkle", "R:Simon & Garfunkel"));
  CHECK(has(lib, "radiohaed creap", "T:Creep")); /* one typo in each word */
  CHECK(has(lib, "hlep", "T:Help!")); /* 4 letters allow 1 edit, and a swap is one */
  /* 3 letters must be exact. */
  CHECK(!has(lib, "abc", "R:ABBA"));
  CHECK(!has(lib, "pob", "G:Swedish Pop"));
}

static void test_prefix(mc_library *lib) {
  CHECK(has(lib, "abb", "A:Abbey Road"));
  CHECK(has(lib, "abb", "R:ABBA"));
  CHECK(has(lib, "come tog", "T:Come Together"));
  CHECK(has(lib, "rad", "R:Radiohead"));
  /* A space after the last word means it's finished. */
  CHECK_STR(search(lib, "abb ", 0), "");
  CHECK(has(lib, "abba ", "R:ABBA"));
  /* Only the last word is a prefix. */
  CHECK(!has(lib, "abb road", "A:Abbey Road"));
}

static void test_folding(mc_library *lib) {
  CHECK_STR(first(lib, "bjork"), "R:Björk");
  CHECK_STR(first(lib, "BJÖRK"), "R:Björk");
  CHECK_STR(first(lib, "Bjo\xCC\x88rk"), "R:Björk"); /* decomposed ö */
  CHECK(has(lib, "joga", "T:Jóga"));
  CHECK_STR(first(lib, "simon and garfunkel"), "R:Simon & Garfunkel");
  CHECK_STR(first(lib, "simon & garfunkel"), "R:Simon & Garfunkel");
  CHECK_STR(first(lib, "acdc"), "R:AC/DC");
  CHECK_STR(first(lib, "ac dc"), "R:AC/DC");
  CHECK_STR(first(lib, "ac/dc"), "R:AC/DC");
  CHECK_STR(first(lib, "thebeatles"), "R:The Beatles");
  CHECK(has(lib, "help", "A:Help!"));
}

static void test_fields(mc_library *lib) {
  CHECK_STR(first(lib, "radiohead creep"), "T:Creep");
  CHECK_STR(first(lib, "beatles abbey"), "A:Abbey Road");
  CHECK(has(lib, "beatles abbey", "T:Come Together")); /* artist and album */
  CHECK(has(lib, "bjork joga", "T:Jóga"));            /* the album's artist */
  CHECK(!has(lib, "beatles creep", "T:Creep"));       /* every word must match */
  /* A track's own name counts for more than its artist's or album's. */
  CHECK_STR(first(lib, "beatles"), "R:The Beatles");
  CHECK(rank(lib, "help", "T:Help!") < rank(lib, "help", "T:Help Me"));
  CHECK(rank(lib, "help", "T:Help!") >= 0);
  /* A name that starts with the query beats one that only has it. */
  CHECK(rank(lib, "road", "P:Road Trip") < rank(lib, "road", "A:Abbey Road"));
  /* An exact word beats a typo: "abba" is 1 edit from the start of "abbey". */
  CHECK(rank(lib, "abba", "R:ABBA") == 0);
  CHECK(rank(lib, "abba", "A:Abbey Road") > 0);
}

/* A track's hit names its album, to open it on; other kinds' don't. */
static void test_album_uid(mc_library *lib) {
  mc_search_results *res = NULL;
  CHECK(mc_library_search(lib, "come together", 0, &res) == MC_OK);
  CHECK(res->count > 0 && res->items[0].kind == MC_SEARCH_TRACK);
  CHECK_STR(res->items[0].album_uid, "al1");
  mc_search_results_free(res);
  CHECK(mc_library_search(lib, "abbey road", 0, &res) == MC_OK);
  CHECK(res->count > 0 && res->items[0].kind == MC_SEARCH_ALBUM);
  CHECK_STR(res->items[0].album_uid, "");
  mc_search_results_free(res);
}

/* Pablo Honey has one track here: a single, found as its track but not as an album. Abbey
 * Road, with two, is an album. */
static void test_singles(mc_library *lib) {
  CHECK(!has(lib, "pablo honey", "A:Pablo Honey"));
  CHECK(has(lib, "pablo honey", "T:Creep"));
  CHECK(has(lib, "abbey road", "A:Abbey Road"));
}

static void test_kinds(mc_library *lib) {
  CHECK(has(lib, "road", "P:Road Trip"));
  CHECK_STR(first(lib, "swedish"), "G:Swedish Pop");
  CHECK_STR(first(lib, "least played"), "P:Least Played*");
  CHECK_STR(first(lib, "new albums"), "G:New Albums*");
  CHECK_STR(search(lib, "weird future", 0), ""); /* a subject this build doesn't know */
  CHECK(count_kind(lib, "beatles", 0, 'T') == 4);
  CHECK(count_kind(lib, "beatles", 2, 'T') == 2);
  CHECK(count_kind(lib, "beatles", 2, 'A') == 2);
  CHECK(count_kind(lib, "beatles", 1, 'R') == 1);
}

static void test_fresh(mc_library *lib, const char *path) {
  CHECK(has(lib, "road trip", "P:Road Trip"));
  int builds = mc_search_builds(lib);
  CHECK(builds >= 1);

  /* Searching again, or recording plays and recents, doesn't rebuild. */
  search(lib, "beatles", 0);
  CHECK(mc_library_record_play(lib, "t1") == MC_OK);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM, "al1") == MC_OK);
  search(lib, "beatles", 0);
  CHECK(mc_search_builds(lib) == builds);

  /* Nor does preparing when nothing changed. */
  CHECK(mc_library_search_prepare(lib) == MC_OK);
  mc_search_wait(lib);
  CHECK(mc_search_builds(lib) == builds);

  /* The search that notices a change starts a build and searches the old index meanwhile. */
  CHECK(mc_playlist_rename(lib, "pl1", "Night Drive") == MC_OK);
  CHECK(has(lib, "road trip", "P:Road Trip"));
  mc_search_wait(lib);
  CHECK(has(lib, "night drive", "P:Night Drive"));
  CHECK(!has(lib, "road trip", "P:Road Trip"));
  CHECK(mc_search_builds(lib) == builds + 1);

  /* A change on another connection, as an import or sync makes, then prepare. */
  mc_library *other = NULL;
  CHECK(mc_library_open(path, &other) == MC_OK);
  if (other) {
    CHECK(exec(mc_library_db(other), "INSERT INTO tracks (uid, album_uid, artist_uid, title)"
                                     " VALUES ('t8', 'al5', 'ar5', 'Anyone Can Play Guitar')") ==
          SQLITE_OK);
    mc_library_close(other);
  }
  CHECK(mc_library_search_prepare(lib) == MC_OK);
  mc_search_wait(lib);
  CHECK(mc_search_builds(lib) == builds + 2);
  CHECK(has(lib, "anyone can play", "T:Anyone Can Play Guitar"));

  CHECK(exec(mc_library_db(lib), "DELETE FROM tracks WHERE uid = 't8'") == SQLITE_OK);
  CHECK(mc_library_search_prepare(lib) == MC_OK);
  mc_search_wait(lib);
  CHECK(!has(lib, "anyone can play", "T:Anyone Can Play Guitar"));

  /* A change while a build runs is seen: by that build if it hadn't read the library yet,
   * else by another round of it. */
  for (int i = 0; i < 20; i++) {
    char sql[128], want[64];
    snprintf(sql, sizeof sql, "UPDATE playlists SET name = 'Round %c' WHERE uid = 'pl1'", 'a' + i);
    CHECK(exec(mc_library_db(lib), sql) == SQLITE_OK);
    CHECK(mc_library_search_prepare(lib) == MC_OK);
    if (i % 5 == 4) {
      mc_search_wait(lib);
      snprintf(want, sizeof want, "P:Round %c", 'a' + i);
      CHECK(has(lib, "round", want));
      CHECK(count_kind(lib, "round", 0, 'P') == 1);
    }
  }

  /* Closing a handle while its build runs. */
  mc_library *busy = NULL;
  CHECK(mc_library_open(path, &busy) == MC_OK);
  if (busy) {
    CHECK(mc_library_search_prepare(busy) == MC_OK);
    mc_library_close(busy);
  }
  CHECK(mc_library_search_prepare(NULL) == MC_ERR_INVALID_ARG);
}

static void test_edges(mc_library *lib) {
  mc_search_results *res = NULL;
  CHECK_STR(search(lib, "", 0), "");
  CHECK_STR(search(lib, "  !!! -- ", 0), "");
  CHECK_STR(search(lib, "zzzzqqq", 0), "");
  CHECK(mc_library_search(NULL, "a", 0, &res) == MC_ERR_INVALID_ARG);
  CHECK(mc_library_search(lib, NULL, 0, &res) == MC_ERR_INVALID_ARG);
  CHECK(mc_library_search(lib, "a", 0, NULL) == MC_ERR_INVALID_ARG);
  /* Words longer than the index keeps, and more words than a query keeps. */
  CHECK_STR(search(lib, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0), "");
  CHECK_STR(search(lib, "a b c d e f g h i j k l m n o p q r s t u v", 0), "");
  mc_search_results_free(NULL);
}

/* MC_SEARCH_BENCH=1: times a build and queries over 100k made-up tracks. Not a pass/fail. */
static void bench(const char *dir) {
  static const char *const syl[] = {"ka", "lo", "mi", "ne", "ru", "sa", "to", "vi", "be", "dor",
                                    "fen", "gal", "hu", "jin", "qua", "wes", "yor", "zel", "an", "el"};
  char path[1100];
  snprintf(path, sizeof path, "%s/bench", dir);
  remove_db(path);
  mc_library *lib = NULL;
  if (mc_library_open(path, &lib) != MC_OK) return;
  sqlite3 *db = mc_library_db(lib);
  unsigned seed = 1;
  char name[64];
#define RAND() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)
#define WORD(buf)                                                                       \
  do {                                                                                  \
    int ns = 2 + RAND() % 3;                                                            \
    for (int s = 0; s < ns; s++) strcat(buf, syl[RAND() % (sizeof syl / sizeof *syl)]); \
  } while (0)
  exec(db, "BEGIN");
  sqlite3_stmt *ar, *al, *tr;
  sqlite3_prepare_v2(db, "INSERT INTO artists (uid, name) VALUES (?1, ?2)", -1, &ar, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO albums (uid, title, artist_uid) VALUES (?1, ?2, ?3)", -1, &al, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO tracks (uid, album_uid, artist_uid, title) VALUES (?1, ?2, ?3, ?4)",
                     -1, &tr, NULL);
  char uid[32], ref[32], ref2[32];
  for (int i = 0; i < 3000; i++) {
    snprintf(uid, sizeof uid, "ar%d", i);
    name[0] = '\0';
    WORD(name);
    strcat(name, " ");
    WORD(name);
    sqlite3_bind_text(ar, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ar, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(ar);
    sqlite3_reset(ar);
  }
  for (int i = 0; i < 10000; i++) {
    snprintf(uid, sizeof uid, "al%d", i);
    snprintf(ref, sizeof ref, "ar%d", i % 3000);
    name[0] = '\0';
    WORD(name);
    sqlite3_bind_text(al, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(al, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(al, 3, ref, -1, SQLITE_TRANSIENT);
    sqlite3_step(al);
    sqlite3_reset(al);
  }
  for (int i = 0; i < 100000; i++) {
    snprintf(uid, sizeof uid, "t%d", i);
    snprintf(ref, sizeof ref, "al%d", i % 10000);
    snprintf(ref2, sizeof ref2, "ar%d", i % 3000);
    name[0] = '\0';
    int nw = 1 + RAND() % 4;
    for (int w = 0; w < nw; w++) {
      if (w) strcat(name, " ");
      WORD(name);
    }
    sqlite3_bind_text(tr, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tr, 2, ref, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tr, 3, ref2, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(tr, 4, name, -1, SQLITE_TRANSIENT);
    sqlite3_step(tr);
    sqlite3_reset(tr);
  }
  sqlite3_finalize(ar);
  sqlite3_finalize(al);
  sqlite3_finalize(tr);
  exec(db, "COMMIT");

  mc_search_results *res = NULL;
  uint64_t t0 = mc_mono_ms();
  mc_library_search(lib, "x", 10, &res);
  mc_search_results_free(res);
  printf("build %llu ms\n", (unsigned long long)(mc_mono_ms() - t0));

  static const char *const queries[] = {"kalomi", "kalmoi", "fengalhu", "ru", "wesyorzel anel",
                                        "dorfen ka", "quajin", "b", "tosavi nekalo"};
  int nq = sizeof queries / sizeof *queries, runs = 100, hits = 0;
  uint64_t total = 0;
  for (int q = 0; q < nq; q++) {
    uint64_t t = mc_mono_ms();
    for (int r = 0; r < runs; r++) {
      mc_library_search(lib, queries[q], 10, &res);
      hits += res ? res->count : 0;
      mc_search_results_free(res);
    }
    t = mc_mono_ms() - t;
    total += t;
    printf("  %-16s %.2f ms\n", queries[q], (double)t / runs);
  }
  printf("%.2f ms per query, %d hits, %d builds\n", (double)total / (runs * nq), hits / runs,
         mc_search_builds(lib));
  mc_library_close(lib);
  remove_db(path);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  if (getenv("MC_SEARCH_BENCH")) {
    bench(dir);
    return 0;
  }
  char path[1024];
  snprintf(path, sizeof path, "%s/test_search.d", dir);
  remove_db(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();
  populate(mc_library_db(lib));

  test_typos(lib);
  test_prefix(lib);
  test_folding(lib);
  test_fields(lib);
  test_album_uid(lib);
  test_singles(lib);
  test_kinds(lib);
  test_fresh(lib, path);
  test_edges(lib);

  mc_library_close(lib);
  return TEST_RESULT();
}
