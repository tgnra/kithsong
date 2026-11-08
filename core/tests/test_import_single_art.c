/* Which cover an import gives a single (a loose track), against a fake MusicBrainz
 * (mb_set_fake) and a fake cover download (mc_coverart_set_fake):
 *
 *   - "Hey Ya!", in Loose/ next to an 800 px cover.png, embeds a picture of its own: the
 *     picture is its cover, not the folder's, and nothing is downloaded;
 *   - "Roses", in Loose/ too, embeds none: the folder's cover is its, as for an album;
 *   - "Ms. Jackson", alone in Bare/ with no art at all: the Cover Art Archive's, downloaded
 *     by the release its search found (a release that can't be looked up);
 *   - "Qwxzzy Demo" matches nothing and has no album tag, but embeds a picture: imported
 *     as-is, it gets an album of its title for the picture to be the cover of;
 *   - "Zzqx Tagged" matches nothing either, embeds nothing, and has an album tag (its title)
 *     and a MusicBrainz release id from an earlier tagger: imported as-is, its cover is
 *     downloaded by that id.
 *
 * Every one shows its cover in the artist's singles (mc_track_info.cover_dir), as the
 * frontends draw it, and its album has art_hash set, which sync sends. The images are PNGs
 * with no pixels: what's measured is the header.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "kithcore/kithcore.h"
#include "tagger/coverart.h"
#include "tagger/musicbrainz.h"
#include "tagger/tag_io.h"
#include "util/platform.h"
#include "test.h"

#define ARTIST_ID "a1111111-2222-3333-4444-555555555555"
#define SINGLE_REL "b1111111-2222-3333-4444-555555555555" /* "Hey Ya!", one track */
#define ALBUM_REL "b2222222-2222-3333-4444-555555555555"  /* "Speakerboxxx", three */
#define GONE_REL "b3333333-2222-3333-4444-555555555555"   /* "Stankonia", not found */
#define TAGGED_REL "b4444444-2222-3333-4444-555555555555" /* in "Zzqx Tagged"'s tags only */

/* --- Files --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  char (*more)[512] = realloc(n->items, (size_t)(n->count + 1) * sizeof *more);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd') remove_tree(path);
    else mc_remove(path);
  }
  free(n.items);
  mc_rmdir(dir);
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void put_u32be(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * (3 - i)));
}

static uint32_t crc32(const unsigned char *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

enum { PNG_LEN = 57 };

/* A size x size PNG with no pixels: its signature, IHDR, an empty IDAT (where a header scan
 * stops) and IEND. tag, the colour type (2 RGB, 6 RGBA), with size tells them apart. */
static void png(unsigned char out[PNG_LEN], uint32_t size, unsigned char tag) {
  memcpy(out, "\x89PNG\r\n\x1a\n", 8);
  put_u32be(out + 8, 13);
  memcpy(out + 12, "IHDR", 4);
  put_u32be(out + 16, size);
  put_u32be(out + 20, size);
  out[24] = 8; /* bit depth */
  out[25] = tag;
  out[26] = out[27] = out[28] = 0; /* compression, filter, interlace */
  put_u32be(out + 29, crc32(out + 12, 17));
  memcpy(out + 33, "\0\0\0\0IDAT", 8);
  put_u32be(out + 41, crc32(out + 37, 4));
  memcpy(out + 45, "\0\0\0\0IEND", 8);
  put_u32be(out + 53, crc32(out + 49, 4));
}

static int write_file(const char *path, const unsigned char *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(data, len, 1, f) == 1;
  return fclose(f) == 0 && ok;
}

static int file_equals(const char *path, const unsigned char *want, size_t len) {
  unsigned char got[128];
  FILE *f = path ? mc_fopen(path, "rb") : NULL;
  if (!f) return 0;
  size_t n = fread(got, 1, sizeof got, f);
  fclose(f);
  return n == len && memcmp(got, want, len) == 0;
}

/* An ID3v2.3 tag holding pic (a PNG) as the front cover, as a WAV's "id3 " chunk carries it. */
static size_t id3_with_picture(unsigned char *out, const unsigned char *pic, size_t pic_len) {
  static const char mime[] = "image/png";
  size_t body = 1 + sizeof mime + 1 + 1 + pic_len; /* encoding, MIME, type, description */
  size_t tag = 10 + body, n = 10;
  memcpy(out, "ID3\x03\x00\x00", 6);
  for (int i = 0; i < 4; i++) out[6 + i] = (unsigned char)((tag >> (7 * (3 - i))) & 0x7F);
  memcpy(out + n, "APIC", 4);
  put_u32be(out + n + 4, (uint32_t)body);
  out[n + 8] = out[n + 9] = 0;
  n += 10;
  out[n++] = 0; /* ISO-8859-1 */
  memcpy(out + n, mime, sizeof mime);
  n += sizeof mime;
  out[n++] = 3; /* front cover */
  out[n++] = 0; /* no description */
  memcpy(out + n, pic, pic_len);
  return n + pic_len;
}

