/* The orphan sweep (sync/orphans.h), straight against a scratch library: one case for each kind
 * of file the app keeps in the library folder, path matching, each refusal, the bounds, the
 * gate, and the sweep after an import. Sessions' sweeps are in test_sync_files.c. Nothing here
 * reaches a real trash: mc_transfer_trash is a folder of the test's. */
#define _POSIX_C_SOURCE 200809L /* symlink() under -std=c11 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "library/edit_internal.h"
#include "library/folder_gate.h"
#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/libmerge.h"
#include "sync/orphans.h"
#include "sync/transfer.h"
#include "tagger/musicbrainz.h"
#include "util/pathkey.h"
#include "util/platform.h"
#include "test.h"

static char g_root[1024];
static int g_case;

typedef struct fx {
  char dir[1200], config[1200];
  mc_library *lib;
} fx;

/* --- The trash --- */

static char g_bin[1200];
static int g_binned, g_fail_trash;
static const char *g_refuse; /* a file name the trash won't take */
static const char *g_arrive; /* a folder someone starts work in on the first trashing */
static struct fx *g_late;    /* a library that gets rows on the first trashing */
static void row_of(struct fx *f, const char *rel);

static mc_status fake_trash(const char *path) {
  if (g_refuse && strstr(path, g_refuse)) {
    g_fail_trash++;
    return MC_ERR_IO;
  }
  if (g_arrive) {
    mc_folder_hold(g_arrive);
    g_arrive = NULL;
  }
  if (g_late) {
    struct fx *f = g_late;
    g_late = NULL;
    row_of(f, strstr(path, "race a") ? "race b.txt" : "race a.txt");
  }
  char to[1300];
  snprintf(to, sizeof to, "%s%c%d", g_bin, MC_PATH_SEP, g_binned);
  mc_status st = mc_rename(path, to, 0);
  if (st == MC_OK) g_binned++;
  return st;
}

/* --- Fixture --- */

static void make_old(const char *path) {
#ifdef _WIN32
  wchar_t *w = mc_win_path(path);
  struct _utimbuf t = {1000000000, 1000000000}; /* 2001 */
  CHECK(w && _wutime(w, &t) == 0);
  free(w);
#else
  struct utimbuf t = {1000000000, 1000000000};
  CHECK(utime(path, &t) == 0);
#endif
}

static void parent_dirs(const char *abs) {
  char *dir = mc_strdup(abs);
  char *sep = strrchr(dir, '/'), *back = strrchr(dir, '\\');
  if (back > sep) sep = back;
  if (sep) *sep = '\0';
  CHECK(mc_mkdirs(dir) == MC_OK);
  free(dir);
}

/* A file at rel, made long ago unless young. */
static void put_as(fx *f, const char *rel, int young) {
  char *abs = mc_library_abspath(f->lib, rel);
  parent_dirs(abs);
  FILE *o = mc_fopen(abs, "wb");
  CHECK(o != NULL);
  if (o) {
    fputs(rel, o);
    fclose(o);
  }
  if (!young) make_old(abs);
  free(abs);
}

static void put(fx *f, const char *rel) {
  put_as(f, rel, 0);
}

static int there(fx *f, const char *rel) {
  char *abs = mc_library_abspath(f->lib, rel);
  int yes = mc_file_exists(abs);
  free(abs);
  return yes;
}

