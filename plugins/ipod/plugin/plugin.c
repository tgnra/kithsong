/* The iPod sync plugin (docs/plugins.md, "The iPod plugin"): kithipod behind the plugin ABI.
 * One device target, "ipod", for click-wheel iPods without the iTunesDB hash, and one
 * encoder for ALAC and MP3. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "kithipod/device.h"
#include "plugin_internal.h"
#include "util.h" /* kithipod's: ipod_fopen() */

#ifndef KITH_IPOD_VERSION
#define KITH_IPOD_VERSION "0.0.0"
#endif

const kith_host *ipod_plugin_host;

void ipod_plugin_log(kith_log_level level, const char *fmt, ...) {
  if (!ipod_plugin_host) return;
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  ipod_plugin_host->log(ipod_plugin_host, level, msg);
}

/* --- Strings ---------------------------------------------------------------------------- */

static const ipod_plugin_strings strings_en = {
    "iPod sync",
    "iPod",
    "Album art",
    "Covers for iPods with a colour screen (iPod photo, nano, video). Off syncs a little "
    "faster.",
    "V0 (about 245 kbps)",
    "V2 (about 190 kbps)",
    "320 kbps",
    "256 kbps",
    "192 kbps",
    "128 kbps",
};

static const struct {
  const char *language; /* a BCP 47 primary tag */
  const ipod_plugin_strings *strings;
} languages[] = {{"en", &strings_en}};

const ipod_plugin_strings *ipod_plugin_strings_for(const char *language) {
  if (language) {
    for (size_t i = 0; i < sizeof languages / sizeof languages[0]; i++) {
      size_t n = strlen(languages[i].language);
      if (strncmp(language, languages[i].language, n) == 0 && (language[n] == 0 || language[n] == '-'))
        return languages[i].strings;
    }
  }
  return &strings_en;
}

/* --- Helpers ------------------------------------------------------------------------------ */

static int from_ipod(int code) {
  switch (code) {
  case IPOD_OK: return KITH_OK;
  case IPOD_ERR_NOMEM: return KITH_ERR_NOMEM;
  case IPOD_ERR_FULL: return KITH_ERR_FULL;
  case IPOD_ERR_NOT_FOUND: return KITH_ERR_NOT_FOUND;
  case IPOD_ERR_FORMAT: return KITH_ERR_FORMAT;
  case IPOD_ERR_UNSUPPORTED: return KITH_ERR_UNSUPPORTED;
  case IPOD_ERR_ARG: return KITH_ERR_INVALID_ARG;
  default: return KITH_ERR_IO;
  }
}

static void copy_str(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src ? src : "");
}

/* A hex SHA-256 of tag, the records nonce and data: what identifies a set of plays or
 * playlists ("" when there's no data). */
static void token_of(const char *tag, const char *nonce, const uint8_t *data, size_t len,
                     char out[KITH_SHA256_LEN]) {
  out[0] = 0;
  if (!data || !len) return;
  unsigned char digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const unsigned char *)tag, strlen(tag) + 1);
  mbedtls_sha256_update(&ctx, (const unsigned char *)nonce, strlen(nonce) + 1);
  mbedtls_sha256_update(&ctx, data, len);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

/* --- The target ----------------------------------------------------------------------------- */

struct kith_target {
  ipod_device *dev;
  int artwork;  /* the "artwork" option */
  char id[64];
  char name[256];
  char model[128];
  kith_device_format formats[3];
};

static int option_on(const char *key, int def) {
  char *v = ipod_plugin_host->settings_get(ipod_plugin_host, key);
  int on = v ? strcmp(v, "0") != 0 : def;
  ipod_plugin_host->free(v);
  return on;
}

