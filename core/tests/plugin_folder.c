/* A test plugin, loaded by the tests through the real loader: a folder target and a WAV
 * encoder. It includes kithcore/plugin.h alone and doesn't link kithcore, as a real plugin
 * mustn't.
 *
 * A device is a folder with a file KITHTEST in it, which holds "key=value" lines:
 *   id, name          the device's stable id and name
 *   capacity          bytes; free space is that less what's in Music/
 *   max_rate, max_bits, max_channels   limits for the wav format
 *   formats           what it plays, comma-separated (default "wav,mp3,lofi")
 * Tracks are Music/<key>.<ext>, and manifest.tsv lists them (one tab-separated line each:
 * key, track_uid, file_uid, setting, sha256, format, kbps, size, title, artist, album,
 * duration_ms, play_count, last_played_ms). A track with an empty track_uid is one
 * "Kithsong didn't write": tests write such lines themselves.
 *
 * Faked plays and playlists, written by the tests: plays.tsv (key, count, last_played_ms per
 * line) and otg.tsv (name, then keys separated by commas, per line). device_done() removes
 * them at the next commit(). playlists_write() writes playlists.tsv (uid, name, keys).
 * Every commit() appends a line to commits.log.
 *
 * Built three times: as is, with TEST_PLUGIN_BAD_ABI (it claims another ABI) and with
 * TEST_PLUGIN_NULL_INIT (its init fails). */
#include "kithcore/plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define SEP "\\"
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0777)
#define SEP "/"
#endif

static const kith_host *host;

static char *dup_str(const char *s) {
  if (!s) s = "";
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d) memcpy(d, s, n);
  return d;
}

static void plugin_free(void *p) {
  free(p);
}

/* --- The WAV encoder --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, unsigned long v) {
  put16(f, (unsigned)(v & 0xffff));
  put16(f, (unsigned)(v >> 16 & 0xffff));
}

/* Writes 16-bit (or 24-bit, if the source is) PCM WAV, whatever the setting. */
static int encode(kith_source *src, const char *format, const char *setting, const char *out_path,
                  const kith_job *job) {
  (void)format;
  (void)setting;
  kith_source_info info;
  if (host->decode_info(src, &info) != KITH_OK) return KITH_ERR_IO;
  int bits = info.format.bits > 16 ? 24 : 16;
  kith_pcm_format want = {0};
  want.sample = bits == 24 ? KITH_SAMPLE_S32 : KITH_SAMPLE_S16;
  want.bits = bits;
  if (host->decode_set_format(src, &want) != KITH_OK) return KITH_ERR_IO;
  if (host->decode_info(src, &info) != KITH_OK) return KITH_ERR_IO;
  int ch = info.format.channels, rate = info.format.sample_rate, bytes = bits / 8;
  FILE *f = fopen(out_path, "wb");
  if (!f) return KITH_ERR_IO;
  fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, (unsigned)ch);
  put32(f, (unsigned long)rate);
  put32(f, (unsigned long)rate * (unsigned long)ch * (unsigned long)bytes);
  put16(f, (unsigned)(ch * bytes));
  put16(f, (unsigned)bits);
  fwrite("data\0\0\0\0", 1, 8, f);
  int32_t buf[4096 * 8];
  unsigned long data = 0;
  int64_t got;
  int rc = KITH_OK;
  if (ch < 1 || ch > 8) {
    fclose(f);
    return KITH_ERR_FORMAT;
  }
  while ((got = host->decode_read(src, buf, 4096)) > 0) {
    if (host->cancelled(job)) {
      rc = KITH_ERR_CANCELLED;
      break;
    }
    int64_t samples = got * ch;
    for (int64_t i = 0; i < samples; i++) {
      if (bits == 16) {
        put16(f, (unsigned)(uint16_t)((int16_t *)buf)[i]);
      } else {
        uint32_t v = (uint32_t)buf[i] >> 8;
        fputc((int)(v & 0xff), f);
        fputc((int)(v >> 8 & 0xff), f);
        fputc((int)(v >> 16 & 0xff), f);
      }
    }
    data += (unsigned long)(samples * bytes);
  }
  if (got < 0) rc = KITH_ERR_IO;
  fseek(f, 4, SEEK_SET);
  put32(f, 36 + data);
  fseek(f, 40, SEEK_SET);
  put32(f, data);
  if (fclose(f) != 0 && rc == KITH_OK) rc = KITH_ERR_IO;
  return rc;
}

