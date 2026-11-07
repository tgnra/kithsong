/* The device layer on a fake iPod photo: put, update, replace, remove, playlists, commit,
 * kithsong.json and the ArtworkDB, then read back from a fresh open. */
#include <stdlib.h>

#include <stb_image_write.h>

#include "fixtures.h"
#include "kithipod/artwork.h"
#include "kithipod/device.h"

static void jpg_sink(void *ctx, void *data, int size) { ipod_buf_put((ipod_buf *)ctx, data, (size_t)size); }

static void make_cover(ipod_buf *out, uint8_t r) {
  uint8_t px[64 * 48 * 3];
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++) {
      uint8_t *p = px + (y * 64 + x) * 3;
      p[0] = r;
      p[1] = (uint8_t)(x * 4);
      p[2] = (uint8_t)(y * 5);
    }
  stbi_write_jpg_to_func(jpg_sink, out, 64, 48, 3, px, 90);
}

static char *make_file(const char *dir, const char *name, size_t size, uint8_t fill) {
  char *p = ipod_path_join(dir, name);
  uint8_t *d = malloc(size);
  memset(d, fill, size);
  test_write(p, d, size);
  free(d);
  return p;
}

static char *rel_exists(const char *mount, const char *rel, int64_t *size) {
  char *p = ipod_path_join(mount, rel);
  if (size) *size = ipod_file_size(p);
  if (!ipod_file_exists(p)) {
    free(p);
    return NULL;
  }
  return p;
}

