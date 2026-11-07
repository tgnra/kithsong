/* The iPod plugin as the app runs it: kithsong-ipod loaded through core's loader from the
 * folder it's built into, turned on, and a fake 4th-generation iPod (a folder with SysInfo
 * and an iTunesDB holding two tracks iTunes put there) synced through mc_tethered.
 *   - A FLAC becomes ALAC, an MP3 is copied as it is, an Opus becomes MP3 V0, and a
 *     24-bit/96 kHz FLAC becomes 16-bit/48 kHz ALAC. iTunes' tracks are left as they are.
 *     Syncing again changes nothing.
 *   - The plugin opened by itself, a track changed and one removed and committed, as an
 *     older core would that never reads plays: Play Counts and the OTG files stay byte for
 *     byte, and the removal waits.
 *   - Plays (a hand-made Play Counts) and On-The-Go lists (OTGPlaylistInfo, _1) become play
 *     log rows from the device, spaced before the last play, counts written back into the
 *     iTunesDB, and the library's "On-The-Go 1" and "2"; the files are deleted. Again:
 *     nothing doubles. With the iPod put back as it was right after the import (an unplug
 *     before the device was told), nothing doubles either.
 *   - A playlist in the profile becomes an iPod playlist. */
#include <opusenc.h>

#include "kithipod/device.h"
#include "kithipod/encoder.h"
#include "kithipod/itunesdb.h"
#include "plugin/plugins_internal.h"
#include "tethered_fixture.h"

#define SECONDS 2
#define LEN_MS (SECONDS * 1000)

/* --- Audio files ----------------------------------------------------------------------- */

typedef struct bits {
  unsigned char *buf;
  size_t len, cap;
  uint64_t acc;
  int n;
} bits;

static void bits_put(bits *b, uint32_t v, int count) {
  for (int i = count - 1; i >= 0; i--) {
    b->acc = b->acc << 1 | ((v >> i) & 1);
    if (++b->n == 8) {
      if (b->len == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->buf = realloc(b->buf, b->cap);
      }
      b->buf[b->len++] = (unsigned char)b->acc;
      b->acc = 0;
      b->n = 0;
    }
  }
}

static uint8_t crc8(const unsigned char *p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (uint8_t)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}

static uint16_t crc16(const unsigned char *p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= (uint16_t)(p[i] << 8);
    for (int k = 0; k < 8; k++) c = (uint16_t)(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
  }
  return c;
}

static int32_t tone(uint32_t i, uint32_t rate, int ch, int bps, double pitch) {
  double v = 0.4 * sin(2 * 3.14159265358979 * (pitch + 50.0 * ch) * i / rate);
  return (int32_t)lrint(v * ((1 << (bps - 1)) - 1));
}

/* A stereo FLAC of a tone, in VERBATIM subframes: what a FLAC needs to be, no more. */
static int write_flac(const char *path, uint32_t rate, int bps, double pitch) {
  const uint32_t block = 4096, total = rate * SECONDS;
  bits b = {0};
  bits_put(&b, 0x664C6143u, 32); /* fLaC */
  bits_put(&b, 1, 1);           /* last metadata block */
  bits_put(&b, 0, 7);           /* STREAMINFO */
  bits_put(&b, 34, 24);
  bits_put(&b, block, 16);
  bits_put(&b, block, 16);
  bits_put(&b, 0, 24);
  bits_put(&b, 0, 24);
  bits_put(&b, rate, 20);
  bits_put(&b, 1, 3); /* two channels */
  bits_put(&b, (uint32_t)bps - 1, 5);
  bits_put(&b, 0, 4);
  bits_put(&b, total, 32);
  for (int i = 0; i < 4; i++) bits_put(&b, 0, 32); /* no MD5 */
  uint32_t frame = 0;
  for (uint32_t at = 0; at < total; at += block, frame++) {
    uint32_t n = total - at < block ? total - at : block;
    size_t start = b.len;
    bits_put(&b, 0xFFF8, 16);
    bits_put(&b, 7, 4); /* block size: 16 bits at the end of the header */
    bits_put(&b, 0, 4); /* sample rate: STREAMINFO's */
    bits_put(&b, 1, 4); /* two independent channels */
    bits_put(&b, bps == 24 ? 6 : 4, 3);
    bits_put(&b, 0, 1);
    bits_put(&b, frame, 8); /* UTF-8 coded, < 128 */
    bits_put(&b, n - 1, 16);
    bits_put(&b, crc8(b.buf + start, b.len - start), 8);
    for (int ch = 0; ch < 2; ch++) {
      bits_put(&b, 0x02, 8); /* VERBATIM */
      for (uint32_t i = 0; i < n; i++) bits_put(&b, (uint32_t)tone(at + i, rate, ch, bps, pitch), bps);
    }
    bits_put(&b, crc16(b.buf + start, b.len - start), 16);
  }
  FILE *f = mc_fopen(path, "wb");
  int ok = f && fwrite(b.buf, 1, b.len, f) == b.len;
  if (f && fclose(f) != 0) ok = 0;
  free(b.buf);
  return ok;
}

