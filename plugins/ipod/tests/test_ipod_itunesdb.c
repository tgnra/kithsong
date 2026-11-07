/* iTunesDB: a database made here round-trips; a hand-built "iTunes" database (older header
 * sizes, a UTF-8 string, unknown mhods and datasets, a smart playlist, a podcast playlist
 * in both forms, old-style mhips) is written back byte for byte when unchanged, and keeps
 * everything foreign when tracks are added and removed. No iTunes-written fixture with a
 * clear licence was found, so the "iTunes" database is built from the documented layout. */
#include <stdlib.h>

#include "fixtures.h"
#include "kithipod/itunesdb.h"

static ipod_track *make_track(const char *title, const char *artist, const char *album, uint32_t n) {
  ipod_track *t = ipod_track_new();
  ipod_set_str(&t->title, title);
  ipod_set_str(&t->artist, artist);
  ipod_set_str(&t->album, album);
  ipod_set_str(&t->genre, "Electronic");
  ipod_set_str(&t->composer, "Ana Composer");
  ipod_set_str(&t->album_artist, "Various");
  ipod_set_str(&t->sort_artist, artist);
  char loc[64];
  snprintf(loc, sizeof loc, ":iPod_Control:Music:F%02u:TRK%u.m4a", n % 3, n);
  ipod_set_str(&t->location, loc);
  ipod_track_set_kind(t, "alac", 0);
  t->size = 1000000 + n;
  t->length_ms = 180000 + n;
  t->track_number = n;
  t->track_total = 12;
  t->disc_number = 1;
  t->disc_total = 2;
  t->year = 1999;
  t->bitrate = 900;
  t->sample_rate = 44100;
  t->play_count = n * 2;
  t->last_played = 3700000000u + n;
  t->time_added = 3600000000u;
  t->compilation = 1;
  t->sample_count = 7938000;
  return t;
}

