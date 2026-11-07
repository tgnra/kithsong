#include "kithipod/device.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#include "kithipod/artwork.h"
#include "kithipod/playcounts.h"
#include "util.h"

/* A play recorded on the iPod, by dbid, kept for folding into the database if it isn't
 * taken in before the next commit. */
typedef struct pending_play {
  uint64_t dbid;
  uint32_t count;
  uint32_t last_played; /* Mac */
} pending_play;

struct ipod_device {
  char *mount, *itunes_dir, *db_path, *kith_path, *pc_path, *backup_path;
  ipod_sysinfo si;
  const ipod_model *model;
  int music_dirs;
  ipod_db *db;
  ipod_kith_map kith;
  ipod_artdb *art;

  ipod_play *plays;
  pending_play *pending;
  size_t n_plays;
  uint8_t *pc_bytes;
  size_t pc_len;
  int has_playcounts;

  ipod_otg_playlist *otg;
  size_t n_otg;
  int has_otg;

  unsigned done;
  int db_dirty, kith_dirty, backed_up;
  int folded; /* pending plays and OTG lists are already in the database */
};

static int empty_str(const char *s) { return !s || !*s; }

void ipod_track_key(const ipod_track *t, char key[IPOD_KEY_LEN]) {
  snprintf(key, IPOD_KEY_LEN, "%016llx", (unsigned long long)t->dbid);
}