static void row_of(struct fx *f, const char *rel) {
  static int n;
  char uid[40];
  snprintf(uid, sizeof uid, "file-%d", ++n);
  sqlite3 *db = mc_library_db(f->lib);
  sqlite3_exec(db, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(db, "INSERT INTO files (uid, track_uid, path, format) VALUES (?1, 'track', ?2, 'flac')",
                           -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, rel, -1, SQLITE_STATIC);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

/* A file the library knows. */
static void track(fx *f, const char *rel) {
  put(f, rel);
  row_of(f, rel);
}

static void meta(fx *f, const char *key, const char *value) {
  CHECK(mc_library_meta_set(f->lib, key, value) == MC_OK);
}

/* A library with ten tracks, so that a few orphans don't trip the guard. */
static void fx_open_bare(fx *f) {
  memset(f, 0, sizeof *f);
  snprintf(f->dir, sizeof f->dir, "%s/%d/lib", g_root, ++g_case);
  snprintf(f->config, sizeof f->config, "%s/%d/config", g_root, g_case);
  CHECK(mc_mkdirs(f->dir) == MC_OK && mc_mkdirs(f->config) == MC_OK);
  CHECK(mc_library_open(f->dir, &f->lib) == MC_OK);
}

static void fx_open(fx *f) {
  fx_open_bare(f);
  for (int i = 0; i < 10; i++) {
    char rel[64];
    snprintf(rel, sizeof rel, "Artist/Album/%02d Song.flac", i);
    track(f, rel);
  }
}

static void fx_close(fx *f) {
  mc_library_close(f->lib);
}

static mc_status sweep(fx *f, mc_orphans_result *r) {
  mc_orphans_args a = {f->lib, f->config, 0, 0, NULL, NULL};
  return mc_orphans_sweep(&a, r);
}

/* --- Path keys --- */

static void same_key(const char *a, const char *b) {
  char *ka = mc_path_key(a), *kb = mc_path_key(b);
  CHECK(ka && kb);
  if (ka && kb && strcmp(ka, kb)) fprintf(stderr, "keys differ: %s / %s\n", a, b);
  CHECK(ka && kb && !strcmp(ka, kb));
  free(ka);
  free(kb);
}

static void other_key(const char *a, const char *b) {
  char *ka = mc_path_key(a), *kb = mc_path_key(b);
  CHECK(ka && kb && strcmp(ka, kb));
  free(ka);
  free(kb);
}

/* NFC and NFD of a name in each script where they differ, and the same with the case changed. */
static const char *const k_names[][2] = {
  {"Caf\xC3\xA9", "Cafe\xCC\x81"},                                  /* é */
  {"Bj\xC3\xB6rk", "Bjo\xCC\x88rk"},                                /* ö */
  {"Sigur R\xC3\xB3s", "Sigur Ro\xCC\x81s"},                        /* ó */
  {"M\xE1\xBB\xB9 T\xC3\xA2m", "My\xCC\x83 Ta\xCC\x82m"},           /* Vietnamese, past Latin-1 */
  {"Vi\xE1\xBB\x87t", "Vie\xCC\xA3\xCC\x82t"},                      /* ệ, two marks */
  {"\xD0\x99\xD0\xBE", "\xD0\x98\xCC\x86\xD0\xBE"},                 /* Cyrillic Й */
  {"\xCE\x86\xCE\xBB\xCF\x86\xCE\xB1", "\xCE\x91\xCC\x81\xCE\xBB\xCF\x86\xCE\xB1"}, /* Greek Ά */
  {"\xE3\x81\x8C", "\xE3\x81\x8B\xE3\x82\x99"},                     /* Japanese が */
  {"\xED\x95\x9C\xEA\xB8\x80", "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB\xE1\x84\x80\xE1\x85\xB3\xE1\x86\xAF"}, /* 한글 */
};
#define NAMES (int)(sizeof k_names / sizeof k_names[0])

static void test_keys(void) {
  for (int i = 0; i < NAMES; i++) same_key(k_names[i][0], k_names[i][1]);
  same_key("Artist/Album/01 Song.FLAC", "artist\\album\\01 song.flac");
  same_key("\xC3\x89t\xC3\xA9", "\xC3\xA9T\xC3\x89");               /* Été, cases */
  same_key("\xD0\xAF", "\xD1\x8F");                                 /* Я */
  same_key("Stra\xC3\x9F" "e", "STRASSE");
  /* Different names stay different. */
  other_key("01 Song.flac", "01 Song.mp3");
  other_key("Song.flac", "Song (2).flac");
  other_key("a/b.flac", "a/c.flac");
  other_key("\xE6\x97\xA5\xE6\x9C\xAC", "\xE4\xB8\xAD\xE5\x9B\xBD"); /* 日本, 中国 */
}

/* --- What's known, a category at a time --- */

static void test_categories(void) {
  fx f;
  fx_open(&f);
  /* Transcodes are files rows like originals. */
  track(&f, "Artist/Album/07 Song.opus");
  /* Covers, in the album's folder and one above a disc folder, every name a cover can have. */
  static const char *const covers[] = {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "folder.jpeg",
                                       "folder.png", "front.jpg", "front.jpeg", "front.png", "COVER.JPG"};
  fx g;
  fx_open(&g);
  for (int i = 0; i < 10; i++) {
    char rel[64];
    snprintf(rel, sizeof rel, "Artist/Album/%s", covers[i]);
    put(&g, rel);
  }
  track(&f, "Box/Set/CD1/01 Disc.flac");
  put(&f, "Box/Set/cover.jpg");
  /* The database and whatever SQLite keeps beside it, and sets aside. */
  put(&f, "library.db-journal");
  put(&f, "library-before-merge-2026-11-01.db-wal");
  /* Hidden edit and refresh copies and backups, in place. */
  put(&f, "Artist/Album/.kithsong-0a1b2c3d-01 Song.flac");
  put(&f, "Artist/Album/.kithsong-0a1b2c3d-01 Song.flac.old");
  put(&f, ".kithsong-probe-001122334455");
  /* Undoing a merge. */
  put(&f, ".merge-restore/library.db");
  /* What belongs to the folder or the volume. */
  put(&f, "desktop.ini");
  put(&f, "Artist/Desktop.ini");
  put(&f, "$RECYCLE.BIN/S-1-5-21/$R1.mp3");
  put(&f, "System Volume Information/tracking.log");
  put(&f, "lost+found/#123");
  put(&f, ".Trash-1000/files/old.mp3");
  put(&f, ".Trash/1000/files/old.mp3");
  /* In flight: too young, whatever it's called. */
  put_as(&f, "Artist/Album/02 Song.flac.part", 1);
  put_as(&f, "Box/Set/cover.png.tmp", 1);

  /* And what isn't: left by a crash, a cover of nothing, the same names not at the top. */
  put(&f, "Artist/Album/03 Song.flac.part");
  put(&f, "Artist/Album/cover.jpg.tmp");
  put(&f, "Gone/Album/cover.jpg");
  put(&f, "Artist/library.db");

  int bin = g_binned;
  mc_orphans_result r;
  CHECK(sweep(&f, &r) == MC_OK);
  CHECK(r.known == 12 && r.orphans == 4 && r.removed == 4 && r.failed == 0 && r.young == 2);
  CHECK(g_binned == bin + 4);
  CHECK(!there(&f, "Artist/Album/03 Song.flac.part") && !there(&f, "Artist/Album/cover.jpg.tmp"));
  CHECK(!there(&f, "Gone/Album/cover.jpg") && !there(&f, "Gone") && !there(&f, "Artist/library.db"));
  static const char *const kept[] = {
    "library.db", "Artist/Album/00 Song.flac", "Artist/Album/07 Song.opus", "Box/Set/CD1/01 Disc.flac",
    "Box/Set/cover.jpg", "library.db-journal", "library-before-merge-2026-11-01.db-wal",
    "Artist/Album/.kithsong-0a1b2c3d-01 Song.flac", "Artist/Album/.kithsong-0a1b2c3d-01 Song.flac.old",
    ".kithsong-probe-001122334455", ".merge-restore/library.db", "desktop.ini", "Artist/Desktop.ini",
    "$RECYCLE.BIN/S-1-5-21/$R1.mp3", "System Volume Information/tracking.log", "lost+found/#123",
    ".Trash-1000/files/old.mp3", ".Trash/1000/files/old.mp3", "Artist/Album/02 Song.flac.part",
    "Box/Set/cover.png.tmp"};
  for (int i = 0; i < (int)(sizeof kept / sizeof kept[0]); i++) {
    if (!there(&f, kept[i])) fprintf(stderr, "gone: %s\n", kept[i]);
    CHECK(there(&f, kept[i]));
  }
  /* Every cover name, next to the album's files. */
  CHECK(sweep(&g, &r) == MC_OK);
  CHECK(r.known == 10 && r.orphans == 0);
  fx_close(&f);
  fx_close(&g);

  /* Stray hidden files aren't spared: "every file". */
  fx_open(&f);
  put(&f, ".hidden.txt");
  put(&f, "Artist/.DS_Store");
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 2);
  CHECK(!there(&f, ".hidden.txt") && !there(&f, "Artist/.DS_Store"));
  fx_close(&f);

  /* Any type, and folders left empty go, however deep; a bare one nearby stays, and the top. */
  fx_open(&f);
  put(&f, "notes.txt");
  put(&f, "Stray/deep/er/a.mp3");
  put(&f, "Stray/deep/b.pdf");
  put(&f, "Artist/Album/photo.heic");
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 4);
  CHECK(!there(&f, "Stray") && there(&f, "Artist/Album/00 Song.flac"));
  CHECK(mc_file_exists(f.dir));
  fx_close(&f);
}

