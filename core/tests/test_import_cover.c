/* Which cover an import gives an album, against a fake MusicBrainz (mb_set_fake) and a fake
 * cover download (mc_coverart_set_fake). Local art, a cover file next to the files or a
 * picture embedded in one, wins unless it's low resolution (MC_COVER_LOW_RES) and the
 * download is bigger:
 *
 *   - "Keep": an 800 px cover.png next to the files is kept, and nothing is downloaded;
 *   - "Upgrade": a 300 px picture embedded in the first track (the second has none) is
 *     replaced by the 1200 px download;
 *   - "Same": a 300 px cover.png is kept when the download is 300 px too (and so it would be
 *     if it were smaller);
 *   - "Bare": no local art at all, so the download is the cover;
 *   - "Discs/CD1": files in a disc folder take the 800 px cover of the album folder above
 *     it over the disc folder's own 200 px one, and nothing is downloaded.
 *
 * The images are PNGs with no pixels: what's measured is the header. But for the last test: a
 * cover the user chose (real, as that's decoded) outlasts an import that brings its own, or one
 * that would otherwise download art because the folder has none.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "kithcore/edit.h"
#include "kithcore/kithcore.h"
#include "tagger/coverart.h"
#include "tagger/musicbrainz.h"
#include "tagger/tag_io.h"
#include "util/platform.h"
#include "test.h"
#include "test_image.h"

enum { KEEP, UPGRADE, SAME, BARE, DISCS, ALBUMS };

static const char *const k_titles[ALBUMS] = {"Keep", "Upgrade", "Same", "Bare", "Discs"};
static const char *const k_folders[ALBUMS] = {"Keep", "Upgrade", "Same", "Bare", "Discs/CD1"};
static const char *const k_ids[ALBUMS] = {
  "a1111111-2222-3333-4444-555555555555", "a2222222-2222-3333-4444-555555555555",
  "a3333333-2222-3333-4444-555555555555", "a4444444-2222-3333-4444-555555555555",
  "a5555555-2222-3333-4444-555555555555"};

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
 * stops) and IEND. tag, the colour type (2 RGB, 6 RGBA), tells two of one size apart. */
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

/* A tenth of a second of silent 8 kHz mono WAV, tagged, with pic embedded unless NULL. */
static int write_track(const char *path, const char *album, const char *title, int track,
                       const unsigned char *pic) {
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
  tags.artist = (char *)"Band";
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

static const char *const k_tracks[2] = {"One", "Two"};

/* The album's two tracks in src/<folder>, the first embedding pic unless NULL. */
static int write_album(const char *src, int which, const unsigned char *pic) {
  char dir[1300], path[1400];
  snprintf(dir, sizeof dir, "%s/%s", src, k_folders[which]);
  if (mc_mkdirs(dir) != MC_OK) return 0;
  for (int i = 0; i < 2; i++) {
    snprintf(path, sizeof path, "%s/0%d %s.wav", dir, i + 1, k_tracks[i]);
    if (!write_track(path, k_titles[which], k_tracks[i], i + 1, i == 0 ? pic : NULL)) return 0;
  }
  return 1;
}

/* --- Fake MusicBrainz and downloads --- */

static int album_of(const char *title_or_id) {
  for (int i = 0; title_or_id && i < ALBUMS; i++)
    if (strcmp(title_or_id, k_titles[i]) == 0 || strcmp(title_or_id, k_ids[i]) == 0) return i;
  return -1;
}

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist;
  (void)track_count;
  int which = album_of(album);
  *n = 0;
  if (which >= 0 && max > 0) memcpy(ids[(*n)++], k_ids[which], MC_UID_STRLEN);
  return MC_OK;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  int which = album_of(id);
  if (which < 0) return MC_ERR_NOT_FOUND;
  out->id = mc_strdup(id);
  out->title = mc_strdup(k_titles[which]);
  out->artist = mc_strdup("Band");
  out->primary_artist = mc_strdup(out->artist);
  char group[] = "b0000000-2222-3333-4444-555555555555";
  group[1] = (char)('1' + which);
  out->releasegroup_id = mc_strdup(group);
  out->release_type = mc_strdup("Album");
  out->year = out->original_year = 2010;
  out->disc_total = 1;
  out->tracks = calloc(2, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  for (int i = 0; i < 2; i++) {
    mb_track *t = &out->tracks[out->track_count++];
    char tid[] = "c0000000-2222-3333-4444-555555555555", rid[] = "d0000000-2222-3333-4444-555555555555";
    tid[1] = rid[1] = (char)('1' + which);
    tid[2] = rid[2] = (char)('1' + i);
    t->id = mc_strdup(tid);
    t->recording_id = mc_strdup(rid);
    t->title = mc_strdup(k_tracks[i]);
    t->artist = mc_strdup("Band");
    t->disc = 1;
    t->number = i + 1;
  }
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, NULL};

static mc_mutex g_lock;
static int g_fetches[ALBUMS];                      /* downloads asked for, by album */
static unsigned char g_download[ALBUMS][PNG_LEN]; /* what each would get */

static mc_status fake_fetch(const char *mb_albumid, const char *mb_releasegroupid, const char *artist,
                            const char *album, mc_buffer *img, const char **ext) {
  (void)mb_releasegroupid;
  (void)artist;
  (void)album;
  int which = album_of(mb_albumid);
  if (which < 0) return MC_ERR_NOT_FOUND;
  mc_mutex_lock(&g_lock);
  g_fetches[which]++;
  mc_mutex_unlock(&g_lock);
  if (!(img->data = malloc(PNG_LEN + 1))) return MC_ERR_NOMEM;
  memcpy(img->data, g_download[which], PNG_LEN);
  img->data[PNG_LEN] = '\0';
  img->len = PNG_LEN;
  *ext = "png";
  return MC_OK;
}

/* --- Session --- */

/* Waits until the session has count items, all imported. */
static int wait_done(mc_import *imp, int count) {
  uint64_t deadline = mc_mono_ms() + 30000;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), done = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      done += it->state == MC_IMPORT_DONE;
      mc_import_item_free(it);
    }
    if (n == count && done == n) return 1;
    mc_sleep_ms(10);
  }
  return 0;
}

