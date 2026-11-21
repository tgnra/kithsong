/* Whether a folder can be a library folder: document portal paths, the reach of a sandbox's
 * folders, the write-and-rename probe, and the whole check. */
#define _POSIX_C_SOURCE 200809L /* symlink */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "kithcore/kithcore.h"
#include "test.h"
#include "util/platform.h"

static char base[1024];

static char *at(const char *rel) {
  static char p[4][1200];
  static int next;
  char *out = p[next++ % 4];
  snprintf(out, sizeof p[0], "%s/%s", base, rel);
  return out;
}

static void write_file(const char *rel) {
  FILE *f = mc_fopen(at(rel), "wb");
  if (f) {
    fputs("x", f);
    fclose(f);
  }
}

static void test_portal(void) {
#ifdef _WIN32
  CHECK(!mc_folder_is_portal("/run/user/1000/doc/abcd/Music"));
#else
  CHECK(mc_folder_is_portal("/run/user/1000/doc/abcd/Music"));
  CHECK(mc_folder_is_portal("/run/user/1000/doc"));
  CHECK(mc_folder_is_portal("/run/flatpak/doc/abcd/Music"));
  CHECK(!mc_folder_is_portal("/run/user/1000/docs/abcd"));
  CHECK(!mc_folder_is_portal("/run/user/x/doc/abcd"));
  CHECK(!mc_folder_is_portal("/run/user//doc/abcd"));
  CHECK(!mc_folder_is_portal("/home/someone/Music/Kithsong Library"));
  CHECK(!mc_folder_is_portal(base));
  CHECK(!mc_folder_is_portal(NULL));
#endif
}

static void test_within(void) {
  mc_mkdirs(at("music/Albums"));
  mc_mkdirs(at("data"));
  mc_mkdirs(at("music2"));
  char music[1200], data[1200], gone[1200];
  snprintf(music, sizeof music, "%s", at("music"));
  snprintf(data, sizeof data, "%s", at("data"));
  snprintf(gone, sizeof gone, "%s", at("gone"));
  const char *roots[] = {music, data, gone, NULL};
  CHECK(mc_folder_within(at("music"), roots));
  CHECK(mc_folder_within(at("music/Albums"), roots));
  CHECK(mc_folder_within(at("music/Kithsong Library"), roots)); /* not made yet */
  CHECK(mc_folder_within(at("data/Kithsong Library/deeper"), roots));
  CHECK(!mc_folder_within(at("music2/Kithsong Library"), roots));
  CHECK(!mc_folder_within(at("music2"), roots));
  CHECK(!mc_folder_within(base, roots));
  CHECK(!mc_folder_within(at("gone/Kithsong Library"), roots)); /* its root isn't there */
  CHECK(mc_folder_within(at("music/../data/x"), roots));
  CHECK(!mc_folder_within(at("music/../music2/x"), roots));
  CHECK(!mc_folder_within(at("music"), NULL));
#ifndef _WIN32
  /* A link inside a root to a folder outside it leads out of reach. */
  CHECK(symlink(at("music2"), at("music/out")) == 0);
  CHECK(!mc_folder_within(at("music/out/Kithsong Library"), roots));
#endif
}

static void count_one(void *user, const char *name, int is_dir) {
  (void)name, (void)is_dir;
  ++*(int *)user;
}

static int entries(const char *dir) {
  int n = 0;
  if (mc_list_dir(dir, count_one, &n) != MC_OK) return -1;
  return n;
}

static void test_probe(void) {
  mc_mkdirs(at("probe"));
  CHECK(mc_folder_probe(at("probe")) == MC_OK);
  CHECK(entries(at("probe")) == 0); /* nothing left behind */
  CHECK(mc_folder_probe(at("probe/not yet/made")) == MC_OK);
  CHECK(!mc_file_exists(at("probe/not yet")));
  CHECK(entries(at("probe")) == 0);
  CHECK(mc_folder_probe(NULL) == MC_ERR_INVALID_ARG);
#ifndef _WIN32
  if (geteuid() != 0) { /* root writes anywhere */
    mc_mkdirs(at("readonly"));
    chmod(at("readonly"), 0555);
    CHECK(mc_folder_probe(at("readonly")) == MC_ERR_IO);
    CHECK(mc_folder_probe(at("readonly/Kithsong Library")) == MC_ERR_IO);
    CHECK(entries(at("readonly")) == 0);
    chmod(at("readonly"), 0755);
  }
#endif
}