/* The rows of a set-aside library: what a merge left behind, and the copy itself. */
static void test_snapshot(void) {
  fx f, s;
  fx_open(&f);
  fx_open_bare(&s);
  row_of(&s, "Kept/By/snapshot.flac");
  put(&f, "Kept/By/snapshot.flac");
  put(&f, "Kept/By/cover.jpg");
  put(&f, "stray.txt");
  char from[1300], to[1300];
  snprintf(from, sizeof from, "%s/library.db", s.dir);
  fx_close(&s);
  snprintf(to, sizeof to, "%s/library-before-merge-2026-11-01.db", f.dir);
  mc_library *probe = NULL;
  CHECK(mc_library_open(s.dir, &probe) == MC_OK);
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(mc_library_db(probe), "VACUUM INTO ?1", -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, to, -1, SQLITE_STATIC);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  mc_library_close(probe);
  mc_orphans_result r;
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 1);
  CHECK(!there(&f, "stray.txt") && there(&f, "Kept/By/snapshot.flac") && there(&f, "Kept/By/cover.jpg"));
  CHECK(there(&f, "library-before-merge-2026-11-01.db"));

  /* One that can't be read might be hiding the same: nothing goes. */
  put(&f, "stray.txt");
  put(&f, "library-before-merge-2026-11-02.db");
  CHECK(sweep(&f, &r) == MC_ERR_DB && r.removed == 0 && there(&f, "stray.txt"));
  fx_close(&f);
}