static void test_new_db(void) {
  ipod_db *db = ipod_db_new("Tania's iPod");
  CHECK(db != NULL);
  ipod_track *a = make_track("Zebra", "beta Band", "Second", 2);
  ipod_track *b = make_track("apple", "Alpha", "First", 1);
  ipod_track *c = make_track("Ünïcödé 🎵", "Ça va", "Third", 3);
  ipod_track_set_kind(c, "mp3", 1);
  CHECK_EQ(ipod_db_add_track(db, a), 0);
  CHECK_EQ(ipod_db_add_track(db, b), 0);
  CHECK_EQ(ipod_db_add_track(db, c), 0);
  CHECK(a->id != 0 && b->id != 0 && a->id != b->id);
  CHECK(a->dbid != 0 && a->dbid != b->dbid);
  ipod_playlist *pl = ipod_playlist_new("Mix");
  ipod_playlist_add(pl, c->id);
  ipod_playlist_add(pl, a->id);
  CHECK_EQ(ipod_db_add_playlist(db, pl), 0);

  uint8_t *out;
  size_t len;
  CHECK_EQ(ipod_db_serialize(db, &out, &len, NULL), 0);
  /* Datasets in iTunes' order: tracks, podcast-style playlists, flat playlists. */
  CHECK(memcmp(out, "mhbd", 4) == 0);
  CHECK_EQ(ipod_rd32(out + 8), len);
  CHECK_EQ(ipod_rd32(out + 0x14), 3);
  size_t off = ipod_rd32(out + 4);
  uint32_t types[3];
  for (int i = 0; i < 3; i++) {
    types[i] = ipod_rd32(out + off + 12);
    off += ipod_rd32(out + off + 8);
  }
  CHECK(types[0] == 1 && types[1] == 3 && types[2] == 2);
  CHECK_EQ(off, len);

  ipod_db *r = ipod_db_parse(out, len, NULL);
  CHECK(r != NULL);
  if (r) {
    CHECK_STR(ipod_db_name(r), "Tania's iPod");
    CHECK_EQ(r->n_tracks, 3);
    CHECK_EQ(r->n_playlists, 2);
    const ipod_track *t = r->tracks[2];
    CHECK_STR(t->title, "Ünïcödé 🎵");
    CHECK_STR(t->artist, "Ça va");
    CHECK_STR(t->filetype_desc, "MPEG audio file");
    CHECK_EQ(t->filetype, IPOD_FILETYPE_MP3);
    CHECK(t->type1 == 1 && t->type2 == 1);
    t = r->tracks[0];
    CHECK_STR(t->title, "Zebra");
    CHECK_STR(t->album, "Second");
    CHECK_STR(t->genre, "Electronic");
    CHECK_STR(t->composer, "Ana Composer");
    CHECK_STR(t->album_artist, "Various");
    CHECK_STR(t->sort_artist, "beta Band");
    CHECK_STR(t->location, ":iPod_Control:Music:F02:TRK2.m4a");
    CHECK_STR(t->filetype_desc, "Apple Lossless audio file");
    CHECK_EQ(t->filetype, IPOD_FILETYPE_M4A);
    CHECK_EQ(t->id, a->id);
    CHECK(t->dbid == a->dbid);
    CHECK_EQ(t->size, 1000002);
    CHECK_EQ(t->length_ms, 180002);
    CHECK_EQ(t->track_number, 2);
    CHECK_EQ(t->track_total, 12);
    CHECK_EQ(t->disc_number, 1);
    CHECK_EQ(t->disc_total, 2);
    CHECK_EQ(t->year, 1999);
    CHECK_EQ(t->bitrate, 900);
    CHECK_EQ(t->sample_rate, 44100);
    CHECK_EQ(t->play_count, 4);
    CHECK_EQ(t->last_played, 3700000002u);
    CHECK_EQ(t->time_added, 3600000000u);
    CHECK_EQ(t->compilation, 1);
    CHECK_EQ(t->has_artwork, 2);
    CHECK_EQ(t->unk126, 0xffff);
    CHECK_EQ(t->unk144, 0x33);
    CHECK(t->sample_count == 7938000);
    const ipod_playlist *m = r->playlists[0];
    CHECK(m->is_master);
    CHECK_EQ(m->n_items, 3);
    const ipod_playlist *p = r->playlists[1];
    CHECK_STR(p->name, "Mix");
    CHECK(!p->is_master);
    CHECK(p->n_items == 2 && p->items[0] == c->id && p->items[1] == a->id);
    CHECK(p->id == pl->id);

    /* Unchanged: the same bytes again. */
    uint8_t *out2;
    size_t len2;
    CHECK_EQ(ipod_db_serialize(r, &out2, &len2, NULL), 0);
    CHECK(len2 == len && memcmp(out, out2, len) == 0);
    free(out2);
    ipod_db_free(r);
  }

  /* The master playlist's library indexes: five mhod 52 (sorted track indexes) each followed
   * by its mhod 53 jump table. Titles fold case: "apple" (index 1) sorts before "Zebra" (0)
   * and "Ünïcödé" (2) after. */
  size_t mhyp = 0;
  for (size_t i = 0; i + 4 <= len; i++)
    if (memcmp(out + i, "mhyp", 4) == 0) {
      mhyp = i;
      break;
    }
  CHECK(mhyp != 0);
  if (mhyp) {
    CHECK_EQ(out[mhyp + 0x14], 1); /* hidden: the master */
    CHECK_EQ(ipod_rd32(out + mhyp + 12), 12); /* name, prefs, 5 x (52 + 53) */
    size_t p = mhyp + ipod_rd32(out + mhyp + 4);
    int n52 = 0, n53 = 0;
    for (int k = 0; k < 12; k++) {
      uint32_t type = ipod_rd32(out + p + 12);
      if (type == 52) {
        uint32_t sort = ipod_rd32(out + p + 24), n = ipod_rd32(out + p + 28);
        CHECK_EQ(n, 3);
        if (sort == 3) { /* title */
          CHECK_EQ(ipod_rd32(out + p + 72), 1);
          CHECK_EQ(ipod_rd32(out + p + 76), 0);
          CHECK_EQ(ipod_rd32(out + p + 80), 2);
        }
        if (sort == 5) { /* artist: Alpha, beta Band, Ça va */
          CHECK_EQ(ipod_rd32(out + p + 72), 1);
          CHECK_EQ(ipod_rd32(out + p + 76), 0);
          CHECK_EQ(ipod_rd32(out + p + 80), 2);
        }
        n52++;
      } else if (type == 53) {
        uint32_t sort = ipod_rd32(out + p + 24), n = ipod_rd32(out + p + 28);
        if (sort == 3) {
          CHECK_EQ(n, 3); /* A, Z, Ü */
          CHECK_EQ(ipod_rd16(out + p + 40), 'A');
          CHECK_EQ(ipod_rd32(out + p + 44), 0);
          CHECK_EQ(ipod_rd32(out + p + 48), 1);
          CHECK_EQ(ipod_rd16(out + p + 52), 'Z');
          CHECK_EQ(ipod_rd16(out + p + 64), 0xDC);
        }
        n53++;
      }
      p += ipod_rd32(out + p + 8);
    }
    CHECK(n52 == 5 && n53 == 5);
  }

  /* Removing a track drops it from playlists. */
  CHECK_EQ(ipod_db_remove_track(db, c->id), 0);
  CHECK_EQ(db->n_tracks, 2);
  CHECK(pl->n_items == 1 && pl->items[0] == a->id);
  CHECK_EQ(ipod_db_remove_track(db, 99999), IPOD_ERR_NOT_FOUND);
  free(out);
  ipod_db_free(db);
}