static int16_t *pcm16(uint32_t rate, double pitch) {
  uint32_t n = rate * SECONDS;
  int16_t *s = malloc((size_t)n * 2 * sizeof *s);
  for (uint32_t i = 0; s && i < n; i++)
    for (int ch = 0; ch < 2; ch++) s[2 * i + ch] = (int16_t)tone(i, rate, ch, 16, pitch);
  return s;
}

/* A 128 kbps CBR MP3, made with the plugin's own encoder. */
static int write_mp3(const char *path, double pitch) {
  int16_t *s = pcm16(44100, pitch);
  ipod_enc_settings es;
  ipod_enc_settings_parse("mp3", "128", &es);
  ipod_pcm_format pf = {44100, 2, 16};
  ipod_tags tags = {0};
  tags.title = "Two (file)";
  ipod_encoder *e = s ? ipod_encoder_open(path, &es, &pf, &tags, NULL) : NULL;
  int ok = e && ipod_encoder_write(e, s, 44100 * SECONDS, NULL) == 0;
  if (e) ok = ipod_encoder_finish(e, NULL, NULL) == 0 && ok;
  free(s);
  return ok;
}

static int write_opus(const char *path, double pitch) {
  int16_t *s = pcm16(48000, pitch);
  int err = 0;
  OggOpusComments *c = ope_comments_create();
  OggOpusEnc *e = s && c ? ope_encoder_create_file(path, c, 48000, 2, 0, &err) : NULL;
  int ok = e && ope_encoder_write(e, s, 48000 * SECONDS) == OPE_OK && ope_encoder_drain(e) == OPE_OK;
  if (e) ope_encoder_destroy(e);
  if (c) ope_comments_destroy(c);
  free(s);
  return ok;
}

/* --- The library ------------------------------------------------------------------------ */

/* A track of album with file rel (already in the library folder) of format and kbps. */
static void add_track(fx *x, const char *album, const char *title, int no, const char *rel, const char *format,
                      int kbps, char uid[MC_UID_STRLEN]) {
  char sql[2048], abs[2048], fuid[MC_UID_STRLEN];
  mc_uid_new(uid);
  CHECK(mc_library_begin_write(x->lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " SELECT '%s', uid, artist_uid, '%s', %d, %d FROM albums WHERE uid = '%s'",
           uid, title, no, LEN_MS, album);
  CHECK(fx_exec(x, sql) == SQLITE_OK);
  CHECK(mc_library_commit(x->lib) == MC_OK);
  if (!rel) return;
  snprintf(abs, sizeof abs, "%s/%s", x->libdir, rel);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(abs, &size, &mtime) == MC_OK);
  mc_uid_new(fuid);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, %lld, %lld)",
           fuid, uid, rel, format, kbps, (long long)size, (long long)mtime);
  CHECK(mc_library_begin_write(x->lib) == MC_OK);
  CHECK(fx_exec(x, sql) == SQLITE_OK);
  CHECK(mc_library_commit(x->lib) == MC_OK);
}

/* --- The iPod ----------------------------------------------------------------------------- */

static void dev_file(fx *x, const char *rel, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s", x->dev, rel);
}

static void write_bytes(const char *path, const void *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  CHECK(f != NULL);
  if (f) {
    CHECK(fwrite(data, 1, len, f) == len);
    fclose(f);
  }
}

static unsigned char *read_bytes(const char *path, size_t *len) {
  *len = 0;
  FILE *f = mc_fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 1 << 16, n = 0;
  unsigned char *buf = malloc(cap), *more;
  size_t got;
  while (buf && (got = fread(buf + n, 1, cap - n, f)) > 0) {
    n += got;
    if (n == cap && (more = realloc(buf, cap *= 2))) buf = more;
    else if (n == cap) break;
  }
  fclose(f);
  *len = n;
  return buf;
}

/* SysInfo of a 20 GB 4th-generation iPod, and an iTunesDB of two tracks iTunes put there:
 * "Far" (Low, Elsewhere), which the library has too, and "Nobody", which it hasn't. */