static int t_probe(const char *mount_path) {
  if (!mount_path) return 0;
  ipod_sysinfo si;
  int r = ipod_probe(mount_path, &si);
  if (r == IPOD_SUPPORTED || r == IPOD_SUPPORT_UNKNOWN_MODEL) return 1;
  if (r == IPOD_UNSUPPORTED_HASH)
    ipod_plugin_log(KITH_LOG_INFO, "%s is an iPod (%s) that needs the iTunesDB hash, which isn't supported",
                    mount_path, si.model_num);
  else if (r == IPOD_UNSUPPORTED_SHUFFLE)
    ipod_plugin_log(KITH_LOG_INFO, "%s is an iPod shuffle, which isn't supported", mount_path);
  return 0;
}

static int t_open(const char *mount_path, kith_target **out, kith_device_info *info) {
  if (!mount_path || !out || !info) return KITH_ERR_INVALID_ARG;
  *out = NULL;
  kith_target *t = calloc(1, sizeof *t);
  if (!t) return KITH_ERR_NOMEM;
  ipod_error err = {0};
  /* Models missing from the table are opened too: the database hash, which is what rules a
   * model out, is checked in the database itself. */
  t->dev = ipod_open(mount_path, IPOD_OPEN_ALLOW_UNKNOWN, &err);
  if (!t->dev) {
    ipod_plugin_log(KITH_LOG_WARNING, "can't open the iPod at %s: %s", mount_path, err.message);
    free(t);
    return from_ipod(err.code);
  }
  ipod_device_info di;
  int r = ipod_device_info_get(t->dev, &di, &err);
  if (r != 0) {
    ipod_plugin_log(KITH_LOG_WARNING, "can't read the iPod at %s: %s", mount_path, err.message);
    ipod_device_close(t->dev);
    free(t);
    return from_ipod(r);
  }
  /* The stable id ("guid:...", "serial:..." or "db:..."), in the characters core takes. */
  size_t k = 0;
  for (const char *p = di.id; *p && k < sizeof t->id - 1 && k < 63; p++)
    t->id[k++] = isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_' ? *p : '_';
  t->id[k] = 0;
  copy_str(t->name, sizeof t->name, di.name && *di.name ? di.name : "iPod");
  copy_str(t->model, sizeof t->model, di.model ? di.model->name : "iPod");
  t->artwork = option_on("artwork", 1);

  int rate = di.max_sample_rate ? (int)di.max_sample_rate : 48000;
  int channels = di.max_channels ? di.max_channels : 2;
  t->formats[0] = (kith_device_format){"alac", rate, di.max_bits ? di.max_bits : 16, channels, 0};
  t->formats[1] = (kith_device_format){"mp3", rate, 0, channels, 320};
  t->formats[2] = (kith_device_format){"aac", rate, 0, channels, 320};

  memset(info, 0, sizeof *info);
  info->id = t->id;
  info->name = t->name;
  info->model = t->model;
  info->capacity_bytes = (int64_t)di.capacity;
  info->free_bytes = (int64_t)di.free_bytes;
  info->formats = t->formats;
  info->format_count = 3;
  info->lossless_format = "alac";
  info->lossless_setting = "";
  info->lossy_format = "mp3";
  info->lossy_setting = "v0";
  info->has_playlists = 1;
  *out = t;
  return KITH_OK;
}

/* The format of a track Kithsong didn't record, from the database. */
static const char *format_of(const ipod_track *t) {
  if (t->filetype == IPOD_FILETYPE_MP3) return "mp3";
  if (t->filetype == IPOD_FILETYPE_M4A || t->filetype == IPOD_FILETYPE_M4P)
    return t->filetype_desc && strstr(t->filetype_desc, "Lossless") ? "alac" : "aac";
  if (t->filetype == IPOD_FILETYPE_WAV) return "wav";
  return "";
}