/* --- An iTunes-style database, by hand ---------------------------------------------------- */

typedef struct foreign {
  ipod_buf b;
  size_t t1, t1_len, t2, t2_len; /* offsets of the two mhit chunks */
  size_t ds4, ds4_len;           /* the unknown dataset */
  size_t smart, smart_len;       /* the smart playlist (type 2) */
  size_t pod3, pod3_len;         /* the podcast playlist in the type 3 dataset */
} foreign;

static void old_mhip(ipod_buf *b, uint32_t track_id, uint32_t pos) {
  /* Pre-iTunes 4.9: the mhip's length covers only itself; the position mhod follows. */
  size_t at = tb_open(b, "mhip", 0x4C);
  ipod_buf_set_le32(b, at + 8, 0x4C);
  ipod_buf_set_le32(b, at + 12, 1);
  ipod_buf_set_le32(b, at + 0x18, track_id);
  ipod_buf_set_le32(b, at + 0x1C, 0x12345678); /* added-to-playlist time */
  size_t m = tb_open(b, "mhod", 24);
  ipod_buf_set_le32(b, m + 12, 100);
  ipod_buf_le32(b, pos);
  ipod_buf_zero(b, 16);
  tb_close(b, m);
}

static void new_mhip(ipod_buf *b, uint32_t track_id, uint32_t pos, uint32_t group_flag, uint32_t group_ref) {
  size_t at = tb_open(b, "mhip", 0x4C);
  ipod_buf_set_le32(b, at + 12, 1);
  ipod_buf_set_le32(b, at + 0x10, group_flag);
  ipod_buf_set_le32(b, at + 0x18, track_id);
  ipod_buf_set_le32(b, at + 0x20, group_ref);
  size_t m = tb_open(b, "mhod", 24);
  ipod_buf_set_le32(b, m + 12, 100);
  ipod_buf_le32(b, pos);
  ipod_buf_zero(b, 16);
  tb_close(b, m);
  tb_close(b, at);
}

static size_t playlist_open(ipod_buf *b, const char *name, int master, uint64_t id, int podcast, uint32_t n_mhods,
                            uint32_t n_items) {
  size_t at = tb_open(b, "mhyp", 0x6C);
  ipod_buf_set_le32(b, at + 12, n_mhods);
  ipod_buf_set_le32(b, at + 16, n_items);
  b->data[at + 0x14] = (uint8_t)master;
  ipod_wr64(b->data + at + 0x1C, id);
  ipod_wr16(b->data + at + 0x28, 1);
  ipod_wr16(b->data + at + 0x2A, (uint16_t)podcast);
  tb_string_mhod(b, 1, name, 0);
  return at;
}

