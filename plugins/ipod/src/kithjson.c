#include "kithipod/kithjson.h"

#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "util.h"

static void entry_free(ipod_kith_entry *e) {
  free(e->key);
  free(e->track_uid);
  free(e->file_uid);
  free(e->format);
  free(e->setting);
  free(e->sha256);
  memset(e, 0, sizeof *e);
}

static void playlist_free(ipod_kith_playlist *p) {
  free(p->uid);
  free(p->name);
  memset(p, 0, sizeof *p);
}

void ipod_kith_free(ipod_kith_map *map) {
  for (size_t i = 0; i < map->n; i++) entry_free(&map->entries[i]);
  free(map->entries);
  for (size_t i = 0; i < map->n_playlists; i++) playlist_free(&map->playlists[i]);
  free(map->playlists);
  memset(map, 0, sizeof *map);
}

const ipod_kith_entry *ipod_kith_find(const ipod_kith_map *map, const char *key) {
  for (size_t i = 0; i < map->n; i++)
    if (strcmp(map->entries[i].key, key) == 0) return &map->entries[i];
  return NULL;
}

int ipod_kith_set2(ipod_kith_map *map, const char *key, const char *track_uid, const char *file_uid,
                   const char *format, const char *setting, const char *sha256) {
  ipod_kith_entry n = {0};
  n.key = ipod_strdup(key);
  n.track_uid = ipod_strdup(track_uid);
  n.file_uid = ipod_strdup(file_uid);
  n.format = ipod_strdup(format);
  n.setting = ipod_strdup(setting);
  n.sha256 = ipod_strdup(sha256);
  if (!n.key || (track_uid && !n.track_uid) || (file_uid && !n.file_uid) || (format && !n.format) ||
      (setting && !n.setting) || (sha256 && !n.sha256)) {
    entry_free(&n);
    return IPOD_ERR_NOMEM;
  }
  for (size_t i = 0; i < map->n; i++) {
    if (strcmp(map->entries[i].key, key) == 0) {
      entry_free(&map->entries[i]);
      map->entries[i] = n;
      return 0;
    }
  }
  if (map->n == map->cap) {
    size_t cap = map->cap ? map->cap * 2 : 64;
    ipod_kith_entry *e = realloc(map->entries, cap * sizeof *e);
    if (!e) {
      entry_free(&n);
      return IPOD_ERR_NOMEM;
    }
    map->entries = e;
    map->cap = cap;
  }
  map->entries[map->n++] = n;
  return 0;
}

int ipod_kith_set(ipod_kith_map *map, const char *key, const char *track_uid, const char *file_uid,
                  const char *format, const char *sha256) {
  return ipod_kith_set2(map, key, track_uid, file_uid, format, NULL, sha256);
}

int ipod_kith_remove(ipod_kith_map *map, const char *key) {
  for (size_t i = 0; i < map->n; i++) {
    if (strcmp(map->entries[i].key, key) == 0) {
      entry_free(&map->entries[i]);
      memmove(map->entries + i, map->entries + i + 1, (map->n - i - 1) * sizeof *map->entries);
      map->n--;
      return 0;
    }
  }
  return IPOD_ERR_NOT_FOUND;
}

const ipod_kith_playlist *ipod_kith_playlist_find(const ipod_kith_map *map, const char *uid) {
  for (size_t i = 0; i < map->n_playlists; i++)
    if (strcmp(map->playlists[i].uid, uid) == 0) return &map->playlists[i];
  return NULL;
}

int ipod_kith_playlist_set(ipod_kith_map *map, const char *uid, const char *name, uint64_t id) {
  ipod_kith_playlist n = {0};
  n.uid = ipod_strdup(uid);
  n.name = ipod_strdup(name ? name : "");
  n.id = id;
  if (!n.uid || !n.name) {
    playlist_free(&n);
    return IPOD_ERR_NOMEM;
  }
  for (size_t i = 0; i < map->n_playlists; i++) {
    if (strcmp(map->playlists[i].uid, uid) == 0) {
      playlist_free(&map->playlists[i]);
      map->playlists[i] = n;
      return 0;
    }
  }
  if (map->n_playlists == map->cap_playlists) {
    size_t cap = map->cap_playlists ? map->cap_playlists * 2 : 8;
    ipod_kith_playlist *p = realloc(map->playlists, cap * sizeof *p);
    if (!p) {
      playlist_free(&n);
      return IPOD_ERR_NOMEM;
    }
    map->playlists = p;
    map->cap_playlists = cap;
  }
  map->playlists[map->n_playlists++] = n;
  return 0;
}

int ipod_kith_playlist_remove(ipod_kith_map *map, const char *uid) {
  for (size_t i = 0; i < map->n_playlists; i++) {
    if (strcmp(map->playlists[i].uid, uid) == 0) {
      playlist_free(&map->playlists[i]);
      memmove(map->playlists + i, map->playlists + i + 1,
              (map->n_playlists - i - 1) * sizeof *map->playlists);
      map->n_playlists--;
      return 0;
    }
  }
  return IPOD_ERR_NOT_FOUND;
}