static int t_list(kith_target *t, kith_track_fn each, void *ctx) {
  if (!t || !each) return KITH_ERR_INVALID_ARG;
  ipod_db *db = ipod_device_db(t->dev);
  int32_t tz = ipod_local_tz_offset();
  for (size_t i = 0; i < db->n_tracks; i++) {
    const ipod_track *tr = db->tracks[i];
    char key[IPOD_KEY_LEN];
    ipod_track_key(tr, key);
    if (ipod_device_removal_deferred(t->dev, key)) continue;
    const ipod_kith_entry *e = ipod_device_kith_entry(t->dev, key);
    int ours = e && e->track_uid && *e->track_uid;
    kith_device_track d;
    memset(&d, 0, sizeof d);
    d.key = key;
    d.track_uid = ours ? e->track_uid : NULL;
    d.file_uid = ours ? e->file_uid : NULL;
    d.setting = ours && e->setting ? e->setting : "";
    d.sha256 = ours ? e->sha256 : NULL;
    d.format = ours && e->format && *e->format ? e->format : format_of(tr);
    d.bitrate_kbps = (int)tr->bitrate;
    d.size_bytes = tr->size;
    d.title = tr->title;
    d.artist = tr->artist;
    d.album = tr->album;
    d.duration_ms = (int)tr->length_ms;
    d.play_count = (int)tr->play_count;
    d.last_played_ms = tr->last_played ? ipod_mac_to_unix(tr->last_played, tz) * 1000 : 0;
    if (each(ctx, &d)) break;
  }
  return KITH_OK;
}

/* --- Reading what a file is, for the database ------------------------------------------- */

static uint8_t *read_head(const char *path, size_t want, size_t *len) {
  *len = 0;
  FILE *f = ipod_fopen(path, "rb"); /* UTF-8 paths, on Windows too */
  if (!f) return NULL;
  uint8_t *buf = malloc(want);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  *len = fread(buf, 1, want, f);
  fclose(f);
  return buf;
}

/* An MP3's sample rate, and whether it's VBR (a Xing or VBRI header in its first frame). */
static void mp3_probe(const uint8_t *d, size_t n, uint32_t *rate, int *vbr) {
  size_t p = 0;
  if (n >= 10 && memcmp(d, "ID3", 3) == 0) {
    size_t size = ((size_t)(d[6] & 0x7f) << 21) | ((size_t)(d[7] & 0x7f) << 14) | ((size_t)(d[8] & 0x7f) << 7) |
                  (size_t)(d[9] & 0x7f);
    p = 10 + size + ((d[5] & 0x10) ? 10 : 0);
  }
  static const uint32_t rates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
  for (; p + 4 <= n; p++) {
    if (d[p] != 0xff || (d[p + 1] & 0xe0) != 0xe0) continue;
    int version = (d[p + 1] >> 3) & 3; /* 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5 */
    int layer = (d[p + 1] >> 1) & 3;   /* 1 = layer III */
    int rate_idx = (d[p + 2] >> 2) & 3;
    int brate_idx = d[p + 2] >> 4;
    if (version == 1 || layer != 1 || rate_idx == 3 || brate_idx == 15 || brate_idx == 0) continue;
    *rate = rates[version == 3 ? 0 : version == 2 ? 1 : 2][rate_idx];
    size_t end = p + 200 < n ? p + 200 : n;
    for (size_t q = p + 4; q + 4 <= end; q++)
      if (memcmp(d + q, "Xing", 4) == 0 || memcmp(d + q, "VBRI", 4) == 0) *vbr = 1;
    return;
  }
}

/* An M4A's sample rate (its media timescale) and length, from mdhd. */
static void m4a_probe(const uint8_t *d, size_t n, uint32_t *rate, uint64_t *samples) {
  for (size_t p = 4; p + 4 <= n; p++) {
    if (memcmp(d + p, "mdhd", 4) != 0) continue;
    size_t b = p + 4;
    if (b + 1 > n) return;
    int v1 = d[b] == 1;
    size_t ts = b + 4 + (v1 ? 16 : 8);
    if (ts + (v1 ? 12 : 8) > n) return;
    uint32_t scale = ((uint32_t)d[ts] << 24) | ((uint32_t)d[ts + 1] << 16) | ((uint32_t)d[ts + 2] << 8) | d[ts + 3];
    uint64_t dur = 0;
    for (int i = 0; i < (v1 ? 8 : 4); i++) dur = dur << 8 | d[ts + 4 + i];
    *rate = scale;
    *samples = dur;
    return;
  }
}