static void build_foreign(foreign *f) {
  ipod_buf *b = &f->b;
  memset(f, 0, sizeof *f);
  size_t bd = tb_open(b, "mhbd", 0x68); /* iTunes 4.x header size */
  ipod_buf_set_le32(b, bd + 0x0C, 1);
  ipod_buf_set_le32(b, bd + 0x10, 0x0c);
  ipod_buf_set_le32(b, bd + 0x14, 4);
  ipod_wr64(b->data + bd + 0x18, 0x1122334455667788ull);
  ipod_wr16(b->data + bd + 0x20, 1);
  b->data[bd + 0x40] = 0xAB; /* an unknown header byte */

  /* Tracks. */
  size_t sd = tb_open(b, "mhsd", 0x60);
  ipod_buf_set_le32(b, sd + 12, 1);
  size_t lt = tb_open(b, "mhlt", 0x5C);
  ipod_buf_set_le32(b, lt + 8, 2);
  f->t1 = tb_open(b, "mhit", 0x9C); /* iTunes 4.0 mhit */
  ipod_buf_set_le32(b, f->t1 + 12, 5);
  ipod_buf_set_le32(b, f->t1 + 0x10, 100);
  ipod_buf_set_le32(b, f->t1 + 0x14, 1);
  ipod_buf_set_le32(b, f->t1 + 0x18, IPOD_FILETYPE_MP3);
  ipod_buf_set_le32(b, f->t1 + 0x24, 4000000);
  ipod_buf_set_le32(b, f->t1 + 0x28, 240000);
  ipod_buf_set_le32(b, f->t1 + 0x3C, 44100u << 16);
  ipod_buf_set_le32(b, f->t1 + 0x50, 7);
  ipod_wr64(b->data + f->t1 + 0x70, 0xAAAA000000000001ull);
  b->data[f->t1 + 0x98] = 0x5A; /* unknown */
  tb_string_mhod(b, 1, "iTunes Song", 0);
  tb_string_mhod(b, 4, "iTunes Artist", 0);
  tb_string_mhod(b, 3, "UTF-8 Albüm", 1);
  tb_raw_mhod(b, 17, 40, 0x10); /* chapter data: not understood, kept */
  tb_string_mhod(b, 2, ":iPod_Control:Music:F01:ABCD.mp3", 0);
  tb_close(b, f->t1);
  f->t1_len = b->len - f->t1;
  f->t2 = tb_open(b, "mhit", 0xF4);
  ipod_buf_set_le32(b, f->t2 + 12, 2);
  ipod_buf_set_le32(b, f->t2 + 0x10, 101);
  ipod_buf_set_le32(b, f->t2 + 0x18, IPOD_FILETYPE_M4A);
  ipod_buf_set_le32(b, f->t2 + 0x3C, (48000u << 16) | 0x1234); /* odd low bits, kept */
  ipod_wr64(b->data + f->t2 + 0x70, 0xAAAA000000000002ull);
  ipod_wr64(b->data + f->t2 + 0xA8, 0xBBBB000000000002ull); /* dbid2 that differs, kept */
  b->data[f->t2 + 0xA7] = 1; /* podcast flag */
  tb_string_mhod(b, 1, "A Podcast Episode", 0);
  tb_string_mhod(b, 2, ":iPod_Control:Music:F00:WXYZ.m4a", 0);
  tb_close(b, f->t2);
  f->t2_len = b->len - f->t2;
  tb_close(b, sd);

  /* An album list (type 4) this library doesn't read. */
  f->ds4 = tb_open(b, "mhsd", 0x60);
  ipod_buf_set_le32(b, f->ds4 + 12, 4);
  size_t la = tb_open(b, "mhla", 0x5C);
  (void)la;
  ipod_buf_put(b, "opaque album data", 17);
  ipod_buf_zero(b, 3);
  tb_close(b, f->ds4);
  f->ds4_len = b->len - f->ds4;

  /* Playlists, flat. */
  sd = tb_open(b, "mhsd", 0x60);
  ipod_buf_set_le32(b, sd + 12, 2);
  size_t lp = tb_open(b, "mhlp", 0x5C);
  ipod_buf_set_le32(b, lp + 8, 3);
  size_t pl = playlist_open(b, "Tania's iPod", 1, 0xC0FFEE, 0, 2, 2);
  tb_raw_mhod(b, 100, 0x288 - 24, 0x40); /* iTunes' column prefs */
  old_mhip(b, 100, 0);
  old_mhip(b, 101, 1);
  tb_close(b, pl);
  f->smart = playlist_open(b, "Recently Played", 0, 0x5A5A, 0, 3, 1);
  tb_raw_mhod(b, 50, 72, 0x01);
  tb_raw_mhod(b, 51, 120, 0x02);
  new_mhip(b, 100, 0, 0, 0);
  tb_close(b, f->smart);
  f->smart_len = b->len - f->smart;
  pl = playlist_open(b, "Podcasts", 0, 0x9090, 1, 1, 1);
  new_mhip(b, 101, 0, 0, 0);
  tb_close(b, pl);
  tb_close(b, sd);

  /* Playlists, with the podcasts grouped by show. */
  sd = tb_open(b, "mhsd", 0x60);
  ipod_buf_set_le32(b, sd + 12, 3);
  lp = tb_open(b, "mhlp", 0x5C);
  ipod_buf_set_le32(b, lp + 8, 2);
  pl = playlist_open(b, "Tania's iPod", 1, 0xC0FFEE, 0, 1, 2);
  new_mhip(b, 100, 0, 0, 0);
  new_mhip(b, 101, 1, 0, 0);
  tb_close(b, pl);
  f->pod3 = playlist_open(b, "Podcasts", 0, 0x9090, 1, 1, 2);
  size_t g = tb_open(b, "mhip", 0x4C); /* the show's group */
  ipod_buf_set_le32(b, g + 12, 1);
  ipod_buf_set_le32(b, g + 0x10, 256);
  ipod_buf_set_le32(b, g + 0x14, 77);
  tb_string_mhod(b, 1, "The Show", 0);
  tb_close(b, g);
  new_mhip(b, 101, 78, 0, 77);
  tb_close(b, f->pod3);
  f->pod3_len = b->len - f->pod3;
  tb_close(b, sd);
  tb_close(b, bd);
}