static int parse_key(const char *key, uint64_t *dbid) {
  if (!key || strlen(key) != 16) return -1;
  uint64_t v = 0;
  for (int i = 0; i < 16; i++) {
    int c = tolower((unsigned char)key[i]);
    if (c >= '0' && c <= '9') v = v * 16 + (uint64_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v = v * 16 + (uint64_t)(c - 'a' + 10);
    else return -1;
  }
  *dbid = v;
  return 0;
}

/* --- Probe and open ----------------------------------------------------------------- */

/* The hash fields of an mhbd header: the hashing scheme at 0x30 and the hash72 marker at
 * 0x70. Both are zero in databases for pre-hash iPods. */
static int db_file_hashed(const char *path) {
  FILE *f = ipod_fopen(path, "rb");
  if (!f) return 0;
  uint8_t h[0x74] = {0};
  size_t n = fread(h, 1, sizeof h, f);
  fclose(f);
  if (n < 0x20 || memcmp(h, "mhbd", 4) != 0) return 0;
  uint32_t hlen = ipod_rd32(h + 4);
  if (hlen >= 0x32 && n >= 0x32 && ipod_rd16(h + 0x30) != 0) return 1;
  if (hlen >= 0x72 && n >= 0x72 && ipod_rd16(h + 0x70) != 0) return 1;
  return 0;
}

int ipod_probe(const char *mount, ipod_sysinfo *si_out) {
  ipod_sysinfo si;
  char *db = ipod_path_join(mount, "iPod_Control/iTunes/iTunesDB");
  if (!db) return IPOD_ERR_NOMEM;
  if (!ipod_file_exists(db)) {
    free(db);
    return IPOD_ERR_NOT_FOUND;
  }
  if (ipod_sysinfo_read(mount, &si, NULL) != 0) memset(&si, 0, sizeof si);
  if (si_out) *si_out = si;
  int support = (int)ipod_model_support(si.model, si.model_num);
  if (db_file_hashed(db)) support = IPOD_UNSUPPORTED_HASH;
  free(db);
  return support;
}

static int count_music_dirs(const char *mount) {
  int n = 0;
  for (int i = 0; i < 100; i++) {
    char rel[32];
    snprintf(rel, sizeof rel, "iPod_Control/Music/F%02d", i);
    char *p = ipod_path_join(mount, rel);
    int ok = p && ipod_dir_exists(p);
    free(p);
    if (!ok) break;
    n++;
  }
  return n;
}

static int make_layout(ipod_device *dev) {
  static const char *dirs[] = {"iPod_Control", "iPod_Control/iTunes", "iPod_Control/Music",
                               "iPod_Control/Device", "iPod_Control/Artwork"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    if (!dev->model || !dev->model->artwork) {
      if (strcmp(dirs[i], "iPod_Control/Artwork") == 0) continue;
    }
    char *p = ipod_path_join(dev->mount, dirs[i]);
    int r = p ? ipod_mkdir(p) : -1;
    free(p);
    if (r != 0) return -1;
  }
  for (int i = 0; i < dev->music_dirs; i++) {
    char rel[32];
    snprintf(rel, sizeof rel, "iPod_Control/Music/F%02d", i);
    char *p = ipod_path_join(dev->mount, rel);
    int r = p ? ipod_mkdir(p) : -1;
    free(p);
    if (r != 0) return -1;
  }
  return 0;
}

static void free_otg(ipod_device *dev) {
  for (size_t i = 0; i < dev->n_otg; i++) free(dev->otg[i].keys);
  free(dev->otg);
  dev->otg = NULL;
  dev->n_otg = 0;
}

/* Reads Play Counts and OTG files and maps their indexes to keys while the database still
 * has the order they were recorded against. */
static int read_device_records(ipod_device *dev, ipod_error *err) {
  ipod_db *db = dev->db;
  size_t len;
  dev->pc_bytes = ipod_file_exists(dev->pc_path) ? ipod_read_file(dev->pc_path, &len) : NULL;
  if (dev->pc_bytes) {
    dev->pc_len = len;
    dev->has_playcounts = 1;
    ipod_playcounts pc;
    ipod_error perr;
    if (ipod_playcounts_parse(dev->pc_bytes, len, &pc, &perr) == 0) {
      size_t n = pc.n < db->n_tracks ? pc.n : db->n_tracks;
      dev->plays = calloc(n ? n : 1, sizeof *dev->plays);
      dev->pending = calloc(n ? n : 1, sizeof *dev->pending);
      if (!dev->plays || !dev->pending) {
        ipod_playcounts_free(&pc);
        return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
      }
      int32_t tz = ipod_local_tz_offset();
      for (size_t i = 0; i < n; i++) {
        const ipod_playcount *e = &pc.entries[i];
        if (e->play_count == 0) continue;
        ipod_play *p = &dev->plays[dev->n_plays];
        ipod_track_key(db->tracks[i], p->key);
        p->count = e->play_count;
        p->last_played = ipod_mac_to_unix(e->last_played, tz);
        p->rating = e->rating;
        dev->pending[dev->n_plays].dbid = db->tracks[i]->dbid;
        dev->pending[dev->n_plays].count = e->play_count;
        dev->pending[dev->n_plays].last_played = e->last_played;
        dev->n_plays++;
      }
      ipod_playcounts_free(&pc);
    }
    /* A file we can't parse is still cleared at the next commit (it can't be valid for the
     * database we write), but yields no plays. */
  }

  ipod_otg otg;
  if (ipod_otg_read_all(dev->itunes_dir, &otg, NULL) == 0) {
    dev->otg = calloc(otg.n ? otg.n : 1, sizeof *dev->otg);
    if (!dev->otg) {
      ipod_otg_free(&otg);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < otg.n; i++) {
      ipod_otg_playlist *pl = &dev->otg[dev->n_otg];
      pl->keys = calloc(otg.lists[i].n, sizeof *pl->keys);
      if (!pl->keys) {
        ipod_otg_free(&otg);
        return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
      }
      for (size_t k = 0; k < otg.lists[i].n; k++) {
        uint32_t idx = otg.lists[i].indexes[k];
        if (idx < db->n_tracks) ipod_track_key(db->tracks[idx], pl->keys[pl->n++]);
      }
      snprintf(pl->name, sizeof pl->name, "On-The-Go %zu", dev->n_otg + 1);
      dev->n_otg++;
    }
    ipod_otg_free(&otg);
  }
  /* Whether any OTG file exists, even an empty one: they're all cleared at commit. */
  char *base = ipod_path_join(dev->itunes_dir, "OTGPlaylistInfo");
  dev->has_otg = dev->n_otg > 0 || (base && ipod_file_exists(base));
  free(base);
  return 0;
}

void ipod_device_close(ipod_device *dev) {
  if (!dev) return;
  free(dev->mount);
  free(dev->itunes_dir);
  free(dev->db_path);
  free(dev->kith_path);
  free(dev->pc_path);
  free(dev->backup_path);
  ipod_db_free(dev->db);
  ipod_kith_free(&dev->kith);
  ipod_artdb_free(dev->art);
  free(dev->plays);
  free(dev->pending);
  free(dev->pc_bytes);
  free_otg(dev);
  free(dev);
}

ipod_device *ipod_open(const char *mount, unsigned flags, ipod_error *err) {
  ipod_device *dev = calloc(1, sizeof *dev);
  if (!dev) {
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  dev->mount = ipod_strdup(mount);
  dev->itunes_dir = ipod_path_join(mount, "iPod_Control/iTunes");
  dev->db_path = ipod_path_join(mount, "iPod_Control/iTunes/iTunesDB");
  dev->kith_path = ipod_path_join(mount, "iPod_Control/kithsong.json");
  dev->pc_path = ipod_path_join(mount, "iPod_Control/iTunes/Play Counts");
  dev->backup_path = ipod_path_join(mount, "iPod_Control/iTunes/iTunesDB.kithsong-backup");
  if (!dev->mount || !dev->itunes_dir || !dev->db_path || !dev->kith_path || !dev->pc_path || !dev->backup_path) {
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    goto fail;
  }

  if (ipod_sysinfo_read(mount, &dev->si, NULL) != 0) memset(&dev->si, 0, sizeof dev->si);
  dev->model = dev->si.model;
  ipod_support support = ipod_model_support(dev->model, dev->si.model_num);
  if (support == IPOD_UNSUPPORTED_HASH) {
    ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "this iPod (%s) needs the iTunesDB hash", dev->si.model_num);
    goto fail;
  }
  if (support == IPOD_UNSUPPORTED_SHUFFLE) {
    ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "iPod shuffles aren't supported");
    goto fail;
  }
  if (support == IPOD_SUPPORT_UNKNOWN_MODEL && !(flags & IPOD_OPEN_ALLOW_UNKNOWN)) {
    ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "unknown iPod model '%s'", dev->si.model_num);
    goto fail;
  }
  dev->music_dirs = dev->model ? dev->model->music_dirs : count_music_dirs(mount);
  if (dev->music_dirs <= 0) dev->music_dirs = 20;

  if (ipod_file_exists(dev->db_path)) {
    if (db_file_hashed(dev->db_path)) {
      ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "this iTunesDB is hashed");
      goto fail;
    }
    dev->db = ipod_db_read(dev->db_path, err);
    if (!dev->db) goto fail;
  } else if (flags & IPOD_OPEN_CREATE) {
    if (make_layout(dev) != 0) {
      ipod_seterr(err, IPOD_ERR_IO, "can't create iPod_Control on %s", mount);
      goto fail;
    }
    dev->db = ipod_db_new("iPod");
    if (!dev->db) {
      ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
      goto fail;
    }
    dev->db_dirty = 1;
  } else {
    ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no iTunesDB on %s", mount);
    goto fail;
  }

  if (ipod_kith_read(dev->kith_path, &dev->kith, NULL) != 0) memset(&dev->kith, 0, sizeof dev->kith);
  dev->art = ipod_artdb_load(mount, dev->model ? dev->model->artwork : NULL, dev->model ? dev->model->n_artwork : 0, err);
  if (!dev->art) goto fail;
  if (read_device_records(dev, err) != 0) goto fail;
  return dev;