static void to_ipod_tags(const kith_tags *k, ipod_tags *t) {
  memset(t, 0, sizeof *t);
  t->title = k->title;
  t->artist = k->artist;
  t->album = k->album;
  t->album_artist = k->album_artist;
  t->genre = k->genre;
  t->composer = k->composer;
  t->year = k->year;
  t->track_number = k->track_no;
  t->track_total = k->track_total;
  t->disc_number = k->disc_no;
  t->disc_total = k->disc_total;
  t->compilation = k->compilation ? 1 : 0;
}

static int t_put(kith_target *t, const kith_track *track, const char *file_path, char key[KITH_KEY_LEN],
                 const kith_job *job) {
  if (!t || !track || !file_path || !key || !track->format) return KITH_ERR_INVALID_ARG;
  const char *codec = NULL;
  if (!strcmp(track->format, "alac")) codec = "alac";
  else if (!strcmp(track->format, "mp3")) codec = "mp3";
  else if (!strcmp(track->format, "aac")) codec = "aac";
  if (!codec) return KITH_ERR_UNSUPPORTED;
  if (ipod_plugin_host->cancelled(job)) return KITH_ERR_CANCELLED;
  const char *setting = track->setting ? track->setting : "";
  key[KITH_KEY_LEN - 1] = 0;
  const char *replace = key[0] ? key : NULL;
  ipod_track *old = replace ? ipod_device_find(t->dev, replace) : NULL;
  if (replace && !old) return KITH_ERR_NOT_FOUND;

  ipod_tags tags;
  to_ipod_tags(&track->tags, &tags);
  if (t->artwork && track->cover.data && track->cover.size) {
    tags.cover = track->cover.data;
    tags.cover_len = track->cover.size;
  }
  ipod_put_info pi;
  memset(&pi, 0, sizeof pi);
  pi.tags = &tags;
  pi.codec = codec;
  uint32_t rate = (uint32_t)(track->sample_rate > 0 ? track->sample_rate : 0);
  uint64_t samples = 0;
  size_t n = 0;
  uint8_t *head = read_head(file_path, 64 * 1024, &n);
  if (head) {
    uint32_t r = 0;
    if (!strcmp(codec, "mp3")) {
      mp3_probe(head, n, &r, &pi.vbr);
    } else {
      m4a_probe(head, n, &r, &samples);
    }
    if (!rate) rate = r;
    free(head);
  }
  if (!strcmp(codec, "mp3") && setting[0] == 'v') pi.vbr = 1;
  pi.sample_rate = rate;
  pi.sample_count = strcmp(codec, "mp3") ? samples : 0;
  pi.length_ms = track->duration_ms > 0 ? (uint32_t)track->duration_ms
                 : rate && samples ? (uint32_t)(samples * 1000 / rate) : 0;
  if (track->bitrate_kbps > 0) {
    pi.bitrate_kbps = (uint32_t)track->bitrate_kbps;
  } else if (pi.length_ms && track->size_bytes > 0) {
    pi.bitrate_kbps = (uint32_t)((uint64_t)track->size_bytes * 8 / pi.length_ms);
  }
  pi.play_count = track->play_count > 0 ? (uint32_t)track->play_count : 0;
  pi.last_played = track->last_played_ms > 0 ? track->last_played_ms / 1000 : 0;
  pi.rating = old ? old->rating : 0; /* Kithsong has no ratings: keep the iPod's */
  pi.track_uid = track->track_uid[0] ? track->track_uid : NULL;
  pi.file_uid = track->file_uid[0] ? track->file_uid : NULL;
  pi.format = track->format;
  pi.setting = setting;
  pi.sha256 = track->sha256[0] ? track->sha256 : NULL;
  pi.replace_key = replace;

  char out_key[IPOD_KEY_LEN];
  ipod_error err = {0};
  int r = ipod_device_put(t->dev, file_path, &pi, out_key, &err);
  if (r != 0) {
    if (r != IPOD_ERR_FULL) ipod_plugin_log(KITH_LOG_ERROR, "can't put %s: %s", file_path, err.message);
    return from_ipod(r);
  }
  copy_str(key, KITH_KEY_LEN, out_key);
  return KITH_OK;
}