static const kith_encoder_setting lofi_settings[] = {
  {"q1", "Quality 1", 128},
  {"q2", "Quality 2", 64},
};

static const kith_encoder_format formats[] = {
  {"wav", "WAV", "wav", 1, NULL, 0, 0, {0, 0, 0, KITH_SAMPLE_DEFAULT, 0, 0, 0}},
  /* Pretends to be lossy: WAV again, at most 16 bits and stereo, for the lossy paths. */
  {"lofi", "Lo-fi", "lofi", 0, lofi_settings, 2, 0, {0, 0, 16, KITH_SAMPLE_S16, 0, 2, 0}},
};

static const kith_encoder encoders[] = {{formats, 2, encode}};

/* --- The folder target --- */

typedef struct track {
  char key[KITH_KEY_LEN];
  char track_uid[KITH_UID_LEN], file_uid[KITH_UID_LEN];
  char setting[32], sha[KITH_SHA256_LEN], format[16];
  int kbps;
  long long size;
  char *title, *artist, *album;
  int duration_ms, play_count;
  long long last_played_ms;
} track;

struct kith_target {
  char *root;
  char id[64], name[128], formats_spec[128];
  long long capacity;
  int max_rate, max_bits, max_channels;
  kith_device_format formats[4];
  int nformats;
  kith_device_info info;
  track *tracks;
  int count, cap;
  int next_key;
  unsigned done_pending;
};

static char *path_of(const char *root, const char *name) {
  size_t n = strlen(root) + strlen(name) + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s" SEP "%s", root, name);
  return p;
}

static int file_exists(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f) fclose(f);
  return f != NULL;
}

static int probe(const char *mount_path) {
  char *p = path_of(mount_path, "KITHTEST");
  int ok = p && file_exists(p);
  free(p);
  return ok;
}

/* The next tab-separated field of *s, NUL-terminated in place. */
static char *field(char **s) {
  char *start = *s;
  if (!start) return (char *)"";
  char *tab = strchr(start, '\t');
  if (tab) {
    *tab = '\0';
    *s = tab + 1;
  } else {
    char *nl = strpbrk(start, "\r\n");
    if (nl) *nl = '\0';
    *s = NULL;
  }
  return start;
}

static void track_clear(track *t) {
  free(t->title);
  free(t->artist);
  free(t->album);
}

static track *add_track(kith_target *t) {
  if (t->count == t->cap) {
    int cap = t->cap ? t->cap * 2 : 16;
    track *more = realloc(t->tracks, (size_t)cap * sizeof *more);
    if (!more) return NULL;
    t->tracks = more;
    t->cap = cap;
  }
  track *tr = &t->tracks[t->count++];
  memset(tr, 0, sizeof *tr);
  return tr;
}

static void copy_to(char *dst, size_t cap, const char *src) {
  snprintf(dst, cap, "%s", src ? src : "");
}