fail:
  ipod_device_close(dev);
  return NULL;
}

/* --- Info ------------------------------------------------------------------------------- */

int ipod_device_info_get(ipod_device *dev, ipod_device_info *info, ipod_error *err) {
  memset(info, 0, sizeof *info);
  info->sysinfo = dev->si;
  info->model = dev->model;
  info->generation = dev->model ? dev->model->generation : IPOD_GEN_UNKNOWN;
  info->name = ipod_db_name(dev->db);
  if (!ipod_sysinfo_stable_id(&dev->si, info->id, sizeof info->id))
    snprintf(info->id, sizeof info->id, "db:%016llx", (unsigned long long)dev->db->id);
  info->artwork = ipod_artdb_enabled(dev->art);
  info->max_sample_rate = 48000;
  info->max_bits = 16;
  info->max_channels = 2;
  return ipod_disk_space(dev->mount, &info->capacity, &info->free_bytes, err);
}

const char *ipod_device_mount(const ipod_device *dev) { return dev->mount; }
ipod_db *ipod_device_db(ipod_device *dev) { return dev->db; }
const ipod_kith_map *ipod_device_kith(const ipod_device *dev) { return &dev->kith; }

ipod_track *ipod_device_find(ipod_device *dev, const char *key) {
  uint64_t dbid;
  if (parse_key(key, &dbid) != 0) return NULL;
  return ipod_db_find_dbid(dev->db, dbid);
}