/* A tenth of a second of silent 8 kHz mono WAV by OutKast, called title, with pic embedded
 * unless NULL, and an album tag and release id unless NULL. */
static int write_track(const char *path, const char *title, const unsigned char *pic, const char *album,
                       const char *album_id) {
  enum { DATA_LEN = 1600 };
  unsigned char id3[128];
  size_t id3_len = pic ? id3_with_picture(id3, pic, PNG_LEN) : 0;
  size_t chunk_len = id3_len ? 8 + id3_len + (id3_len & 1) : 0;
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, (uint32_t)(36 + DATA_LEN + chunk_len));
  put_u32(hdr + 16, 16);
  hdr[20] = 1; /* PCM */
  hdr[22] = 1; /* mono */
  put_u32(hdr + 24, 8000);
  put_u32(hdr + 28, 16000);
  hdr[32] = 2;
  hdr[34] = 16;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, DATA_LEN);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char silence[DATA_LEN];
  int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, sizeof silence, 1, f) == 1;
  if (ok && chunk_len) {
    unsigned char chunk_hdr[8] = "id3 ";
    put_u32(chunk_hdr + 4, (uint32_t)id3_len);
    ok = fwrite(chunk_hdr, sizeof chunk_hdr, 1, f) == 1 && fwrite(id3, id3_len, 1, f) == 1 &&
         (!(id3_len & 1) || fputc(0, f) != EOF);
  }
  if (fclose(f) != 0 || !ok) return 0;

  mc_tags tags = {0};
  tags.artist = (char *)"OutKast";
  tags.title = (char *)title;
  tags.album = (char *)album;
  tags.mb_albumid = (char *)album_id;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* --- Fake MusicBrainz --- */

/* Track i of a release: ids made from the release's first letters and i. */
static void fake_track(mb_track *t, char rel, int i, const char *title, int number) {
  char tid[] = "c0000000-2222-3333-4444-555555555555", rid[] = "d0000000-2222-3333-4444-555555555555";
  tid[1] = rid[1] = rel;
  tid[7] = rid[7] = (char)('1' + i);
  t->id = mc_strdup(tid);
  t->recording_id = mc_strdup(rid);
  t->title = mc_strdup(title);
  t->artist = mc_strdup("OutKast");
  t->artist_id = mc_strdup(ARTIST_ID);
  t->disc = 1;
  t->number = number;
}

static void fake_release(mb_release *out, const char *id, const char *title) {
  memset(out, 0, sizeof *out);
  out->id = mc_strdup(id);
  out->title = mc_strdup(title);
  out->artist = mc_strdup("OutKast");
  out->artist_id = mc_strdup(ARTIST_ID);
  out->primary_artist = mc_strdup("OutKast");
  out->primary_artist_id = mc_strdup(ARTIST_ID);
  char rg[] = "e0000000-2222-3333-4444-555555555555";
  rg[1] = id[1];
  out->releasegroup_id = mc_strdup(rg);
  out->year = out->original_year = 2003;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  if (strcmp(id, SINGLE_REL) == 0) {
    fake_release(out, id, "Hey Ya!");
    out->release_type = mc_strdup("Single");
    out->disc_total = 1;
    if (!(out->tracks = calloc(1, sizeof *out->tracks))) return MC_ERR_NOMEM;
    out->track_count = 1;
    fake_track(&out->tracks[0], '1', 0, "Hey Ya!", 1);
    return MC_OK;
  }
  if (strcmp(id, ALBUM_REL) == 0) {
    static const char *const titles[] = {"GhettoMusick", "Roses", "Bowtie"};
    fake_release(out, id, "Speakerboxxx");
    out->release_type = mc_strdup("Album");
    out->disc_total = 1;
    if (!(out->tracks = calloc(3, sizeof *out->tracks))) return MC_ERR_NOMEM;
    out->track_count = 3;
    for (int i = 0; i < 3; i++) fake_track(&out->tracks[i], '2', i, titles[i], i + 1);
    return MC_OK;
  }
  return MC_ERR_NOT_FOUND;
}