static int read_manifest(kith_target *t) {
  char *p = path_of(t->root, "manifest.tsv");
  FILE *f = p ? fopen(p, "rb") : NULL;
  free(p);
  if (!f) return KITH_OK;
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    char *s = line;
    if (!*s || *s == '\n') continue;
    track *tr = add_track(t);
    if (!tr) break;
    copy_to(tr->key, sizeof tr->key, field(&s));
    copy_to(tr->track_uid, sizeof tr->track_uid, field(&s));
    copy_to(tr->file_uid, sizeof tr->file_uid, field(&s));
    copy_to(tr->setting, sizeof tr->setting, field(&s));
    copy_to(tr->sha, sizeof tr->sha, field(&s));
    copy_to(tr->format, sizeof tr->format, field(&s));
    tr->kbps = atoi(field(&s));
    tr->size = atoll(field(&s));
    tr->title = dup_str(field(&s));
    tr->artist = dup_str(field(&s));
    tr->album = dup_str(field(&s));
    tr->duration_ms = atoi(field(&s));
    tr->play_count = atoi(field(&s));
    tr->last_played_ms = atoll(field(&s));
    int n = 0;
    if (sscanf(tr->key, "T%d", &n) == 1 && n >= t->next_key) t->next_key = n + 1;
  }
  fclose(f);
  return KITH_OK;
}

static int write_manifest(kith_target *t) {
  char *p = path_of(t->root, "manifest.tsv"), *tmp = path_of(t->root, "manifest.tmp");
  FILE *f = tmp ? fopen(tmp, "wb") : NULL;
  int rc = f ? KITH_OK : KITH_ERR_IO;
  for (int i = 0; f && i < t->count; i++) {
    track *tr = &t->tracks[i];
    fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\t%d\t%lld\t%s\t%s\t%s\t%d\t%d\t%lld\n", tr->key,
            tr->track_uid, tr->file_uid, tr->setting, tr->sha, tr->format, tr->kbps, tr->size,
            tr->title ? tr->title : "", tr->artist ? tr->artist : "", tr->album ? tr->album : "",
            tr->duration_ms, tr->play_count, tr->last_played_ms);
  }
  if (f && fclose(f) != 0) rc = KITH_ERR_IO;
  if (rc == KITH_OK) {
    remove(p);
    if (rename(tmp, p) != 0) rc = KITH_ERR_IO;
  }
  free(p);
  free(tmp);
  return rc;
}

static int open_target(const char *mount_path, kith_target **out, kith_device_info *info) {
  kith_target *t = calloc(1, sizeof *t);
  if (!t || !(t->root = dup_str(mount_path))) {
    free(t);
    return KITH_ERR_NOMEM;
  }
  t->capacity = 1LL << 30;
  snprintf(t->formats_spec, sizeof t->formats_spec, "wav,mp3,lofi");
  char *p = path_of(mount_path, "KITHTEST");
  FILE *f = p ? fopen(p, "rb") : NULL;
  free(p);
  char line[512];
  while (f && fgets(line, sizeof line, f)) {
    char *eq = strchr(line, '='), *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';
    if (!eq) continue;
    *eq = '\0';
    const char *k = line, *v = eq + 1;
    if (!strcmp(k, "id")) copy_to(t->id, sizeof t->id, v);
    else if (!strcmp(k, "name")) copy_to(t->name, sizeof t->name, v);
    else if (!strcmp(k, "capacity")) t->capacity = atoll(v);
    else if (!strcmp(k, "max_rate")) t->max_rate = atoi(v);
    else if (!strcmp(k, "max_bits")) t->max_bits = atoi(v);
    else if (!strcmp(k, "max_channels")) t->max_channels = atoi(v);
    else if (!strcmp(k, "formats")) copy_to(t->formats_spec, sizeof t->formats_spec, v);
  }
  if (f) fclose(f);
  if (!t->id[0]) copy_to(t->id, sizeof t->id, "TESTDEVICE");
  if (!t->name[0]) copy_to(t->name, sizeof t->name, "Test Device");
  read_manifest(t);

  char spec[128];
  copy_to(spec, sizeof spec, t->formats_spec);
  for (char *tok = strtok(spec, ","); tok && t->nformats < 4; tok = strtok(NULL, ",")) {
    kith_device_format *df = &t->formats[t->nformats++];
    memset(df, 0, sizeof *df);
    df->format = !strcmp(tok, "wav") ? "wav" : !strcmp(tok, "mp3") ? "mp3" : "lofi";
    if (!strcmp(tok, "wav")) {
      df->max_sample_rate = t->max_rate;
      df->max_bits = t->max_bits;
      df->max_channels = t->max_channels;
    }
    if (!strcmp(tok, "mp3")) df->max_kbps = 320;
  }
  long long used = 0;
  for (int i = 0; i < t->count; i++) used += t->tracks[i].size;
  memset(&t->info, 0, sizeof t->info);
  t->info.id = t->id;
  t->info.name = t->name;
  t->info.model = "Folder";
  t->info.capacity_bytes = t->capacity;
  t->info.free_bytes = t->capacity - used;
  t->info.formats = t->formats;
  t->info.format_count = t->nformats;
  t->info.lossless_format = "wav";
  t->info.lossless_setting = "";
  t->info.lossy_format = "lofi";
  t->info.lossy_setting = "q1";
  t->info.has_playlists = 1;
  *info = t->info;
  *out = t;
  return KITH_OK;
}

