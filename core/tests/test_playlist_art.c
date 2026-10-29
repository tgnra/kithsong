/* mc_playlist_art(): which covers are picked, the fallbacks for fewer than four, the cache
 * and pruning. Covers are solid colours, so each quadrant of a grid says which album it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

#define PL1 "00000000-0000-7000-8000-000000000001"
#define PL2 "00000000-0000-7000-8000-000000000002"
#define PL3 "00000000-0000-7000-8000-000000000003"
#define PL4 "00000000-0000-7000-8000-000000000004"
#define PL_GONE "00000000-0000-7000-8000-0000000000ff"
#define ALBUMS 8

static char g_lib_dir[1024], g_cache[1100];

/* Album i: a colour, a cover (PNG for odd i, and not square, to exercise the crop) and an
 * art hash. Albums 6 and 7 share album 5's hash; 6 has its cover file, 7 has none here. */
static const unsigned char k_colors[ALBUMS][3] = {
  {230, 30, 30}, {30, 200, 30}, {30, 30, 220}, {220, 220, 30},
  {220, 30, 220}, {30, 210, 210}, {128, 128, 128}, {250, 250, 250},
};
static char g_hash[ALBUMS][65];

static int exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
  sqlite3_free(err);
  return rc;
}

static void write_cb(void *user, void *data, int size) {
  fwrite(data, 1, (size_t)size, user);
}

static void write_image(const char *path, int w, int h, const unsigned char rgb[3], int png) {
  unsigned char *px = malloc((size_t)w * h * 3);
  for (int i = 0; i < w * h; i++) memcpy(px + i * 3, rgb, 3);
  /* A stripe down the left edge, cropped off by the centre square of a wide image. */
  if (w > h)
    for (int y = 0; y < h; y++)
      for (int x = 0; x < (w - h) / 2; x++) memset(px + ((size_t)y * w + x) * 3, 0, 3);
  FILE *f = mc_fopen(path, "wb");
  if (png) stbi_write_png_to_func(write_cb, f, w, h, 3, px, w * 3);
  else stbi_write_jpg_to_func(write_cb, f, w, h, 3, px, 95);
  fclose(f);
  free(px);
}

static void hex_sha256(const char *s, char out[65]) {
  unsigned char d[32];
  mbedtls_sha256((const unsigned char *)s, strlen(s), d, 0);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* Albums al0..al7, track t<i> on each, file in folder A<i> beside its cover. */
static void populate(mc_library *lib) {
  sqlite3 *db = mc_library_db(lib);
  char sql[1024], dir[1100], path[1200];
  for (int i = 0; i < ALBUMS; i++) {
    char seed[8];
    snprintf(seed, sizeof seed, "art%d", i < 6 ? i : 5);
    hex_sha256(seed, g_hash[i]);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, art_hash) VALUES ('al%d', 'Album %d', '%s');"
             "INSERT INTO tracks (uid, album_uid, title) VALUES ('t%d', 'al%d', 'Track %d');"
             "INSERT INTO files (uid, track_uid, path, format) VALUES ('f%d', 't%d', 'A%d/t.flac', 'flac');",
             i, i, g_hash[i], i, i, i, i, i, i);
    CHECK(exec(db, sql) == SQLITE_OK);
    snprintf(dir, sizeof dir, "%s/A%d", g_lib_dir, i);
    mc_mkdirs(dir);
    if (i == 7) continue;
    snprintf(path, sizeof path, "%s/cover.%s", dir, i % 2 ? "png" : "jpg");
    write_image(path, i % 2 ? 160 : 120, 120, k_colors[i], i % 2);
  }
  /* An album with no art, and one with art but no files here. */
  CHECK(exec(db, "INSERT INTO albums (uid, title) VALUES ('alx', 'No Art');"
                 "INSERT INTO tracks (uid, album_uid, title) VALUES ('tx', 'alx', 'X');"
                 "INSERT INTO files (uid, track_uid, path, format) VALUES ('fx', 'tx', 'AX/x.flac', 'flac');"
                 "INSERT INTO albums (uid, title, art_hash) VALUES ('aly', 'Elsewhere', 'abc');"
                 "INSERT INTO tracks (uid, album_uid, title) VALUES ('ty', 'aly', 'Y');") == SQLITE_OK);
}

/* Puts tracks (a string of album indexes, 'x'/'y' for the odd ones) in a new playlist. */
static void make_playlist(mc_library *lib, const char *uid, const char *tracks) {
  sqlite3 *db = mc_library_db(lib);
  char sql[512];
  snprintf(sql, sizeof sql, "INSERT INTO playlists (uid, name) VALUES ('%s', 'P')", uid);
  CHECK(exec(db, sql) == SQLITE_OK);
  for (int i = 0; tracks[i]; i++) {
    snprintf(sql, sizeof sql,
             "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
             " VALUES ('%s-%d', '%s', 't%c', 'a%d')",
             uid, i, uid, tracks[i], i);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
}

static void add_track(mc_library *lib, const char *playlist, char track) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
           " VALUES ('%s-add%c', '%s', 't%c', 'z%c')",
           playlist, track, playlist, track, track);
  CHECK(exec(mc_library_db(lib), sql) == SQLITE_OK);
}

