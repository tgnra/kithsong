/* Moving a library to another folder: the checks on the destination, a copy that moves
 * everything (and fixes up an absolute path in the database), a rename on the same volume,
 * and failures that must leave the old library as it was. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "library/move_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

static char base[1024];

static void path(char *out, size_t cap, const char *rel) {
  snprintf(out, cap, "%s/%s", base, rel);
}

static void write_file(const char *rel, const char *text) {
  char p[1200];
  path(p, sizeof p, rel);
  char *slash = strrchr(p, '/');
  *slash = '\0';
  mc_mkdirs(p);
  *slash = '/';
  FILE *f = mc_fopen(p, "wb");
  if (f) {
    fputs(text, f);
    fclose(f);
  }
}

/* Whether rel holds exactly text. */
static int has_file(const char *rel, const char *text) {
  char p[1200], buf[256];
  path(p, sizeof p, rel);
  FILE *f = mc_fopen(p, "rb");
  if (!f) return 0;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';
  return !strcmp(buf, text);
}

static int exists(const char *rel) {
  char p[1200];
  path(p, sizeof p, rel);
  return mc_file_exists(p);
}

typedef struct rm_ctx {
  const char *dir;
} rm_ctx;

static void remove_tree(const char *dir);

static void rm_entry(void *user, const char *name, int is_dir) {
  rm_ctx *c = user;
  char p[1200];
  snprintf(p, sizeof p, "%s/%s", c->dir, name);
  if (is_dir) remove_tree(p);
  else mc_remove(p);
}

static void remove_tree(const char *dir) {
  rm_ctx c = {dir};
  mc_list_dir(dir, rm_entry, &c);
  mc_rmdir(dir);
}

static const char *const k_files[][2] = {
  {"src/Radiohead/OK Computer (1997)/01 Airbag.flac", "airbag"},
  {"src/Radiohead/OK Computer (1997)/02 Paranoid Android.flac", "paranoid android"},
  {"src/Radiohead/OK Computer (1997)/cover.jpg", "cover"},
  {"src/Low/Secret Name/01 Starfire.mp3", "starfire"},
  {"src/.hidden", "hidden"},
};
enum { NFILES = sizeof k_files / sizeof k_files[0] };

/* A library in base/src: a database with one track, whose file row holds an absolute path
 * (as a database written by something else might), and some files. */
