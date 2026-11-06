/* Running from the source code (kithcore/source_run.h): finding a checkout, the library such
 * a run makes, and above all what Reset may delete. Its fixtures are in the temp folder, as
 * this test's own build folder is inside a checkout. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* symlink() under -std=c11 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "kithcore/kithcore.h"
#include "kithcore/settings.h"
#include "kithcore/source_run.h"
#include "test.h"
#include "util/platform.h"

static char g_scratch[1024];

static const char *const k_repo_files[] = {
  "core/include/kithcore/kithcore.h",
  "desktop/windows/src/main.c",
  "desktop/linux/src/main.c",
  "strings/en.json",
  "CMakePresets.json",
};
enum { REPO_FILES = sizeof k_repo_files / sizeof k_repo_files[0] };

static void path(char *out, size_t cap, const char *dir, const char *name) {
  snprintf(out, cap, "%s%c%s", dir, MC_PATH_SEP, name);
  for (char *p = out + strlen(dir); *p; p++)
    if (*p == '/') *p = MC_PATH_SEP;
}

static void write_file(const char *p) {
  FILE *f = mc_fopen(p, "wb");
  if (f) {
    fputs("x", f);
    fclose(f);
  }
}

/* A file at dir/name, with the folders it needs. */
static void put_file(const char *dir, const char *name) {
  char p[1400];
  path(p, sizeof p, dir, name);
  char *sep = strrchr(p, MC_PATH_SEP);
  *sep = '\0';
  CHECK(mc_mkdirs(p) == MC_OK);
  *sep = MC_PATH_SEP;
  write_file(p);
}

static int has_file(const char *dir, const char *name) {
  char p[1400];
  path(p, sizeof p, dir, name);
  return mc_file_exists(p);
}

static int make_link(const char *link, const char *target) {
#ifdef _WIN32
  char cmd[2800];
  snprintf(cmd, sizeof cmd, "mklink /J \"%s\" \"%s\" >NUL", link, target);
  return system(cmd) == 0 && mc_is_link(link);
#else
  return symlink(target, link) == 0;
#endif
}

/* A fake checkout with all the files but skip (-1 for none). */
static void make_repo(const char *root, int skip) {
  for (int i = 0; i < REPO_FILES; i++)
    if (i != skip) put_file(root, k_repo_files[i]);
}

static int same_full(const char *a, const char *b) {
  char *fa = mc_full_path(a), *fb = mc_full_path(b);
  int same = fa && fb && mc_same_path(fa, fb);
  mc_free(fa);
  mc_free(fb);
  return same;
}

static void test_finding_a_checkout(void) {
  char root[1100], deep[1300], worktree[1100], plain[1100], p[1300];

  /* Found from the build folder, and from the root. */
  path(root, sizeof root, g_scratch, "repo");
  make_repo(root, -1);
  path(deep, sizeof deep, root, "build/windows-x64/desktop/windows");
  CHECK(mc_mkdirs(deep) == MC_OK);
  char *found = mc_source_root(deep);
  CHECK(found && same_full(found, root));
  mc_free(found);
  found = mc_source_root(root);
  CHECK(found && same_full(found, root));
  mc_free(found);

  /* A worktree, where .git is a file, and nested inside another checkout's folder. */
  path(worktree, sizeof worktree, root, ".claude/worktrees/agent");
  make_repo(worktree, -1);
  put_file(worktree, ".git");
  path(deep, sizeof deep, worktree, "build/linux-gtk/desktop/linux");
  CHECK(mc_mkdirs(deep) == MC_OK);
  found = mc_source_root(deep);
  CHECK(found && same_full(found, worktree));
  mc_free(found);

  /* One file short is no checkout, whichever it is. */
  for (int i = 0; i < REPO_FILES; i++) {
    char name[32];
    snprintf(name, sizeof name, "partial%d", i);
    path(p, sizeof p, g_scratch, name);
    make_repo(p, i);
    CHECK(mc_source_root(p) == NULL);
  }

  /* An installed app's folder: a .git and a core folder don't make one either. */
  path(plain, sizeof plain, g_scratch, "Program Files/Kithsong");
  CHECK(mc_mkdirs(plain) == MC_OK);
  put_file(plain, ".git");
  put_file(plain, "core/README.txt");
  CHECK(mc_source_root(plain) == NULL);

  /* Too far down to be a build folder. */
  path(deep, sizeof deep, root, "a/b/c/d/e/f/g/h/i");
  CHECK(mc_mkdirs(deep) == MC_OK);
  CHECK(mc_source_root(deep) == NULL);
  path(deep, sizeof deep, root, "a/b/c/d/e/f/g/h");
  found = mc_source_root(deep);
  CHECK(found != NULL);
  mc_free(found);

  CHECK(mc_source_root(NULL) == NULL);
  CHECK(mc_source_root("") == NULL);
}