/* The album indexes in rank order for playlist, as the core ranks them; a shared hash
 * appears once, under its lowest index. */
static int expected_order(const char *playlist, const char *albums, int out[ALBUMS]) {
  int n = 0;
  unsigned char rank[ALBUMS][32];
  for (int k = 0; albums[k]; k++) {
    int i = albums[k] - '0', dup = 0;
    for (int j = 0; j < n; j++) dup |= !strcmp(g_hash[out[j]], g_hash[i]);
    if (dup) continue;
    char buf[200];
    snprintf(buf, sizeof buf, "%s\n%s", playlist, g_hash[i]);
    mbedtls_sha256((const unsigned char *)buf, strlen(buf), rank[n], 0);
    out[n++] = i;
  }
  for (int a = 0; a < n; a++) /* selection sort by rank */
    for (int b = a + 1; b < n; b++)
      if (memcmp(rank[b], rank[a], 8) < 0) {
        unsigned char t[32];
        memcpy(t, rank[a], 32);
        memcpy(rank[a], rank[b], 32);
        memcpy(rank[b], t, 32);
        int ti = out[a];
        out[a] = out[b];
        out[b] = ti;
      }
  return n;
}

static int near(const unsigned char *px, const unsigned char rgb[3]) {
  for (int c = 0; c < 3; c++)
    if (abs(px[c] - rgb[c]) > 12) return 0;
  return 1;
}

/* Whether the grid at path holds albums want[0..3], top left first. */
static int grid_is(const char *path, const int want[4]) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return 0;
  static unsigned char buf[1 << 20];
  size_t len = fread(buf, 1, sizeof buf, f);
  fclose(f);
  int w, h, comp;
  unsigned char *px = stbi_load_from_memory(buf, (int)len, &w, &h, &comp, 3);
  if (!px) return 0;
  int ok = w == MC_PLAYLIST_ART_SIZE && h == MC_PLAYLIST_ART_SIZE;
  int q = MC_PLAYLIST_ART_SIZE / 2;
  for (int i = 0; ok && i < 4; i++) {
    /* Centre of the quadrant, and near its left edge, where a stripe would show if the
     * crop were wrong. */
    int cx = (i % 2) * q + q / 2, cy = (i / 2) * q + q / 2, ex = (i % 2) * q + 4;
    ok = near(px + ((size_t)cy * w + cx) * 3, k_colors[want[i]]) &&
         near(px + ((size_t)cy * w + ex) * 3, k_colors[want[i]]);
  }
  stbi_image_free(px);
  return ok;
}

static int is_cover_of(const char *path, int album) {
  char want[64];
  snprintf(want, sizeof want, "A%d%ccover.%s", album, MC_PATH_SEP, album % 2 ? "png" : "jpg");
  size_t n = path ? strlen(path) : 0, m = strlen(want);
  return n > m && !strcmp(path + n - m, want);
}

static int in_cache(const char *path) {
  return path && !strncmp(path, g_cache, strlen(g_cache)) && strstr(path, ".jpg");
}