/* --- Path matching --- */

static void test_paths(void) {
  fx f;
  fx_open(&f);
  mc_orphans_result r;
  /* A row in one normal form, the file on disk in the other: neither way round is an orphan. */
  for (int i = 0; i < NAMES; i++) {
    char row[128], file[128];
    snprintf(row, sizeof row, "Normal/%s/nfc%d.flac", k_names[i][0], i);
    snprintf(file, sizeof file, "Normal/%s/nfc%d.flac", k_names[i][1], i);
    row_of(&f, row);
    put(&f, file);
    snprintf(row, sizeof row, "Normal/%s/nfd%d.flac", k_names[i][1], i);
    snprintf(file, sizeof file, "Normal/%s/nfd%d.flac", k_names[i][0], i);
    row_of(&f, row);
    put(&f, file);
  }
  /* Letter case, separators in a row, and a real non-ASCII name found as stored. */
  row_of(&f, "Case/ALBUM/01 SONG.FLAC");
  put(&f, "Case/Album/01 Song.flac");
  row_of(&f, "Sep\\Album\\01 Song.flac");
  put(&f, "Sep/Album/01 Song.flac");
  track(&f, "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E/\xD0\x9C\xD1\x83\xD0\xB7\xD1\x8B\xD0\xBA\xD0\xB0/01 \xF0\x9F\x8E\xB5.flac");
  /* An orphan beside them all, with a non-ASCII name. */
  put(&f, "Normal/\xC3\xA9tranger.txt");
  int bin = g_binned;
  CHECK(sweep(&f, &r) == MC_OK);
  CHECK(r.orphans == 1 && r.removed == 1 && g_binned == bin + 1);
  CHECK(r.known == 10 + 2 * NAMES + 3);
  CHECK(!there(&f, "Normal/\xC3\xA9tranger.txt"));
  for (int i = 0; i < NAMES; i++) {
    char file[128];
    snprintf(file, sizeof file, "Normal/%s/nfc%d.flac", k_names[i][1], i);
    CHECK(there(&f, file));
  }
  fx_close(&f);

  /* Past MAX_PATH, known and not. */
  fx_open(&f);
  char deep[900] = "Long";
  for (int i = 0; i < 16; i++) strcat(deep, "/a-folder-with-quite-a-long-name-for-a-folder");
  char known[1000], stray[1000];
  snprintf(known, sizeof known, "%s/\xC3\xA9 known.flac", deep);
  snprintf(stray, sizeof stray, "%s/stray.txt", deep);
  CHECK(strlen(known) > 300);
  track(&f, known);
  put(&f, stray);
  CHECK(sweep(&f, &r) == MC_OK);
  CHECK(r.known == 11 && r.removed == 1 && there(&f, known) && !there(&f, stray));
  fx_close(&f);
}