static void make_ipod(fx *x) {
  static const char *dirs[] = {"iPod_Control/iTunes", "iPod_Control/Device", "iPod_Control/Music/F00"};
  char p[2048];
  for (size_t i = 0; i < 3; i++) {
    dev_file(x, dirs[i], p, sizeof p);
    CHECK(mc_mkdirs(p) == MC_OK);
  }
  dev_file(x, "iPod_Control/Device/SysInfo", p, sizeof p);
  CHECK(fx_write_text(p, "BoardHwName: iPod Q21\npszSerialNumber: JQ4123ABCDE\nModelNumStr: M9282\n"
                         "FirewireGuid: 0x000A2700137F5D2A\nvisibleBuildID: 0x03118000 (3.1.1)\n"));
  ipod_db *db = ipod_db_new("Walkman");
  const char *titles[] = {"Far", "Nobody"}, *artists[] = {"Low", "Nope"}, *albums[] = {"Elsewhere", "Nil"};
  for (int i = 0; i < 2; i++) {
    char rel[64];
    snprintf(rel, sizeof rel, "iPod_Control/Music/F00/ITN%d.mp3", i);
    dev_file(x, rel, p, sizeof p);
    unsigned char junk[4000];
    memset(junk, 0x40 + i, sizeof junk);
    write_bytes(p, junk, sizeof junk);
    ipod_track *t = ipod_track_new();
    ipod_set_str(&t->title, titles[i]);
    ipod_set_str(&t->artist, artists[i]);
    ipod_set_str(&t->album, albums[i]);
    char *loc = ipod_path_to_location(rel);
    t->location = loc;
    ipod_track_set_kind(t, "mp3", 0);
    t->size = sizeof junk;
    t->length_ms = LEN_MS;
    t->bitrate = 128;
    t->sample_rate = 44100;
    t->play_count = 7;
    ipod_db_add_track(db, t);
  }
  dev_file(x, "iPod_Control/iTunes/iTunesDB", p, sizeof p);
  CHECK(ipod_db_write(db, p, NULL, NULL) == 0);
  ipod_db_free(db);
}

static ipod_db *read_db(fx *x) {
  char p[2048];
  dev_file(x, "iPod_Control/iTunes/iTunesDB", p, sizeof p);
  return ipod_db_read(p, NULL);
}

/* The iPod's key for the track Kithsong wrote for track_uid ("" if none), from kithsong.json. */
static void key_of(fx *x, const char *track_uid, char key[IPOD_KEY_LEN]) {
  key[0] = 0;
  ipod_device *d = ipod_open(x->dev, IPOD_OPEN_ALLOW_UNKNOWN, NULL);
  CHECK(d != NULL);
  if (!d) return;
  const ipod_kith_map *m = ipod_device_kith(d);
  for (size_t i = 0; i < m->n; i++)
    if (m->entries[i].track_uid && !strcmp(m->entries[i].track_uid, track_uid))
      snprintf(key, IPOD_KEY_LEN, "%s", m->entries[i].key);
  ipod_device_close(d);
}

static long index_of(const ipod_db *db, const char *key) {
  for (size_t i = 0; i < db->n_tracks; i++) {
    char k[IPOD_KEY_LEN];
    ipod_track_key(db->tracks[i], k);
    if (!strcmp(k, key)) return (long)i;
  }
  return -1;
}

static long index_titled(const ipod_db *db, const char *title) {
  for (size_t i = 0; i < db->n_tracks; i++)
    if (db->tracks[i]->title && !strcmp(db->tracks[i]->title, title)) return (long)i;
  return -1;
}

static void le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

/* ALAC's bit depth, channels and sample rate from an M4A's magic cookie. */
static int alac_format(const char *path, int *bits_out, int *channels, uint32_t *rate) {
  size_t len;
  unsigned char *d = read_bytes(path, &len);
  int found = 0;
  /* The sample entry is 'alac', and the cookie a full box 'alac' inside it. */
  for (size_t i = 0, seen = 0; d && i + 4 + 4 + 24 <= len; i++) {
    if (memcmp(d + i, "alac", 4) != 0 || ++seen < 2) continue;
    const unsigned char *c = d + i + 8; /* after the tag and version/flags */
    *bits_out = c[5];
    *channels = c[9];
    *rate = (uint32_t)c[20] << 24 | (uint32_t)c[21] << 16 | (uint32_t)c[22] << 8 | c[23];
    found = 1;
    break;
  }
  free(d);
  return found;
}

/* Copies of the iPod's database and record files, to put back. */
static const char *const saved_files[] = {"iPod_Control/iTunes/iTunesDB", "iPod_Control/iTunes/Play Counts",
                                          "iPod_Control/iTunes/OTGPlaylistInfo",
                                          "iPod_Control/iTunes/OTGPlaylistInfo_1", "iPod_Control/kithsong.json",
                                          "iPod_Control/kithsong-records"};
