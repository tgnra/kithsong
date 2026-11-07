#include "kithcore/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "settings_internal.h"
#include "util/platform.h"

#define SETTINGS_FILE "settings.json"
#define MAX_SETTINGS_BYTES (1u << 20)
#define RESET_FILE "reset" /* in dir: clear it on the next start (mc_settings_request_reset()) */

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

void mc_settings_move_old_dir(const char *old_dir, const char *dir) {
  if (old_dir && dir && !mc_file_exists(dir) && mc_file_exists(old_dir)) mc_rename(old_dir, dir, 0);
}

/* dir with suffix appended: malloc'd, or NULL. */
static char *with_suffix(const char *dir, const char *suffix) {
  size_t n = strlen(dir) + strlen(suffix) + 1;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%s", dir, suffix);
  return p;
}

/* dir/reset, the mark mc_settings_request_reset() leaves: malloc'd, or NULL. */
static char *mark_path(const char *dir) {
  size_t n = strlen(dir) + sizeof RESET_FILE + 1;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c" RESET_FILE, dir, MC_PATH_SEP);
  return p;
}

mc_status mc_settings_request_reset(const char *dir) {
  if (!dir) return MC_ERR_INVALID_ARG;
  mc_status st = mc_mkdirs(dir);
  if (st != MC_OK) return st;
  char *mark = mark_path(dir);
  if (!mark) return MC_ERR_NOMEM;
  FILE *f = mc_fopen(mark, "wb");
  st = f && fclose(f) == 0 ? MC_OK : MC_ERR_IO;
  free(mark);
  return st;
}

mc_status mc_settings_reset_if_requested(const char *dir, int *reset, char **left_behind) {
  if (reset) *reset = 0;
  if (left_behind) *left_behind = NULL;
  if (!dir) return MC_ERR_INVALID_ARG;
  char *mark = mark_path(dir);
  char *old = with_suffix(dir, ".old");
  mc_status st = mark && old ? MC_OK : MC_ERR_NOMEM;
  if (st == MC_OK) mc_remove_tree(old); /* an earlier reset's leftovers, if any */
  if (st == MC_OK && mc_file_exists(mark)) {
    if (reset) *reset = 1;
    st = mc_rename(dir, old, 0);
    if (st != MC_OK) { /* still marked, so the next start tries again */
      if (left_behind) *left_behind = mc_strdup(dir);
    } else {
      mc_mkdirs(dir); /* empty, as on a first run; saving makes it again if this fails */
      st = mc_remove_tree(old);
      if (st != MC_OK && left_behind) *left_behind = mc_strdup(old);
    }
  }
  free(mark);
  free(old);
  return st;
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

const char *mc_settings_device_name(const mc_settings *s) {
  if (!s) return NULL;
  const char *v = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "device_name"));
  return v && *v ? v : NULL;
}

mc_status mc_settings_set_device_name(mc_settings *s, const char *name) {
  if (!s) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_obj_remove_key(root, "device_name");
  if (!name || !*name) return MC_OK;
  return yyjson_mut_obj_add_strcpy(s->doc, root, "device_name", name) ? MC_OK : MC_ERR_NOMEM;
}

int mc_settings_bool(const mc_settings *s, const char *key, int def) {
  if (!s || !key) return def;
  yyjson_mut_val *v = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), key);
  return yyjson_mut_is_bool(v) ? yyjson_mut_get_bool(v) : def;
}

mc_status mc_settings_set_bool(mc_settings *s, const char *key, int value) {
  if (!s || !key || !*key) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_obj_remove_key(root, key);
  /* The key is copied: yyjson_mut_obj_add_bool() would keep the caller's pointer. */
  yyjson_mut_val *k = yyjson_mut_strcpy(s->doc, key), *v = yyjson_mut_bool(s->doc, value != 0);
  return k && v && yyjson_mut_obj_add(root, k, v) ? MC_OK : MC_ERR_NOMEM;
}

const char *mc_settings_string(const mc_settings *s, const char *key) {
  if (!s || !key) return NULL;
  const char *v = yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), key));
  return v && *v ? v : NULL;
}