const ipod_kith_entry *ipod_device_kith_entry(const ipod_device *dev, const char *key) {
  return ipod_kith_find(&dev->kith, key);
}

char *ipod_device_track_path(ipod_device *dev, const char *key) {
  ipod_track *t = ipod_device_find(dev, key);
  if (!t || empty_str(t->location)) return NULL;
  char *rel = ipod_location_to_path(t->location);
  if (!rel) return NULL;
  char *p = ipod_path_join(dev->mount, rel);
  free(rel);
  return p;
}

/* --- Changing tracks ------------------------------------------------------------------------ */

static int apply_tags(ipod_track *t, const ipod_tags *g) {
  int r = 0;
  r |= ipod_set_str(&t->title, g->title);
  r |= ipod_set_str(&t->artist, g->artist);
  r |= ipod_set_str(&t->album, g->album);
  r |= ipod_set_str(&t->album_artist, g->album_artist);
  r |= ipod_set_str(&t->genre, g->genre);
  r |= ipod_set_str(&t->composer, g->composer);
  r |= ipod_set_str(&t->comment, g->comment);
  r |= ipod_set_str(&t->grouping, g->grouping);
  r |= ipod_set_str(&t->sort_title, g->sort_title);
  r |= ipod_set_str(&t->sort_artist, g->sort_artist);
  r |= ipod_set_str(&t->sort_album, g->sort_album);
  r |= ipod_set_str(&t->sort_album_artist, g->sort_album_artist);
  r |= ipod_set_str(&t->sort_composer, g->sort_composer);
  t->year = g->year > 0 ? (uint32_t)g->year : 0;
  t->track_number = g->track_number > 0 ? (uint32_t)g->track_number : 0;
  t->track_total = g->track_total > 0 ? (uint32_t)g->track_total : 0;
  t->disc_number = g->disc_number > 0 ? (uint32_t)g->disc_number : 0;
  t->disc_total = g->disc_total > 0 ? (uint32_t)g->disc_total : 0;
  t->compilation = g->compilation ? 1 : 0;
  t->bpm = g->bpm > 0 && g->bpm < 65536 ? (uint16_t)g->bpm : 0;
  return r ? IPOD_ERR_NOMEM : 0;
}

static int apply_cover(ipod_device *dev, ipod_track *t, const ipod_tags *g, ipod_error *err) {
  if (!ipod_artdb_enabled(dev->art)) {
    t->artwork_count = 0;
    t->artwork_size = 0;
    t->has_artwork = 2;
    return 0;
  }
  if (g && g->cover && g->cover_len) {
    int r = ipod_artdb_set(dev->art, t->dbid, g->cover, g->cover_len, err);
    if (r == 0) {
      t->artwork_count = 1;
      t->artwork_size = (uint32_t)g->cover_len;
      t->has_artwork = 1;
      return 0;
    }
    if (r != IPOD_ERR_FORMAT) return r;
    /* A picture that won't decode just means no cover. */
  }
  ipod_artdb_remove(dev->art, t->dbid);
  t->artwork_count = 0;
  t->artwork_size = 0;
  t->has_artwork = 2;
  return 0;
}