/* The cover in the library folder of the album titled title, or NULL (free with mc_free). */
static char *album_cover(mc_library *lib, const char *title) {
  mc_album_list *list = NULL;
  char *cover = NULL;
  if (mc_library_list_albums(lib, &list) != MC_OK) return NULL;
  for (int i = 0; i < list->count; i++)
    if (list->items[i].title && strcmp(list->items[i].title, title) == 0 && list->items[i].folder)
      cover = mc_coverart_find(list->items[i].folder);
  mc_album_list_free(list);
  return cover;
}

/* Takes track Two of the album with file path rel off the library, so an import brings it back. */
static void forget_two(mc_library *lib, const char *rel, const char *abs) {
  char sql[1024];
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "DELETE FROM tracks WHERE uid = (SELECT track_uid FROM files WHERE path = '%s');", rel);
  CHECK(sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);
  CHECK(mc_remove(abs) == MC_OK);
}

static void test_chosen(mc_library *lib, const char *root, const char *lib_dir) {
  char album[MC_UID_STRLEN] = "", src[1200], path[1400], two[1400], want[65], have[65];
  mc_album_list *list = NULL;
  CHECK(mc_library_list_albums(lib, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].title, "Bare")) snprintf(album, sizeof album, "%s", list->items[i].uid);
  mc_album_list_free(list);
  CHECK(album[0]);
  test_image mine = test_image_make(0, 1, 32);
  CHECK(mc_album_art_set(lib, album, mine.data, mine.len) == MC_OK);
  char *cover = album_cover(lib, "Bare");
  CHECK(cover && mc_coverart_hash(cover, want) == MC_OK);
  mc_free(cover);

  /* Its second track comes again, from a folder with a big cover of its own. */
  const char *rel = "Band/Bare (2010)/02 Two.wav";
  snprintf(two, sizeof two, "%s/%s", lib_dir, rel);
  snprintf(src, sizeof src, "%s/again", root);
  CHECK(write_album(src, BARE, NULL));
  unsigned char big[PNG_LEN];
  png(big, 800, 6);
  snprintf(path, sizeof path, "%s/Bare/cover.png", src);
  CHECK(write_file(path, big, PNG_LEN));
  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  const char *paths[] = {src};
  for (int gone = 0; gone < 2; gone++) {
    /* The second time this device has lost the file, as it hasn't got it yet when the choice was
     * made elsewhere: the folder still gets no other, and nothing's downloaded. */
    if (gone) {
      cover = album_cover(lib, "Bare");
      CHECK(cover && mc_remove(cover) == MC_OK);
      mc_free(cover);
    }
    forget_two(lib, rel, two);
    mc_mutex_lock(&g_lock);
    int fetches = g_fetches[BARE];
    mc_mutex_unlock(&g_lock);
    mc_import *imp = NULL;
    CHECK(mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
    CHECK(imp && wait_done(imp, 1));
    mc_import_free(imp);
    CHECK(mc_file_exists(two));
    mc_mutex_lock(&g_lock);
    CHECK(g_fetches[BARE] == fetches);
    mc_mutex_unlock(&g_lock);
    cover = album_cover(lib, "Bare");
    if (gone) CHECK(cover == NULL);
    else CHECK(cover && mc_coverart_hash(cover, have) == MC_OK && !strcmp(have, want));
    mc_free(cover);
  }
  free(mine.data);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mc_mutex_init(&g_lock);
  char root[1024], src[1100], lib_dir[1100], path[1400];
  snprintf(root, sizeof root, "%s/test_import_cover.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(lib_dir) == MC_OK);

  unsigned char big[PNG_LEN], small[PNG_LEN], tiny[PNG_LEN];
  png(big, 800, 2);
  png(small, 300, 2);
  png(tiny, 200, 2);
  CHECK(800 >= MC_COVER_LOW_RES && 300 < MC_COVER_LOW_RES);
  mc_buffer b = {(char *)big, PNG_LEN};
  CHECK(mc_coverart_short_side(&b) == 800);

  for (int i = 0; i < ALBUMS; i++) png(g_download[i], 1200, 2);
  png(g_download[SAME], 300, 6); /* as big as the local one, but not the same bytes */

  CHECK(write_album(src, KEEP, NULL));
  snprintf(path, sizeof path, "%s/Keep/cover.png", src);
  CHECK(write_file(path, big, PNG_LEN));
  CHECK(write_album(src, UPGRADE, small));
  CHECK(write_album(src, SAME, NULL));
  snprintf(path, sizeof path, "%s/Same/cover.png", src);
  CHECK(write_file(path, small, PNG_LEN));
  CHECK(write_album(src, BARE, NULL));
  CHECK(write_album(src, DISCS, NULL));
  snprintf(path, sizeof path, "%s/Discs/cover.png", src);
  CHECK(write_file(path, big, PNG_LEN));
  snprintf(path, sizeof path, "%s/Discs/CD1/cover.png", src);
  CHECK(write_file(path, tiny, PNG_LEN));

  mb_set_fake(&k_fake);
  mc_coverart_set_fake(fake_fetch);
  mc_library *lib = NULL;
  mc_import *imp = NULL;
  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  const char *paths[] = {src};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  CHECK(imp && wait_done(imp, ALBUMS));
  mc_import_free(imp);

  mc_mutex_lock(&g_lock);
  CHECK(g_fetches[KEEP] == 0);
  CHECK(g_fetches[UPGRADE] == 1);
  CHECK(g_fetches[SAME] == 1);
  CHECK(g_fetches[BARE] == 1);
  CHECK(g_fetches[DISCS] == 0);
  mc_mutex_unlock(&g_lock);

  char *cover = album_cover(lib, "Keep");
  CHECK(file_equals(cover, big, PNG_LEN));
  mc_free(cover);
  cover = album_cover(lib, "Upgrade");
  CHECK(file_equals(cover, g_download[UPGRADE], PNG_LEN));
  mc_free(cover);
  cover = album_cover(lib, "Same");
  CHECK(file_equals(cover, small, PNG_LEN));
  mc_free(cover);
  cover = album_cover(lib, "Bare");
  CHECK(file_equals(cover, g_download[BARE], PNG_LEN));
  mc_free(cover);
  cover = album_cover(lib, "Discs");
  CHECK(file_equals(cover, big, PNG_LEN));
  mc_free(cover);

  /* The originals are as they were: the embedded picture is still there, the covers too. */
  unsigned char *pic = NULL;
  size_t pic_len = 0;
  snprintf(path, sizeof path, "%s/Upgrade/01 One.wav", src);
  CHECK(mc_tags_read_picture(path, &pic, &pic_len) == MC_OK && pic_len == PNG_LEN &&
        memcmp(pic, small, PNG_LEN) == 0);
  free(pic);
  snprintf(path, sizeof path, "%s/Same/cover.png", src);
  CHECK(file_equals(path, small, PNG_LEN));

  test_chosen(lib, root, lib_dir);

  mc_library_close(lib);
  mc_coverart_set_fake(NULL);
  mb_set_fake(NULL);
  mc_mutex_destroy(&g_lock);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
