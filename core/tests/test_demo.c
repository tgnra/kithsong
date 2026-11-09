/* The demo library (kithcore/demo.h): the silent FLAC decodes to the length asked for, covers
 * and files come out byte for byte the same every time, and mc_demo_create() makes a library
 * of the six albums with their tags and covers, without the network, and nothing else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "playback/decoder.h"
#include "test.h"
#include "util/platform.h"

static char *join(const char *a, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *p = malloc(n);
  snprintf(p, n, "%s%c%s", a, MC_PATH_SEP, b);
  return p;
}

static unsigned char *slurp(const char *path, size_t *len) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *p = malloc(n > 0 ? (size_t)n : 1);
  *len = p ? fread(p, 1, (size_t)n, f) : 0;
  fclose(f);
  return p;
}

static int write_all(const char *path, const void *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}

static void test_flac(const char *base) {
  /* Not a whole number of blocks, so the last frame is a short one. */
  const uint64_t samples = 44100ull * 3 + 1234;
  unsigned char *data = NULL;
  size_t len = 0;
  CHECK(mc_demo_silent_flac(samples, &data, &len) == MC_OK);
  CHECK(data && len > 42 && len < 2000 && !memcmp(data, "fLaC", 4));
  char *path = join(base, "test_demo.flac");
  CHECK(write_all(path, data, len));
  mc_free(data);

  mc_decoder *d = NULL;
  CHECK(mc_decoder_open(path, &d) == MC_OK);
  if (d) {
    CHECK(mc_decoder_rate(d) == 44100 && mc_decoder_channels(d) == 1);
    CHECK(mc_decoder_length(d) == samples);
    float buf[4096];
    uint64_t got = 0, n;
    int silent = 1;
    while ((n = mc_decoder_read(d, buf, 4096)) > 0) {
      for (uint64_t i = 0; i < n; i++) silent &= buf[i] == 0.0f;
      got += n;
    }
    CHECK(got == samples);
    CHECK(silent);
    mc_decoder_close(d);
  }

  mc_tags tags = {.title = "Tagged", .artist = "Someone", .track = 3};
  CHECK(mc_tags_write(path, &tags) == MC_OK);
  mc_tags back = {0};
  CHECK(mc_tags_read(path, &back) == MC_OK);
  CHECK_STR(back.title, "Tagged");
  CHECK(back.track == 3 && back.sample_rate == 44100 && back.duration_ms >= 3020 && back.duration_ms <= 3040);
  mc_tags_clear(&back);
  mc_remove(path);
  free(path);

  CHECK(mc_demo_silent_flac(0, &data, &len) == MC_ERR_INVALID_ARG && !data);
}

static void test_cover(void) {
  unsigned char *a = NULL, *b = NULL;
  size_t na = 0, nb = 0;
  for (int i = 0; i < MC_DEMO_ALBUMS; i++) {
    CHECK(mc_demo_cover_jpeg(i, 300, &a, &na) == MC_OK);
    CHECK(mc_demo_cover_jpeg(i, 300, &b, &nb) == MC_OK);
    CHECK(a && b && na == nb && na > 1000 && !memcmp(a, b, na));
    CHECK(a && a[0] == 0xFF && a[1] == 0xD8);
    mc_free(a);
    mc_free(b);
  }
  CHECK(mc_demo_cover_jpeg(MC_DEMO_ALBUMS, 300, &a, &na) == MC_ERR_INVALID_ARG);
  CHECK(mc_demo_cover_jpeg(0, 10, &a, &na) == MC_ERR_INVALID_ARG);
  CHECK(mc_demo_album_title(5) && !strcmp(mc_demo_album_title(5), "Copperline"));
  CHECK(mc_demo_album_artist(-1) == NULL);
}

typedef struct files {
  size_t root_len;
  char **paths; /* relative */
  int count;
} files;

static int add_file(void *user, const mc_tree_entry *e) {
  files *f = user;
  if (e->is_dir) return 0;
  char **more = realloc(f->paths, (size_t)(f->count + 1) * sizeof *more);
  if (!more) return -1;
  f->paths = more;
  f->paths[f->count++] = mc_strdup(e->path + f->root_len + 1);
  return 0;
}