static int list(kith_target *t, kith_track_fn each, void *ctx) {
  for (int i = 0; i < t->count; i++) {
    track *tr = &t->tracks[i];
    kith_device_track d;
    memset(&d, 0, sizeof d);
    d.key = tr->key;
    d.track_uid = tr->track_uid[0] ? tr->track_uid : NULL;
    d.file_uid = tr->file_uid[0] ? tr->file_uid : NULL;
    d.setting = tr->setting;
    d.sha256 = tr->sha[0] ? tr->sha : NULL;
    d.format = tr->format;
    d.bitrate_kbps = tr->kbps;
    d.size_bytes = tr->size;
    d.title = tr->title;
    d.artist = tr->artist;
    d.album = tr->album;
    d.duration_ms = tr->duration_ms;
    d.play_count = tr->play_count;
    d.last_played_ms = tr->last_played_ms;
    if (each(ctx, &d)) break;
  }
  return KITH_OK;
}

static track *find(kith_target *t, const char *key) {
  for (int i = 0; i < t->count; i++)
    if (!strcmp(t->tracks[i].key, key)) return &t->tracks[i];
  return NULL;
}

static const char *ext_of(const char *format) {
  return !strcmp(format, "mp3") ? "mp3" : !strcmp(format, "lofi") ? "lofi" : "wav";
}

static char *music_path(kith_target *t, const char *key, const char *format) {
  char name[128];
  snprintf(name, sizeof name, "Music" SEP "%s.%s", key, ext_of(format));
  return path_of(t->root, name);
}

static void set_tags(track *tr, const kith_track *k) {
  track_clear(tr);
  tr->title = dup_str(k->tags.title);
  tr->artist = dup_str(k->tags.artist);
  tr->album = dup_str(k->tags.album);
  tr->duration_ms = k->duration_ms;
  tr->play_count = k->play_count;
  tr->last_played_ms = k->last_played_ms;
}

static int put(kith_target *t, const kith_track *k, const char *file_path, char key[KITH_KEY_LEN],
               const kith_job *job) {
  long long used = 0;
  for (int i = 0; i < t->count; i++) used += t->tracks[i].size;
  track *old = key[0] ? find(t, key) : NULL;
  if (key[0] && !old) return KITH_ERR_NOT_FOUND;
  if (used - (old ? old->size : 0) + k->size_bytes > t->capacity) return KITH_ERR_FULL;
  char *dir = path_of(t->root, "Music");
  if (dir) MKDIR(dir);
  free(dir);
  char newkey[KITH_KEY_LEN];
  if (old) copy_to(newkey, sizeof newkey, old->key);
  else snprintf(newkey, sizeof newkey, "T%04d", t->next_key++);
  if (old) {
    char *was = music_path(t, old->key, old->format);
    if (was) remove(was);
    free(was);
  }
  char *dst = music_path(t, newkey, k->format);
  FILE *in = fopen(file_path, "rb"), *out = dst ? fopen(dst, "wb") : NULL;
  int rc = in && out ? KITH_OK : KITH_ERR_IO;
  char buf[65536];
  size_t n;
  while (rc == KITH_OK && (n = fread(buf, 1, sizeof buf, in)) > 0) {
    if (host->cancelled(job)) rc = KITH_ERR_CANCELLED;
    else if (fwrite(buf, 1, n, out) != n) rc = KITH_ERR_IO;
  }
  if (in) fclose(in);
  if (out && fclose(out) != 0) rc = KITH_ERR_IO;
  if (rc != KITH_OK) {
    if (dst) remove(dst);
    free(dst);
    return rc;
  }
  free(dst);
  track *tr = old ? old : add_track(t);
  if (!tr) return KITH_ERR_NOMEM;
  copy_to(tr->key, sizeof tr->key, newkey);
  copy_to(tr->track_uid, sizeof tr->track_uid, k->track_uid);
  copy_to(tr->file_uid, sizeof tr->file_uid, k->file_uid);
  copy_to(tr->setting, sizeof tr->setting, k->setting);
  copy_to(tr->sha, sizeof tr->sha, k->sha256);
  copy_to(tr->format, sizeof tr->format, k->format);
  tr->kbps = k->bitrate_kbps;
  tr->size = k->size_bytes;
  set_tags(tr, k);
  copy_to(key, KITH_KEY_LEN, newkey);
  return KITH_OK;
}