static void test_library_folder(void) {
  char a[1100], b[1100], with_sep[1110];
  path(a, sizeof a, g_scratch, "config-a");
  path(b, sizeof b, g_scratch, "config-b");
  snprintf(with_sep, sizeof with_sep, "%s%c", a, MC_PATH_SEP);

  char *la = mc_source_library_dir(a), *lb = mc_source_library_dir(b), *la2 = mc_source_library_dir(with_sep);
  char *temp = mc_temp_dir();
  CHECK(la && lb && la2 && temp);
  CHECK(strcmp(la, lb) != 0); /* two instances, two libraries */
  CHECK(!strcmp(la, la2));
  CHECK(!strncmp(la, temp, strlen(temp)));
  CHECK(strstr(la, MC_SOURCE_LIBRARY_PREFIX) != NULL);
#ifdef _WIN32
  char upper[1100];
  snprintf(upper, sizeof upper, "%s", a);
  for (char *p = upper; *p; p++)
    if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
  char *lu = mc_source_library_dir(upper);
  CHECK(lu && !strcmp(la, lu)); /* one folder, however it's spelt */
  mc_free(lu);
#endif
  mc_free(temp);
  mc_free(la2);
  mc_free(lb);
  mc_free(la);
  CHECK(mc_source_library_dir(NULL) == NULL);
  CHECK(mc_source_library_dir("") == NULL);
}

/* A settings folder with library_dir saved, and a "reset" mark if marked. */
static void make_settings(const char *dir, const char *library, int marked) {
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  if (library) CHECK(mc_settings_set_library_dir(s, library) == MC_OK);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);
  if (marked) CHECK(mc_settings_request_reset(dir) == MC_OK);
}

/* A source run's first start in dir: the library it gets, filled with a few files. malloc'd. */
static char *start_source_run(const char *dir) {
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_library_dir(s) == NULL);
  CHECK(mc_source_library_prepare(dir, s) == MC_OK);
  char *library = mc_source_library_dir(dir);
  CHECK(library && !strcmp(mc_settings_library_dir(s), library));
  mc_settings_free(s);
  put_file(library, "library.db");
  put_file(library, "Artist/Album/01 Track.opus");
  return library;
}

static void test_prepare(void) {
  char dir[1100], foreign[1100];
  path(dir, sizeof dir, g_scratch, "prepare");
  path(foreign, sizeof foreign, g_scratch, "foreign-library");

  /* The first start makes it and saves it. */
  char *library = mc_source_library_dir(dir);
  CHECK(library != NULL);
  mc_remove_tree(library);
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_source_library_prepare(dir, s) == MC_OK);
  mc_settings_free(s);
  CHECK(has_file(library, MC_SOURCE_LIBRARY_MARKER));
  CHECK(!strncmp(MC_SOURCE_LIBRARY_MARKER, ".kithsong-", 10)); /* so the orphan sweep leaves it */
  CHECK(mc_settings_load(dir, &s) == MC_OK); /* from the file this time */
  CHECK(!strcmp(mc_settings_library_dir(s), library));

  /* Later starts change nothing; if the system clears the temp folder, it comes back. */
  put_file(library, "library.db");
  CHECK(mc_source_library_prepare(dir, s) == MC_OK);
  CHECK(has_file(library, "library.db"));
  CHECK(mc_remove_tree(library) == MC_OK);
  CHECK(mc_source_library_prepare(dir, s) == MC_OK);
  CHECK(has_file(library, MC_SOURCE_LIBRARY_MARKER));
  CHECK(mc_remove_tree(library) == MC_OK);

  /* A library chosen elsewhere stays, and nothing is made for it or in it. */
  CHECK(mc_settings_set_library_dir(s, foreign) == MC_OK);
  CHECK(mc_source_library_prepare(dir, s) == MC_OK);
  CHECK(!strcmp(mc_settings_library_dir(s), foreign));
  CHECK(!mc_file_exists(library));
  CHECK(!mc_file_exists(foreign));

  /* A link where the library would go is no place for one. */
  CHECK(mc_settings_set_string(s, "library_dir", NULL) == MC_OK);
  CHECK(mc_mkdirs(foreign) == MC_OK);
  CHECK(make_link(library, foreign));
  CHECK(mc_source_library_prepare(dir, s) != MC_OK);
  CHECK(mc_settings_library_dir(s) == NULL);
  CHECK(!has_file(foreign, MC_SOURCE_LIBRARY_MARKER));
  CHECK(mc_remove_tree(library) == MC_OK);
  CHECK(mc_file_exists(foreign));

  mc_settings_free(s);
  mc_free(library);
  CHECK(mc_source_library_prepare(NULL, NULL) == MC_ERR_INVALID_ARG);
}