/* --- Refusals --- */

static void test_refusals(void) {
  fx f;
  mc_orphans_result r;

  /* The library knows nothing. */
  fx_open_bare(&f);
  put(&f, "a.mp3");
  CHECK(sweep(&f, &r) == MC_ERR_INVALID_ARG && there(&f, "a.mp3"));
  /* Rows, but none of its files found: a library pointed at the wrong folder. */
  row_of(&f, "Somewhere/else.flac");
  CHECK(sweep(&f, &r) == MC_ERR_INVALID_ARG && there(&f, "a.mp3"));
  fx_close(&f);

  /* The guard: one orphan too many, then exactly as many. */
  fx_open(&f);
  for (int i = 0; i < 11; i++) {
    char rel[32];
    snprintf(rel, sizeof rel, "unimported %d.mp3", i);
    put(&f, rel);
  }
  CHECK(sweep(&f, &r) == MC_ERR_INVALID_ARG);
  CHECK(r.known == 10 && r.orphans == 11 && r.removed == 0 && there(&f, "unimported 0.mp3"));
  char *one = mc_library_abspath(f.lib, "unimported 10.mp3");
  CHECK(mc_remove(one) == MC_OK);
  free(one);
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 10);
  fx_close(&f);

  /* The walk hits an error part-way: nothing's removed, not even what it had already seen. */
  fx_open(&f);
  put(&f, "a stray.txt");
  put(&f, "zzz/stray.txt");
  mc_tree_test_fail = "zzz";
  CHECK(sweep(&f, &r) == MC_ERR_IO);
  mc_tree_test_fail = NULL;
  CHECK(there(&f, "a stray.txt") && there(&f, "zzz/stray.txt"));
  /* A root that's gone, or a file. */
  CHECK(mc_tree_walk("no/such/folder", NULL, NULL) == MC_ERR_NOT_FOUND);
  char *db = mc_library_abspath(f.lib, MC_LIBRARY_DB_NAME);
  CHECK(mc_tree_walk(db, NULL, NULL) == MC_ERR_IO);
  free(db);

  /* An edit's swap or a merge not finished. */
  static const char *const keys[] = {MC_EDIT_SWAP_META, MC_LIBMERGE_META, MC_LIBMERGE_WAIT_META};
  for (int i = 0; i < 3; i++) {
    meta(&f, keys[i], "x");
    CHECK(sweep(&f, &r) == MC_ERR_LOCKED && there(&f, "a stray.txt"));
    meta(&f, keys[i], NULL);
  }

  /* The config folder and the library the same place. */
  mc_orphans_args a = {f.lib, f.dir, 0, 0, NULL, NULL};
  CHECK(mc_orphans_sweep(&a, &r) == MC_ERR_INVALID_ARG && there(&f, "a stray.txt"));

  /* An archive reports. */
  a = (mc_orphans_args){f.lib, f.config, 1, 0, NULL, NULL};
  CHECK(mc_orphans_sweep(&a, &r) == MC_OK && r.orphans == 2 && r.removed == 0);
  CHECK(there(&f, "a stray.txt") && there(&f, "zzz/stray.txt"));
  fx_close(&f);
}

/* --- Bounds --- */

static int make_link(const char *link, const char *target) {
#ifdef _WIN32
  char l[1300], t[1300], cmd[2800];
  snprintf(l, sizeof l, "%s", link);
  snprintf(t, sizeof t, "%s", target);
  for (char *p = l; *p; p++)
    if (*p == '/') *p = '\\';
  for (char *p = t; *p; p++)
    if (*p == '/') *p = '\\';
  snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", l, t);
  return system(cmd) == 0 && mc_is_link(link);
#else
  return symlink(target, link) == 0;
#endif
}