static void test_args(mc_library *lib) {
  char *path = (char *)"x";
  CHECK(mc_playlist_art(NULL, PL1, g_cache, &path) == MC_ERR_INVALID_ARG && !path);
  CHECK(mc_playlist_art(lib, "pl1", g_cache, &path) == MC_ERR_INVALID_ARG);
  CHECK(mc_playlist_art(lib, PL1, NULL, &path) == MC_ERR_INVALID_ARG);
  CHECK(mc_playlist_art(lib, PL1, g_cache, NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_playlist_art(lib, PL_GONE, g_cache, &path) == MC_ERR_NOT_FOUND && !path);
}

/* No covers: no art. 1 to 3 (counting shared images once): the top-ranked one alone. */
static void test_few(mc_library *lib) {
  char *path = NULL;
  make_playlist(lib, PL1, "xy");
  CHECK(mc_playlist_art(lib, PL1, g_cache, &path) == MC_OK && !path);

  add_track(lib, PL1, '2');
  CHECK(mc_playlist_art(lib, PL1, g_cache, &path) == MC_OK && is_cover_of(path, 2));
  mc_free(path);

  /* 5, 6 and 7 are one image; 7's file isn't here. Three distinct covers in all. */
  add_track(lib, PL1, '5');
  add_track(lib, PL1, '6');
  add_track(lib, PL1, '7');
  add_track(lib, PL1, '1');
  int order[ALBUMS];
  CHECK(expected_order(PL1, "2561", order) == 3);
  CHECK(mc_playlist_art(lib, PL1, g_cache, &path) == MC_OK);
  CHECK(is_cover_of(path, order[0]) || (order[0] == 5 && is_cover_of(path, 6)));
  mc_free(path);
}

static void test_grid(mc_library *lib) {
  char *path = NULL, *again = NULL;
  make_playlist(lib, PL2, "01234");
  int order[ALBUMS];
  CHECK(expected_order(PL2, "01234", order) == 5);

  CHECK(mc_playlist_art(lib, PL2, g_cache, &path) == MC_OK && in_cache(path));
  CHECK(path && grid_is(path, order));

  /* Cached: the same file, not made again. */
  int64_t size, mtime, mtime2;
  CHECK(mc_file_info(path, &size, &mtime) == MC_OK);
  mc_sleep_ms(20);
  CHECK(mc_playlist_art(lib, PL2, g_cache, &again) == MC_OK && again && !strcmp(path, again));
  CHECK(mc_file_info(path, &size, &mtime2) == MC_OK && mtime == mtime2);
  mc_free(again);

  /* The playlist's order doesn't matter, only which albums are in it. */
  make_playlist(lib, PL3, "43210");
  CHECK(mc_playlist_art(lib, PL3, g_cache, &again) == MC_OK && in_cache(again));
  CHECK(expected_order(PL3, "43210", order) == 5 && again && grid_is(again, order));
  mc_free(again);

  /* Adding an album that ranks below the four changes nothing; one that ranks among them
   * makes a new grid and the old one goes. Album 5 is one or the other. */
  int with5[ALBUMS];
  expected_order(PL2, "012345", with5);
  add_track(lib, PL2, '5');
  CHECK(mc_playlist_art(lib, PL2, g_cache, &again) == MC_OK && again);
  if (with5[4] == 5 || with5[5] == 5) {
    CHECK(again && !strcmp(again, path));
  } else {
    CHECK(again && strcmp(again, path) && grid_is(again, with5));
    CHECK(!mc_file_exists(path));
  }
  mc_free(again);
  mc_free(path);
}

/* An album group's cover: its first four distinct images, in the group's order. Pruning
 * keeps it. */
#define GROUP1 "00000000-0000-7000-8000-000000000011"
static void test_group(mc_library *lib) {
  char *path = (char *)"x";
  CHECK(mc_album_group_art(lib, PL1, g_cache, &path) == MC_ERR_NOT_FOUND && !path); /* a playlist's uid */
  CHECK(exec(mc_library_db(lib), "INSERT INTO album_groups (uid, name) VALUES ('" GROUP1 "', 'G')") == SQLITE_OK);
  CHECK(mc_album_group_art(lib, GROUP1, g_cache, &path) == MC_OK && !path);

  CHECK(mc_album_group_add(lib, GROUP1, "al3") == MC_OK);
  CHECK(mc_album_group_art(lib, GROUP1, g_cache, &path) == MC_OK && is_cover_of(path, 3));
  mc_free(path);

  /* 6 shares 5's image, so it's passed over though it isn't next to 5. */
  const char *const more[] = {"al5", "al1", "al6", "al0", "al2"};
  for (int i = 0; i < 5; i++) CHECK(mc_album_group_add(lib, GROUP1, more[i]) == MC_OK);
  static const int want[4] = {3, 5, 1, 0};
  path = NULL;
  CHECK(mc_album_group_art(lib, GROUP1, g_cache, &path) == MC_OK && in_cache(path));
  CHECK(path && grid_is(path, want));
  CHECK(mc_playlist_art_prune(lib, g_cache) == MC_OK);
  CHECK(path && mc_file_exists(path));
  mc_free(path);
}

/* A cover that won't decode gives way to the next; with fewer than four left, the best one
 * that does decode, alone. */
static void test_bad_cover(mc_library *lib) {
  char p[1200], *path = NULL;
  snprintf(p, sizeof p, "%s/A0/cover.jpg", g_lib_dir);
  FILE *f = mc_fopen(p, "wb");
  fputs("\xff\xd8\xff not really a jpeg", f);
  fclose(f);

  make_playlist(lib, PL4, "01234");
  int order[ALBUMS], want[4], n = 0;
  expected_order(PL4, "01234", order);
  for (int i = 0; i < 5; i++)
    if (order[i] != 0) want[n++] = order[i];
  CHECK(mc_playlist_art(lib, PL4, g_cache, &path) == MC_OK && in_cache(path));
  CHECK(path && grid_is(path, want));
  mc_free(path);

  /* Now only 0, 1, 2 and 3: three decode. */
  CHECK(exec(mc_library_db(lib), "DELETE FROM playlist_entries WHERE playlist_uid = '" PL4 "' AND track_uid = 't4'") ==
        SQLITE_OK);
  expected_order(PL4, "0123", order);
  int best = order[0] != 0 ? order[0] : order[1];
  CHECK(mc_playlist_art(lib, PL4, g_cache, &path) == MC_OK && is_cover_of(path, best));
  mc_free(path);
}

static void count_file(void *user, const char *name, int is_dir) {
  (void)name;
  if (!is_dir) ++*(int *)user;
}

static int count_files(const char *dir) {
  int n = 0;
  mc_list_dir(dir, count_file, &n);
  return n;
}

static void test_prune(mc_library *lib) {
  char p[1200], *path = NULL;
  /* Something else in the folder, and a stale grid of a playlist that's gone. */
  snprintf(p, sizeof p, "%s/notes.txt", g_cache);
  FILE *f = mc_fopen(p, "wb");
  fclose(f);
  snprintf(p, sizeof p, "%s/%s-0123456789abcdef.jpg", g_cache, PL_GONE);
  f = mc_fopen(p, "wb");
  fclose(f);
  int before = count_files(g_cache);

  CHECK(mc_playlist_art(lib, PL3, g_cache, &path) == MC_OK && in_cache(path));
  CHECK(exec(mc_library_db(lib), "DELETE FROM playlists WHERE uid = '" PL3 "'") == SQLITE_OK);
  CHECK(mc_playlist_art_prune(lib, g_cache) == MC_OK);
  CHECK(path && !mc_file_exists(path));
  CHECK(!mc_file_exists(p));
  snprintf(p, sizeof p, "%s/notes.txt", g_cache);
  CHECK(mc_file_exists(p));
  CHECK(count_files(g_cache) == before - 2);
  mc_free(path);

  snprintf(p, sizeof p, "%s/missing", g_cache);
  CHECK(mc_playlist_art_prune(lib, p) == MC_OK);
}

/* Removes a folder's files (one level of subfolders deep), so each run starts clean. */
static void touch_in(const char *dir, const char *name) {
  char path[1200];
  snprintf(path, sizeof path, "%s%c%s", dir, MC_PATH_SEP, name);
  FILE *f = mc_fopen(path, "wb");
  if (f) fclose(f);
}

/* Ends with name, matched exactly: the path keeps the file's own case. */
static int found_is(char *path, const char *name) {
  size_t n = path ? strlen(path) : 0, k = strlen(name);
  int ok = n > k && !strcmp(path + n - k, name) && path[n - k - 1] == MC_PATH_SEP;
  mc_free(path);
  return ok;
}

/* mc_coverart_find(): the frontends' lookup. cover before folder before front, any case. */
static void test_cover_find(void) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s%ccover-find", g_lib_dir, MC_PATH_SEP);
  CHECK(mc_mkdirs(dir) == MC_OK);
  CHECK(mc_coverart_find(NULL) == NULL);
  CHECK(mc_coverart_find(dir) == NULL);
  touch_in(dir, "notes.txt");
  touch_in(dir, "front.png");
  CHECK(found_is(mc_coverart_find(dir), "front.png"));
  touch_in(dir, "Folder.JPG");
  CHECK(found_is(mc_coverart_find(dir), "Folder.JPG"));
  touch_in(dir, "cover.png");
  CHECK(found_is(mc_coverart_find(dir), "cover.png"));
  touch_in(dir, "Cover.jpeg");
  CHECK(found_is(mc_coverart_find(dir), "Cover.jpeg"));
}

static void clear_dir(const char *dir);
static void clear_entry(void *user, const char *name, int is_dir) {
  char p[1200];
  snprintf(p, sizeof p, "%s/%s", (const char *)user, name);
  if (is_dir) {
    clear_dir(p);
    mc_rmdir(p);
  } else {
    mc_remove(p);
  }
}
static void clear_dir(const char *dir) {
  mc_list_dir(dir, clear_entry, (void *)dir);
}

int main(int argc, char **argv) {
  snprintf(g_lib_dir, sizeof g_lib_dir, "%s/test_playlist_art.d", argc > 1 ? argv[1] : ".");
  clear_dir(g_lib_dir);
  snprintf(g_cache, sizeof g_cache, "%s%cart-cache", g_lib_dir, MC_PATH_SEP);
  mc_library *lib = NULL;
  CHECK(mc_library_open(g_lib_dir, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();
  populate(lib);

  test_args(lib);
  test_few(lib);
  test_grid(lib);
  test_group(lib);
  test_bad_cover(lib);
  test_prune(lib);
  test_cover_find();

  mc_library_close(lib);
  return TEST_RESULT();
}