static void test_foreign_db(void) {
  foreign f;
  build_foreign(&f);
  CHECK(!f.b.oom);
  ipod_error err = {0};
  ipod_db *db = ipod_db_parse(f.b.data, f.b.len, &err);
  CHECK(db != NULL);
  if (!db) {
    fprintf(stderr, "parse: %s\n", err.message);
    ipod_buf_free(&f.b);
    return;
  }
  CHECK(!ipod_db_is_hashed(db));
  CHECK_EQ(db->version, 0x0c);
  CHECK_EQ(db->n_tracks, 2);
  CHECK_STR(db->tracks[0]->title, "iTunes Song");
  CHECK_STR(db->tracks[0]->album, "UTF-8 Albüm");
  CHECK_STR(db->tracks[0]->location, ":iPod_Control:Music:F01:ABCD.mp3");
  CHECK_EQ(db->tracks[0]->play_count, 7);
  CHECK_EQ(db->tracks[0]->sample_rate, 44100);
  CHECK_EQ(db->tracks[1]->sample_rate, 48000);
  CHECK_STR(ipod_db_name(db), "Tania's iPod");
  CHECK_EQ(db->n_playlists, 3);
  ipod_playlist *smart = db->playlists[1], *pod = db->playlists[2];
  CHECK(smart->is_smart && !smart->is_podcast);
  CHECK(pod->is_podcast && pod->n_items == 1 && pod->items[0] == 101);
  CHECK(db->playlists[0]->n_items == 2); /* old-style mhips read */

  /* Unchanged: identical bytes. */
  uint8_t *out;
  size_t len;
  CHECK_EQ(ipod_db_serialize(db, &out, &len, NULL), 0);
  CHECK(len == f.b.len && memcmp(out, f.b.data, len) == 0);
  free(out);

  /* Add a track and change another: the rest stays as it was. */
  ipod_track *t = make_track("New One", "Kithsong", "Fresh", 5);
  CHECK_EQ(ipod_db_add_track(db, t), 0);
  CHECK_EQ(t->id, 102);
  db->tracks[0]->play_count = 9;
  CHECK_EQ(ipod_db_serialize(db, &out, &len, NULL), 0);
  CHECK(test_contains(out, len, f.b.data + f.t2, f.t2_len));       /* untouched track */
  CHECK(test_contains(out, len, f.b.data + f.ds4, f.ds4_len));     /* unknown dataset */
  CHECK(test_contains(out, len, f.b.data + f.smart, f.smart_len)); /* smart playlist */
  CHECK(test_contains(out, len, f.b.data + f.pod3, f.pod3_len));   /* grouped podcasts */
  ipod_db *r = ipod_db_parse(out, len, &err);
  CHECK(r != NULL);
  if (r) {
    CHECK_EQ(r->n_tracks, 3);
    CHECK_EQ(r->tracks[0]->play_count, 9);
    CHECK_STR(r->tracks[0]->album, "UTF-8 Albüm");
    CHECK_STR(r->tracks[2]->title, "New One");
    CHECK_EQ(r->playlists[0]->n_items, 3);
    CHECK_EQ(r->tracks[0]->priv ? 1 : 0, 1);
    /* Its 0x9C header still has the unknown byte, and the chapter mhod survived. */
    uint8_t chap[40];
    for (int i = 0; i < 40; i++) chap[i] = (uint8_t)(0x10 + i);
    CHECK(test_contains(out, len, chap, sizeof chap));
    CHECK(test_contains(out, len, (const uint8_t *)"\x5A", 1));
    /* The master playlist kept iTunes' prefs mhod and gained the library indexes. */
    uint8_t prefs[16];
    for (int i = 0; i < 16; i++) prefs[i] = (uint8_t)(0x40 + i);
    CHECK(test_contains(out, len, prefs, sizeof prefs));
    CHECK_EQ(ipod_rd32(out + 0x14), 4);
    CHECK_EQ(out[0x40], 0xAB);
    ipod_db_free(r);
  }
  free(out);

  /* Removing the podcast episode rewrites the playlists that held it. */
  CHECK_EQ(ipod_db_remove_track(db, 101), 0);
  CHECK_EQ(pod->n_items, 0);
  CHECK_EQ(ipod_db_serialize(db, &out, &len, NULL), 0);
  CHECK(!test_contains(out, len, f.b.data + f.pod3, f.pod3_len));
  r = ipod_db_parse(out, len, NULL);
  CHECK(r && r->n_tracks == 2 && r->playlists[2]->n_items == 0 && r->playlists[2]->is_podcast);
  ipod_db_free(r);
  free(out);
  ipod_db_free(db);

  /* A hashed database is refused for writing. */
  ipod_wr16(f.b.data + 0x30, 1);
  db = ipod_db_parse(f.b.data, f.b.len, NULL);
  CHECK(db && ipod_db_is_hashed(db));
  if (db) CHECK_EQ(ipod_db_serialize(db, &out, &len, NULL), IPOD_ERR_UNSUPPORTED);
  ipod_db_free(db);

  /* Truncated input fails cleanly. */
  ipod_wr16(f.b.data + 0x30, 0);
  for (size_t cut = 1; cut < f.b.len; cut += 37) {
    ipod_db *bad = ipod_db_parse(f.b.data, cut, NULL);
    ipod_db_free(bad);
  }
  ipod_buf_free(&f.b);
}