static void test_check(void) {
  mc_mkdirs(at("check/music/other"));
  mc_mkdirs(at("check/data"));
  mc_mkdirs(at("check/outside"));
  write_file("check/music/other/notes.txt");
  mc_mkdirs(at("check/music/lib"));
  write_file("check/music/lib/library.db");
  char music[1200], data[1200];
  snprintf(music, sizeof music, "%s", at("check/music"));
  snprintf(data, sizeof data, "%s", at("check/data"));
  const char *roots[] = {music, data, NULL};

  /* No sandbox: only what's in it, the probe and the free space. */
  CHECK(mc_library_folder_check(at("check/outside/Kithsong Library"), MC_FOLDER_USE_LIBRARY, NULL) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(at("check/music/other"), MC_FOLDER_USE_LIBRARY, NULL) == MC_FOLDER_NOT_EMPTY);
  CHECK(mc_library_folder_check(at("check/music/other"), MC_FOLDER_USE_ANY, NULL) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(at("check/music/lib"), MC_FOLDER_USE_LIBRARY, NULL) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(at("check/music/lib"), MC_FOLDER_USE_EMPTY, NULL) == MC_FOLDER_NOT_EMPTY);
  CHECK(mc_library_folder_check(at("check/data"), MC_FOLDER_USE_EMPTY, NULL) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(NULL, MC_FOLDER_USE_ANY, NULL) == MC_FOLDER_NOT_WRITABLE);
#ifndef _WIN32
  CHECK(mc_library_folder_check("/run/user/1000/doc/abcd/Music", MC_FOLDER_USE_ANY, NULL) != MC_FOLDER_PORTAL);
#endif

  /* In a sandbox. */
  CHECK(mc_library_folder_check(at("check/music/Kithsong Library"), MC_FOLDER_USE_LIBRARY, roots) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(at("check/data/Kithsong Library"), MC_FOLDER_USE_NEW, roots) == MC_FOLDER_OK);
  CHECK(mc_library_folder_check(at("check/outside/Kithsong Library"), MC_FOLDER_USE_LIBRARY, roots) ==
        MC_FOLDER_OUT_OF_REACH);
  CHECK(mc_library_folder_check(at("check/music/other"), MC_FOLDER_USE_LIBRARY, roots) == MC_FOLDER_NOT_EMPTY);
#ifndef _WIN32
  CHECK(mc_library_folder_check("/run/user/1000/doc/abcd/Music", MC_FOLDER_USE_ANY, roots) == MC_FOLDER_PORTAL);
  if (geteuid() != 0) {
    mc_mkdirs(at("check/music/locked"));
    chmod(at("check/music/locked"), 0555);
    CHECK(mc_library_folder_check(at("check/music/locked"), MC_FOLDER_USE_LIBRARY, roots) == MC_FOLDER_NOT_WRITABLE);
    CHECK(mc_library_folder_check(at("check/music/locked/Kithsong Library"), MC_FOLDER_USE_ANY, roots) ==
          MC_FOLDER_NOT_WRITABLE);
    chmod(at("check/music/locked"), 0755);
  }
#endif
  /* No probe files left: just other, lib and perhaps locked. */
  CHECK(entries(at("check/music")) == 2 + mc_file_exists(at("check/music/locked")));
  CHECK(entries(at("check/data")) == 0);
}

int main(int argc, char **argv) {
  char *dir = mc_full_path(argc > 1 ? argv[1] : ".");
  snprintf(base, sizeof base, "%s/library_folder_scratch", dir ? dir : ".");
  free(dir);
  mc_remove_tree(base);
  mc_mkdirs(base);
  test_portal();
  test_within();
  test_probe();
  test_check();
  mc_remove_tree(base);
  return TEST_RESULT();
}