#define N_SAVED (sizeof saved_files / sizeof saved_files[0])

typedef struct saved {
  unsigned char *data[N_SAVED];
  size_t len[N_SAVED];
} saved;

static void save_ipod(fx *x, saved *s) {
  for (size_t i = 0; i < N_SAVED; i++) {
    char p[2048];
    dev_file(x, saved_files[i], p, sizeof p);
    s->data[i] = read_bytes(p, &s->len[i]);
  }
}

static void restore_ipod(fx *x, const saved *s) {
  for (size_t i = 0; i < N_SAVED; i++) {
    char p[2048];
    dev_file(x, saved_files[i], p, sizeof p);
    if (s->data[i]) write_bytes(p, s->data[i], s->len[i]);
    else mc_remove(p);
  }
}

static void free_saved(saved *s) {
  for (size_t i = 0; i < N_SAVED; i++) free(s->data[i]);
}

static int same_file(fx *x, const char *rel, const unsigned char *data, size_t len) {
  char p[2048];
  dev_file(x, rel, p, sizeof p);
  size_t n;
  unsigned char *d = read_bytes(p, &n);
  int same = d && data && n == len && !memcmp(d, data, n);
  free(d);
  return same;
}

static int dev_exists(fx *x, const char *rel) {
  char p[2048];
  dev_file(x, rel, p, sizeof p);
  return mc_file_exists(p);
}