mc_status mc_settings_set_string(mc_settings *s, const char *key, const char *value) {
  if (!s || !key || !*key) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_obj_remove_key(root, key);
  if (!value || !*value) return MC_OK;
  yyjson_mut_val *k = yyjson_mut_strcpy(s->doc, key), *v = yyjson_mut_strcpy(s->doc, value);
  return k && v && yyjson_mut_obj_add(root, k, v) ? MC_OK : MC_ERR_NOMEM;
}

int mc_settings_shuffle(const mc_settings *s) { return mc_settings_bool(s, "shuffle", 0); }

mc_status mc_settings_set_shuffle(mc_settings *s, int on) { return mc_settings_set_bool(s, "shuffle", on); }

int mc_settings_album_in_order(const mc_settings *s) { return mc_settings_bool(s, "album_in_order", 1); }

mc_status mc_settings_set_album_in_order(mc_settings *s, int on) {
  return mc_settings_set_bool(s, "album_in_order", on);
}

int mc_settings_queue_tracks(const mc_settings *s) { return mc_settings_bool(s, "queue_tracks", 0); }

mc_status mc_settings_set_queue_tracks(mc_settings *s, int on) {
  return mc_settings_set_bool(s, "queue_tracks", on);
}

int mc_settings_volume(const mc_settings *s) {
  if (!s) return 100;
  yyjson_mut_val *v = yyjson_mut_obj_get(yyjson_mut_doc_get_root(s->doc), "volume");
  if (!yyjson_mut_is_num(v)) return 100;
  double d = yyjson_mut_get_num(v);
  return d <= 0 ? 0 : d >= 100 ? 100 : (int)(d + 0.5);
}

mc_status mc_settings_set_volume(mc_settings *s, int volume) {
  if (!s) return MC_ERR_INVALID_ARG;
  volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_obj_remove_key(root, "volume");
  return yyjson_mut_obj_add_int(s->doc, root, "volume", volume) ? MC_OK : MC_ERR_NOMEM;
}

int mc_settings_scrobble(const mc_settings *s) { return mc_settings_bool(s, "scrobble", 1); }

mc_status mc_settings_set_scrobble(mc_settings *s, int on) { return mc_settings_set_bool(s, "scrobble", on); }

/* --- Plugins (settings_internal.h) ---
 * "plugins": {"enabled": [file ids], "known": {file id: {"id", "name", "version"}},
 *             "settings": {plugin id: {key: text}}} */

/* root.plugins[key], made an object if create and it isn't one. */
static yyjson_mut_val *plugins_obj(const mc_settings *s, const char *key, int create) {
  yyjson_mut_val *root = yyjson_mut_doc_get_root(s->doc);
  yyjson_mut_val *p = yyjson_mut_obj_get(root, "plugins");
  if (!yyjson_mut_is_obj(p)) {
    if (!create) return NULL;
    yyjson_mut_obj_remove_key(root, "plugins");
    p = yyjson_mut_obj(s->doc);
    if (!p || !yyjson_mut_obj_add_val(s->doc, root, "plugins", p)) return NULL;
  }
  if (!key) return p;
  yyjson_mut_val *v = yyjson_mut_obj_get(p, key);
  if (!v && create) {
    v = !strcmp(key, "enabled") ? yyjson_mut_arr(s->doc) : yyjson_mut_obj(s->doc);
    if (!v || !yyjson_mut_obj_add_val(s->doc, p, key, v)) return NULL;
  }
  return v;
}

int mc_settings_plugin_enabled(const mc_settings *s, const char *file_id) {
  if (!s || !file_id) return 0;
  yyjson_mut_val *arr = plugins_obj(s, "enabled", 0), *v;
  if (!yyjson_mut_is_arr(arr)) return 0;
  yyjson_mut_arr_iter it;
  yyjson_mut_arr_iter_init(arr, &it);
  while ((v = yyjson_mut_arr_iter_next(&it)))
    if (yyjson_mut_is_str(v) && !strcmp(yyjson_mut_get_str(v), file_id)) return 1;
  return 0;
}