static mc_status fake_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                                        int *n) {
  (void)artist;
  *n = 0;
  if (max < 1) return MC_OK;
  const char *rel = NULL, *album = NULL;
  char key = 0;
  int index = 0;
  if (strcmp(title, "Hey Ya!") == 0) rel = SINGLE_REL, album = "Hey Ya!", key = '1', index = 0;
  else if (strcmp(title, "Roses") == 0) rel = ALBUM_REL, album = "Speakerboxxx", key = '2', index = 1;
  else if (strcmp(title, "Ms. Jackson") == 0) rel = GONE_REL, album = "Stankonia", key = '3', index = 3;
  if (!rel) return MC_OK;
  fake_release(out, rel, album);
  out->is_single = 1;
  if (!(out->tracks = calloc(1, sizeof *out->tracks))) return MC_ERR_NOMEM;
  out->track_count = 1;
  fake_track(&out->tracks[0], key, index, title, index + 1);
  *n = 1;
  return MC_OK;
}

static const mb_fake k_fake = {NULL, fake_lookup, fake_search_recordings, NULL, NULL};

/* --- Fake downloads --- */

enum { DL_SINGLE, DL_ALBUM, DL_GONE, DL_TAGGED, DLS };
static const char *const k_dl_ids[DLS] = {SINGLE_REL, ALBUM_REL, GONE_REL, TAGGED_REL};

static mc_mutex g_lock;
static int g_fetches[DLS];                      /* downloads asked for, by release */
static int g_named;                             /* asked with names, for iTunes, but no ids */
static unsigned char g_download[DLS][PNG_LEN]; /* what each would get */

static mc_status fake_fetch(const char *mb_albumid, const char *mb_releasegroupid, const char *artist,
                            const char *album, mc_buffer *img, const char **ext) {
  (void)mb_releasegroupid;
  int which = -1;
  for (int i = 0; mb_albumid && i < DLS; i++)
    if (strcmp(mb_albumid, k_dl_ids[i]) == 0) which = i;
  mc_mutex_lock(&g_lock);
  if (which >= 0) g_fetches[which]++;
  else if (artist || album) g_named++;
  mc_mutex_unlock(&g_lock);
  if (which < 0) return MC_ERR_NOT_FOUND;
  if (!(img->data = malloc(PNG_LEN + 1))) return MC_ERR_NOMEM;
  memcpy(img->data, g_download[which], PNG_LEN);
  img->data[PNG_LEN] = '\0';
  img->len = PNG_LEN;
  *ext = "png";
  return MC_OK;
}

/* --- Session --- */

static mc_import_state item_state(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return MC_IMPORT_FAILED;
  mc_import_state s = it->state;
  mc_import_item_free(it);
  return s;
}

/* Waits until every item has left PENDING, MATCHING and WRITING, or timeout_ms passes. */
static int wait_settled(mc_import *imp, int count, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_state s = item_state(imp, i);
      settled += s != MC_IMPORT_PENDING && s != MC_IMPORT_MATCHING && s != MC_IMPORT_WRITING;
    }
    if (n == count && settled == n) return 1;
    mc_sleep_ms(20);
  }
  return 0;
}

/* The item whose album (a single's title) is title, or -1. */
static int find_item(mc_import *imp, const char *title) {
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it;
    if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
    int same = it->album && strcmp(it->album, title) == 0;
    mc_import_item_free(it);
    if (same) return i;
  }
  return -1;
}

static int accept_as_is(mc_import *imp, const char *title) {
  int idx = find_item(imp, title);
  if (idx < 0 || item_state(imp, idx) != MC_IMPORT_REVIEW) return 0;
  if (mc_import_accept_as_is(imp, idx) != MC_OK) return 0;
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline && item_state(imp, idx) != MC_IMPORT_DONE) mc_sleep_ms(20);
  return item_state(imp, idx) == MC_IMPORT_DONE;
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