/* Not from source: an installed app keeps its library, even one that looks like a debug one. */
static void test_reset_installed(void) {
  char dir[1100];
  path(dir, sizeof dir, g_scratch, "reset-installed");
  char *library = start_source_run(dir);
  CHECK(mc_source_reset_deletes_library(0, dir, library) == 0);
  CHECK(mc_settings_request_reset(dir) == MC_OK);
  int reset = 0;
  char *left = NULL;
  CHECK(mc_source_reset_if_requested(dir, 0, &reset, &left) == MC_OK);
  CHECK(reset == 1);
  CHECK(left == NULL);
  CHECK(!has_file(dir, "settings.json"));
  CHECK(has_file(library, "library.db"));
  CHECK(has_file(library, "Artist/Album/01 Track.opus"));
  mc_remove_tree(library);
  mc_free(library);
}

/* From source with a foreign library: Open Another Library, a real music folder. */
static void test_reset_foreign(void) {
  char dir[1100], foreign[1100];
  path(dir, sizeof dir, g_scratch, "reset-foreign");
  path(foreign, sizeof foreign, g_scratch, "real-music");
  put_file(foreign, "library.db");
  put_file(foreign, "Artist/Album/01 Track.opus");
  make_settings(dir, foreign, 1);

  CHECK(mc_source_reset_deletes_library(1, dir, foreign) == 0);
  int reset = 0;
  char *left = NULL;
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(reset == 1);
  CHECK(left == NULL);
  CHECK(!has_file(dir, "settings.json"));
  CHECK(has_file(foreign, "library.db"));
  CHECK(has_file(foreign, "Artist/Album/01 Track.opus"));

  /* Even with the marker in it, or another settings folder's debug library. */
  put_file(foreign, MC_SOURCE_LIBRARY_MARKER);
  CHECK(mc_source_reset_deletes_library(1, dir, foreign) == 0);
  char other_dir[1100];
  path(other_dir, sizeof other_dir, g_scratch, "reset-foreign-other");
  char *other = start_source_run(other_dir);
  make_settings(dir, other, 1);
  CHECK(mc_source_reset_deletes_library(1, dir, other) == 0);
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(has_file(other, "library.db"));
  mc_remove_tree(other);
  mc_free(other);
}

/* From source with its own library: it goes, for good, and only it. */
static void test_reset_debug_library(void) {
  char dir[1100], outside[1100], link[1300], p[1300];
  path(dir, sizeof dir, g_scratch, "reset-debug");
  path(outside, sizeof outside, g_scratch, "outside");
  put_file(outside, "keep.opus");
  char *library = start_source_run(dir);
  path(link, sizeof link, library, "Artist/link");
  CHECK(make_link(link, outside));
  path(p, sizeof p, library, "Artist/Album/01 Track.opus");
#ifdef _WIN32
  char cmd[1500];
  snprintf(cmd, sizeof cmd, "attrib +R \"%s\"", p); /* a copy keeps this */
  CHECK(system(cmd) == 0);
#endif

  /* Only once asked. */
  CHECK(mc_source_reset_deletes_library(1, dir, library) == 1);
  int reset = -1;
  char *left = NULL;
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(reset == 0);
  CHECK(has_file(library, "library.db"));

  CHECK(mc_settings_request_reset(dir) == MC_OK);
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(reset == 1);
  CHECK(left == NULL);
  CHECK(!mc_file_exists(library));
  CHECK(!has_file(dir, "settings.json"));
  CHECK(has_file(outside, "keep.opus")); /* the link went, not what it led to */

  /* The next start makes a new one, as on a first run. */
  char *again = start_source_run(dir);
  CHECK(!strcmp(again, library));
  mc_remove_tree(again);
  mc_free(again);
  mc_free(library);
}