/* Counts mhii in an ArtworkDB and checks the first one's song id. */
static int artdb_images(const char *mount, uint64_t *first_dbid) {
  char *p = ipod_path_join(mount, "iPod_Control/Artwork/ArtworkDB");
  size_t len;
  uint8_t *d = ipod_read_file(p, &len);
  free(p);
  if (!d) return -1;
  int n = -1;
  if (len > 0x84 && memcmp(d, "mhfd", 4) == 0 && ipod_rd32(d + 8) == len && ipod_rd32(d + 20) == 3) {
    size_t sd = ipod_rd32(d + 4);
    if (memcmp(d + sd, "mhsd", 4) == 0 && ipod_rd16(d + sd + 12) == 1) {
      size_t li = sd + ipod_rd32(d + sd + 4);
      n = (int)ipod_rd32(d + li + 8);
      size_t ii = li + ipod_rd32(d + li + 4);
      if (n > 0 && memcmp(d + ii, "mhii", 4) == 0) {
        *first_dbid = ipod_rd64(d + ii + 20);
        /* Two sizes: each an mhod 2 with an mhni naming its .ithmb. */
        if (ipod_rd32(d + ii + 12) != 2) n = -2;
        size_t mo = ii + ipod_rd32(d + ii + 4);
        if (memcmp(d + mo, "mhod", 4) != 0 || ipod_rd16(d + mo + 12) != 2) n = -3;
        size_t ni = mo + 0x18;
        if (memcmp(d + ni, "mhni", 4) != 0 || ipod_rd32(d + ni + 16) != 1017 || ipod_rd16(d + ni + 34) != 56) n = -4;
      }
    }
  }
  free(d);
  return n;
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  char *mount = test_scratch(root, "ipod");
  char *src = test_scratch(root, "src");
  test_fake_ipod(mount, "MA079");

  CHECK_EQ(ipod_probe(mount, NULL), IPOD_ERR_NOT_FOUND); /* no iTunesDB yet */
  ipod_error err = {0};
  CHECK(ipod_open(mount, 0, &err) == NULL);
  CHECK_EQ(err.code, IPOD_ERR_NOT_FOUND);
  ipod_device *dev = ipod_open(mount, IPOD_OPEN_CREATE, &err);
  CHECK(dev != NULL);
  if (!dev) {
    fprintf(stderr, "open: %s\n", err.message);
    return 1;
  }
  ipod_device_info info;
  CHECK_EQ(ipod_device_info_get(dev, &info, NULL), 0);
  CHECK_STR(info.id, "guid:000A2700137F5D2A");
  CHECK_STR(info.name, "iPod");
  CHECK(info.artwork);
  CHECK(info.generation == IPOD_GEN_PHOTO);
  CHECK(info.capacity > 0);
  CHECK(info.max_bits == 16 && info.max_channels == 2 && info.max_sample_rate == 48000);
  CHECK_EQ(ipod_db_set_name(ipod_device_db(dev), "Tania's iPod"), 0);

  ipod_buf cover = {0};
  make_cover(&cover, 200);
  CHECK(cover.len > 100);
  char *f1 = make_file(src, "one.mp3", 5000, 0x11);
  char *f2 = make_file(src, "two.m4a", 7000, 0x22);
  ipod_tags tags = {0};
  tags.title = "First Song";
  tags.artist = "Artist";
  tags.album = "Album";
  tags.year = 2001;
  tags.track_number = 1;
  tags.track_total = 9;
  tags.cover = cover.data;
  tags.cover_len = cover.len;
  ipod_put_info put = {0};
  put.tags = &tags;
  put.codec = "mp3";
  put.vbr = 1;
  put.length_ms = 200000;
  put.bitrate_kbps = 245;
  put.sample_rate = 44100;
  put.play_count = 3;
  put.last_played = 1700000000;
  put.track_uid = "track-uid-1";
  put.file_uid = "file-uid-1";
  put.format = "mp3";
  put.setting = "v0";
  put.sha256 = "aa11";
  char k1[IPOD_KEY_LEN], k2[IPOD_KEY_LEN];
  CHECK_EQ(ipod_device_put(dev, f1, &put, k1, &err), 0);
  ipod_tags tags2 = {0};
  tags2.title = "Second Song";
  tags2.artist = "Artist";
  put.tags = &tags2;
  put.codec = "alac";
  put.track_uid = "track-uid-2";
  put.file_uid = "file-uid-2";
  put.format = "alac";
  put.setting = NULL;
  put.sha256 = "bb22";
  CHECK_EQ(ipod_device_put(dev, f2, &put, k2, &err), 0);
  CHECK(strcmp(k1, k2) != 0 && strlen(k1) == 16);
  const char *fav[] = {k2, k1, "ffffffffffffffff"};
  CHECK_EQ(ipod_device_set_playlist(dev, "Favourites", fav, 3, NULL), 0);

  char *p1 = ipod_device_track_path(dev, k1);
  CHECK(p1 && ipod_file_exists(p1) && ipod_file_size(p1) == 5000);
  CHECK(p1 && strstr(p1, "iPod_Control/Music/F") && strstr(p1, ".mp3"));
  CHECK_EQ(ipod_device_commit(dev, &err), 0);
  ipod_device_close(dev);

  CHECK_EQ(ipod_probe(mount, NULL), IPOD_SUPPORTED);
  int64_t sz;
  char *x = rel_exists(mount, "iPod_Control/Artwork/F1017_1.ithmb", &sz);
  CHECK(x && sz == 56 * 56 * 2);
  free(x);
  x = rel_exists(mount, "iPod_Control/Artwork/F1016_1.ithmb", &sz);
  CHECK(x && sz == 140 * 140 * 2);
  free(x);
  x = rel_exists(mount, "iPod_Control/kithsong.json", NULL);
  CHECK(x != NULL);
  free(x);

  /* Read it all back. */
  dev = ipod_open(mount, 0, &err);
  CHECK(dev != NULL);
  if (!dev) return 1;
  ipod_db *db = ipod_device_db(dev);
  CHECK_EQ(db->n_tracks, 2);
  CHECK_STR(ipod_db_name(db), "Tania's iPod");
  ipod_track *t1 = ipod_device_find(dev, k1), *t2 = ipod_device_find(dev, k2);
  CHECK(t1 && t2);
  if (t1 && t2) {
    CHECK_STR(t1->title, "First Song");
    CHECK_EQ(t1->year, 2001);
    CHECK_EQ(t1->size, 5000);
    CHECK_EQ(t1->filetype, IPOD_FILETYPE_MP3);
    CHECK_EQ(t1->type1, 1);
    CHECK_EQ(t1->play_count, 3);
    CHECK_EQ(t1->last_played, ipod_unix_to_mac(1700000000, ipod_local_tz_offset()));
    CHECK_EQ(t1->has_artwork, 1);
    CHECK_EQ(t1->artwork_count, 1);
    CHECK_EQ(t1->artwork_size, cover.len);
    CHECK_EQ(t2->has_artwork, 2);
    CHECK_EQ(t2->filetype, IPOD_FILETYPE_M4A);
    CHECK_STR(t2->filetype_desc, "Apple Lossless audio file");
    uint64_t first = 0;
    CHECK_EQ(artdb_images(mount, &first), 1);
    CHECK(first == t1->dbid);
  }
  const ipod_kith_entry *ke = ipod_device_kith_entry(dev, k1);
  CHECK(ke && strcmp(ke->track_uid, "track-uid-1") == 0 && strcmp(ke->sha256, "aa11") == 0);
  CHECK(ke && strcmp(ke->format, "mp3") == 0 && ke->setting && strcmp(ke->setting, "v0") == 0);
  CHECK(ke && strcmp(ke->file_uid, "file-uid-1") == 0);
  ipod_playlist *fp = ipod_db_find_playlist(db, "Favourites");
  CHECK(fp && fp->n_items == 2 && t2 && fp->items[0] == t2->id);

  /* Update tags and plays; replace a file; remove a track. */
  ipod_tags tags3 = tags;
  tags3.title = "First Song (Remastered)";
  tags3.cover = NULL;
  tags3.cover_len = 0;
  ipod_update_info up = {0};
  up.tags = &tags3;
  up.set_plays = 1;
  up.play_count = 8;
  up.last_played = 1700100000;
  CHECK_EQ(ipod_device_update(dev, k1, &up, NULL), 0);
  char *old2 = ipod_device_track_path(dev, k2);
  char *f3 = make_file(src, "three.m4a", 9000, 0x33);
  put.tags = &tags2;
  put.replace_key = k2;
  put.sha256 = "cc33";
  char k2b[IPOD_KEY_LEN];
  CHECK_EQ(ipod_device_put(dev, f3, &put, k2b, NULL), 0);
  CHECK_STR(k2b, k2);
  CHECK(old2 && !ipod_file_exists(old2));
  char *new2 = ipod_device_track_path(dev, k2);
  CHECK(new2 && ipod_file_size(new2) == 9000);
  CHECK_EQ(ipod_device_remove(dev, k1, NULL), 0);
  CHECK(p1 && !ipod_file_exists(p1));
  CHECK(ipod_device_find(dev, k1) == NULL);
  CHECK(ipod_device_kith_entry(dev, k1) == NULL);
  CHECK_EQ(ipod_device_remove(dev, k1, NULL), IPOD_ERR_NOT_FOUND);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  ipod_device_close(dev);

  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (dev) {
    db = ipod_device_db(dev);
    CHECK_EQ(db->n_tracks, 1);
    t2 = ipod_device_find(dev, k2);
    CHECK(t2 && t2->size == 9000);
    ke = ipod_device_kith_entry(dev, k2);
    CHECK(ke && strcmp(ke->sha256, "cc33") == 0);
    fp = ipod_db_find_playlist(db, "Favourites");
    CHECK(fp && fp->n_items == 1);
    uint64_t first = 0;
    CHECK_EQ(artdb_images(mount, &first), 0); /* the only cover went with its track */
    CHECK_EQ(ipod_device_set_playlist(dev, "Favourites", NULL, 0, NULL), 0);
    CHECK(ipod_db_find_playlist(db, "Favourites") == NULL);
    ipod_device_close(dev);
  }

  /* Models the library refuses, and unknown ones only on request. */
  char *hashed = test_scratch(root, "classic");
  test_fake_ipod(hashed, "MB029");
  CHECK(ipod_open(hashed, IPOD_OPEN_CREATE | IPOD_OPEN_ALLOW_UNKNOWN, &err) == NULL);
  CHECK_EQ(err.code, IPOD_ERR_UNSUPPORTED);
  char *unknown = test_scratch(root, "unknown");
  test_fake_ipod(unknown, "MA999");
  CHECK(ipod_open(unknown, IPOD_OPEN_CREATE, &err) == NULL);
  dev = ipod_open(unknown, IPOD_OPEN_CREATE | IPOD_OPEN_ALLOW_UNKNOWN, &err);
  CHECK(dev != NULL);
  if (dev) {
    CHECK_EQ(ipod_device_info_get(dev, &info, NULL), 0);
    CHECK(!info.artwork);
    ipod_device_close(dev);
  }

  ipod_buf_free(&cover);
  free(f1);
  free(f2);
  free(f3);
  free(p1);
  free(old2);
  free(new2);
  free(hashed);
  free(unknown);
  free(src);
  free(mount);
  return TEST_RESULT();
}
