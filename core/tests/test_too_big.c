/* Track files too big for the build (mc_file_too_big(), util/platform.h): on 32-bit Android a
 * file of 2^31 bytes or more is never imported or restored. The limit is a variable, so the
 * test sets it as a 32-bit build has it and checks the boundary: 2^31 - 1 bytes is taken,
 * 2^31 refused, and an import of a 2^31-byte file (sparse, so nothing is written) fails that
 * item without reading it. Then, with a small limit so the files that are taken stay small,
 * an import of a folder and a zip (the file one byte under the limit is imported, the one at
 * it is a failed item, and never extracted from the zip), and a restore of a library export
 * holding one of each (the restore goes ahead, the big file's track stays without a file). */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* truncate */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "kithcore/i18n.h"
#include "kithcore/kithcore.h"
#include "kithcore/library.h"
#include "kithcore/library_archive.h"
#include "kithcore/tagger.h"
#include "library/library_internal.h"
#include "miniz.h"
#include "test.h"
#include "util/platform.h"

/* The small limit, and the file sizes either side of it. */
#define LIMIT 20000
#define UNDER (LIMIT - 1)

static char root[1024];

static void path_in(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s%c%s", root, MC_PATH_SEP, name);
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* An untagged 8 kHz 8-bit mono WAV of exactly size bytes, in memory (malloc'd). */
static unsigned char *wav_bytes(size_t size) {
  unsigned char *b = calloc(1, size);
  if (!b) return NULL;
  uint32_t data = (uint32_t)(size - 44);
  memcpy(b, "RIFF....WAVEfmt ", 16);
  put_u32(b + 4, 36 + data);
  put_u32(b + 16, 16);
  b[20] = 1;
  b[22] = 1;
  put_u32(b + 24, 8000);
  put_u32(b + 28, 8000);
  b[32] = 1;
  b[34] = 8;
  memcpy(b + 36, "data", 4);
  put_u32(b + 40, data);
  memset(b + 44, 0x80, data);
  return b;
}

static int write_wav(const char *path, size_t size) {
  unsigned char *b = wav_bytes(size);
  FILE *f = b ? mc_fopen(path, "wb") : NULL;
  int ok = f && fwrite(b, 1, size, f) == size;
  if (f && fclose(f) != 0) ok = 0;
  free(b);
  return ok;
}

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

typedef struct zip_entry {
  const char *name;
  const unsigned char *data;
  uint32_t len;
} zip_entry;

/* A zip of n entries, stored, at path (miniz is built here without its writer). */
static int write_zip(const char *path, const zip_entry *e, int n) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t offsets[8], crcs[8];
  for (int i = 0; i < n; i++) {
    offsets[i] = (uint32_t)ftell(f);
    crcs[i] = (uint32_t)mz_crc32(MZ_CRC32_INIT, e[i].data, e[i].len);
    put32(f, 0x04034b50);
    put16(f, 20);
    put16(f, 0);
    put16(f, 0);    /* stored */
    put16(f, 0);
    put16(f, 0x21); /* 1980-01-01 */
    put32(f, crcs[i]);
    put32(f, e[i].len);
    put32(f, e[i].len);
    put16(f, (unsigned)strlen(e[i].name));
    put16(f, 0);
    fwrite(e[i].name, 1, strlen(e[i].name), f);
    fwrite(e[i].data, 1, e[i].len, f);
  }
  uint32_t cd = (uint32_t)ftell(f);
  for (int i = 0; i < n; i++) {
    put32(f, 0x02014b50);
    put16(f, 20);
    put16(f, 20);
    put16(f, 0);
    put16(f, 0);
    put16(f, 0);
    put16(f, 0x21);
    put32(f, crcs[i]);
    put32(f, e[i].len);
    put32(f, e[i].len);
    put16(f, (unsigned)strlen(e[i].name));
    put16(f, 0);
    put16(f, 0);
    put16(f, 0);
    put16(f, 0);
    put32(f, 0);
    put32(f, offsets[i]);
    fwrite(e[i].name, 1, strlen(e[i].name), f);
  }
  uint32_t cd_size = (uint32_t)ftell(f) - cd;
  put32(f, 0x06054b50);
  put16(f, 0);
  put16(f, 0);
  put16(f, (unsigned)n);
  put16(f, (unsigned)n);
  put32(f, cd_size);
  put32(f, cd);
  put16(f, 0);
  return fclose(f) == 0;
}

static int wait_settled(mc_import *imp, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      settled += it->state == MC_IMPORT_DONE || it->state == MC_IMPORT_FAILED || it->state == MC_IMPORT_SKIPPED;
      mc_import_item_free(it);
    }
    if (n > 0 && settled == n) return n;
    mc_sleep_ms(20);
  }
  return 0;
}