static const char *obj_str(yyjson_val *o, const char *k) {
  yyjson_val *v = yyjson_obj_get(o, k);
  return yyjson_is_str(v) ? yyjson_get_str(v) : NULL;
}

static int parse_hex64(const char *s, uint64_t *out) {
  if (!s || !*s || strlen(s) > 16) return -1;
  uint64_t v = 0;
  for (; *s; s++) {
    int c = (unsigned char)*s, d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = v * 16 + (uint64_t)d;
  }
  *out = v;
  return 0;
}

int ipod_kith_parse(const char *json, size_t len, ipod_kith_map *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  yyjson_doc *doc = yyjson_read(json, len, 0);
  if (!doc) return ipod_seterr(err, IPOD_ERR_FORMAT, "kithsong.json isn't valid JSON");
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *tracks = yyjson_obj_get(root, "tracks");
  int r = 0;
  if (yyjson_is_obj(tracks)) {
    yyjson_obj_iter it;
    yyjson_obj_iter_init(tracks, &it);
    yyjson_val *k;
    while (r == 0 && (k = yyjson_obj_iter_next(&it)) != NULL) {
      yyjson_val *v = yyjson_obj_iter_get_val(k);
      if (!yyjson_is_obj(v)) continue;
      r = ipod_kith_set2(out, yyjson_get_str(k), obj_str(v, "track_uid"), obj_str(v, "file_uid"),
                         obj_str(v, "format"), obj_str(v, "setting"), obj_str(v, "sha256"));
    }
  }
  yyjson_val *lists = yyjson_obj_get(root, "playlists");
  if (r == 0 && yyjson_is_obj(lists)) {
    yyjson_obj_iter it;
    yyjson_obj_iter_init(lists, &it);
    yyjson_val *k;
    while (r == 0 && (k = yyjson_obj_iter_next(&it)) != NULL) {
      yyjson_val *v = yyjson_obj_iter_get_val(k);
      uint64_t id;
      if (!yyjson_is_obj(v) || parse_hex64(obj_str(v, "id"), &id) != 0) continue;
      r = ipod_kith_playlist_set(out, yyjson_get_str(k), obj_str(v, "name"), id);
    }
  }
  yyjson_doc_free(doc);
  if (r != 0) {
    ipod_kith_free(out);
    return ipod_seterr(err, r, "out of memory");
  }
  return 0;
}

int ipod_kith_read(const char *path, ipod_kith_map *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  if (!ipod_file_exists(path)) return 0;
  size_t len;
  uint8_t *data = ipod_read_file(path, &len);
  if (!data) return ipod_seterr(err, IPOD_ERR_IO, "can't read %s", path);
  int r = ipod_kith_parse((const char *)data, len, out, err);
  free(data);
  return r;
}

char *ipod_kith_serialize(const ipod_kith_map *map, size_t *out_len) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return NULL;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_int(doc, root, "version", 1);
  yyjson_mut_val *tracks = yyjson_mut_obj_add_obj(doc, root, "tracks");
  for (size_t i = 0; tracks && i < map->n; i++) {
    const ipod_kith_entry *e = &map->entries[i];
    yyjson_mut_val *o = yyjson_mut_obj_add_obj(doc, tracks, e->key);
    if (!o) break;
    if (e->track_uid) yyjson_mut_obj_add_str(doc, o, "track_uid", e->track_uid);
    if (e->file_uid) yyjson_mut_obj_add_str(doc, o, "file_uid", e->file_uid);
    if (e->format) yyjson_mut_obj_add_str(doc, o, "format", e->format);
    if (e->setting) yyjson_mut_obj_add_str(doc, o, "setting", e->setting);
    if (e->sha256) yyjson_mut_obj_add_str(doc, o, "sha256", e->sha256);
  }
  if (map->n_playlists) {
    yyjson_mut_val *lists = yyjson_mut_obj_add_obj(doc, root, "playlists");
    for (size_t i = 0; lists && i < map->n_playlists; i++) {
      const ipod_kith_playlist *p = &map->playlists[i];
      yyjson_mut_val *o = yyjson_mut_obj_add_obj(doc, lists, p->uid);
      if (!o) break;
      char id[17];
      snprintf(id, sizeof id, "%016llx", (unsigned long long)p->id);
      yyjson_mut_obj_add_str(doc, o, "name", p->name);
      yyjson_mut_obj_add_strcpy(doc, o, "id", id);
    }
  }
  size_t len = 0;
  char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return NULL;
  /* yyjson allocates with malloc by default; copy anyway so free() is always right. */
  char *copy = ipod_strndup(json, len);
  free(json);
  if (copy && out_len) *out_len = len;
  return copy;
}

int ipod_kith_write(const char *path, const ipod_kith_map *map, ipod_error *err) {
  size_t len = 0;
  char *json = ipod_kith_serialize(map, &len);
  if (!json) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  int r = ipod_write_file_atomic(path, json, len, err);
  free(json);
  return r;
}