/* The library's folder named another way, by a link, or swapped for one. */
static void test_reset_links(void) {
  char dir[1100], music[1100], via[1100];
  path(dir, sizeof dir, g_scratch, "reset-links");
  path(music, sizeof music, g_scratch, "linked-music");
  path(via, sizeof via, g_scratch, "via-link");
  put_file(music, "library.db");

  /* Settings naming a link to the library: not what was made, so left alone. */
  char *library = start_source_run(dir);
  CHECK(make_link(via, library));
  make_settings(dir, via, 1);
  CHECK(mc_source_reset_deletes_library(1, dir, via) == 0);
  int reset = 0;
  char *left = NULL;
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(has_file(library, "library.db"));
  CHECK(mc_remove_tree(via) == MC_OK);

  /* The library's own path now a link to real music, marker and all. */
  CHECK(mc_remove_tree(library) == MC_OK);
  put_file(music, MC_SOURCE_LIBRARY_MARKER);
  CHECK(make_link(library, music));
  make_settings(dir, library, 1);
  CHECK(mc_source_reset_deletes_library(1, dir, library) == 0);
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(reset == 1);
  CHECK(has_file(music, "library.db"));
  CHECK(mc_is_link(library));
  CHECK(mc_remove_tree(library) == MC_OK);
  CHECK(has_file(music, "library.db"));

  /* The right folder, but this feature didn't make it: no marker. */
  put_file(library, "library.db");
  make_settings(dir, library, 1);
  CHECK(mc_source_reset_deletes_library(1, dir, library) == 0);
  CHECK(mc_source_reset_if_requested(dir, 1, &reset, &left) == MC_OK);
  CHECK(has_file(library, "library.db"));
  mc_remove_tree(library);
  mc_free(library);

  /* Nothing to go on: no settings, a folder that's gone, no library. */
  CHECK(mc_source_reset_deletes_library(1, NULL, music) == 0);
  CHECK(mc_source_reset_deletes_library(1, dir, NULL) == 0);
  path(via, sizeof via, g_scratch, "never-made");
  CHECK(mc_source_reset_deletes_library(1, dir, via) == 0);
  CHECK(mc_source_reset_if_requested(NULL, 1, &reset, &left) == MC_ERR_INVALID_ARG);
}

static void test_paths(void) {
  char dir[1100], link[1100], file[1200];
  path(dir, sizeof dir, g_scratch, "real-dir");
  path(link, sizeof link, g_scratch, "link-dir");
  path(file, sizeof file, dir, "a.txt");
  CHECK(mc_mkdirs(dir) == MC_OK);
  write_file(file);
  CHECK(make_link(link, dir));

  char *a = mc_real_path(dir), *b = mc_real_path(link), *c = mc_real_path(file);
  CHECK(a && b && c);
  CHECK(mc_same_path(a, b));
  CHECK(!mc_same_path(a, c));
  CHECK(strstr(a, "\\\\?\\") == NULL);
  mc_free(a);
  mc_free(b);
  mc_free(c);
  path(file, sizeof file, dir, "missing");
  CHECK(mc_real_path(file) == NULL);
  CHECK(mc_real_path(NULL) == NULL);
  CHECK(mc_remove_tree(link) == MC_OK);

  /* This test itself is a running executable, wherever its build folder is. */
  char *exe = mc_exe_path();
  CHECK(exe && mc_file_exists(exe));
  mc_free(exe);
  CHECK(mc_source_run() == 0 || mc_source_run() == 1);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  char *temp = mc_temp_dir();
  CHECK(temp != NULL);
  uint32_t id = 0;
  CHECK(mc_random_bytes(&id, sizeof id) == MC_OK);
  snprintf(g_scratch, sizeof g_scratch, "%s%ctest_source_run-%08x", temp, MC_PATH_SEP, (unsigned)id);
  mc_free(temp);
  CHECK(mc_mkdirs(g_scratch) == MC_OK);

  test_finding_a_checkout();
  test_library_folder();
  test_prepare();
  test_reset_installed();
  test_reset_foreign();
  test_reset_debug_library();
  test_reset_links();
  test_paths();

  mc_remove_tree(g_scratch);
  return TEST_RESULT();
}