static void test_write_file(const char *root) {
  char *dir = test_scratch(root, "write");
  char *path = ipod_path_join(dir, "iTunesDB"), *bak = ipod_path_join(dir, "iTunesDB.bak");
  ipod_db *db = ipod_db_new("A");
  CHECK_EQ(ipod_db_write(db, path, bak, NULL), 0);
  CHECK(ipod_file_exists(path));
  CHECK(!ipod_file_exists(bak)); /* nothing to back up the first time */
  ipod_db_set_name(db, "B");
  CHECK_EQ(ipod_db_write(db, path, bak, NULL), 0);
  ipod_db *old = ipod_db_read(bak, NULL), *cur = ipod_db_read(path, NULL);
  CHECK(old && cur);
  if (old && cur) {
    CHECK_STR(ipod_db_name(old), "A");
    CHECK_STR(ipod_db_name(cur), "B");
  }
  ipod_db_free(old);
  ipod_db_free(cur);
  ipod_db_free(db);
  char *tmp = ipod_asprintf("%s.tmp", path);
  CHECK(!ipod_file_exists(tmp));
  free(tmp);

  char *p = ipod_location_to_path(":iPod_Control:Music:F07:KQ3Z.mp3");
  CHECK_STR(p, "iPod_Control/Music/F07/KQ3Z.mp3");
  char *l = ipod_path_to_location(p);
  CHECK_STR(l, ":iPod_Control:Music:F07:KQ3Z.mp3");
  free(p);
  free(l);
  free(path);
  free(bak);
  free(dir);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  test_new_db();
  test_foreign_db();
  test_write_file(root);
  return TEST_RESULT();
}
