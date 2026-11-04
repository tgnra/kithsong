#include "musiccore/source_run.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/platform.h"

/* build/<preset>/desktop/windows is four up from the root; this leaves room, and keeps the
 * search from wandering far above an installed app. */
#define MAX_LEVELS 8

/* All of them, the rarest first, so anywhere else stops at the first. */
static const char *const k_markers[] = {
  "core/include/musiccore/musiccore.h",
  "desktop/windows/src/main.c",
  "desktop/linux/src/main.c",
  "strings/en.json",
  "CMakePresets.json",
};

/* dir, a separator and name, with name's / as the platform's: malloc'd, or NULL. */
static char *join(const char *dir, const char *name) {
  size_t len = strlen(dir), n = len + strlen(name) + 2;
  char *p = malloc(n);
  if (!p) return NULL;
  snprintf(p, n, "%s%c%s", dir, MC_PATH_SEP, name);
  for (char *c = p + len; *c; c++)
    if (*c == '/') *c = MC_PATH_SEP;
  return p;
}

static int is_root(const char *dir) {
  for (size_t i = 0; i < sizeof k_markers / sizeof k_markers[0]; i++) {
    char *p = join(dir, k_markers[i]);
    int found = p && mc_file_exists(p);
    free(p);
    if (!found) return 0;
  }
  return 1;
}

static int is_separator(char c) {
#ifdef _WIN32
  if (c == '\\') return 1;
#endif
  return c == '/';
}

/* Cuts path, a full path, back to its parent. 0 if it has none. */
static int to_parent(char *path) {
  size_t n = strlen(path);
  while (n > 0 && !is_separator(path[n - 1])) n--;
  if (n == 0 || !path[n]) return 0; /* a root: "C:\" or "/" */
  size_t cut = n - 1;
  /* Keep the separator of a root, which is its whole name. */
  if (cut == 0 || (cut == 2 && path[1] == ':')) cut++;
  path[cut] = '\0';
  return 1;
}

char *mc_source_root(const char *dir) {
  if (!dir || !*dir) return NULL;
  char *at = mc_full_path(dir);
  if (!at) return NULL;
  for (int i = 0; i <= MAX_LEVELS; i++) {
    if (is_root(at)) return at;
    if (!to_parent(at)) break;
  }
  free(at);
  return NULL;
}

int mc_source_run(void) {
  char *exe = mc_exe_path();
  char *real = exe ? mc_real_path(exe) : NULL; /* the file itself, not a link to it */
  char *root = real && to_parent(real) ? mc_source_root(real) : NULL;
  int found = root != NULL;
  free(root);
  free(real);
  free(exe);
  return found;
}

/* FNV-1a of a full path, as mc_same_path() compares paths. */
static uint64_t path_hash(const char *path) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    unsigned char c = *p;
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c == '/') c = '\\';
#endif
    h = (h ^ c) * UINT64_C(1099511628211);
  }
  return h;
}

char *mc_source_library_dir(const char *settings_dir) {
  if (!settings_dir || !*settings_dir) return NULL;
  char *full = mc_full_path(settings_dir), *temp = mc_temp_dir(), *out = NULL;
  if (full && temp) {
    char name[sizeof MC_SOURCE_LIBRARY_PREFIX + 16];
    snprintf(name, sizeof name, MC_SOURCE_LIBRARY_PREFIX "%016" PRIx64, path_hash(full));
    out = join(temp, name);
  }
  free(full);
  free(temp);
  return out;
}

static mc_status write_marker(const char *dir) {
  char *path = join(dir, MC_SOURCE_LIBRARY_MARKER);
  FILE *f = path ? mc_fopen(path, "wb") : NULL;
  free(path);
  if (!f) return MC_ERR_IO;
  int ok = fputs("A library made by a Kithsong build running from its source code, for testing. "
                 "Reset Kithsong in that build's Settings deletes this folder.\n", f) >= 0;
  return fclose(f) == 0 && ok ? MC_OK : MC_ERR_IO;
}

/* Whether library, as settings have it, is the folder expected. */
static int names_folder(const char *library, const char *expected) {
  char *a = mc_full_path(library), *b = mc_full_path(expected);
  int same = a && b && mc_same_path(a, b);
  free(a);
  free(b);
  return same;
}

mc_status mc_source_library_prepare(const char *settings_dir, mc_settings *settings) {
  if (!settings_dir || !settings) return MC_ERR_INVALID_ARG;
  char *expected = mc_source_library_dir(settings_dir);
  if (!expected) return MC_ERR_NOMEM;
  const char *current = mc_settings_library_dir(settings);
  if (current && !names_folder(current, expected)) {
    free(expected);
    return MC_OK; /* chosen, or written by a test: not this one's to change */
  }
  mc_status st = MC_OK;
  if (mc_is_link(expected)) {
    st = MC_ERR_IO; /* someone else's doing, and it leads who knows where */
  } else if (!mc_file_exists(expected)) {
    st = mc_mkdirs(expected);
    if (st == MC_OK) st = write_marker(expected);
  }
  if (st == MC_OK && !current) {
    st = mc_settings_set_library_dir(settings, expected);
    if (st == MC_OK) st = mc_settings_save(settings);
  }
  free(expected);
  return st;
}

int mc_source_reset_deletes_library(int from_source, const char *settings_dir, const char *library_dir) {
  if (!from_source || !settings_dir || !library_dir) return 0;
  char *expected = mc_source_library_dir(settings_dir);
  char *marker = expected ? join(expected, MC_SOURCE_LIBRARY_MARKER) : NULL;
  char *real_expected = expected ? mc_real_path(expected) : NULL;
  char *real_library = mc_real_path(library_dir);
  int ours = marker && real_expected && real_library && mc_same_path(real_expected, real_library) &&
             !mc_is_link(expected) && !mc_is_link(library_dir) && mc_file_exists(marker) && !mc_is_link(marker);
  free(real_library);
  free(real_expected);
  free(marker);
  free(expected);
  return ours;
}

/* A copy keeps a file's read-only attribute, which stops mc_remove_tree() on Windows. */
static int clear_read_only(void *user, const mc_tree_entry *entry) {
  (void)user;
  mc_clear_read_only(entry->path);
  return 0;
}

mc_status mc_source_reset_if_requested(const char *dir, int from_source, int *reset, char **left_behind) {
  if (!from_source) return mc_settings_reset_if_requested(dir, reset, left_behind);

  /* Decided from the settings before they go, and only ever deleting the folder worked out
   * here, not whatever the settings said. */
  char *library = NULL;
  mc_settings *settings = NULL;
  if (dir && mc_settings_load(dir, &settings) == MC_OK &&
      mc_source_reset_deletes_library(1, dir, mc_settings_library_dir(settings)))
    library = mc_source_library_dir(dir);
  mc_settings_free(settings);

  int was_reset = 0;
  char *left = NULL;
  mc_status st = mc_settings_reset_if_requested(dir, &was_reset, &left);
  /* Not cleared at all, so still marked: the next start does both. */
  int kept = st != MC_OK && left && !strcmp(left, dir);
  if (library && was_reset && !kept) {
    mc_tree_walk(library, clear_read_only, NULL);
    mc_status removed = mc_remove_tree(library);
    if (removed != MC_OK && st == MC_OK) {
      st = removed;
      left = mc_strdup(library);
    }
  }
  free(library);
  if (reset) *reset = was_reset;
  if (left_behind) *left_behind = left;
  else free(left);
  return st;
}