static const char *file_ext(const char *path, const char *codec) {
  const char *dot = strrchr(path, '.');
  const char *slash1 = strrchr(path, '/'), *slash2 = strrchr(path, '\\');
  const char *slash = slash1 > slash2 ? slash1 : slash2;
  if (dot && (!slash || dot > slash) && strlen(dot) <= 5) {
    static const char *ok[] = {".mp3", ".m4a", ".m4b", ".m4p", ".aac", ".wav", ".aif", ".aiff"};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
      size_t n = strlen(ok[i]);
      int same = strlen(dot) == n;
      for (size_t k = 0; same && k < n; k++) same = tolower((unsigned char)dot[k]) == ok[i][k];
      if (same) return ok[i];
    }
  }
  return codec && strcmp(codec, "mp3") == 0 ? ".mp3" : ".m4a";
}

/* A fresh relative path in iPod_Control/Music/Fnn/: four random capitals, as iTunes names
 * files, FAT-safe and short. */
static char *new_music_path(ipod_device *dev, const char *ext, ipod_error *err) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  for (int attempt = 0; attempt < 200; attempt++) {
    uint8_t rnd[8];
    ipod_random_bytes(rnd, sizeof rnd);
    int dir = (int)(rnd[0] % (unsigned)dev->music_dirs);
    int len = attempt < 100 ? 4 : 6;
    char name[8];
    for (int i = 0; i < len; i++) name[i] = alphabet[rnd[1 + i] % 26];
    name[len] = 0;
    char dirrel[32];
    snprintf(dirrel, sizeof dirrel, "iPod_Control/Music/F%02d", dir);
    char *dirabs = ipod_path_join(dev->mount, dirrel);
    if (!dirabs || ipod_mkdirs(dirabs) != 0) {
      free(dirabs);
      ipod_seterr(err, IPOD_ERR_IO, "can't create %s", dirrel);
      return NULL;
    }
    free(dirabs);
    char *rel = ipod_asprintf("%s/%s%s", dirrel, name, ext);
    char *abs = rel ? ipod_path_join(dev->mount, rel) : NULL;
    int taken = !abs || ipod_file_exists(abs);
    free(abs);
    if (!taken) return rel;
    free(rel);
  }
  ipod_seterr(err, IPOD_ERR_IO, "can't find a free file name");
  return NULL;
}

static void remove_track_file(ipod_device *dev, const ipod_track *t) {
  if (empty_str(t->location)) return;
  char *rel = ipod_location_to_path(t->location);
  char *abs = rel ? ipod_path_join(dev->mount, rel) : NULL;
  if (abs) ipod_remove(abs);
  free(rel);
  free(abs);
}