static void make_library(void) {
  char src[1200], abs[1300];
  path(src, sizeof src, "src");
  remove_tree(src);
  for (int i = 0; i < NFILES; i++) write_file(k_files[i][0], k_files[i][1]);
  mc_library *lib = NULL;
  CHECK(mc_library_open(src, &lib) == MC_OK);
  if (!lib) return;
  snprintf(abs, sizeof abs, "%s%cLow%cSecret Name%c01 Starfire.mp3", src, MC_PATH_SEP, MC_PATH_SEP, MC_PATH_SEP);
  sqlite3 *db = mc_library_db(lib);
  CHECK(sqlite3_exec(db,
                     "INSERT INTO tracks (uid, title) VALUES ('t1', 'Starfire');"
                     "INSERT INTO tracks (uid, title) VALUES ('t2', 'Airbag');"
                     "INSERT INTO files (uid, track_uid, path, format) VALUES"
                     " ('f2', 't2', 'Radiohead/OK Computer (1997)/01 Airbag.flac', 'flac');",
                     NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(db, "INSERT INTO files (uid, track_uid, path, format) VALUES ('f1', 't1', ?, 'mp3')", -1,
                     &stmt, NULL);
  sqlite3_bind_text(stmt, 1, abs, -1, SQLITE_TRANSIENT);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  mc_library_close(lib);
}

/* Whether base/src still holds the whole library. */
static int library_intact(const char *folder) {
  char rel[1200];
  int ok = 1;
  for (int i = 0; i < NFILES; i++) {
    snprintf(rel, sizeof rel, "%s%s", folder, k_files[i][0] + 3); /* past "src" */
    ok &= has_file(rel, k_files[i][1]);
  }
  snprintf(rel, sizeof rel, "%s/library.db", folder);
  return ok && exists(rel);
}

static void reset_hooks(void) {
  mc_move_test_free_space = -1;
  mc_move_test_fail_at = 0;
  mc_move_test_no_rename = 0;
}

static void test_checks(void) {
  char src[1200], dst[1200];
  mc_library_move_info info;
  make_library();
  path(src, sizeof src, "src");

  /* The same folder, one inside the library and one holding it. */
  CHECK(mc_library_move_check(src, src, &info) == MC_OK && info.problem == MC_MOVE_SAME_FOLDER);
  path(dst, sizeof dst, "src/Radiohead/new");
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_INSIDE_LIBRARY);
  CHECK(mc_library_move_check(src, base, &info) == MC_OK && info.problem == MC_MOVE_CONTAINS_LIBRARY);

  /* A folder with something in it, and a file. */
  write_file("full/keep.txt", "keep");
  path(dst, sizeof dst, "full");
  CHECK(!mc_folder_is_empty(dst));
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_NOT_EMPTY);
  mc_library_move_result result;
  CHECK(mc_library_move(src, dst, NULL, NULL, NULL, &result) == MC_ERR_INVALID_ARG);
  CHECK(result.problem == MC_MOVE_NOT_EMPTY);
  mc_library_move_result_free(&result);
  CHECK(library_intact("src") && has_file("full/keep.txt", "keep"));
  path(dst, sizeof dst, "full/keep.txt");
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_NOT_EMPTY);

  /* An empty folder, and one that doesn't exist yet (nor does its parent), are fine. */
  path(dst, sizeof dst, "empty");
  remove_tree(dst);
  mc_mkdirs(dst);
  CHECK(mc_folder_is_empty(dst));
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_OK);
  CHECK(info.file_count == NFILES + 1); /* and library.db */
  CHECK(info.library_bytes > 0);
  path(dst, sizeof dst, "missing/too");
  CHECK(mc_folder_is_empty(dst));
  int64_t free_bytes = 0;
  CHECK(mc_folder_free_space(dst, &free_bytes) == MC_OK && free_bytes > 0);
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_OK);
  CHECK(!exists("missing")); /* checking makes nothing */

  int64_t bytes = 0;
  int files = 0;
  CHECK(mc_folder_size(src, &bytes, &files) == MC_OK && files == NFILES + 1 && bytes == info.library_bytes);

  /* Not enough room on another volume: needs the library's size and a margin. */
  mc_move_test_no_rename = 1;
  mc_move_test_free_space = 1000;
  path(dst, sizeof dst, "empty");
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_NO_SPACE);
  CHECK(info.needed_bytes > info.library_bytes && info.free_bytes == 1000);
  CHECK(mc_library_move(src, dst, NULL, NULL, NULL, &result) == MC_ERR_INVALID_ARG);
  CHECK(result.problem == MC_MOVE_NO_SPACE);
  mc_library_move_result_free(&result);
  CHECK(library_intact("src"));
  CHECK(mc_folder_is_empty(dst));
  reset_hooks();
}

typedef struct progress_log {
  int calls, last_done, total, went_back, undid;
  int64_t bytes_total;
} progress_log;

static void on_progress(void *user, const mc_library_move_progress *p) {
  progress_log *log = user;
  log->calls++;
  if (p->files_done < log->last_done) log->went_back = 1;
  log->last_done = p->files_done;
  log->total = p->files_total;
  log->bytes_total = p->bytes_total;
  if (p->phase == MC_MOVE_PHASE_UNDOING) log->undid = 1;
}

/* Copying fails at the third file: the old library stays, and the copy goes. */
static void test_failure(void) {
  char src[1200], dst[1200], cfg[1200];
  make_library();
  path(src, sizeof src, "src");
  path(dst, sizeof dst, "dest/library");
  path(cfg, sizeof cfg, "config");
  remove_tree(dst);
  mc_rmdir(dst);
  path(dst, sizeof dst, "dest");
  remove_tree(dst);
  path(dst, sizeof dst, "dest/library");

  mc_settings *settings = NULL;
  CHECK(mc_settings_load(cfg, &settings) == MC_OK);
  mc_settings_set_library_dir(settings, src);
  mc_settings_save(settings);

  mc_move_test_no_rename = 1;
  mc_move_test_fail_at = 3;
  progress_log log = {0};
  mc_library_move_result result;
  CHECK(mc_library_move(src, dst, settings, on_progress, &log, &result) == MC_ERR_IO);
  CHECK(result.failed_path != NULL);
  CHECK(!result.renamed);
  CHECK(log.undid);
  mc_library_move_result_free(&result);
  CHECK(library_intact("src"));
  CHECK(!exists("dest")); /* made for the move, so removed again */
  CHECK(!strcmp(mc_settings_library_dir(settings), src));
  mc_settings_free(settings);
  settings = NULL;
  CHECK(mc_settings_load(cfg, &settings) == MC_OK);
  CHECK(!strcmp(mc_settings_library_dir(settings), src));
  mc_settings_free(settings);

  /* The library still opens where it was. */
  mc_library *lib = NULL;
  CHECK(mc_library_open(src, &lib) == MC_OK);
  mc_library_close(lib);
  reset_hooks();
}