static void test_bounds(void) {
  fx f;
  fx_open(&f);
  char out[1300], stray[1400], link[1300];
  snprintf(out, sizeof out, "%s/%d/outside", g_root, g_case);
  CHECK(mc_mkdirs(out) == MC_OK);
  snprintf(stray, sizeof stray, "%s/precious.mp3", out);
  FILE *o = mc_fopen(stray, "wb");
  CHECK(o != NULL);
  if (o) fclose(o);
  make_old(stray);
  snprintf(link, sizeof link, "%s/Linked", f.dir);
  int linked = make_link(link, out);
  if (!linked) fprintf(stderr, "can't make a folder link; skipping that case\n");
  put(&f, "stray.txt");
  /* The config folder, moved inside by a user. */
  char config[1300];
  snprintf(config, sizeof config, "%s%cConfig", f.dir, MC_PATH_SEP);
  put(&f, "Config/sync.db");
  put(&f, "Config/transcode-cache/x/opus-96.opus");
  mc_orphans_args a = {f.lib, config, 0, 0, NULL, NULL};
  mc_orphans_result r;
  CHECK(mc_orphans_sweep(&a, &r) == MC_OK && r.removed == 1);
  CHECK(!there(&f, "stray.txt") && mc_file_exists(stray));
  CHECK(there(&f, "Config/sync.db") && there(&f, "Config/transcode-cache/x/opus-96.opus"));
  if (linked) CHECK(mc_is_link(link));
  fx_close(&f);
}

/* --- When it runs --- */

static int say_stop(void *user) {
  (void)user;
  return 1;
}

static void test_timing(void) {
  fx f;
  mc_orphans_result r;
  fx_open(&f);

  /* Someone in the folder: it doesn't start. */
  put(&f, "stray.txt");
  put(&f, "stray2.txt");
  mc_folder_hold(f.dir);
  CHECK(sweep(&f, &r) == MC_ERR_LOCKED && there(&f, "stray.txt"));
  /* The caller's own hold doesn't count. */
  mc_folder_hold(f.dir);
  mc_orphans_args own = {f.lib, f.config, 1, 1, NULL, NULL};
  CHECK(mc_orphans_sweep(&own, &r) == MC_ERR_LOCKED);
  mc_folder_release(f.dir);
  CHECK(mc_orphans_sweep(&own, &r) == MC_OK);
  mc_folder_release(f.dir);

  /* A sweep watching: another doesn't start beside it. */
  mc_folder_watch *w = mc_folder_watch_begin(f.dir, 0);
  CHECK(w && !mc_folder_watch_begin(f.dir, 0));
  CHECK(mc_folder_watch_clear(w));
  mc_folder_hold(f.dir);
  CHECK(!mc_folder_watch_clear(w));
  mc_folder_release(f.dir);
  CHECK(!mc_folder_watch_clear(w)); /* gone again, but it did come */
  mc_folder_watch_end(w);

  /* Someone arrives after the first removal: the rest wait. */
  g_arrive = f.dir;
  CHECK(sweep(&f, &r) == MC_ERR_LOCKED && r.removed == 1);
  CHECK(there(&f, "stray.txt") != there(&f, "stray2.txt"));
  mc_folder_release(f.dir);
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 1);
  CHECK(!there(&f, "stray.txt") && !there(&f, "stray2.txt"));

  /* Stopping the app stops it, walking or removing. */
  put(&f, "stray.txt");
  mc_orphans_args stop = {f.lib, f.config, 0, 0, say_stop, NULL};
  CHECK(mc_orphans_sweep(&stop, &r) == MC_ERR_IN_USE && there(&f, "stray.txt"));

  /* Age: young by its modified time, and old by that but freshly copied in. */
  put_as(&f, "new.txt", 1);
  CHECK(sweep(&f, &r) == MC_OK && r.young == 1 && there(&f, "new.txt"));
  put(&f, "copied in.mp3");
  mc_orphans_test_mtime_only = 0;
  CHECK(sweep(&f, &r) == MC_OK);
  CHECK(there(&f, "copied in.mp3"));
  mc_orphans_test_mtime_only = 1;
  fx_close(&f);

  /* One the trash won't take: the rest go, it stays, and it's never deleted instead. */
  fx_open(&f);
  put(&f, "a.txt");
  put(&f, "stuck.txt");
  put(&f, "z.txt");
  g_refuse = "stuck";
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 2 && r.failed == 1);
  g_refuse = NULL;
  CHECK(!there(&f, "a.txt") && there(&f, "stuck.txt") && !there(&f, "z.txt"));

  /* Android's way: gone for good, no trash. */
  int bin = g_binned;
  mc_orphans_permanent = 1;
  CHECK(sweep(&f, &r) == MC_OK && r.removed == 1);
  mc_orphans_permanent = 0;
  CHECK(!there(&f, "stuck.txt") && g_binned == bin);
  CHECK(there(&f, "Artist/Album/00 Song.flac"));

  /* A row that turns up after the rows were read, as a second app on the library could write:
   * the first file's removal makes it, and whichever of the two is left then has one. */
  put(&f, "race a.txt");
  put(&f, "race b.txt");
  g_late = &f;
  CHECK(sweep(&f, &r) == MC_OK && r.orphans == 2 && r.removed == 1 && r.failed == 0);
  CHECK(there(&f, "race a.txt") != there(&f, "race b.txt"));
  fx_close(&f);
}