static int update(kith_target *t, const char *key, const kith_track *k) {
  track *tr = find(t, key);
  if (!tr) return KITH_ERR_NOT_FOUND;
  set_tags(tr, k);
  return KITH_OK;
}

static int remove_track(kith_target *t, const char *key) {
  track *tr = find(t, key);
  if (!tr) return KITH_ERR_NOT_FOUND;
  char *p = music_path(t, tr->key, tr->format);
  if (p) remove(p);
  free(p);
  track_clear(tr);
  int i = (int)(tr - t->tracks);
  memmove(tr, tr + 1, (size_t)(t->count - i - 1) * sizeof *tr);
  t->count--;
  return KITH_OK;
}

static char *file_path(kith_target *t, const char *key) {
  track *tr = find(t, key);
  return tr ? music_path(t, tr->key, tr->format) : NULL;
}

/* FNV-1a of a file as 16 hex digits; "" if it's missing or empty. */
static void file_token(const char *path, char token[KITH_SHA256_LEN]) {
  token[0] = '\0';
  FILE *f = fopen(path, "rb");
  if (!f) return;
  unsigned long long h = 1469598103934665603ULL;
  int c, any = 0;
  while ((c = fgetc(f)) != EOF) {
    h = (h ^ (unsigned char)c) * 1099511628211ULL;
    any = 1;
  }
  fclose(f);
  if (any) snprintf(token, KITH_SHA256_LEN, "%016llx", h);
}

static int plays_read(kith_target *t, char token[KITH_SHA256_LEN], kith_play_fn each, void *ctx) {
  char *p = path_of(t->root, "plays.tsv");
  if (!p) return KITH_ERR_NOMEM;
  file_token(p, token);
  FILE *f = each && token[0] ? fopen(p, "rb") : NULL;
  free(p);
  char line[512];
  while (f && fgets(line, sizeof line, f)) {
    char *s = line;
    kith_device_play play;
    play.key = field(&s);
    play.count = atoi(field(&s));
    play.last_played_ms = atoll(field(&s));
    if (play.key[0] && play.count > 0 && each(ctx, &play)) break;
  }
  if (f) fclose(f);
  return KITH_OK;
}

static int playlists_read(kith_target *t, char token[KITH_SHA256_LEN], kith_playlist_fn each,
                          void *ctx) {
  char *p = path_of(t->root, "otg.tsv");
  if (!p) return KITH_ERR_NOMEM;
  file_token(p, token);
  FILE *f = each && token[0] ? fopen(p, "rb") : NULL;
  free(p);
  char line[2048];
  while (f && fgets(line, sizeof line, f)) {
    char *s = line;
    kith_device_playlist pl;
    const char *keys[64];
    pl.name = field(&s);
    char *list = field(&s);
    int n = 0;
    for (char *k = strtok(list, ","); k && n < 64; k = strtok(NULL, ",")) keys[n++] = k;
    pl.keys = keys;
    pl.count = n;
    if (each(ctx, &pl)) break;
  }
  if (f) fclose(f);
  return KITH_OK;
}