mc_status mc_settings_set_plugin_enabled(mc_settings *s, const char *file_id, int on) {
  if (!s || !file_id || !*file_id) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *arr = plugins_obj(s, "enabled", 1), *v;
  if (!yyjson_mut_is_arr(arr)) return MC_ERR_NOMEM;
  yyjson_mut_arr_iter it;
  yyjson_mut_arr_iter_init(arr, &it);
  while ((v = yyjson_mut_arr_iter_next(&it)))
    if (yyjson_mut_is_str(v) && !strcmp(yyjson_mut_get_str(v), file_id)) yyjson_mut_arr_iter_remove(&it);
  if (!on) return MC_OK;
  return yyjson_mut_arr_add_strcpy(s->doc, arr, file_id) ? MC_OK : MC_ERR_NOMEM;
}

const char *mc_settings_plugin_known(const mc_settings *s, const char *file_id, const char *field) {
  if (!s || !file_id || !field) return NULL;
  yyjson_mut_val *known = plugins_obj(s, "known", 0);
  return yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_obj_get(known, file_id), field));
}

mc_status mc_settings_set_plugin_known(mc_settings *s, const char *file_id, const char *id,
                                       const char *name, const char *version) {
  if (!s || !file_id || !*file_id) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *known = plugins_obj(s, "known", 1);
  if (!yyjson_mut_is_obj(known)) return MC_ERR_NOMEM;
  yyjson_mut_obj_remove_key(known, file_id);
  yyjson_mut_val *o = yyjson_mut_obj(s->doc), *k = yyjson_mut_strcpy(s->doc, file_id);
  if (!o || !k || !yyjson_mut_obj_add(known, k, o)) return MC_ERR_NOMEM;
  if (id && !yyjson_mut_obj_add_strcpy(s->doc, o, "id", id)) return MC_ERR_NOMEM;
  if (name && !yyjson_mut_obj_add_strcpy(s->doc, o, "name", name)) return MC_ERR_NOMEM;
  if (version && !yyjson_mut_obj_add_strcpy(s->doc, o, "version", version)) return MC_ERR_NOMEM;
  return MC_OK;
}

void mc_settings_plugin_values(const mc_settings *s, const char *plugin_id,
                               void (*each)(void *user, const char *key, const char *value),
                               void *user) {
  if (!s || !plugin_id || !each) return;
  yyjson_mut_val *obj = yyjson_mut_obj_get(plugins_obj(s, "settings", 0), plugin_id), *k;
  if (!yyjson_mut_is_obj(obj)) return;
  yyjson_mut_obj_iter it;
  yyjson_mut_obj_iter_init(obj, &it);
  while ((k = yyjson_mut_obj_iter_next(&it))) {
    yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
    if (yyjson_mut_is_str(v)) each(user, yyjson_mut_get_str(k), yyjson_mut_get_str(v));
  }
}

mc_status mc_settings_set_plugin_value(mc_settings *s, const char *plugin_id, const char *key,
                                       const char *value) {
  if (!s || !plugin_id || !*plugin_id || !key || !*key) return MC_ERR_INVALID_ARG;
  yyjson_mut_val *all = plugins_obj(s, "settings", 1);
  if (!yyjson_mut_is_obj(all)) return MC_ERR_NOMEM;
  yyjson_mut_val *obj = yyjson_mut_obj_get(all, plugin_id);
  if (!yyjson_mut_is_obj(obj)) {
    yyjson_mut_obj_remove_key(all, plugin_id);
    obj = yyjson_mut_obj(s->doc);
    yyjson_mut_val *k = yyjson_mut_strcpy(s->doc, plugin_id);
    if (!obj || !k || !yyjson_mut_obj_add(all, k, obj)) return MC_ERR_NOMEM;
  }
  yyjson_mut_obj_remove_key(obj, key);
  if (!value) return MC_OK;
  yyjson_mut_val *k = yyjson_mut_strcpy(s->doc, key), *v = yyjson_mut_strcpy(s->doc, value);
  return k && v && yyjson_mut_obj_add(obj, k, v) ? MC_OK : MC_ERR_NOMEM;
}