static int t_update(kith_target *t, const char *key, const kith_track *track) {
  if (!t || !key || !track) return KITH_ERR_INVALID_ARG;
  ipod_tags tags;
  to_ipod_tags(&track->tags, &tags);
  ipod_update_info ui;
  memset(&ui, 0, sizeof ui);
  ui.tags = &tags;
  ui.keep_cover = 1;
  ui.set_plays = 1;
  ui.play_count = track->play_count > 0 ? (uint32_t)track->play_count : 0;
  ui.last_played = track->last_played_ms > 0 ? track->last_played_ms / 1000 : 0;
  ipod_error err = {0};
  int r = ipod_device_update(t->dev, key, &ui, &err);
  if (r != 0 && r != IPOD_ERR_NOT_FOUND) ipod_plugin_log(KITH_LOG_ERROR, "can't update %s: %s", key, err.message);
  return from_ipod(r);
}

static int t_remove(kith_target *t, const char *key) {
  if (!t || !key) return KITH_ERR_INVALID_ARG;
  ipod_error err = {0};
  int r = ipod_device_remove(t->dev, key, &err);
  if (r != 0 && r != IPOD_ERR_NOT_FOUND) ipod_plugin_log(KITH_LOG_ERROR, "can't remove %s: %s", key, err.message);
  return from_ipod(r);
}

static char *t_file_path(kith_target *t, const char *key) {
  if (!t || !key) return NULL;
  char *p = ipod_device_track_path(t->dev, key);
#if defined(_WIN32)
  for (char *c = p; c && *c; c++)
    if (*c == '/') *c = '\\';
#endif
  return p;
}

static int t_plays_read(kith_target *t, char token[KITH_SHA256_LEN], kith_play_fn each, void *ctx) {
  if (!t || !token) return KITH_ERR_INVALID_ARG;
  size_t len = 0;
  const uint8_t *bytes = ipod_device_playcounts_bytes(t->dev, &len);
  token_of("plays", ipod_device_records_nonce(t->dev), bytes, len, token);
  if (!each || !bytes) return KITH_OK;
  size_t n = 0;
  const ipod_play *plays = ipod_device_plays(t->dev, &n);
  for (size_t i = 0; i < n; i++) {
    kith_device_play p;
    p.key = plays[i].key;
    p.count = (int)plays[i].count;
    p.last_played_ms = plays[i].last_played > 0 ? plays[i].last_played * 1000 : 0;
    if (p.count > 0 && each(ctx, &p)) break;
  }
  return KITH_OK;
}

static int t_playlists_read(kith_target *t, char token[KITH_SHA256_LEN], kith_playlist_fn each, void *ctx) {
  if (!t || !token) return KITH_ERR_INVALID_ARG;
  size_t len = 0;
  const uint8_t *bytes = ipod_device_otg_bytes(t->dev, &len);
  token_of("playlists", ipod_device_records_nonce(t->dev), bytes, len, token);
  if (!each || !bytes) return KITH_OK;
  size_t n = 0;
  const ipod_otg_playlist *lists = ipod_device_otg_playlists(t->dev, &n);
  for (size_t i = 0; i < n; i++) {
    const char **keys = lists[i].n ? malloc(lists[i].n * sizeof *keys) : NULL;
    if (lists[i].n && !keys) return KITH_ERR_NOMEM;
    for (size_t k = 0; k < lists[i].n; k++) keys[k] = lists[i].keys[k];
    kith_device_playlist pl;
    pl.name = NULL; /* the iPod doesn't name them */
    pl.keys = keys;
    pl.count = (int)lists[i].n;
    int stop = each(ctx, &pl);
    free(keys);
    if (stop) break;
  }
  return KITH_OK;
}

