/* Exporting the library as a zip and restoring it into an empty one (library_archive.c). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/demo.h"
#include "kithcore/kithcore.h"
#include "kithcore/library.h"
#include "kithcore/library_archive.h"
#include "miniz.h"
#include "test.h"
#include "util/platform.h"

#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

static char root[1024];

static void path_in(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s%c%s", root, MC_PATH_SEP, name);
}

static int to_file(void *user, const void *buf, size_t len) {
  return fwrite(buf, 1, len, user) == len ? 0 : EIO;
}

/* A destination that fills up after limit bytes. */
typedef struct small_disk {
  size_t used, limit;
} small_disk;

static int to_small_disk(void *user, const void *buf, size_t len) {
  (void)buf;
  small_disk *d = user;
  if (d->used + len > d->limit) return ENOSPC;
  d->used += len;
  return 0;
}

static int64_t last_done, last_total;
static int stop_at_half;

static int on_progress(void *user, int64_t done, int64_t total) {
  (void)user;
  CHECK(done >= last_done && done <= total);
  last_done = done;
  last_total = total;
  return stop_at_half && done * 2 > total;
}

static int64_t tracks(mc_library *lib) {
  int64_t n = -1;
  CHECK(mc_library_count(lib, MC_ENTITY_TRACKS, &n) == MC_OK);
  return n;
}