typedef struct outcome {
  int done, too_big;
  char failed[4][1100]; /* the too-big items' folders */
} outcome;

/* Imports path into lib with the tags it has, and counts how it went. */
static void import(mc_library *lib, const char *path, outcome *o) {
  memset(o, 0, sizeof *o);
  char staging[1100];
  path_in(staging, sizeof staging, "staging");
  CHECK(mc_mkdirs(staging) == MC_OK);
  const char *paths[] = {path};
  mc_import_options opts = {.skip_tagging = 1, .staging_dir = staging};
  mc_import *imp = NULL;
  CHECK(mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (!imp) return;
  CHECK(wait_settled(imp, 20000) > 0);
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it;
    if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
    if (it->state == MC_IMPORT_DONE) o->done += it->file_count;
    if (it->state == MC_IMPORT_FAILED && it->error && !strcmp(it->error, mc_str(MC_STR_IMPORT_ERROR_TOO_BIG))) {
      CHECK(it->file_count == 0);
      if (o->too_big < 4) snprintf(o->failed[o->too_big], sizeof o->failed[0], "%s", it->folder);
      o->too_big++;
    }
    mc_import_item_free(it);
  }
  mc_import_free(imp);
}

static int64_t count(mc_library *lib, mc_entity e) {
  int64_t n = -1;
  CHECK(mc_library_count(lib, e, &n) == MC_OK);
  return n;
}

static int ends_with(const char *s, const char *end) {
  size_t a = strlen(s), b = strlen(end);
  return a >= b && !strcmp(s + a - b, end);
}

/* The boundary as a 32-bit build has it. */
static void test_boundary(void) {
  mc_max_file_bytes = 0;
  CHECK(!mc_file_too_big(INT64_MAX));
  CHECK(mc_file_size_limit() == 0);
  mc_max_file_bytes = MC_32BIT_MAX_FILE_BYTES;
  CHECK(MC_32BIT_MAX_FILE_BYTES == 2147483648LL);
  CHECK(mc_file_size_limit() == 2147483648LL);
  CHECK(!mc_file_too_big(0));
  CHECK(!mc_file_too_big(2147483647LL));
  CHECK(mc_file_too_big(2147483648LL));
  CHECK(mc_file_too_big(INT64_MAX));
}

#ifndef _WIN32
/* A sparse file of 2^31 bytes: refused before TagLib reads it. */
static void test_import_2gib(void) {
  char dir[1100], big[1200], lib_dir[1100];
  path_in(dir, sizeof dir, "sparse");
  path_in(lib_dir, sizeof lib_dir, "lib-sparse");
  snprintf(big, sizeof big, "%s/big.wav", dir);
  CHECK(mc_mkdirs(dir) == MC_OK);
  CHECK(write_wav(big, 44 + 100));
  CHECK(truncate(big, (off_t)MC_32BIT_MAX_FILE_BYTES) == 0);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(big, &size, &mtime) == MC_OK && size == MC_32BIT_MAX_FILE_BYTES);

  mc_max_file_bytes = MC_32BIT_MAX_FILE_BYTES;
  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  outcome o;
  uint64_t t0 = mc_mono_ms();
  if (lib) import(lib, dir, &o);
  CHECK(mc_mono_ms() - t0 < 10000); /* not read */
  CHECK(o.done == 0 && o.too_big == 1);
  CHECK(!strcmp(o.failed[0], big));
  CHECK(lib && count(lib, MC_ENTITY_TRACKS) == 0);
  mc_library_close(lib);
  mc_remove(big);
}
#endif

