/* Play Counts and OTGPlaylistInfo: parsing every entry size, and the device layer's
 * handling (mapped to keys at open, cleared or folded in at commit). The files are built
 * from the documented layout. */
#include <stdlib.h>

#include "fixtures.h"
#include "kithipod/device.h"
#include "kithipod/playcounts.h"

static void put_playcounts(ipod_buf *b, uint32_t entry_len, uint32_t n, const uint32_t (*e)[7]) {
  ipod_buf_tag(b, "mhdp");
  ipod_buf_le32(b, 0x60);
  ipod_buf_le32(b, entry_len);
  ipod_buf_le32(b, n);
  ipod_buf_zero(b, 0x60 - 16);
  for (uint32_t i = 0; i < n; i++) {
    uint8_t *p = ipod_buf_grow(b, entry_len);
    for (uint32_t k = 0; k < 7 && 4 * (k + 1) <= entry_len; k++) ipod_wr32(p + 4 * k, e[i][k]);
  }
}

static void put_otg(ipod_buf *b, uint32_t n, const uint32_t *idx) {
  ipod_buf_tag(b, "mhpo");
  ipod_buf_le32(b, 0x14);
  ipod_buf_le32(b, 4);
  ipod_buf_le32(b, n);
  ipod_buf_le32(b, 0);
  for (uint32_t i = 0; i < n; i++) ipod_buf_le32(b, idx[i]);
}

static void test_parse(void) {
  /* play count, last played, bookmark, rating, unknown, skips, last skipped */
  const uint32_t e[3][7] = {{3, 3700000000u, 0, 60, 0, 1, 3699999000u},
                            {0, 0, 0, 0, 0, 0, 0},
                            {1, 3700000500u, 12000, 100, 0, 0, 0}};
  static const uint32_t sizes[] = {0x0C, 0x10, 0x14, 0x1C, 0x20};
  for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
    ipod_buf b = {0};
    put_playcounts(&b, sizes[s], 3, e);
    ipod_playcounts pc;
    CHECK_EQ(ipod_playcounts_parse(b.data, b.len, &pc, NULL), 0);
    CHECK_EQ(pc.n, 3);
    CHECK_EQ(pc.entry_len, sizes[s]);
    CHECK_EQ(pc.entries[0].play_count, 3);
    CHECK_EQ(pc.entries[0].last_played, 3700000000u);
    CHECK_EQ(pc.entries[2].bookmark_ms, 12000);
    CHECK_EQ(pc.entries[0].rating, sizes[s] >= 0x10 ? 60 : -1);
    CHECK_EQ(pc.entries[0].skip_count, sizes[s] >= 0x1C ? 1 : 0);
    CHECK_EQ(pc.entries[0].last_skipped, sizes[s] >= 0x1C ? 3699999000u : 0);
    ipod_playcounts_free(&pc);
    /* Truncated: refused. */
    CHECK_EQ(ipod_playcounts_parse(b.data, b.len - 1, &pc, NULL), IPOD_ERR_FORMAT);
    ipod_buf_free(&b);
  }
  ipod_playcounts pc;
  CHECK_EQ(ipod_playcounts_parse((const uint8_t *)"mhpo", 4, &pc, NULL), IPOD_ERR_FORMAT);

  ipod_buf b = {0};
  const uint32_t idx[] = {2, 0, 2};
  put_otg(&b, 3, idx);
  ipod_otg_list l;
  CHECK_EQ(ipod_otg_parse(b.data, b.len, &l, NULL), 0);
  CHECK(l.n == 3 && l.indexes[0] == 2 && l.indexes[1] == 0 && l.indexes[2] == 2);
  free(l.indexes);
  CHECK_EQ(ipod_otg_parse(b.data, b.len - 2, &l, NULL), IPOD_ERR_FORMAT);
  ipod_buf_free(&b);

  /* Mac time conversions. */
  CHECK_EQ(ipod_mac_to_unix(IPOD_MAC_EPOCH_OFFSET + 100, 0), 100);
  CHECK_EQ(ipod_mac_to_unix(IPOD_MAC_EPOCH_OFFSET + 100, 3600), 100 - 3600);
  CHECK_EQ(ipod_unix_to_mac(100, 3600), IPOD_MAC_EPOCH_OFFSET + 3700);
  CHECK_EQ(ipod_mac_to_unix(0, 3600), 0);
  CHECK_EQ(ipod_unix_to_mac(0, 3600), 0);
}