static int t_playlists_write(kith_target *t, const kith_playlist *lists, int count) {
  if (!t || count < 0 || (count && !lists)) return KITH_ERR_INVALID_ARG;
  ipod_kith_playlist_in *in = count ? calloc((size_t)count, sizeof *in) : NULL;
  if (count && !in) return KITH_ERR_NOMEM;
  for (int i = 0; i < count; i++) {
    in[i].uid = lists[i].uid;
    in[i].name = lists[i].name;
    in[i].keys = lists[i].keys;
    in[i].n = lists[i].count > 0 ? (size_t)lists[i].count : 0;
  }
  ipod_error err = {0};
  int r = ipod_device_set_kith_playlists(t->dev, in, (size_t)count, &err);
  free(in);
  if (r != 0) ipod_plugin_log(KITH_LOG_ERROR, "can't write playlists: %s", err.message);
  return from_ipod(r);
}

static int t_device_done(kith_target *t, unsigned what) {
  if (!t) return KITH_ERR_INVALID_ARG;
  unsigned w = 0;
  if (what & KITH_DONE_PLAYS) w |= IPOD_DONE_PLAYS;
  if (what & KITH_DONE_PLAYLISTS) w |= IPOD_DONE_PLAYLISTS;
  ipod_error err = {0};
  int r = ipod_device_done(t->dev, w, &err);
  if (r != 0) ipod_plugin_log(KITH_LOG_ERROR, "%s", err.message);
  return from_ipod(r);
}

static int t_commit(kith_target *t) {
  if (!t) return KITH_ERR_INVALID_ARG;
  ipod_error err = {0};
  int r = ipod_device_commit(t->dev, &err);
  if (r != 0) ipod_plugin_log(KITH_LOG_ERROR, "can't write the iPod's database: %s", err.message);
  return from_ipod(r);
}

static void t_close(kith_target *t) {
  if (!t) return;
  ipod_device_close(t->dev);
  free(t);
}

/* --- The plugin ---------------------------------------------------------------------------- */

static void plugin_free(void *p) { free(p); }

static kith_option options[1];
static kith_device_target targets[1];
static kith_encoder encoders[1];
static kith_plugin plugin;

KITH_PLUGIN_EXPORT const kith_plugin *kith_plugin_init(const kith_host *h) {
  if (!h || h->abi != KITH_PLUGIN_ABI || h->size != (int)sizeof(kith_host)) return NULL;
  ipod_plugin_host = h;
  const ipod_plugin_strings *s = ipod_plugin_strings_for(h->string ? h->string("language") : NULL);

  memset(options, 0, sizeof options);
  options[0].key = "artwork";
  options[0].kind = KITH_OPTION_SWITCH;
  options[0].label = s->artwork_label;
  options[0].description = s->artwork_desc;
  options[0].def = "1";

  memset(targets, 0, sizeof targets);
  targets[0].id = "ipod";
  targets[0].name = s->target_name;
  targets[0].probe = t_probe;
  targets[0].open = t_open;
  targets[0].list = t_list;
  targets[0].put = t_put;
  targets[0].update = t_update;
  targets[0].remove = t_remove;
  targets[0].file_path = t_file_path;
  targets[0].plays_read = t_plays_read;
  targets[0].playlists_read = t_playlists_read;
  targets[0].playlists_write = t_playlists_write;
  targets[0].device_done = t_device_done;
  targets[0].commit = t_commit;
  targets[0].close = t_close;
  targets[0].eject = NULL; /* the OS's own eject is enough: every write is flushed as it's made */

  ipod_plugin_encoder_init(s, &encoders[0]);

  memset(&plugin, 0, sizeof plugin);
  plugin.abi = KITH_PLUGIN_ABI;
  plugin.size = (int)sizeof plugin;
  plugin.id = "ng.kithso.ipod";
  plugin.name = s->plugin_name;
  plugin.version = KITH_IPOD_VERSION;
  plugin.free = plugin_free;
  plugin.options = options;
  plugin.option_count = 1;
  plugin.device_targets = targets;
  plugin.device_target_count = 1;
  plugin.encoders = encoders;
  plugin.encoder_count = 1;
  return &plugin;
}