static void check_moved(const char *dst_rel, const char *dst) {
  CHECK(library_intact(dst_rel));
  char rel[1200];
  snprintf(rel, sizeof rel, "%s/.hidden", dst_rel);
  CHECK(has_file(rel, "hidden"));

  /* The old folder is kept, empty. */
  char src[1200];
  path(src, sizeof src, "src");
  CHECK(mc_file_exists(src) && mc_folder_is_empty(src));

  /* The database opens in its new place, with every path relative. */
  mc_library *lib = NULL;
  CHECK(mc_library_open(dst, &lib) == MC_OK);
  if (!lib) return;
  sqlite3_stmt *stmt = NULL;
  sqlite3_prepare_v2(mc_library_db(lib), "SELECT path FROM files WHERE uid = 'f1'", -1, &stmt, NULL);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  CHECK_STR((const char *)sqlite3_column_text(stmt, 0), "Low/Secret Name/01 Starfire.mp3");
  sqlite3_finalize(stmt);
  mc_library_close(lib);
}

/* A copy to another (pretend) volume: everything arrives, settings follow, the old goes. */
static void test_copy(void) {
  char src[1200], dst[1200], cfg[1200];
  make_library();
  path(src, sizeof src, "src");
  path(dst, sizeof dst, "copied");
  path(cfg, sizeof cfg, "config");
  remove_tree(dst);
  mc_mkdirs(dst); /* an empty folder that's there already */

  mc_settings *settings = NULL;
  CHECK(mc_settings_load(cfg, &settings) == MC_OK);
  mc_settings_set_library_dir(settings, src);

  mc_move_test_no_rename = 1;
  progress_log log = {0};
  mc_library_move_result result;
  CHECK(mc_library_move(src, dst, settings, on_progress, &log, &result) == MC_OK);
  CHECK(!result.renamed && result.left_behind == 0 && result.failed_path == NULL);
  mc_library_move_result_free(&result);
  CHECK(log.calls >= NFILES + 1 && !log.went_back && !log.undid);
  CHECK(log.last_done == log.total && log.total == NFILES + 1 && log.bytes_total > 0);
  mc_settings_free(settings);
  settings = NULL;
  CHECK(mc_settings_load(cfg, &settings) == MC_OK);
  CHECK(!strcmp(mc_settings_library_dir(settings), dst));
  mc_settings_free(settings);
  check_moved("copied", dst);
  reset_hooks();
}

/* On the same volume the entries are renamed. */
static void test_rename(void) {
  char src[1200], dst[1200];
  make_library();
  path(src, sizeof src, "src");
  path(dst, sizeof dst, "renamed");
  remove_tree(dst);
  path(dst, sizeof dst, "renamed/lib");

  mc_library_move_info info;
  CHECK(mc_library_move_check(src, dst, &info) == MC_OK && info.problem == MC_MOVE_OK);
  CHECK(info.same_volume && info.needed_bytes == 0);
  mc_move_test_free_space = 1000; /* a rename needs no room */
  mc_library_move_result result;
  CHECK(mc_library_move(src, dst, NULL, NULL, NULL, &result) == MC_OK);
  CHECK(result.renamed);
  mc_library_move_result_free(&result);
  check_moved("renamed/lib", dst);
  reset_hooks();
}

int main(int argc, char **argv) {
  /* Full, as a library folder is; the database's absolute path has to look like one. */
  char *dir = mc_full_path(argc > 1 ? argv[1] : ".");
  snprintf(base, sizeof base, "%s/test_library_move", dir ? dir : ".");
  free(dir);
  remove_tree(base);
  mc_mkdirs(base);
  test_checks();
  test_failure();
  test_copy();
  test_rename();
  return TEST_RESULT();
}
