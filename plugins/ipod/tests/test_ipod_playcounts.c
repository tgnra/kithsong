/* Play Counts and OTGPlaylistInfo: parsing every entry size, and the device layer's
 * handling (mapped to keys at open, deleted only once taken in, kept true for a rewritten
 * database until then) and Kithsong's own playlists. The files are built
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

static void ipod_otg_clear_dir(const char *mount) {
  char *dir = ipod_path_join(mount, "iPod_Control/iTunes");
  ipod_otg_clear(dir);
  free(dir);
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
  CHECK_EQ(ipod_device_done(dev, IPOD_DONE_PLAYS | IPOD_DONE_PLAYLISTS, NULL), 0);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0); /* nothing else changed */
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/Play Counts"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo_1"));
  CHECK(!exists_rel(mount, "iPod_Control/iTunes/iTunesDB.kithsong-backup")); /* not rewritten */
  ipod_device_close(dev);
  free(mount);

  /* Not taken in, but the database is rewritten (an added track, a changed one, a removal):
   * Play Counts and the OTG files stay exactly as they were, and still index the right
   * tracks, because the removal waits. */
  mount = setup(root, "kept", keys);
  size_t pc_before_len = 0, otg_before_len = 0;
  char *pc_path = ipod_path_join(mount, "iPod_Control/iTunes/Play Counts");
  char *otg_path = ipod_path_join(mount, "iPod_Control/iTunes/OTGPlaylistInfo");
  uint8_t *pc_before = ipod_read_file(pc_path, &pc_before_len);
  uint8_t *otg_before = ipod_read_file(otg_path, &otg_before_len);
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (!dev) {
    free(mount);
    return;
  }
  const char *nonce0 = ipod_device_records_nonce(dev);
  CHECK(nonce0 && !*nonce0);
  size_t otg_len = 0;
  CHECK(ipod_device_otg_bytes(dev, &otg_len) != NULL && otg_len > otg_before_len);
  ipod_update_info up = {0};
  up.set_rating = 1;
  up.rating = 40;
  CHECK_EQ(ipod_device_update(dev, keys[1], &up, NULL), 0);
  CHECK_EQ(ipod_device_remove(dev, keys[0], NULL), 0); /* put off */
  CHECK(ipod_device_removal_deferred(dev, keys[0]));
  CHECK(!ipod_device_removal_deferred(dev, keys[1]));
  char *f0 = ipod_device_track_path(dev, keys[0]);
  CHECK(f0 == NULL); /* setup's tracks have no files: the track itself is what stays */
  char *src = test_scratch(root, "kept-src");
  char *added = ipod_path_join(src, "new.mp3");
  test_write(added, "ID3", 3);
  ipod_put_info put = {0};
  ipod_tags tags = {0};
  tags.title = "Added";
  put.tags = &tags;
  put.codec = "mp3";
  char k_added[IPOD_KEY_LEN];
  CHECK_EQ(ipod_device_put(dev, added, &put, k_added, NULL), 0);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  CHECK(exists_rel(mount, "iPod_Control/iTunes/iTunesDB.kithsong-backup"));
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  ipod_device_close(dev);
  size_t len_after = 0;
  uint8_t *pc_after = ipod_read_file(pc_path, &len_after);
  CHECK(pc_after && len_after == pc_before_len && memcmp(pc_after, pc_before, len_after) == 0);
  free(pc_after);
  uint8_t *otg_after = ipod_read_file(otg_path, &len_after);
  CHECK(otg_after && len_after == otg_before_len && memcmp(otg_after, otg_before, len_after) == 0);
  free(otg_after);
  CHECK(exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo_1"));

  /* Opened again, the same plays and lists come out, against the right tracks; the counts
   * in the database were never touched. */
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (dev) {
    ipod_db *db = ipod_device_db(dev);
    CHECK_EQ(db->n_tracks, 4);
    CHECK(ipod_device_find(dev, keys[0]) != NULL); /* the removal waited */
    CHECK(ipod_device_find(dev, k_added) != NULL);
    CHECK_EQ(db->tracks[0]->play_count, 10);
    CHECK_EQ(db->tracks[2]->play_count, 10);
    CHECK_EQ(db->tracks[1]->rating, 40);
    CHECK(ipod_db_find_playlist(db, "On-The-Go 1") == NULL); /* nothing folded */
    plays = ipod_device_plays(dev, &n);
    CHECK_EQ(n, 2);
    if (n == 2) {
      CHECK_STR(plays[0].key, keys[0]);
      CHECK_EQ(plays[0].count, 2);
      CHECK_STR(plays[1].key, keys[2]);
    }
    otg = ipod_device_otg_playlists(dev, &n);
    CHECK_EQ(n, 2);
    if (n == 2) CHECK(otg[0].n == 2 && strcmp(otg[0].keys[0], keys[2]) == 0);

    /* Taken in now: deleted at once, the waiting removal done once nothing waits, and the
     * nonce renewed then too (not while the OTG files wait: their token must hold). */
    CHECK_EQ(ipod_device_remove(dev, keys[0], NULL), 0);
    CHECK_EQ(ipod_device_done(dev, IPOD_DONE_PLAYS, NULL), 0);
    CHECK(!exists_rel(mount, "iPod_Control/iTunes/Play Counts"));
    CHECK(exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo"));
    CHECK(ipod_device_find(dev, keys[0]) != NULL); /* the OTG files still wait */
    CHECK(!*ipod_device_records_nonce(dev));
    CHECK(!exists_rel(mount, "iPod_Control/kithsong-records"));
    CHECK(ipod_device_playcounts_bytes(dev, &n) == NULL);
    CHECK_EQ(ipod_device_done(dev, IPOD_DONE_PLAYLISTS, NULL), 0);
    CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo"));
    CHECK(!exists_rel(mount, "iPod_Control/iTunes/OTGPlaylistInfo_1"));
    CHECK(strlen(ipod_device_records_nonce(dev)) == 32);
    CHECK(exists_rel(mount, "iPod_Control/kithsong-records"));
    CHECK(ipod_device_find(dev, keys[0]) == NULL);
    CHECK(ipod_device_otg_bytes(dev, &n) == NULL);
    CHECK_EQ(ipod_device_commit(dev, NULL), 0);
    ipod_device_close(dev);
  }
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (dev) {
    CHECK_EQ(ipod_device_db(dev)->n_tracks, 3);
    CHECK(strlen(ipod_device_records_nonce(dev)) == 32);
    plays = ipod_device_plays(dev, &n);
    CHECK_EQ(n, 0);
    ipod_device_close(dev);
  }
  free(pc_before);
  free(otg_before);
  free(pc_path);
  free(otg_path);
  free(f0);
  free(added);
  free(src);
  free(mount);

  /* A Play Counts file of nothing but zeros holds nothing to lose: it doesn't hold back a
   * removal, and goes when the database is rewritten. */
  mount = setup(root, "zeros", keys);
  {
    const uint32_t e[3][7] = {{0}};
    ipod_buf b = {0};
    put_playcounts(&b, 0x1C, 3, e);
    char *path = ipod_path_join(mount, "iPod_Control/iTunes/Play Counts");
    test_write(path, b.data, b.len);
    free(path);
    ipod_buf_free(&b);
    ipod_otg_clear_dir(mount);
  }
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (dev) {
    CHECK(ipod_device_playcounts_bytes(dev, &n) == NULL);
    CHECK_EQ(ipod_device_remove(dev, keys[1], NULL), 0);
    CHECK(!ipod_device_removal_deferred(dev, keys[1]));
    CHECK(ipod_device_find(dev, keys[1]) == NULL);
    CHECK_EQ(ipod_device_commit(dev, NULL), 0);
    CHECK(!exists_rel(mount, "iPod_Control/iTunes/Play Counts"));
    ipod_device_close(dev);
  }
  free(mount);
}

/* Kithsong's playlists replace only Kithsong's. */
static void test_kith_playlists(const char *root) {
  char keys[3][IPOD_KEY_LEN];
  char *mount = setup(root, "lists", keys);
  ipod_otg_clear_dir(mount);
  ipod_device *dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (!dev) {
    free(mount);
    return;
  }
  CHECK_EQ(ipod_device_set_playlist(dev, "Mix", (const char *const[]){keys[2]}, 1, NULL), 0); /* "iTunes'" */
  const char *ab[] = {keys[0], keys[1]}, *c[] = {keys[2]};
  ipod_kith_playlist_in in[2] = {{"uid-mix", "Mix", ab, 2}, {"uid-run", "Run", c, 1}};
  CHECK_EQ(ipod_device_set_kith_playlists(dev, in, 2, NULL), 0);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  ipod_device_close(dev);

  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (!dev) {
    free(mount);
    return;
  }
  ipod_db *db = ipod_device_db(dev);
  ipod_playlist *theirs = ipod_db_find_playlist(db, "Mix");
  ipod_playlist *mine = ipod_db_find_playlist(db, "Mix (Kithsong)");
  CHECK(theirs && theirs->n_items == 1);
  CHECK(mine && mine->n_items == 2);
  CHECK(ipod_db_find_playlist(db, "Run") != NULL);
  const ipod_kith_playlist *kp = ipod_kith_playlist_find(ipod_device_kith(dev), "uid-run");
  CHECK(kp && strcmp(kp->name, "Run") == 0);
  /* Run renamed Jog, Mix dropped. */
  ipod_kith_playlist_in in2[1] = {{"uid-run", "Jog", ab, 2}};
  CHECK_EQ(ipod_device_set_kith_playlists(dev, in2, 1, NULL), 0);
  CHECK_EQ(ipod_device_commit(dev, NULL), 0);
  ipod_device_close(dev);
  dev = ipod_open(mount, 0, NULL);
  CHECK(dev != NULL);
  if (dev) {
    db = ipod_device_db(dev);
    CHECK(ipod_db_find_playlist(db, "Mix") != NULL);
    CHECK(ipod_db_find_playlist(db, "Mix (Kithsong)") == NULL);
    CHECK(ipod_db_find_playlist(db, "Run") == NULL);
    ipod_playlist *jog = ipod_db_find_playlist(db, "Jog");
    CHECK(jog && jog->n_items == 2);
    CHECK_EQ(db->n_playlists, 3);
    ipod_device_close(dev);
  }
  free(mount);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  test_parse();
  test_device(root);
  test_kith_playlists(root);
  return TEST_RESULT();
}