/* A fake iPod with three tracks, plus the files the iPod writes between syncs. */
static char *setup(const char *root, const char *name, char keys[3][IPOD_KEY_LEN]) {
  char *mount = test_scratch(root, name);
  test_fake_ipod(mount, "M9282");
  ipod_db *db = ipod_db_new("Pod");
  for (int i = 0; i < 3; i++) {
    ipod_track *t = ipod_track_new();
    char title[16];
    snprintf(title, sizeof title, "Song %d", i);
    ipod_set_str(&t->title, title);
    t->play_count = 10;
    t->last_played = 3600000000u;
    ipod_db_add_track(db, t);
    ipod_track_key(t, keys[i]);
  }
  char *path = ipod_path_join(mount, "iPod_Control/iTunes/iTunesDB");
  CHECK_EQ(ipod_db_write(db, path, NULL, NULL), 0);
  free(path);
  ipod_db_free(db);

  const uint32_t e[3][7] = {{2, 3700000000u, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}, {1, 3700000900u, 0, 80, 0, 0, 0}};
  ipod_buf b = {0};
  put_playcounts(&b, 0x1C, 3, e);
  path = ipod_path_join(mount, "iPod_Control/iTunes/Play Counts");
  test_write(path, b.data, b.len);
  free(path);
  ipod_buf_free(&b);

  const uint32_t otg0[] = {2, 0}, otg1[] = {1};
  put_otg(&b, 2, otg0);
  path = ipod_path_join(mount, "iPod_Control/iTunes/OTGPlaylistInfo");
  test_write(path, b.data, b.len);
  free(path);
  ipod_buf_free(&b);
  put_otg(&b, 1, otg1);
  path = ipod_path_join(mount, "iPod_Control/iTunes/OTGPlaylistInfo_1");
  test_write(path, b.data, b.len);
  free(path);
  ipod_buf_free(&b);
  return mount;
}

static int exists_rel(const char *mount, const char *rel) {
  char *p = ipod_path_join(mount, rel);
  int r = ipod_file_exists(p);
  free(p);
  return r;
}

static void test_device(const char *root) {
  char keys[3][IPOD_KEY_LEN];
  /* Taken in: plays and lists are reported, then cleared without touching the counts. */
  char *mount = setup(root, "taken", keys);
  ipod_error err = {0};
  ipod_device *dev = ipod_open(mount, 0, &err);
  CHECK(dev != NULL);
  if (!dev) {
    fprintf(stderr, "open: %s\n", err.message);
    free(mount);
    return;
  }
  size_t n;
  const ipod_play *plays = ipod_device_plays(dev, &n);
  CHECK_EQ(n, 2);
  if (n == 2) {
    CHECK_STR(plays[0].key, keys[0]);
    CHECK_EQ(plays[0].count, 2);
    CHECK_EQ(plays[0].last_played, ipod_mac_to_unix(3700000000u, ipod_local_tz_offset()));
    CHECK_EQ(plays[0].rating, 0);
    CHECK_STR(plays[1].key, keys[2]);
    CHECK_EQ(plays[1].rating, 80);
  }
  size_t pc_len;
  CHECK(ipod_device_playcounts_bytes(dev, &pc_len) != NULL && pc_len == 0x60 + 3 * 0x1C);
  const ipod_otg_playlist *otg = ipod_device_otg_playlists(dev, &n);
  CHECK_EQ(n, 2);
  if (n == 2) {
    CHECK_STR(otg[0].name, "On-The-Go 1");
    CHECK(otg[0].n == 2 && strcmp(otg[0].keys[0], keys[2]) == 0 && strcmp(otg[0].keys[1], keys[0]) == 0);
    CHECK(otg[1].n == 1 && strcmp(otg[1].keys[0], keys[1]) == 0);
  }
  ipod_device_done(dev, IPOD_DONE_PLAYS | IPOD_DONE_PLAYLISTS);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0); /* nothing else changed: only clears */
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/Play Counts"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo_1"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/iTunesDB.kithsong-backup")); /* not rewritten */
  ipod_device_close(dev);
  free(mount);

  /* Not taken in, but the database is rewritten: folded in so the iPod keeps them. */
  mount = setup(root, "folded", keys);
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (!dev) {
    free(mount);
    return;
  }
  ipod_update_info up = {0};
  up.set_rating = 1;
  up.rating = 40;
  CHECK_EQ(ipod_device_update(dev, keys[1], &up, NULL), 0);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/Play Counts"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo"));
  CHECK(exists_rel(mount, "iPod_Control/iTunes/iTunesDB.kithsong-backup"));
  CHECK_EQ(ipod_device_commit(dev, NULL), 0); /* again: nothing doubles */
  ipod_device_close(dev);
  char *path = ipod_path_join(mount, "iPod_Control/iTunes/iTunesDB");
  ipod_db *db = ipod_db_read(path, NULL);
  free(path);
  CHECK(db != NULL);
  if (db) {
    CHECK_EQ(db->tracks[0]->play_count, 12);
    CHECK_EQ(db->tracks[0]->last_played, 3700000000u);
    CHECK_EQ(db->tracks[1]->play_count, 10);
    CHECK_EQ(db->tracks[1]->rating, 40);
    CHECK_EQ(db->tracks[2]->play_count, 11);
    CHECK_EQ(db->n_playlists, 3);
    ipod_playlist *o1 = ipod_db_find_playlist(db, "On-The-Go 1");
    CHECK(o1 && o1->n_items == 2 && o1->items[0] == db->tracks[2]->id);
    CHECK(ipod_db_find_playlist(db, "On-The-Go 2") != NULL);
  }
  ipod_db_free(db);
  free(mount);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  test_parse();
  test_device(root);
  return TEST_RESULT();
}