/* --- After an import --- */

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

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void write_wav(const char *path, const char *title) {
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
  parent_dirs(path);
  FILE *f = mc_fopen(path, "wb");
  CHECK(f != NULL);
  if (!f) return;
  static const unsigned char silence[DATA_LEN];
  CHECK(fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, sizeof silence, 1, f) == 1);
  fclose(f);
  mc_tags tags = {0};
  tags.artist = "Someone";
  tags.title = (char *)title;
  CHECK(mc_tags_write(path, &tags) == MC_OK);
}

static int item_state(mc_import *imp, int i) {
  mc_import_item *it;
  if (mc_import_item_get(imp, i, &it) != MC_OK) return -1;
  int state = it->state;
  mc_import_item_free(it);
  return state;
}

/* Until every item is past matching. */
static int wait_items(mc_import *imp, int n) {
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    int settled = 0;
    if (mc_import_item_count(imp) >= n)
      for (int i = 0; i < n; i++) {
        int s = item_state(imp, i);
        settled += s == MC_IMPORT_DONE || s == MC_IMPORT_SKIPPED || s == MC_IMPORT_FAILED || s == MC_IMPORT_REVIEW;
      }
    if (settled == n) return 1;
    mc_sleep_ms(10);
  }
  return 0;
}

/* Until the import has let go of the folder: its sweep is over. */
static int wait_let_go(const char *dir) {
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    mc_folder_watch *w = mc_folder_watch_begin(dir, 0);
    if (w) {
      mc_folder_watch_end(w);
      return 1;
    }
    mc_sleep_ms(10);
  }
  return 0;
}

static void source(fx *f, char *out, size_t cap, int items) {
  snprintf(out, cap, "%s/%d/src", g_root, g_case);
  (void)f;
  for (int i = 0; i < items; i++) {
    char path[1400], title[32];
    snprintf(path, sizeof path, "%s/%d.wav", out, i);
    snprintf(title, sizeof title, "Qwxzzy Demo %d", i);
    write_wav(path, title);
  }
}