/* A folder and a zip, each with a file one byte under the limit and one at it. */
static void test_import_small_limit(void) {
  char dir[1100], p[1200], zip[1100], lib_dir[1100];
  path_in(dir, sizeof dir, "music");
  path_in(zip, sizeof zip, "zipped.zip");
  path_in(lib_dir, sizeof lib_dir, "lib-small");
  snprintf(p, sizeof p, "%s%cUnder", dir, MC_PATH_SEP);
  CHECK(mc_mkdirs(p) == MC_OK);
  snprintf(p, sizeof p, "%s%cUnder%cunder.wav", dir, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(write_wav(p, UNDER));
  snprintf(p, sizeof p, "%s%cAt", dir, MC_PATH_SEP);
  CHECK(mc_mkdirs(p) == MC_OK);
  snprintf(p, sizeof p, "%s%cAt%cat.wav", dir, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(write_wav(p, LIMIT));
  snprintf(p, sizeof p, "%s%cAt%cvideo.mkv", dir, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(write_wav(p, LIMIT)); /* not imported anyway, so not reported */

  {
    unsigned char *under = wav_bytes(UNDER), *at = wav_bytes(LIMIT);
    if (under) under[100] = 0x81; /* not the folder's file again, which is a duplicate */
    const zip_entry e[] = {{"Zipped/under.wav", under, UNDER}, {"Zipped/at.wav", at, LIMIT},
                           {"Zipped/video.mkv", at, LIMIT}};
    CHECK(under && at && write_zip(zip, e, 3));
    free(under);
    free(at);
  }

  mc_max_file_bytes = LIMIT;
  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  outcome o;
  if (lib) import(lib, dir, &o);
  CHECK(o.done == 1 && o.too_big == 1);
  CHECK(ends_with(o.failed[0], "at.wav"));
  if (lib) import(lib, zip, &o);
  CHECK(o.done == 1 && o.too_big == 1);
  snprintf(p, sizeof p, "%s/Zipped/at.wav", zip);
  CHECK(!strcmp(o.failed[0], p));
  CHECK(lib && count(lib, MC_ENTITY_TRACKS) == 2);
  mc_library_close(lib);
}

/* A file of a track, its row's size, and whether its file is in the folder. */
static int64_t file_size_of(mc_library *lib, const char *title, int *on_disk) {
  sqlite3_stmt *stmt;
  int64_t size = -1;
  *on_disk = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.size_bytes, f.path FROM files f JOIN tracks t ON t.uid = f.track_uid"
                         " WHERE t.title = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, title, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    size = sqlite3_column_int64(stmt, 0);
    char *abs = mc_library_abspath(lib, (const char *)sqlite3_column_text(stmt, 1));
    *on_disk = abs && mc_file_exists(abs);
    free(abs);
  }
  sqlite3_finalize(stmt);
  return size;
}

static int to_file(void *user, const void *buf, size_t len) {
  return fwrite(buf, 1, len, user) == len ? 0 : 1;
}

/* What the restore's swap hook saw in the staged database, and whether it fails the restore. */
static int64_t staged_files = -1;
static int fail_swap;

static int on_swap(const char *db_path) {
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  staged_files = -1;
  if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
      sqlite3_prepare_v2(db, "SELECT count(*) FROM files", -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW)
    staged_files = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return fail_swap;
}

static void count_entry(void *user, const char *name, int is_dir) {
  (void)name;
  (void)is_dir;
  (*(int *)user)++;
}

/* An export of two tracks and a big file that isn't the library's, restored where the larger
 * track's file is too big: first failing at the last step (the library stays empty), then for
 * real. The too-big row is gone from the staged database before it replaces the library's. */
static void test_restore(void) {
  char dir[1100], p[1200], a_dir[1100], b_dir[1100], zip[1100], temp[1100];
  path_in(dir, sizeof dir, "restore-music");
  path_in(a_dir, sizeof a_dir, "lib-a");
  path_in(b_dir, sizeof b_dir, "lib-b");
  path_in(zip, sizeof zip, "export.zip");
  path_in(temp, sizeof temp, "temp");
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s%csmall.wav", dir, MC_PATH_SEP);
  CHECK(write_wav(p, 4000));
  snprintf(p, sizeof p, "%s%clarge.wav", dir, MC_PATH_SEP);
  CHECK(write_wav(p, 9000));

  mc_max_file_bytes = 0;
  mc_library *a = NULL;
  CHECK(mc_library_open(a_dir, &a) == MC_OK);
  outcome o;
  if (a) import(a, dir, &o);
  CHECK(o.done == 2);
  int here = 0;
  /* As imported, with the tags it was given: one byte past the small one is the limit. */
  int64_t small = a ? file_size_of(a, "small", &here) : -1, large = a ? file_size_of(a, "large", &here) : -1;
  CHECK(small > 0 && large > small);
  snprintf(p, sizeof p, "%s%cextra.bin", a_dir, MC_PATH_SEP);
  CHECK(write_wav(p, 9000)); /* not in the database: left out unremarked */
  FILE *f = mc_fopen(zip, "wb");
  mc_archive_problem problem = MC_ARCHIVE_BAD;
  CHECK(f && a);
  if (f && a) CHECK(mc_library_export(a, temp, to_file, NULL, f, &problem) == MC_OK);
  if (f) CHECK(fclose(f) == 0);
  mc_library_close(a);

  mc_max_file_bytes = small + 1;
  mc_library *b = NULL;
  CHECK(mc_library_open(b_dir, &b) == MC_OK);
  int too_big = -1, entries = 0;
  mc_library_restore_test_swap = on_swap;
  fail_swap = 1;
  CHECK(b && mc_library_restore(b, zip, temp, NULL, NULL, &problem, &too_big) == MC_ERR_IO);
  CHECK(problem == MC_ARCHIVE_WRITE && too_big == 0 && staged_files == 1);
  if (b) {
    CHECK(count(b, MC_ENTITY_TRACKS) == 0);
    mc_list_dir(b_dir, count_entry, &entries);
    CHECK(entries == 1 || entries == 3); /* library.db, maybe with -wal and -shm */
  }
  fail_swap = 0;
  staged_files = -1;
  CHECK(b && mc_library_restore(b, zip, temp, NULL, NULL, &problem, &too_big) == MC_OK);
  CHECK(problem == MC_ARCHIVE_OK && too_big == 1 && staged_files == 1);
  mc_library_restore_test_swap = NULL;
  snprintf(p, sizeof p, "%s%cextra.bin", b_dir, MC_PATH_SEP);
  CHECK(!mc_file_exists(p));
  if (b) {
    CHECK(count(b, MC_ENTITY_TRACKS) == 2); /* both tracks, */
    CHECK(count(b, MC_ENTITY_FILES) == 1);  /* one file */
    CHECK(file_size_of(b, "small", &here) == small && here);
    CHECK(file_size_of(b, "large", &here) == -1 && !here);
  }
  mc_library_close(b);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  snprintf(root, sizeof root, "%s%ctest_too_big.d", argc > 1 ? argv[1] : ".", MC_PATH_SEP);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  test_boundary();
#ifndef _WIN32
  test_import_2gib();
#endif
  test_import_small_limit();
  test_restore();

  mc_max_file_bytes = 0;
  mc_remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