int ipod_device_put(ipod_device *dev, const char *src_path, const ipod_put_info *info,
                    char key_out[IPOD_KEY_LEN], ipod_error *err) {
  if (!info || !info->codec) return ipod_seterr(err, IPOD_ERR_ARG, "no codec");
  int64_t size = ipod_file_size(src_path);
  if (size < 0) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "can't read %s", src_path);
  if (size > (int64_t)UINT32_MAX) return ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "file over 4 GB");
  ipod_track *old = NULL;
  if (info->replace_key) {
    old = ipod_device_find(dev, info->replace_key);
    if (!old) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no track %s", info->replace_key);
  }
  uint64_t cap = 0, avail = 0;
  if (ipod_disk_space(dev->mount, &cap, &avail, NULL) == 0) {
    /* Room for the file plus a margin for the database and artwork. */
    if ((uint64_t)size + (2u << 20) > avail) return ipod_seterr(err, IPOD_ERR_FULL, "not enough space on the iPod");
  }

  char *rel = new_music_path(dev, file_ext(src_path, info->codec), err);
  if (!rel) return err ? err->code : IPOD_ERR_IO;
  char *abs = ipod_path_join(dev->mount, rel);
  if (!abs) {
    free(rel);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  int r = ipod_copy_file(src_path, abs, err);
  free(abs);
  if (r != 0) {
    free(rel);
    return r;
  }
  char *location = ipod_path_to_location(rel);
  free(rel);
  if (!location) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");

  ipod_track *t = old;
  if (old) {
    remove_track_file(dev, old);
  } else {
    t = ipod_track_new();
    if (!t) {
      free(location);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
  }
  free(t->location);
  t->location = location;

  int64_t now = (int64_t)time(NULL);
  int32_t tz = ipod_local_tz_offset();
  ipod_track_set_kind(t, info->codec, info->vbr);
  if (info->tags && apply_tags(t, info->tags) != 0) r = IPOD_ERR_NOMEM;
  t->size = (uint32_t)size;
  t->length_ms = info->length_ms;
  t->bitrate = info->bitrate_kbps;
  t->sample_rate = info->sample_rate;
  t->sample_count = info->sample_count;
  t->play_count = info->play_count;
  t->last_played = ipod_unix_to_mac(info->last_played, tz);
  t->rating = (uint8_t)(info->rating < 0 ? 0 : info->rating > 100 ? 100 : info->rating);
  t->time_modified = ipod_unix_to_mac(now, tz);
  if (!old) {
    t->time_added = ipod_unix_to_mac(info->added ? info->added : now, tz);
    if (dev->model && (dev->model->generation == IPOD_GEN_VIDEO_1 || dev->model->generation == IPOD_GEN_VIDEO_2))
      t->media_type = 1;
    if (ipod_db_add_track(dev->db, t) != 0) {
      remove_track_file(dev, t);
      ipod_track_free(t);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
  }
  dev->db_dirty = 1;
  if (r != 0) return ipod_seterr(err, r, "out of memory");
  r = apply_cover(dev, t, info->tags, err);
  if (r != 0) return r;

  char key[IPOD_KEY_LEN];
  ipod_track_key(t, key);
  if (key_out) memcpy(key_out, key, IPOD_KEY_LEN);
  if (info->track_uid) {
    if (ipod_kith_set(&dev->kith, key, info->track_uid, info->file_uid, info->format, info->sha256) != 0)
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    dev->kith_dirty = 1;
  } else if (ipod_kith_remove(&dev->kith, key) == 0) {
    dev->kith_dirty = 1;
  }
  return 0;
}

int ipod_device_update(ipod_device *dev, const char *key, const ipod_update_info *info, ipod_error *err) {
  ipod_track *t = ipod_device_find(dev, key);
  if (!t) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no track %s", key);
  int32_t tz = ipod_local_tz_offset();
  if (info->tags) {
    if (apply_tags(t, info->tags) != 0) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    int r = apply_cover(dev, t, info->tags, err);
    if (r != 0) return r;
  }
  if (info->set_plays) {
    t->play_count = info->play_count;
    t->last_played = ipod_unix_to_mac(info->last_played, tz);
  }
  if (info->set_rating) t->rating = (uint8_t)(info->rating < 0 ? 0 : info->rating > 100 ? 100 : info->rating);
  t->time_modified = ipod_unix_to_mac((int64_t)time(NULL), tz);
  dev->db_dirty = 1;
  return 0;
}

int ipod_device_remove(ipod_device *dev, const char *key, ipod_error *err) {
  ipod_track *t = ipod_device_find(dev, key);
  if (!t) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no track %s", key);
  remove_track_file(dev, t);
  ipod_artdb_remove(dev->art, t->dbid);
  ipod_db_remove_track(dev->db, t->id);
  if (ipod_kith_remove(&dev->kith, key) == 0) dev->kith_dirty = 1;
  dev->db_dirty = 1;
  return 0;
}

int ipod_device_set_playlist(ipod_device *dev, const char *name, const char *const *keys, size_t n,
                             ipod_error *err) {
  if (!name || !*name) return ipod_seterr(err, IPOD_ERR_ARG, "playlist without a name");
  ipod_playlist *pl = ipod_db_find_playlist(dev->db, name);
  if (!keys && n == 0) {
    if (pl) {
      ipod_db_remove_playlist(dev->db, pl);
      dev->db_dirty = 1;
    }
    return 0;
  }
  if (!pl) {
    pl = ipod_playlist_new(name);
    if (!pl || ipod_db_add_playlist(dev->db, pl) != 0) {
      ipod_playlist_free(pl);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
  }
  ipod_playlist_clear(pl);
  for (size_t i = 0; i < n; i++) {
    ipod_track *t = ipod_device_find(dev, keys[i]);
    if (t && ipod_playlist_add(pl, t->id) != 0) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  dev->db_dirty = 1;
  return 0;
}

/* --- Plays and On-The-Go ------------------------------------------------------------------------ */

const ipod_play *ipod_device_plays(const ipod_device *dev, size_t *n) {
  *n = dev->n_plays;
  return dev->plays;
}

const uint8_t *ipod_device_playcounts_bytes(const ipod_device *dev, size_t *len) {
  *len = dev->pc_len;
  return dev->pc_bytes;
}

const ipod_otg_playlist *ipod_device_otg_playlists(const ipod_device *dev, size_t *n) {
  *n = dev->n_otg;
  return dev->otg;
}

void ipod_device_done(ipod_device *dev, unsigned what) { dev->done |= what; }

/* Before a new database is written: whatever the iPod recorded and nobody took in goes into
 * the database itself, so it isn't lost when the files are cleared. */
static int fold_pending(ipod_device *dev) {
  if (dev->folded) return 0;
  if (dev->has_playcounts && !(dev->done & IPOD_DONE_PLAYS)) {
    for (size_t i = 0; i < dev->n_plays; i++) {
      ipod_track *t = ipod_db_find_dbid(dev->db, dev->pending[i].dbid);
      if (!t) continue;
      t->play_count += dev->pending[i].count;
      if (dev->pending[i].last_played > t->last_played) t->last_played = dev->pending[i].last_played;
    }
  }
  if (dev->has_otg && !(dev->done & IPOD_DONE_PLAYLISTS)) {
    for (size_t i = 0; i < dev->n_otg; i++) {
      const ipod_otg_playlist *o = &dev->otg[i];
      ipod_playlist *pl = ipod_playlist_new(o->name);
      if (!pl) return IPOD_ERR_NOMEM;
      for (size_t k = 0; k < o->n; k++) {
        ipod_track *t = ipod_device_find(dev, o->keys[k]);
        if (t && ipod_playlist_add(pl, t->id) != 0) {
          ipod_playlist_free(pl);
          return IPOD_ERR_NOMEM;
        }
      }
      if (ipod_db_add_playlist(dev->db, pl) != 0) {
        ipod_playlist_free(pl);
        return IPOD_ERR_NOMEM;
      }
    }
  }
  dev->folded = 1;
  return 0;
}

static void clear_records(ipod_device *dev, int plays, int otg) {
  if (plays && dev->has_playcounts) {
    ipod_remove(dev->pc_path);
    dev->has_playcounts = 0;
    dev->n_plays = 0;
    free(dev->pc_bytes);
    dev->pc_bytes = NULL;
    dev->pc_len = 0;
  }
  if (otg && dev->has_otg) {
    ipod_otg_clear(dev->itunes_dir);
    dev->has_otg = 0;
    free_otg(dev);
  }
}

int ipod_device_commit(ipod_device *dev, ipod_error *err) {
  int r;
  if (ipod_artdb_dirty(dev->art)) dev->db_dirty = 1;
  if (dev->db_dirty) {
    if ((r = fold_pending(dev)) != 0) return ipod_seterr(err, r, "out of memory");
    if ((r = ipod_artdb_write(dev->art, dev->db, err)) != 0) return r;
    const char *backup = dev->backed_up ? NULL : dev->backup_path;
    if ((r = ipod_db_write(dev->db, dev->db_path, backup, err)) != 0) return r;
    dev->backed_up = 1;
    dev->db_dirty = 0;
    /* The new database's order no longer matches what the iPod recorded. */
    clear_records(dev, 1, 1);
  } else {
    clear_records(dev, (dev->done & IPOD_DONE_PLAYS) != 0, (dev->done & IPOD_DONE_PLAYLISTS) != 0);
  }
  if (dev->kith_dirty) {
    if ((r = ipod_kith_write(dev->kith_path, &dev->kith, err)) != 0) return r;
    dev->kith_dirty = 0;
  }
  return 0;
}