static void test_import(void) {
  mc_import_options as_is = {.threshold = 0, .fetch_art = 0, .skip_tagging = 1};
  mc_import_options review = {.threshold = 0, .fetch_art = 0};
  char src[1300];
  const char *paths[] = {src};
  mc_import *imp;
  fx f;

  /* Complete: the stray goes, what was imported stays. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 1);
  CHECK(mc_import_start(f.lib, paths, 1, &as_is, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 1) && item_state(imp, 0) == MC_IMPORT_DONE);
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(!there(&f, "stray.txt") && there(&f, "Artist/Album/00 Song.flac"));
  mc_orphans_result r;
  CHECK(sweep(&f, &r) == MC_OK && r.known == 11 && r.orphans == 0);
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* In review, as an item awaiting a decision is: nothing sweeps, from either side. Once the
   * user decides, the import's own sweep runs. A skip leaves no import to sweep after. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 2);
  CHECK(mc_import_start(f.lib, paths, 1, &review, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 2));
  CHECK(item_state(imp, 0) == MC_IMPORT_REVIEW && item_state(imp, 1) == MC_IMPORT_REVIEW);
  CHECK(sweep(&f, &r) == MC_ERR_LOCKED && there(&f, "stray.txt"));
  CHECK(mc_import_accept_as_is(imp, 0) == MC_OK);
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline && item_state(imp, 0) != MC_IMPORT_DONE) mc_sleep_ms(10);
  CHECK(item_state(imp, 0) == MC_IMPORT_DONE);
  mc_sleep_ms(200);
  CHECK(sweep(&f, &r) == MC_ERR_LOCKED && there(&f, "stray.txt")); /* one still undecided */
  CHECK(mc_import_skip(imp, 1) == MC_OK);
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(!there(&f, "stray.txt"));
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* Cancelled: closed with an item still in review. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 1);
  CHECK(mc_import_start(f.lib, paths, 1, &review, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 1));
  mc_import_free(imp);
  CHECK(there(&f, "stray.txt"));
  CHECK(wait_let_go(f.dir)); /* and it let go */
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* Everything skipped: no import happened. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 1);
  CHECK(mc_import_start(f.lib, paths, 1, &review, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 1));
  CHECK(mc_import_skip(imp, 0) == MC_OK);
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(there(&f, "stray.txt"));
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* Failed: a zip that can't be read beside a good file. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 1);
  char bad[1400];
  snprintf(bad, sizeof bad, "%s/broken.zip", src);
  FILE *z = mc_fopen(bad, "wb");
  CHECK(z != NULL);
  if (z) {
    fputs("not a zip", z);
    fclose(z);
  }
  CHECK(mc_import_start(f.lib, paths, 1, &as_is, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 2));
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(there(&f, "stray.txt"));
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* A source inside the library folder: its skipped files, and all else, are left. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  snprintf(src, sizeof src, "%s/Inbox", f.dir);
  char in[1400];
  snprintf(in, sizeof in, "%s/0.wav", src);
  write_wav(in, "Qwxzzy Inside");
  make_old(in);
  CHECK(mc_import_start(f.lib, paths, 1, &as_is, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 1) && item_state(imp, 0) == MC_IMPORT_DONE);
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(there(&f, "stray.txt"));
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* The library inside the source, likewise. */
  fx_open(&f);
  mc_folder_note_config(f.dir, f.config);
  put(&f, "stray.txt");
  const char *above[] = {f.dir};
  mc_orphans_after_import(f.lib, above, 1, 0, NULL, NULL);
  CHECK(there(&f, "stray.txt"));
  char up[1300];
  snprintf(up, sizeof up, "%s/%d", g_root, g_case);
  above[0] = up;
  mc_orphans_after_import(f.lib, above, 1, 0, NULL, NULL);
  CHECK(there(&f, "stray.txt"));
  /* A sibling whose name starts the same isn't inside. */
  snprintf(up, sizeof up, "%s/%d/lib2", g_root, g_case);
  mc_orphans_after_import(f.lib, above, 1, 0, NULL, NULL);
  CHECK(!there(&f, "stray.txt"));
  mc_folder_note_config(f.dir, NULL);
  fx_close(&f);

  /* No sync running: nothing to say what this device may do. */
  fx_open(&f);
  put(&f, "stray.txt");
  source(&f, src, sizeof src, 1);
  CHECK(mc_import_start(f.lib, paths, 1, &as_is, NULL, NULL, &imp) == MC_OK);
  CHECK(wait_items(imp, 1));
  CHECK(wait_let_go(f.dir));
  mc_import_free(imp);
  CHECK(there(&f, "stray.txt"));
  fx_close(&f);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  snprintf(g_root, sizeof g_root, "%s/test_orphans.d", argc > 1 ? argv[1] : ".");
  mc_remove_tree(g_root);
  snprintf(g_bin, sizeof g_bin, "%s/bin", g_root);
  CHECK(mc_mkdirs(g_bin) == MC_OK);
  mc_transfer_trash = fake_trash;
  mc_orphans_test_mtime_only = 1;

  test_keys();
  test_categories();
  test_snapshot();
  test_paths();
  test_refusals();
  test_bounds();
  test_timing();
  test_import();
  mc_remove_tree(g_root);
  return TEST_RESULT();
}