enum { TRACKS = 5 };
static const char *const k_titles[TRACKS] = {"Hey Ya!", "Roses", "Ms. Jackson", "Qwxzzy Demo", "Zzqx Tagged"};

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mc_mutex_init(&g_lock);
  mb_set_fake(&k_fake);
  mc_coverart_set_fake(fake_fetch);

  unsigned char embedded[PNG_LEN], demo_pic[PNG_LEN], folder[PNG_LEN];
  png(embedded, 800, 6);
  png(demo_pic, 700, 6);
  png(folder, 800, 2);
  for (int i = 0; i < DLS; i++) png(g_download[i], 1200 + (uint32_t)i, 2);

  char root[1024], src[1100], dir[1200], path[1400], lib_dir[1100];
  snprintf(root, sizeof root, "%s/test_import_single_art.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(lib_dir) == MC_OK);

  snprintf(dir, sizeof dir, "%s/Loose", src);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/cover.png", dir);
  CHECK(write_file(path, folder, PNG_LEN));
  snprintf(path, sizeof path, "%s/1.wav", dir);
  CHECK(write_track(path, k_titles[0], embedded, NULL, NULL));
  snprintf(path, sizeof path, "%s/2.wav", dir);
  CHECK(write_track(path, k_titles[1], NULL, NULL, NULL));
  snprintf(dir, sizeof dir, "%s/Bare", src);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/3.wav", dir);
  CHECK(write_track(path, k_titles[2], NULL, NULL, NULL));
  snprintf(dir, sizeof dir, "%s/Own", src);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/4.wav", dir);
  CHECK(write_track(path, k_titles[3], demo_pic, NULL, NULL));
  snprintf(dir, sizeof dir, "%s/Tagged", src);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/5.wav", dir);
  CHECK(write_track(path, k_titles[4], NULL, k_titles[4], TAGGED_REL));

  /* The embedded picture is read back as written, the way import reads it. */
  {
    mc_buffer b;
    const char *ext = NULL;
    snprintf(path, sizeof path, "%s/Loose/1.wav", src);
    CHECK(mc_coverart_read_embedded(path, &b, &ext) == MC_OK && b.len == PNG_LEN &&
          memcmp(b.data, embedded, PNG_LEN) == 0 && ext && !strcmp(ext, "png"));
    mc_buffer_free(&b);
    snprintf(path, sizeof path, "%s/Loose/2.wav", src);
    CHECK(mc_coverart_read_embedded(path, &b, &ext) == MC_ERR_NOT_FOUND);
  }

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  const char *paths[] = {src};
  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    /* Five singles: three matched and imported by themselves, two left in review. */
    CHECK(wait_settled(imp, TRACKS, 20000));
    for (int i = 0; i < 3; i++) {
      int idx = find_item(imp, k_titles[i]);
      CHECK(idx >= 0 && item_state(imp, idx) == MC_IMPORT_DONE);
    }
    for (int i = 3; i < TRACKS; i++) {
      mc_import_item *it;
      int idx = find_item(imp, k_titles[i]);
      CHECK(idx >= 0 && mc_import_item_get(imp, idx, &it) == MC_OK);
      if (idx >= 0) {
        CHECK(it->kind == MC_IMPORT_SINGLE);
        mc_import_item_free(it);
      }
      CHECK(accept_as_is(imp, k_titles[i]));
    }
    mc_import_free(imp);
  }

  /* Downloads: only where there's no local art, by id, and never by name for as-is tags. */
  CHECK(g_fetches[DL_SINGLE] == 0);
  CHECK(g_fetches[DL_ALBUM] == 0);
  CHECK(g_fetches[DL_GONE] == 1);
  CHECK(g_fetches[DL_TAGGED] == 1);
  CHECK(g_named == 0);

  /* What each single's cover should be. */
  const unsigned char *want[TRACKS] = {embedded, folder, g_download[DL_GONE], demo_pic,
                                       g_download[DL_TAGGED]};
  if (lib) {
    CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE album_uid IS NULL") == 0);
    CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE art_hash IS NULL") == 0);
    CHECK(query_int(lib, "SELECT count(*) FROM albums") == TRACKS);
    /* The as-is single that embeds a picture: an album of its title, no ids. */
    CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE title = 'Qwxzzy Demo'"
                         " AND mb_albumid IS NULL AND mb_releasegroupid IS NULL") == 1);
    snprintf(path, sizeof path, "%s/OutKast/Qwxzzy Demo/Qwxzzy Demo.wav", lib_dir);
    CHECK(mc_file_exists(path));

    mc_artist_list *artists = NULL;
    CHECK(mc_library_list_artists(lib, &artists) == MC_OK && artists && artists->count == 1);
    if (artists && artists->count == 1) {
      mc_track_list *singles = NULL;
      CHECK(mc_library_list_artist_singles(lib, artists->items[0].uid, &singles) == MC_OK && singles &&
            singles->count == TRACKS);
      int seen = 0;
      for (int i = 0; singles && i < singles->count; i++) {
        const mc_track_info *t = &singles->items[i];
        for (int k = 0; k < TRACKS; k++) {
          if (!t->title || strcmp(t->title, k_titles[k]) != 0) continue;
          seen++;
          char *cover = t->cover_dir ? mc_coverart_find(t->cover_dir) : NULL;
          if (!file_equals(cover, want[k], PNG_LEN)) {
            fprintf(stderr, "%s: wrong cover %s\n", k_titles[k], cover ? cover : "(none)");
            CHECK(0);
          }
          mc_free(cover);
        }
      }
      CHECK(seen == TRACKS);
      mc_track_list_free(singles);
    }
    mc_artist_list_free(artists);
    mc_library_close(lib);
  }

  mc_coverart_set_fake(NULL);
  mb_set_fake(NULL);
  if (!test_failures) remove_tree(root);
  mc_mutex_destroy(&g_lock);
  mc_shutdown();
  return TEST_RESULT();
}
