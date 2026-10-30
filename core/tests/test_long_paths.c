/* Paths of MAX_PATH (260) characters or more. On Windows the test binary has no manifest, so
 * it isn't long-path aware whatever LongPathsEnabled says: everything here has to go through
 * platform.c's \\?\ paths. Checks the file calls one by one, TagLib and the decoder, and an
 * import from a deep folder, where an unreadable file with a long path becomes a failed item
 * instead of being passed over. Elsewhere the same calls just have to work. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/i18n.h"
#include "musiccore/library.h"
#include "musiccore/musiccore.h"
#include "musiccore/tagger.h"
#include "playback/decoder.h"
#include "util/platform.h"
#include "test.h"

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A tenth of a second of silent 8 kHz mono WAV, untagged. */
static int write_wav(const char *path) {
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
  return fclose(f) == 0 && ok;
}

typedef struct found {
  const char *want;
  int files, matched;
} found;

static int on_file(void *user, const char *path) {
  found *f = user;
  f->files++;
  f->matched += strcmp(path, f->want) == 0;
  return 0;
}

static void count_entry(void *user, const char *name, int is_dir) {
  (void)name;
  (void)is_dir;
  (*(int *)user)++;
}

/* Waits until the session has items and none is still being worked on. */
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

/* The file calls, on a file and folders past MAX_PATH. */
static void test_files(const char *deep) {
  char wav[1200], copy[1200], moved[1200];
  snprintf(wav, sizeof wav, "%s%c01 Track.wav", deep, MC_PATH_SEP);
  snprintf(copy, sizeof copy, "%s%ccopy.wav", deep, MC_PATH_SEP);
  snprintf(moved, sizeof moved, "%s%cmoved.wav", deep, MC_PATH_SEP);
  CHECK(strlen(wav) > 300);

  CHECK(write_wav(wav));
  CHECK(mc_file_exists(wav));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(wav, &size, &mtime) == MC_OK && size == 44 + 1600);
  CHECK(mc_touch(wav) == MC_OK);
  CHECK(!mc_is_link(wav));
  CHECK(mc_clear_read_only(wav) == MC_OK);
#ifdef _WIN32
  CHECK(mc_long_path(wav));
#endif
  CHECK(!mc_long_path("short.wav"));

  /* TagLib, both ways. */
  mc_tags tags = {0};
  tags.title = (char *)"Deep";
  CHECK(mc_tags_write(wav, &tags) == MC_OK);
  mc_tags got;
  CHECK(mc_tags_read(wav, &got) == MC_OK);
  CHECK_STR(got.title, "Deep");
  mc_tags_clear(&got);

  mc_decoder *dec = NULL;
  CHECK(mc_decoder_open(wav, &dec) == MC_OK && dec && mc_decoder_rate(dec) == 8000);
  if (dec) mc_decoder_close(dec);

  CHECK(mc_copy_file(wav, copy, 0) == MC_OK);
  CHECK(mc_copy_file(wav, copy, 0) != MC_OK); /* exists */
  CHECK(mc_rename(copy, moved, 0) == MC_OK && !mc_file_exists(copy) && mc_file_exists(moved));
  CHECK(mc_rename_in_place(moved, copy) == MC_OK && mc_file_exists(copy));
  CHECK(mc_same_volume(copy, wav));
  CHECK(mc_remove(copy) == MC_OK && !mc_file_exists(copy));

  /* The walk hands back paths as they were given, without a \\?\ prefix. */
  found f = {wav, 0, 0};
  CHECK(mc_walk(deep, on_file, &f) == MC_OK);
  CHECK(f.files == 1 && f.matched == 1);
  int entries = 0;
  CHECK(mc_list_dir(deep, count_entry, &entries) == MC_OK && entries == 1);

  char *full = mc_full_path(wav);
  CHECK(full && strncmp(full, "\\\\?\\", 4) != 0 && strlen(full) > 300);
  free(full);
}

/* An import from the deep folder: the WAV is imported, the file that isn't audio becomes a
 * failed item on Windows, where its long path may be why it couldn't be read. */
static void test_import(const char *root, const char *deep) {
  char bad[1200], lib_dir[1200];
  snprintf(bad, sizeof bad, "%s%cbroken.flac", deep, MC_PATH_SEP);
  snprintf(lib_dir, sizeof lib_dir, "%s%clib", root, MC_PATH_SEP);
  FILE *f = mc_fopen(bad, "wb");
  CHECK(f != NULL);
  if (f) {
    fputs("not a flac", f);
    fclose(f);
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  const char *paths[] = {root};
  mc_import_options opts = {.fetch_art = 0, .skip_tagging = 1};
  mc_import *imp = NULL;
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
#ifdef _WIN32
    int want = 2;
#else
    int want = 1;
#endif
    CHECK(wait_settled(imp, 20000) == want);
    int done = 0, failed = 0;
    for (int i = 0; i < mc_import_item_count(imp); i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      if (it->state == MC_IMPORT_DONE) {
        done++;
        CHECK(it->file_count == 1);
      } else if (it->state == MC_IMPORT_FAILED) {
        failed++;
        CHECK(it->file_count == 0);
        CHECK_STR(it->album, "broken.flac");
        CHECK(it->folder && strcmp(it->folder, bad) == 0);
        CHECK_STR(it->error, mc_str(MC_STR_IMPORT_ERROR_LONG_PATH));
      }
      mc_import_item_free(it);
    }
    CHECK(done == 1 && failed == want - 1);
    mc_import_free(imp);
  }
  int64_t tracks = -1;
  if (lib) mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  CHECK(tracks == 1);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  char root[1024], deep[1200];
  snprintf(root, sizeof root, "%s/test_long_paths.d", argc > 1 ? argv[1] : ".");
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  /* Six 50-character folders: well past 260 wherever the build directory is. */
  int n = snprintf(deep, sizeof deep, "%s", root);
  for (int i = 0; i < 6; i++)
    n += snprintf(deep + n, sizeof deep - (size_t)n, "%cfolder %d with a name long enough to add up quickly", MC_PATH_SEP, i);
  CHECK(mc_mkdirs(deep) == MC_OK);
  CHECK(mc_file_exists(deep));

  test_files(deep);
  test_import(root, deep);

  CHECK(mc_remove_tree(root) == MC_OK);
  CHECK(!mc_file_exists(root));
  mc_shutdown();
  return TEST_RESULT();
}