static mc_status export_to(mc_library *lib, const char *zip, const char *temp, mc_archive_problem *problem) {
  FILE *f = mc_fopen(zip, "wb");
  CHECK(f != NULL);
  last_done = 0;
  mc_status st = mc_library_export(lib, temp, to_file, on_progress, f, problem);
  CHECK(fclose(f) == 0);
  return st;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  snprintf(root, sizeof root, "%s%ctest_library_archive.d", scratch, MC_PATH_SEP);
  mc_remove_tree(root);
  char a_dir[1100], staging[1100], temp[1100], zip[1100], b_dir[1100];
  path_in(a_dir, sizeof a_dir, "a");
  path_in(staging, sizeof staging, "staging");
  path_in(temp, sizeof temp, "temp");
  path_in(zip, sizeof zip, "library.zip");
  path_in(b_dir, sizeof b_dir, "b");
  CHECK(mc_demo_create(a_dir, staging) == MC_OK);

  mc_library *a = NULL;
  CHECK(mc_library_open(a_dir, &a) == MC_OK);
  CHECK(tracks(a) == 34);

  /* The export: a valid zip, the manifest first, then library.db, then the files by name. */
  mc_archive_problem problem = MC_ARCHIVE_BAD;
  CHECK(export_to(a, zip, temp, &problem) == MC_OK && problem == MC_ARCHIVE_OK);
  CHECK(last_done == last_total && last_total > 0);
  {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    CHECK(mz_zip_reader_init_file(&z, zip, 0));
    CHECK(mz_zip_validate_archive(&z, 0));
    mz_uint count = mz_zip_reader_get_num_files(&z);
    CHECK(count > 34 + 2);
    char name[512], prev[512] = "";
    mz_zip_reader_get_filename(&z, 0, name, sizeof name);
    CHECK_TEXT(name, MC_LIBRARY_ARCHIVE_MANIFEST);
    mz_zip_reader_get_filename(&z, 1, name, sizeof name);
    CHECK_TEXT(name, MC_LIBRARY_DB_NAME);
    for (mz_uint i = 2; i < count; i++) {
      mz_zip_archive_file_stat st;
      CHECK(mz_zip_reader_file_stat(&z, i, &st));
      CHECK(st.m_method == 0 && st.m_comp_size == st.m_uncomp_size);
      CHECK(strcmp(prev, st.m_filename) < 0);
      CHECK(!strchr(st.m_filename, '\\') && strncmp(st.m_filename, "library.db", 10) != 0);
      snprintf(prev, sizeof prev, "%s", st.m_filename);
      /* Each entry's local header has its size and checksum, with no descriptor after. */
      CHECK((st.m_bit_flag & 8) == 0 && (st.m_bit_flag & (1 << 11)) != 0);
    }
    mz_zip_reader_end(&z);
  }
  mc_archive_info info;
  CHECK(mc_library_archive_info(zip, &info) == MC_OK);
  CHECK_TEXT(info.library_uid, mc_library_uid(a));
  CHECK(info.schema == mc_library_schema_version(a) && info.files > 34 && info.bytes > 0);
  CHECK(!mc_file_exists(temp) || mc_folder_is_empty(temp));

  /* Stopped half way, or a full destination: says so. */
  stop_at_half = 1;
  char cut[1100];
  path_in(cut, sizeof cut, "cut.zip");
  CHECK(export_to(a, cut, temp, &problem) == MC_ERR_CANCELLED);
  stop_at_half = 0;
  small_disk disk = {0, 100000};
  CHECK(mc_library_export(a, temp, to_small_disk, NULL, &disk, &problem) == MC_ERR_IO);
  CHECK(problem == MC_ARCHIVE_NO_SPACE);

  /* Other zips aren't exports. */
  CHECK(mc_library_archive_info(cut, &info) == MC_ERR_FORMAT);

  /* Restored into an empty library: the same library, with a node id of its own. */
  mc_library *b = NULL;
  CHECK(mc_library_open(b_dir, &b) == MC_OK);
  char b_node_before[MC_UID_STRLEN];
  snprintf(b_node_before, sizeof b_node_before, "%s", mc_library_node_uid(b));
  last_done = 0;
  CHECK(mc_library_restore(b, zip, temp, on_progress, NULL, &problem) == MC_OK && problem == MC_ARCHIVE_OK);
  CHECK(tracks(b) == 34);
  CHECK_TEXT(mc_library_uid(b), mc_library_uid(a));
  CHECK(strcmp(mc_library_node_uid(b), mc_library_node_uid(a)) != 0);
  CHECK(strcmp(mc_library_node_uid(b), b_node_before) != 0);
  char manifest[1200];
  snprintf(manifest, sizeof manifest, "%s%c%s", b_dir, MC_PATH_SEP, MC_LIBRARY_ARCHIVE_MANIFEST);
  CHECK(!mc_file_exists(manifest));
  {
    /* Every file the library names is there. */
    mc_track_list *list = NULL;
    mc_album_list *albums = NULL;
    CHECK(mc_library_list_albums(b, &albums) == MC_OK && albums->count == 6);
    CHECK(mc_library_list_album_tracks(b, albums->items[0].uid, &list) == MC_OK && list->count > 0);
    for (int i = 0; i < list->count; i++) CHECK(list->items[i].path && mc_file_exists(list->items[i].path));
    mc_track_list_free(list);
    mc_album_list_free(albums);
  }
  /* A new connection sees it too, and the library can be edited. */
  mc_library_close(b);
  CHECK(mc_library_open(b_dir, &b) == MC_OK);
  CHECK(tracks(b) == 34);

  /* Not into a library that has music already. */
  CHECK(mc_library_restore(b, zip, temp, NULL, NULL, &problem) == MC_ERR_LIBRARY_MISMATCH);
  CHECK(problem == MC_ARCHIVE_NOT_EMPTY);
  mc_library_close(b);

  /* Cancelled part way: the library stays empty and the files that came out go again. */
  char c_dir[1100];
  path_in(c_dir, sizeof c_dir, "c");
  mc_library *c = NULL;
  CHECK(mc_library_open(c_dir, &c) == MC_OK);
  stop_at_half = 1;
  last_done = 0;
  CHECK(mc_library_restore(c, zip, temp, on_progress, NULL, &problem) == MC_ERR_CANCELLED);
  stop_at_half = 0;
  CHECK(tracks(c) == 0);
  mc_library_close(c);
  CHECK(mc_folder_takes_new_library(c_dir));

  mc_library_close(a);
  mc_remove_tree(root);
  return TEST_RESULT();
}
