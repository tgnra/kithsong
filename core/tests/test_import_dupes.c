/* Duplicates through the import thread, without the network (skip_tagging, so as-is): a
 * better copy of an album swaps in tagged as the library's tracks, not with its own tags, and
 * importing the same source files again is recognised by their source hash although the
 * library's copies were tagged since. dupes.c's rules themselves are in test_dupes.c. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "musiccore/tagger.h"
#include "test.h"
#include "util/platform.h"

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

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A tenth of a second of silent 8 kHz mono WAV at bits per sample, tagged. */
static int write_track(const char *path, int bits, const char *album, const char *title, int track) {
  uint32_t data_len = 800 * (uint32_t)bits / 8;
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + data_len);
  put_u32(hdr + 16, 16);
  hdr[20] = 1; /* PCM */
  hdr[22] = 1; /* mono */
  put_u32(hdr + 24, 8000);
  put_u32(hdr + 28, 8000 * (uint32_t)bits / 8);
  hdr[32] = (unsigned char)(bits / 8);
  hdr[34] = (unsigned char)bits;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, data_len);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(hdr, sizeof hdr, 1, f) == 1;
  for (uint32_t i = 0; ok && i < data_len; i++) ok = fputc(0, f) != EOF;
  if (fclose(f) != 0 || !ok) return 0;

  mc_tags tags = {0};
  tags.artist = "Nobody";
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* An album of two tracks in dir. */
static int write_album(const char *dir, int bits, const char *album, const char *first,
                       const char *second) {
  char path[1400];
  if (mc_mkdirs(dir) != MC_OK) return 0;
  snprintf(path, sizeof path, "%s/01.wav", dir);
  if (!write_track(path, bits, album, first, 1)) return 0;
  snprintf(path, sizeof path, "%s/02.wav", dir);
  return write_track(path, bits, album, second, 2);
}

/* Imports dir as-is and waits for its one item to settle; returns it (free it). */
static mc_import_item *import(mc_library *lib, const char *dir) {
  const char *paths[] = {dir};
  mc_import_options opts = {.skip_tagging = 1};
  mc_import *imp = NULL;
  if (mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) != MC_OK) return NULL;
  mc_import_item *it = NULL;
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    if (mc_import_item_count(imp) == 1 && mc_import_item_get(imp, 0, &it) == MC_OK) {
      if (it->state == MC_IMPORT_DONE || it->state == MC_IMPORT_SKIPPED ||
          it->state == MC_IMPORT_FAILED)
        break;
      mc_import_item_free(it);
      it = NULL;
    }
    mc_sleep_ms(20);
  }
  mc_import_free(imp);
  return it;
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  char root[1024], src1[1100], src2[1100], libdir[1100], path[1400];
  snprintf(root, sizeof root, "%s/test_import_dupes.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src1, sizeof src1, "%s/src1/Album", root);
  snprintf(src2, sizeof src2, "%s/src2/Album", root);
  snprintf(libdir, sizeof libdir, "%s/lib", root);
  CHECK(mc_mkdirs(libdir) == MC_OK);
  CHECK(write_album(src1, 16, "Dupe Album", "First", "Second"));
  /* The same album in 24 bits, tagged differently. */
  CHECK(write_album(src2, 24, "DUPE ALBUM", "FIRST", "SECOND"));

  mc_library *lib = NULL;
  CHECK(mc_library_open(libdir, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  mc_import_item *it = import(lib, src1);
  CHECK(it && it->state == MC_IMPORT_DONE && it->outcome == MC_IMPORT_OUTCOME_NONE);
  mc_import_item_free(it);
  /* Imported from outside: each file knows the hash of what it was imported from. */
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE length(source_sha256) = 64") == 2);

  /* The better copy replaces it, tagged as the library's tracks, under their names. */
  it = import(lib, src2);
  CHECK(it && it->state == MC_IMPORT_DONE && it->outcome == MC_IMPORT_OUTCOME_REPLACED &&
        it->replaced_files == 2);
  mc_import_item_free(it);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE title = 'Dupe Album'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE title IN ('First', 'Second')") == 2);
  snprintf(path, sizeof path, "%s/Nobody/Dupe Album/01 First.wav", libdir);
  mc_tags tags;
  CHECK(mc_tags_read(path, &tags) == MC_OK);
  CHECK_STR(tags.title, "First");
  CHECK_STR(tags.album, "Dupe Album");
  CHECK(tags.track == 1 && tags.track_total == 2);
  CHECK(tags.bits_per_sample == 24); /* the new file */
  mc_tags_clear(&tags);
  snprintf(path, sizeof path, "%s/Nobody/Dupe Album/02 Second.wav", libdir);
  CHECK(mc_tags_read(path, &tags) == MC_OK);
  CHECK_STR(tags.title, "Second");
  mc_tags_clear(&tags);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE length(source_sha256) = 64") == 2);

  /* Renamed in the library, the album isn't found by name any more, and the copies' bytes
   * differ from src2's since they were tagged: only the source hashes know them. */
  CHECK(sqlite3_exec(mc_library_db(lib), "UPDATE albums SET title = 'Renamed'", NULL, NULL,
                     NULL) == SQLITE_OK);
  it = import(lib, src2);
  CHECK(it && it->state == MC_IMPORT_SKIPPED && it->outcome == MC_IMPORT_OUTCOME_DUPLICATE &&
        it->duplicate_files == 2);
  mc_import_item_free(it);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 2);

  mc_library_close(lib);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