static int by_name(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void test_folder(const char *base) {
  char *one = join(base, "test_demo_one"), *two = join(base, "test_demo_two");
  mc_remove_tree(one);
  mc_remove_tree(two);
  CHECK(mc_demo_write_folder(one) == MC_OK);
  CHECK(mc_demo_write_folder(two) == MC_OK);
  CHECK(mc_demo_write_folder(one) == MC_ERR_EXISTS);

  files f = {strlen(one), NULL, 0};
  CHECK(mc_tree_walk(one, add_file, &f) == MC_OK);
  CHECK(f.count == MC_DEMO_TRACKS + MC_DEMO_ALBUMS);
  qsort(f.paths, (size_t)f.count, sizeof *f.paths, by_name);
  size_t total = 0;
  for (int i = 0; i < f.count; i++) {
    char *pa = join(one, f.paths[i]), *pb = join(two, f.paths[i]);
    size_t na = 0, nb = 0;
    unsigned char *a = slurp(pa, &na), *b = slurp(pb, &nb);
    CHECK(a && b && na == nb && !memcmp(a, b, na)); /* the same every run */
    total += na;
    free(a);
    free(b);
    free(pa);
    free(pb);
  }
  CHECK(total < 4 * 1024 * 1024); /* small enough to make anywhere */

  char *folder = join(one, "Ivy Sunderholt"), *album = join(folder, "Copperline");
  char *path = join(album, "03 Paper Towns Burn Bright.flac");
  free(folder);
  free(album);
  mc_tags t = {0};
  CHECK(mc_tags_read(path, &t) == MC_OK);
  CHECK_STR(t.title, "Paper Towns Burn Bright");
  CHECK_STR(t.artist, "Ivy Sunderholt");
  CHECK_STR(t.album_artist, "Ivy Sunderholt");
  CHECK_STR(t.album, "Copperline");
  CHECK_STR(t.genre, "Indie Rock");
  CHECK(t.year == 2024 && t.track == 3 && t.track_total == 6 && t.disc == 1);
  CHECK(t.duration_ms >= 148000 && t.duration_ms <= 293000);
  mc_tags_clear(&t);
  free(path);

  for (int i = 0; i < f.count; i++) free(f.paths[i]);
  free(f.paths);
  mc_remove_tree(one);
  mc_remove_tree(two);
  free(one);
  free(two);
}

static void test_create(const char *base) {
  char *dir = join(base, "test_demo_lib"), *staging = join(base, "test_demo_staging");
  mc_remove_tree(dir);
  mc_remove_tree(staging);
  CHECK(mc_mkdirs(staging) == MC_OK);
  /* KITHSONG_MUSICBRAINZ_PROBE isn't needed: skip_tagging never looks anything up. */
  CHECK(mc_demo_create(dir, staging) == MC_OK);
  CHECK(mc_demo_is_complete(dir));
  CHECK(mc_folder_is_empty(staging)); /* the files went with their folder */
  CHECK(mc_demo_create(dir, staging) == MC_ERR_EXISTS);

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  int64_t n = 0;
  CHECK(mc_library_count(lib, MC_ENTITY_TRACKS, &n) == MC_OK && n == MC_DEMO_TRACKS);
  CHECK(mc_library_count(lib, MC_ENTITY_ALBUMS, &n) == MC_OK && n == MC_DEMO_ALBUMS);
  CHECK(mc_library_count(lib, MC_ENTITY_ARTISTS, &n) == MC_OK && n == MC_DEMO_ALBUMS);

  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  int found = 0;
  for (int i = 0; albums && i < albums->count; i++) {
    CHECK(albums->items[i].has_art);
    for (int k = 0; k < MC_DEMO_ALBUMS; k++)
      if (!strcmp(albums->items[i].title, mc_demo_album_title(k)) &&
          !strcmp(albums->items[i].artist, mc_demo_album_artist(k)))
        found |= 1 << k;
    if (!strcmp(albums->items[i].title, "Small Hours, Big Rooms")) {
      CHECK(albums->items[i].year == 2022);
      mc_track_list *tracks = NULL;
      CHECK(mc_library_list_album_tracks(lib, albums->items[i].uid, &tracks) == MC_OK);
      CHECK(tracks && tracks->count == 6);
      if (tracks && tracks->count == 6) {
        CHECK_STR(tracks->items[5].title, "Goodnight, Mezzanine");
        CHECK(tracks->items[0].duration_ms > 140000);
      }
      mc_track_list_free(tracks);
    }
  }
  CHECK(found == (1 << MC_DEMO_ALBUMS) - 1);
  mc_album_list_free(albums);

  mc_recent_list *recents = NULL;
  CHECK(mc_library_list_recents(lib, 0, 0, &recents) == MC_OK);
  CHECK(recents && recents->count == 3);
  if (recents && recents->count == 3) {
    CHECK_STR(recents->items[0].name, "Copperline");
    CHECK_STR(recents->items[2].name, "Nightjar Season");
  }
  mc_recent_list_free(recents);
  mc_library_stats stats;
  CHECK(mc_library_read_stats(lib, &stats) == MC_OK);
  CHECK(stats.plays > stats.tracks_played && stats.tracks_played > 3 && stats.tracks_played < MC_DEMO_TRACKS);
  mc_library_close(lib);

  /* One cut off is made again; a folder with anything else in it is left alone. */
  char *marker = join(dir, MC_DEMO_MARKER);
  CHECK(write_all(marker, "status generating\n", 18));
  CHECK(!mc_demo_is_complete(dir));
  CHECK(mc_demo_create(dir, staging) == MC_OK);
  CHECK(mc_demo_is_complete(dir));
  mc_remove(marker);
  CHECK(mc_demo_create(dir, staging) == MC_ERR_EXISTS);
  CHECK(mc_file_exists(dir));
  free(marker);

  mc_remove_tree(dir);
  mc_remove_tree(staging);
  free(dir);
  free(staging);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  const char *base = argc > 1 ? argv[1] : ".";
  test_flac(base);
  test_cover();
  test_folder(base);
  test_create(base);
  mc_shutdown();
  return TEST_RESULT();
}