static int playlists_write(kith_target *t, const kith_playlist *lists, int count) {
  char *p = path_of(t->root, "playlists.tsv");
  FILE *f = p ? fopen(p, "wb") : NULL;
  free(p);
  if (!f) return KITH_ERR_IO;
  for (int i = 0; i < count; i++) {
    fprintf(f, "%s\t%s\t", lists[i].uid, lists[i].name);
    for (int k = 0; k < lists[i].count; k++) fprintf(f, "%s%s", k ? "," : "", lists[i].keys[k]);
    fputc('\n', f);
  }
  return fclose(f) == 0 ? KITH_OK : KITH_ERR_IO;
}

static int device_done(kith_target *t, unsigned what) {
  t->done_pending |= what;
  return KITH_OK;
}

static int commit(kith_target *t) {
  int rc = write_manifest(t);
  if (rc == KITH_OK && (t->done_pending & KITH_DONE_PLAYS)) {
    char *p = path_of(t->root, "plays.tsv");
    if (p) remove(p);
    free(p);
  }
  if (rc == KITH_OK && (t->done_pending & KITH_DONE_PLAYLISTS)) {
    char *p = path_of(t->root, "otg.tsv");
    if (p) remove(p);
    free(p);
  }
  if (rc == KITH_OK) t->done_pending = 0;
  char *log = path_of(t->root, "commits.log");
  FILE *f = log ? fopen(log, "ab") : NULL;
  if (f) {
    fprintf(f, "commit %d\n", t->count);
    fclose(f);
  }
  free(log);
  return rc;
}

static void close_target(kith_target *t) {
  if (!t) return;
  for (int i = 0; i < t->count; i++) track_clear(&t->tracks[i]);
  free(t->tracks);
  free(t->root);
  free(t);
}

static const kith_device_target targets[] = {{
  "folder", "Test folder", probe, open_target, list, put, update, remove_track, file_path,
  plays_read, playlists_read, playlists_write, device_done, commit, close_target, NULL,
}};

static const kith_option_choice choices[] = {{"a", "Choice A"}, {"b", "Choice B"}};
static const kith_option options[] = {
  {"fast", KITH_OPTION_SWITCH, "Go fast", NULL, "0", NULL, 0, 0, 0},
  {"mode", KITH_OPTION_CHOICE, "Mode", "How it works", "a", choices, 2, 0, 0},
  {"level", KITH_OPTION_NUMBER, "Level", NULL, "5", NULL, 0, 1, 10},
};

static kith_plugin plugin = {
  KITH_PLUGIN_ABI, (int)sizeof(kith_plugin), "ng.kithso.test.folder", "Test folder", "1.2.3",
  plugin_free, NULL, options, 3, targets, 1, encoders, 1,
};

KITH_PLUGIN_EXPORT const kith_plugin *kith_plugin_init(const kith_host *h) {
  if (!h || h->abi != KITH_PLUGIN_ABI || h->size != (int)sizeof(kith_host)) return NULL;
  host = h;
#if defined(TEST_PLUGIN_NULL_INIT)
  host->log(host, KITH_LOG_ERROR, "refusing to start, as told");
  (void)plugin;
  return NULL;
#elif defined(TEST_PLUGIN_BAD_ABI)
  plugin.abi = KITH_PLUGIN_ABI + 1;
  plugin.id = "ng.kithso.test.badabi";
  return &plugin;
#else
  /* Remembers how often it was started, through its settings. */
  char *runs = host->settings_get(host, "runs");
  char buf[16];
  snprintf(buf, sizeof buf, "%d", runs ? atoi(runs) + 1 : 1);
  host->free(runs);
  host->settings_set(host, "runs", buf);
  return &plugin;
#endif
}