/* --- The test ------------------------------------------------------------------------------- */

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  CHECK(mc_init() == MC_OK);
  fx x;
  memset(&x, 0, sizeof x);
  mc_mutex_init(&x.lock);
  snprintf(x.base, sizeof x.base, "%s", argv[1]);
  snprintf(x.libdir, sizeof x.libdir, "%s/lib", x.base);
  snprintf(x.cfg, sizeof x.cfg, "%s/cfg", x.base);
  snprintf(x.dev, sizeof x.dev, "%s/ipod", x.base);
  mc_remove_tree(x.base);
  CHECK(mc_mkdirs(x.libdir) == MC_OK && mc_mkdirs(x.cfg) == MC_OK && mc_mkdirs(x.dev) == MC_OK);
  CHECK(mc_settings_load(x.cfg, &x.s) == MC_OK);

  /* The real plugin, through the loader, off until turned on. */
  const char *dirs[] = {IPOD_PLUGIN_DIR};
  CHECK(mc_plugins_load(x.s, dirs, 1) == MC_OK);
  CHECK(mc_plugins_set_enabled("kithsong-ipod", 1) == MC_OK);
  mc_plugin_list *pl = NULL;
  CHECK(mc_plugins_list(&pl) == MC_OK && pl);
  int loaded = 0;
  for (int i = 0; pl && i < pl->count; i++)
    if (!strcmp(pl->items[i].file_id, "kithsong-ipod")) {
      loaded = pl->items[i].state == MC_PLUGIN_LOADED && pl->items[i].id &&
               !strcmp(pl->items[i].id, "ng.kithso.ipod") && pl->items[i].device_targets == 1 &&
               pl->items[i].encoders == 1;
      if (pl->items[i].reason) fprintf(stderr, "refused: %s\n", pl->items[i].reason);
    }
  mc_plugin_list_free(pl);
  CHECK(loaded);
  if (!loaded) return TEST_RESULT();
  mc_plugin_option_list *opts = NULL;
  CHECK(mc_plugins_options("kithsong-ipod", &opts) == MC_OK && opts && opts->count == 1);
  if (opts && opts->count == 1)
    CHECK(!strcmp(opts->items[0].key, "artwork") && !strcmp(opts->items[0].value, "1") &&
          !strcmp(opts->items[0].label, "Album art"));
  mc_plugin_option_list_free(opts);

  CHECK(mc_library_open(x.libdir, &x.lib) == MC_OK);
  mc_tethered_events ev = {fx_on_devices, fx_on_progress, fx_on_done};
  CHECK(mc_tethered_new(x.lib, x.cfg, &ev, &x, &x.t) == MC_OK);

  /* The library: Things (FLAC, MP3, Opus, hi-res FLAC) and Elsewhere's Far. */
  char things[MC_UID_STRLEN], elsewhere[MC_UID_STRLEN], one[MC_UID_STRLEN], two[MC_UID_STRLEN],
    three[MC_UID_STRLEN], four[MC_UID_STRLEN], far[MC_UID_STRLEN];
  fx_album(&x, "Low", "Things", things);
  fx_album(&x, "Low", "Elsewhere", elsewhere);
  char p[2048];
  snprintf(p, sizeof p, "%s/one.flac", x.libdir);
  CHECK(write_flac(p, 44100, 16, 330));
  snprintf(p, sizeof p, "%s/two.mp3", x.libdir);
  CHECK(write_mp3(p, 440));
  snprintf(p, sizeof p, "%s/three.opus", x.libdir);
  CHECK(write_opus(p, 550));
  snprintf(p, sizeof p, "%s/four.flac", x.libdir);
  CHECK(write_flac(p, 96000, 24, 660));
  add_track(&x, things, "One", 1, "one.flac", "flac", 0, one);
  add_track(&x, things, "Two", 2, "two.mp3", "mp3", 128, two);
  add_track(&x, things, "Three", 3, "three.opus", "opus", 96, three);
  add_track(&x, things, "Four", 4, "four.flac", "flac", 0, four);
  add_track(&x, elsewhere, "Far", 1, NULL, NULL, 0, far);

  make_ipod(&x);
  char uid[MC_UID_STRLEN];
  CHECK(mc_tethered_volume_added(x.t, x.dev, uid) == MC_OK);
  mc_tethered_device_list *dl = NULL;
  CHECK(mc_tethered_list(x.t, &dl) == MC_OK && dl && dl->count == 1);
  mc_tethered_device_list_free(dl);

  char mix[MC_UID_STRLEN];
  CHECK(mc_playlist_create(x.lib, "Mix", mix) == MC_OK);
  CHECK(mc_playlist_append(x.lib, mix, three, NULL) == MC_OK);
  CHECK(mc_playlist_append(x.lib, mix, one, NULL) == MC_OK);
  mc_profile_item items[2];
  memset(items, 0, sizeof items);
  items[0].kind = MC_PROFILE_ALBUM;
  memcpy(items[0].ref_uid, things, MC_UID_STRLEN);
  items[1].kind = MC_PROFILE_PLAYLIST;
  memcpy(items[1].ref_uid, mix, MC_UID_STRLEN);
  mc_sync_profile prof;
  memset(&prof, 0, sizeof prof);
  prof.items = items;
  prof.count = 1;
  CHECK(mc_tethered_set_profile(x.t, uid, &prof, NULL) == MC_OK);

  /* --- The first sync --- */
  mc_tethered_result r;
  CHECK(fx_sync(&x, uid, &r));
  fx_print("first", &r);
  CHECK(r.status == MC_OK && r.written == 4 && r.failed == 0 && r.foreign == 2 && r.removed == 0);

  char k_one[IPOD_KEY_LEN], k_two[IPOD_KEY_LEN], k_three[IPOD_KEY_LEN], k_four[IPOD_KEY_LEN];
  key_of(&x, one, k_one);
  key_of(&x, two, k_two);
  key_of(&x, three, k_three);
  key_of(&x, four, k_four);
  CHECK(k_one[0] && k_two[0] && k_three[0] && k_four[0]);
  ipod_device *d = ipod_open(x.dev, 0, NULL);
  CHECK(d != NULL);
  if (d) {
    CHECK(ipod_device_db(d)->n_tracks == 6);
    const ipod_kith_entry *e = ipod_device_kith_entry(d, k_one);
    CHECK(e && !strcmp(e->format, "alac") && (!e->setting || !*e->setting));
    e = ipod_device_kith_entry(d, k_two);
    CHECK(e && !strcmp(e->format, "mp3") && (!e->setting || !*e->setting));
    e = ipod_device_kith_entry(d, k_three);
    CHECK(e && !strcmp(e->format, "mp3") && e->setting && !strcmp(e->setting, "v0"));
    e = ipod_device_kith_entry(d, k_four);
    CHECK(e && !strcmp(e->format, "alac"));

    ipod_track *t = ipod_device_find(d, k_one);
    CHECK(t && t->filetype == IPOD_FILETYPE_M4A && t->sample_rate == 44100 && t->title && !strcmp(t->title, "One"));
    CHECK(t && t->album && !strcmp(t->album, "Things") && t->track_number == 1 && t->length_ms == LEN_MS);
    char *f = ipod_device_track_path(d, k_one);
    int bits = 0, ch = 0;
    uint32_t rate = 0;
    CHECK(f && strstr(f, ".m4a") && alac_format(f, &bits, &ch, &rate) && bits == 16 && ch == 2 && rate == 44100);
    free(f);

    /* The MP3, byte for byte. */
    t = ipod_device_find(d, k_two);
    CHECK(t && t->filetype == IPOD_FILETYPE_MP3 && t->type1 == 0 && t->sample_rate == 44100 && t->bitrate == 128);
    f = ipod_device_track_path(d, k_two);
    char sha_dev[65], sha_src[65];
    fx_sha(f ? f : "", sha_dev);
    snprintf(p, sizeof p, "%s/two.mp3", x.libdir);
    fx_sha(p, sha_src);
    CHECK(f && sha_src[0] && !strcmp(sha_dev, sha_src));
    free(f);

    /* The Opus, as MP3 V0 (VBR, with a Xing header). */
    t = ipod_device_find(d, k_three);
    CHECK(t && t->filetype == IPOD_FILETYPE_MP3 && t->type1 == 1 && t->sample_rate == 48000);
    f = ipod_device_track_path(d, k_three);
    size_t n = 0;
    unsigned char *mp3 = f ? read_bytes(f, &n) : NULL;
    int xing = 0;
    for (size_t i = 0; mp3 && i + 4 <= n && i < 8192 && !xing; i++) xing = !memcmp(mp3 + i, "Xing", 4);
    CHECK(f && strstr(f, ".mp3") && xing);
    free(mp3);
    free(f);

    /* The 24-bit/96 kHz FLAC, as 16-bit/48 kHz ALAC. */
    t = ipod_device_find(d, k_four);
    CHECK(t && t->sample_rate == 48000);
    f = ipod_device_track_path(d, k_four);
    CHECK(f && alac_format(f, &bits, &ch, &rate) && bits == 16 && ch == 2 && rate == 48000);
    free(f);

    /* iTunes' tracks: as they were, files and all, and not Kithsong's. */
    ipod_db *db = ipod_device_db(d);
    long far_i = index_titled(db, "Far"), nobody_i = index_titled(db, "Nobody");
    CHECK(far_i == 0 && nobody_i == 1);
    if (far_i == 0 && nobody_i == 1) {
      char k[IPOD_KEY_LEN];
      ipod_track_key(db->tracks[0], k);
      CHECK(ipod_device_kith_entry(d, k) == NULL && db->tracks[0]->play_count == 7);
      CHECK(dev_exists(&x, "iPod_Control/Music/F00/ITN0.mp3") && dev_exists(&x, "iPod_Control/Music/F00/ITN1.mp3"));
    }
    CHECK(!strcmp(ipod_db_name(db), "Walkman"));
    ipod_device_close(d);
  }

  /* Again: nothing to do, and the database isn't even rewritten. */
  size_t db_len = 0;
  dev_file(&x, "iPod_Control/iTunes/iTunesDB", p, sizeof p);
  unsigned char *db_before = read_bytes(p, &db_len);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("again", &r);
  CHECK(r.status == MC_OK && r.written == 0 && r.removed == 0 && r.updated == 0 && r.failed == 0);
  CHECK(same_file(&x, "iPod_Control/iTunes/iTunesDB", db_before, db_len));
  free(db_before);

  /* --- Plays and On-The-Go, made on the iPod --- */
  ipod_db *db = read_db(&x);
  CHECK(db != NULL);
  if (!db) return TEST_RESULT();
  long i_one = index_of(db, k_one), i_two = index_of(db, k_two), i_far = index_titled(db, "Far");
  CHECK(i_one >= 0 && i_two >= 0 && i_far >= 0);
  int64_t now_s = (int64_t)(mc_wall_ms() / 1000);
  int64_t last_one = (now_s - 3600) * 1000, last_far = (now_s - 7200) * 1000;
  int32_t tz = ipod_local_tz_offset();
  size_t ntracks = db->n_tracks, pc_len = 0x60 + ntracks * 0x1C;
  unsigned char *pc = calloc(1, pc_len);
  memcpy(pc, "mhdp", 4);
  le32(pc + 4, 0x60);
  le32(pc + 8, 0x1C);
  le32(pc + 12, (uint32_t)ntracks);
  le32(pc + 0x60 + i_one * 0x1C, 3);
  le32(pc + 0x60 + i_one * 0x1C + 4, ipod_unix_to_mac(last_one / 1000, tz));
  le32(pc + 0x60 + i_far * 0x1C, 1);
  le32(pc + 0x60 + i_far * 0x1C + 4, ipod_unix_to_mac(last_far / 1000, tz));
  dev_file(&x, "iPod_Control/iTunes/Play Counts", p, sizeof p);
  write_bytes(p, pc, pc_len);
  unsigned char otg0[0x14 + 8] = {'m', 'h', 'p', 'o'}, otg1[0x14 + 4] = {'m', 'h', 'p', 'o'};
  le32(otg0 + 4, 0x14);
  le32(otg0 + 8, 4);
  le32(otg0 + 12, 2);
  le32(otg0 + 0x14, (uint32_t)i_one);
  le32(otg0 + 0x18, (uint32_t)i_two);
  le32(otg1 + 4, 0x14);
  le32(otg1 + 8, 4);
  le32(otg1 + 12, 1);
  le32(otg1 + 0x14, (uint32_t)i_far);
  dev_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo", p, sizeof p);
  write_bytes(p, otg0, sizeof otg0);
  dev_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo_1", p, sizeof p);
  write_bytes(p, otg1, sizeof otg1);
  ipod_db_free(db);

  /* An older core: opens, changes a track and removes one, commits, never reads plays. */
  mc_plugin_target pt;
  CHECK(mc_plugins_find_target("ng.kithso.ipod", "ipod", &pt) == MC_OK);
  kith_target *kt = NULL;
  kith_device_info info;
  CHECK(pt.target->open(x.dev, &kt, &info) == KITH_OK && kt);
  if (kt) {
    CHECK(!strcmp(info.id, "guid_000A2700137F5D2A") && !strcmp(info.name, "Walkman"));
    CHECK(!strcmp(info.model, "iPod (4th generation)") || info.model[0]);
    CHECK(!strcmp(info.lossless_format, "alac") && !strcmp(info.lossy_format, "mp3") &&
          !strcmp(info.lossy_setting, "v0") && info.has_playlists && info.format_count >= 2);
    CHECK(!strcmp(info.formats[0].format, "alac") && info.formats[0].max_bits == 16 &&
          info.formats[0].max_sample_rate == 48000 && info.formats[0].max_channels == 2);
    kith_track kt2;
    memset(&kt2, 0, sizeof kt2);
    kt2.tags.title = "Two, retitled";
    kt2.tags.album = "Things";
    kt2.tags.artist = "Low";
    kt2.play_count = 0;
    CHECK(pt.target->update(kt, k_two, &kt2) == KITH_OK);
    CHECK(pt.target->remove(kt, k_four) == KITH_OK);
    CHECK(pt.target->commit(kt) == KITH_OK);
    pt.target->close(kt);
  }
  CHECK(same_file(&x, "iPod_Control/iTunes/Play Counts", pc, pc_len));
  CHECK(same_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo", otg0, sizeof otg0));
  CHECK(same_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo_1", otg1, sizeof otg1));
  db = read_db(&x);
  CHECK(db && db->n_tracks == ntracks && index_of(db, k_four) >= 0); /* the removal waited */
  CHECK(db && index_of(db, k_one) == i_one && index_of(db, k_two) == i_two); /* indexes still true */
  if (db) {
    long i = index_of(db, k_two);
    CHECK(i >= 0 && db->tracks[i]->title && !strcmp(db->tracks[i]->title, "Two, retitled"));
  }
  ipod_db_free(db);
  free(pc);

  /* The iPod as it is now: what an unplug right after the import would leave it as. */
  saved before;
  save_ipod(&x, &before);

  CHECK(fx_sync(&x, uid, &r));
  fx_print("plays", &r);
  CHECK(r.status == MC_OK && r.plays_imported == 4 && r.plays_unmatched == 0 && r.playlists_imported == 2);
  CHECK(r.written == 0 && r.removed == 0 && r.failed == 0);
  CHECK(r.updated == 2); /* One's count, Two's title */
  CHECK(!dev_exists(&x, "iPod_Control/iTunes/Play Counts"));
  CHECK(!dev_exists(&x, "iPod_Control/iTunes/OTGPlaylistInfo"));
  CHECK(!dev_exists(&x, "iPod_Control/iTunes/OTGPlaylistInfo_1"));
  mc_play_log *log = NULL;
  CHECK(mc_library_list_play_events(x.lib, 0, INT64_MAX, &log) == MC_OK && log);
  if (log) {
    CHECK(log->count == 4);
    int ones = 0;
    for (int i = 0; i < log->count; i++) {
      const mc_play_event *e = &log->items[i].event;
      CHECK(e->context == MC_PLAY_CONTEXT_DEVICE && !strcmp(e->context_uid, uid));
      CHECK(e->end == MC_PLAY_END_FINISHED);
      if (!strcmp(e->track_uid, one)) {
        CHECK(e->started_ms == last_one - (int64_t)(2 - ones) * (LEN_MS + 30000));
        CHECK(e->listened_ms == LEN_MS);
        ones++;
      } else {
        CHECK(!strcmp(e->track_uid, far) && e->started_ms == last_far);
      }
    }
    CHECK(ones == 3);
  }
  mc_play_log_free(log);
  char sql[1024];
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", one);
  CHECK(fx_int(&x, sql) == 3);
  db = read_db(&x);
  CHECK(db != NULL);
  if (db) {
    long i = index_of(db, k_one);
    CHECK(i >= 0 && db->tracks[i]->play_count == 3 &&
          db->tracks[i]->last_played == ipod_unix_to_mac(last_one / 1000, tz));
    i = index_titled(db, "Far");
    CHECK(i >= 0 && db->tracks[i]->play_count == 7); /* iTunes' own: untouched */
    CHECK(index_of(db, k_four) >= 0); /* still wanted: it stays */
    i = index_of(db, k_two);
    CHECK(i >= 0 && db->tracks[i]->title && !strcmp(db->tracks[i]->title, "Two"));
    ipod_db_free(db);
  }
  char otg_1[MC_UID_STRLEN] = "", otg_2[MC_UID_STRLEN] = "", got[MC_UID_STRLEN];
  fx_text(&x, "SELECT uid FROM playlists WHERE name = 'On-The-Go 1'", otg_1, sizeof otg_1);
  fx_text(&x, "SELECT uid FROM playlists WHERE name = 'On-The-Go 2'", otg_2, sizeof otg_2);
  CHECK(otg_1[0] && otg_2[0]);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM playlist_entries WHERE playlist_uid = '%s'", otg_1);
  CHECK(fx_int(&x, sql) == 2);
  snprintf(sql, sizeof sql,
           "SELECT track_uid FROM playlist_entries WHERE playlist_uid = '%s' ORDER BY position, uid", otg_1);
  fx_text(&x, sql, got, sizeof got);
  CHECK(!strcmp(got, one));
  snprintf(sql, sizeof sql, "SELECT track_uid FROM playlist_entries WHERE playlist_uid = '%s'", otg_2);
  fx_text(&x, sql, got, sizeof got);
  CHECK(!strcmp(got, far));

  /* Again: nothing doubles. */
  CHECK(fx_sync(&x, uid, &r));
  fx_print("plays again", &r);
  CHECK(r.status == MC_OK && r.plays_imported == 0 && r.playlists_imported == 0 && r.updated == 0);
  CHECK(fx_int(&x, "SELECT count(*) FROM play_events") == 4);
  CHECK(fx_int(&x, "SELECT count(*) FROM playlists") == 3);

  /* Unplugged right after the import: the same plays and lists are back, and still nothing
   * doubles; they're cleared and the counts written back all the same. */
  restore_ipod(&x, &before);
  CHECK(dev_exists(&x, "iPod_Control/iTunes/Play Counts"));
  CHECK(fx_sync(&x, uid, &r));
  fx_print("unplugged", &r);
  CHECK(r.status == MC_OK && r.plays_imported == 0 && r.plays_unmatched == 0 && r.playlists_imported == 0);
  CHECK(fx_int(&x, "SELECT count(*) FROM play_events") == 4);
  CHECK(fx_int(&x, "SELECT count(*) FROM playlists") == 3);
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", one);
  CHECK(fx_int(&x, sql) == 3);
  CHECK(!dev_exists(&x, "iPod_Control/iTunes/Play Counts"));
  CHECK(!dev_exists(&x, "iPod_Control/iTunes/OTGPlaylistInfo"));
  db = read_db(&x);
  if (db) {
    long i = index_of(db, k_one);
    CHECK(i >= 0 && db->tracks[i]->play_count == 3);
    ipod_db_free(db);
  }
  free_saved(&before);

  /* The same On-The-Go lists made again later are new ones, though their bytes are the
   * same as the ones taken in before. */
  dev_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo", p, sizeof p);
  write_bytes(p, otg0, sizeof otg0);
  dev_file(&x, "iPod_Control/iTunes/OTGPlaylistInfo_1", p, sizeof p);
  write_bytes(p, otg1, sizeof otg1);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("otg again", &r);
  CHECK(r.status == MC_OK && r.playlists_imported == 2);
  CHECK(fx_int(&x, "SELECT count(*) FROM playlists") == 5);

  /* Mix to the iPod. */
  prof.count = 2;
  CHECK(mc_tethered_set_profile(x.t, uid, &prof, NULL) == MC_OK);
  CHECK(fx_sync(&x, uid, &r));
  fx_print("playlist", &r);
  CHECK(r.status == MC_OK && r.written == 0);
  db = read_db(&x);
  if (db) {
    ipod_playlist *mp = ipod_db_find_playlist(db, "Mix");
    CHECK(mp && mp->n_items == 2);
    if (mp && mp->n_items == 2) {
      ipod_track *t0 = ipod_db_find_track(db, mp->items[0]);
      CHECK(t0 && t0->title && !strcmp(t0->title, "Three"));
    }
    ipod_db_free(db);
  }

  mc_tethered_free(x.t);
  mc_library_close(x.lib);
  mc_plugins_unload();
  mc_settings_free(x.s);
  mc_mutex_destroy(&x.lock);
  int failed = test_failures;
  if (!failed) mc_remove_tree(x.base);
  return TEST_RESULT();
}
