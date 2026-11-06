/* Resetting the per-device folder: mc_settings_request_reset() marks it, and
 * mc_settings_reset_if_requested() clears it on the next start, leaving it empty and
 * keeping everything else (a sibling folder, the library) alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "kithcore/settings.h"
#include "test.h"
#include "util/platform.h"

static char g_scratch[1024];

static void path(char *out, size_t cap, const char *dir, const char *name) {
  snprintf(out, cap, "%s%c%s", dir, MC_PATH_SEP, name);
}

static void write_file(const char *p) {
  FILE *f = mc_fopen(p, "wb");
  if (f) {
    fputs("x", f);
    fclose(f);
  }
}

int main(int argc, char **argv) {
  snprintf(g_scratch, sizeof g_scratch, "%s%ctest_settings_reset.d", argc > 1 ? argv[1] : ".", MC_PATH_SEP);
  mc_remove_tree(g_scratch);
  CHECK(mc_mkdirs(g_scratch) == MC_OK);

  char dir[1100], old[1110], sibling[1100], p[1200];
  path(dir, sizeof dir, g_scratch, "kithsong");
  snprintf(old, sizeof old, "%s.old", dir);
  path(sibling, sizeof sibling, g_scratch, "library");

  /* A folder as the app leaves it: settings, a database, a subfolder of pictures. */
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_set_library_dir(s, sibling) == MC_OK);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);
  path(p, sizeof p, dir, "sync.db");
  write_file(p);
  path(p, sizeof p, dir, "playlist-art");
  CHECK(mc_mkdirs(p) == MC_OK);
  snprintf(p, sizeof p, "%s%cplaylist-art%ca.jpg", dir, MC_PATH_SEP, MC_PATH_SEP);
  write_file(p);
  CHECK(mc_mkdirs(sibling) == MC_OK);
  path(p, sizeof p, sibling, "library.db");
  write_file(p);

  /* Not marked: nothing happens. */
  int reset = -1;
  char *left = (char *)"unset";
  CHECK(mc_settings_reset_if_requested(dir, &reset, &left) == MC_OK);
  CHECK(reset == 0);
  CHECK(left == NULL);
  path(p, sizeof p, dir, "sync.db");
  CHECK(mc_file_exists(p));

  /* Marked: the next start finds it empty, as on a first run, and only it. */
  CHECK(mc_settings_request_reset(dir) == MC_OK);
  path(p, sizeof p, dir, "reset");
  CHECK(mc_file_exists(p));
  CHECK(mc_settings_reset_if_requested(dir, &reset, &left) == MC_OK);
  CHECK(reset == 1);
  CHECK(left == NULL);
  CHECK(mc_file_exists(dir));
  CHECK(!mc_file_exists(old));
  const char *gone[] = {"reset", "settings.json", "sync.db", "playlist-art"};
  for (size_t i = 0; i < sizeof gone / sizeof gone[0]; i++) {
    path(p, sizeof p, dir, gone[i]);
    CHECK(!mc_file_exists(p));
  }
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_library_dir(s) == NULL);
  mc_settings_free(s);
  path(p, sizeof p, sibling, "library.db");
  CHECK(mc_file_exists(p));

  /* Once is enough: the next start leaves the new settings alone. */
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_set_device_name(s, "Study") == MC_OK);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);
  CHECK(mc_settings_reset_if_requested(dir, &reset, &left) == MC_OK);
  CHECK(reset == 0);
  path(p, sizeof p, dir, "settings.json");
  CHECK(mc_file_exists(p));

  /* An earlier reset's leftovers go quietly, marked or not. */
  CHECK(mc_mkdirs(old) == MC_OK);
  path(p, sizeof p, old, "stuck.db");
  write_file(p);
  CHECK(mc_settings_reset_if_requested(dir, &reset, &left) == MC_OK);
  CHECK(reset == 0);
  CHECK(!mc_file_exists(old));

  /* A folder that was never made, marked or not, is fine. */
  path(p, sizeof p, g_scratch, "never");
  CHECK(mc_settings_reset_if_requested(p, &reset, &left) == MC_OK);
  CHECK(reset == 0);
  CHECK(mc_settings_reset_if_requested(NULL, &reset, &left) == MC_ERR_INVALID_ARG);

  mc_remove_tree(g_scratch);
  return TEST_RESULT();
}
