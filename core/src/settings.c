#include "musiccore/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "util/platform.h"

#define SETTINGS_FILE "settings.json"
#define MAX_SETTINGS_BYTES (1u << 20)

struct mc_settings {
  char *path; /* dir/settings.json */
  char *dir;
  yyjson_mut_doc *doc;
};

static char *read_file(const char *path, size_t *len) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return NULL;
  char *buf = malloc(MAX_SETTINGS_BYTES);
  size_t n = buf ? fread(buf, 1, MAX_SETTINGS_BYTES, f) : 0;
  fclose(f);
  *len = n;
  return buf;
}

mc_status mc_settings_load(const char *dir, mc_settings **out) {
  if (!dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_settings *s = calloc(1, sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  size_t plen = strlen(dir) + sizeof SETTINGS_FILE + 1;
  s->dir = mc_strdup(dir);
  s->path = malloc(plen);
  if (!s->dir || !s->path) {
    mc_settings_free(s);
    return MC_ERR_NOMEM;
  }
  snprintf(s->path, plen, "%s%c" SETTINGS_FILE, dir, MC_PATH_SEP);

  size_t len = 0;
  char *text = read_file(s->path, &len);
  yyjson_doc *doc = text ? yyjson_read(text, len, 0) : NULL;
  free(text);
  if (doc && yyjson_is_obj(yyjson_doc_get_root(doc))) s->doc = yyjson_doc_mut_copy(doc, NULL);
  yyjson_doc_free(doc);

  if (!s->doc) { /* missing or damaged: start over */
    s->doc = yyjson_mut_doc_new(NULL);
    if (s->doc) yyjson_mut_doc_set_root(s->doc, yyjson_mut_obj(s->doc));
  }
  if (!s->doc || !yyjson_mut_doc_get_root(s->doc)) {
    mc_settings_free(s);
    return MC_ERR_NOMEM;
  }
  *out = s;
  return MC_OK;
}

void mc_settings_free(mc_settings *s) {
  if (!s) return;
  yyjson_mut_doc_free(s->doc);
  free(s->path);
  free(s->dir);
  free(s);
}

mc_status mc_settings_save(const mc_settings *s) {
  if (!s) return MC_ERR_INVALID_ARG;
  mc_status st = mc_mkdirs(s->dir);
  if (st != MC_OK) return st;

  size_t len = 0;
  char *json = yyjson_mut_write(s->doc, YYJSON_WRITE_PRETTY, &len);
  if (!json) return MC_ERR_NOMEM;

  /* Write a temporary file, then replace: a crash never leaves half a settings file. */
  size_t tlen = strlen(s->path) + 5;
  char *tmp = malloc(tlen);
  st = MC_ERR_IO;
  if (tmp) {
    snprintf(tmp, tlen, "%s.tmp", s->path);
    FILE *f = mc_fopen(tmp, "wb");
    if (f) {
      int ok = fwrite(json, 1, len, f) == len;
      if (fclose(f) == 0 && ok) st = mc_rename(tmp, s->path, 1);
      if (st != MC_OK) mc_remove(tmp);
    }
  } else {
    st = MC_ERR_NOMEM;
  }
  free(tmp);
  free(json);
  return st;
}

const char *mc_settings_library_dir(const mc_settings *s) {
  if (!s) return NULL;
  const char *v = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "library_dir"));
  return v && *v ? v : NULL;
}

mc_status mc_settings_set_library_dir(mc_settings *s, const char *dir) {
  if (!s || !dir || !*dir) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_obj_remove_key(root, "library_dir");
  return yyjson_mut_obj_add_strcpy(s->doc, root, "library_dir", dir) ? MC_OK : MC_ERR_NOMEM;
}
