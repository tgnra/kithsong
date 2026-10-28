/* Playlist cover art: a 2x2 grid of album covers, see playlist.h.
 *
 * Picking: every distinct cover (albums.art_hash) among the playlist's albums gets a rank,
 * the first 8 bytes of SHA-256(playlist uid, '\n', art hash), and the lowest four win. That
 * is a random choice seeded by the playlist, but unlike shuffling with a seeded generator it
 * only depends on each cover's own rank: adding or removing an album leaves the picture
 * alone unless that album's cover is (or would be) one of the four.
 *
 * Caching: the grid is saved as <cache_dir>/<playlist uid>-<key>.jpg, the key being a hash of
 * the four art hashes in grid order (and ART_VERSION). Art hashes are of the image contents,
 * so a replaced cover changes the key too. Nothing is stored in the database.
 *
 * Album groups get the same grid, but their covers rank by the group's order (so the first
 * four win) and share the cache folder: uids never clash. */
#include "musiccore/playlist.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include "library/library_internal.h"
#include "tagger/coverart.h"
#include "util/platform.h"

#define GRID_COVERS 4
#define TILE (MC_PLAYLIST_ART_SIZE / 2)
#define KEY_LEN 16                         /* hex digits of the cache key */
#define MAX_COVER_BYTES (32u * 1024u * 1024u)
#define MAX_COVER_SIDE 10000               /* pixels; bigger covers are passed over */
#define JPEG_QUALITY 90
#define ART_VERSION "grid-1"               /* change to remake every cached grid */
#define STALE_TMP_MS (60 * 60 * 1000)      /* a temp file this old was abandoned */

/* A cover candidate: one album with art, ranked for this playlist. */
typedef struct cover_row {
  uint8_t rank[8];
  char hash[MC_ART_HASH_LEN];
  char *rel; /* one of the album's files, relative to the library */
} cover_row;

static int compare_rows(const void *a, const void *b) {
  const cover_row *x = a, *y = b;
  int c = memcmp(x->rank, y->rank, sizeof x->rank);
  return c ? c : strcmp(x->hash, y->hash);
}

static void free_rows(cover_row *rows, int count) {
  for (int i = 0; i < count; i++) free(rows[i].rel);
  free(rows);
}

static int sha256_parts(const char *const *parts, int count, uint8_t digest[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  int rc = mbedtls_sha256_starts(&ctx, 0);
  for (int i = 0; rc == 0 && i < count; i++)
    rc = mbedtls_sha256_update(&ctx, (const unsigned char *)parts[i], strlen(parts[i]));
  if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  return rc;
}

/* Any one file finds the album's folder, as in ALBUM_COLS; originals first. */
#define ALBUM_FILE                                                                   \
  "(SELECT f.path FROM tracks t2 JOIN files f ON f.track_uid = t2.uid"              \
  "  WHERE t2.album_uid = al.uid ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)"

/* The playlist's (or album group's) albums with art, best ranked first. MC_ERR_NOT_FOUND if
 * there's no such playlist (or group). */
static mc_status load_rows(mc_library *lib, const char *uid, int group, cover_row **out, int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt;
  const char *exists = group ? "SELECT 1 FROM album_groups WHERE uid = ?1" : "SELECT 1 FROM playlists WHERE uid = ?1";
  if (sqlite3_prepare_v2(lib->db, exists, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) return MC_ERR_NOT_FOUND;
  if (rc != SQLITE_ROW) return MC_ERR_DB;

  static const char playlist_sql[] =
    "SELECT al.art_hash, " ALBUM_FILE
    " FROM albums al WHERE al.art_hash IS NOT NULL AND al.uid IN"
    "  (SELECT t.album_uid FROM playlist_entries e JOIN tracks t ON t.uid = e.track_uid"
    "   WHERE e.playlist_uid = ?1)";
  static const char group_sql[] =
    "SELECT al.art_hash, " ALBUM_FILE
    " FROM album_group_entries e JOIN albums al ON al.uid = e.album_uid"
    " WHERE e.group_uid = ?1 AND al.art_hash IS NOT NULL"
    " GROUP BY al.uid ORDER BY min(e.position), al.uid";
  if (sqlite3_prepare_v2(lib->db, group ? group_sql : playlist_sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);

  cover_row *rows = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *hash = (const char *)sqlite3_column_text(stmt, 0);
    const char *rel = (const char *)sqlite3_column_text(stmt, 1);
    if (!hash || !rel || strlen(hash) >= MC_ART_HASH_LEN) continue; /* no files here */
    if (n == cap) {
      cover_row *grown = realloc(rows, (size_t)(cap = cap ? cap * 2 : 16) * sizeof *rows);
      if (!grown) {
        st = MC_ERR_NOMEM;
        break;
      }
      rows = grown;
    }
    cover_row *r = &rows[n];
    strcpy(r->hash, hash);
    if (group) { /* the group's order: n, big-endian */
      for (int b = 0; b < 8; b++) r->rank[b] = (uint8_t)((uint64_t)n >> (56 - 8 * b));
    } else {
      uint8_t digest[32];
      const char *parts[] = {uid, "\n", hash};
      if (sha256_parts(parts, 3, digest) != 0) {
        st = MC_ERR_IO;
        break;
      }
      memcpy(r->rank, digest, sizeof r->rank);
    }
    if (!(r->rel = mc_strdup(rel))) st = MC_ERR_NOMEM;
    else n++;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    free_rows(rows, n);
    return st;
  }
  if (n > 1) qsort(rows, (size_t)n, sizeof *rows, compare_rows);
  *out = rows;
  *count = n;
  return MC_OK;
}

/* Walks the ranked rows: the next cover file on this device whose image isn't spent (see
 * spend_hash). Returns its row index and sets *path (malloc'd), or -1 at the end. */
static int next_cover(mc_library *lib, const cover_row *rows, int count, int *next, char **path) {
  *path = NULL;
  for (; *next < count; (*next)++) {
    int i = *next;
    if (!rows[i].hash[0]) continue; /* spent: its image is already in use */
    char *dir = mc_library_abspath(lib, rows[i].rel);
    char *sep = dir ? strrchr(dir, MC_PATH_SEP) : NULL;
    if (sep) *sep = '\0';
    *path = sep ? mc_coverart_find(dir) : NULL;
    free(dir);
    if (!*path) continue;
    (*next)++;
    return i;
  }
  return -1;
}

/* After row i gives a cover, later rows with the same image are spent. A playlist's rank
 * them next to each other; an album group's can be anywhere after. */
static void spend_hash(cover_row *rows, int count, int i) {
  for (int j = i + 1; j < count; j++)
    if (strcmp(rows[j].hash, rows[i].hash) == 0) rows[j].hash[0] = '\0';
}

/* Reads a whole file of at most MAX_COVER_BYTES. */
static unsigned char *read_file(const char *path, size_t *len) {
  int64_t size = 0, mtime;
  if (mc_file_info(path, &size, &mtime) != MC_OK || size <= 0 || size > (int64_t)MAX_COVER_BYTES)
    return NULL;
  FILE *f = mc_fopen(path, "rb");
  if (!f) return NULL;
  unsigned char *buf = malloc((size_t)size);
  if (buf && fread(buf, 1, (size_t)size, f) != (size_t)size) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  *len = (size_t)size;
  return buf;
}

/* Decodes a cover, crops it to the centre square and scales that to TILE x TILE, averaging
 * the source pixels under each output pixel. Writes RGB into the grid at (gx, gy). */
static int draw_tile(const char *path, unsigned char *grid, int gx, int gy) {
  size_t len = 0;
  unsigned char *data = read_file(path, &len);
  if (!data) return 0;
  int w = 0, h = 0, comp = 0;
  unsigned char *px = NULL;
  if (len <= INT32_MAX && stbi_info_from_memory(data, (int)len, &w, &h, &comp) && w > 0 && h > 0 &&
      w <= MAX_COVER_SIDE && h <= MAX_COVER_SIDE)
    px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
  free(data);
  if (!px) return 0;

  int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
  for (int ty = 0; ty < TILE; ty++) {
    int sy0 = y0 + ty * side / TILE, sy1 = y0 + (ty + 1) * side / TILE;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    unsigned char *out = grid + ((size_t)(gy * TILE + ty) * MC_PLAYLIST_ART_SIZE + (size_t)gx * TILE) * 3;
    for (int tx = 0; tx < TILE; tx++) {
      int sx0 = x0 + tx * side / TILE, sx1 = x0 + (tx + 1) * side / TILE;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      uint32_t sum[3] = {0, 0, 0};
      for (int y = sy0; y < sy1; y++) {
        const unsigned char *p = px + ((size_t)y * (size_t)w + (size_t)sx0) * 3;
        for (int x = sx0; x < sx1; x++, p += 3) {
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
        }
      }
      uint32_t n = (uint32_t)((sy1 - sy0) * (sx1 - sx0));
      for (int c = 0; c < 3; c++) out[tx * 3 + c] = (unsigned char)((sum[c] + n / 2) / n);
    }
  }
  stbi_image_free(px);
  return 1;
}

/* <cache_dir>/<playlist uid>-<key>.jpg for the four covers' art hashes, in grid order. */
static char *grid_path(const char *cache_dir, const char *playlist_uid, const char *const hashes[GRID_COVERS]) {
  const char *parts[] = {ART_VERSION, "\n", hashes[0], "\n", hashes[1], "\n", hashes[2], "\n", hashes[3]};
  uint8_t digest[32];
  if (sha256_parts(parts, 9, digest) != 0) return NULL;
  char key[KEY_LEN + 1];
  for (int i = 0; i < KEY_LEN / 2; i++) snprintf(key + 2 * i, 3, "%02x", digest[i]);
  size_t len = strlen(cache_dir) + strlen(playlist_uid) + KEY_LEN + 8;
  char *path = malloc(len);
  if (path) snprintf(path, len, "%s%c%s-%s.jpg", cache_dir, MC_PATH_SEP, playlist_uid, key);
  return path;
}

typedef struct jpeg_sink {
  FILE *f;
  int failed;
} jpeg_sink;

static void write_jpeg(void *user, void *data, int size) {
  jpeg_sink *s = user;
  if (!s->failed && fwrite(data, 1, (size_t)size, s->f) != (size_t)size) s->failed = 1;
}

/* Writes the grid to path, atomically: a temp file with a random name, then a rename, so
 * overlapping calls never see a partial file. */
static mc_status save_grid(const unsigned char *grid, const char *path) {
  uint8_t r[4];
  if (mc_random_bytes(r, sizeof r) != MC_OK) return MC_ERR_IO;
  size_t len = strlen(path) + 16;
  char *tmp = malloc(len);
  if (!tmp) return MC_ERR_NOMEM;
  snprintf(tmp, len, "%s.%02x%02x%02x%02x.tmp", path, r[0], r[1], r[2], r[3]);

  mc_status st = MC_ERR_IO;
  jpeg_sink sink = {mc_fopen(tmp, "wb"), 0};
  if (sink.f) {
    int ok = stbi_write_jpg_to_func(write_jpeg, &sink, MC_PLAYLIST_ART_SIZE, MC_PLAYLIST_ART_SIZE, 3,
                                    grid, JPEG_QUALITY);
    if (fclose(sink.f) == 0 && ok && !sink.failed) st = mc_rename(tmp, path, 1);
    if (st != MC_OK) mc_remove(tmp);
  }
  free(tmp);
  return st;
}

/* A cache file name: <uid>-<key>.jpg. */
static int is_grid_name(const char *name) {
  size_t n = strlen(name);
  if (n != MC_UID_STRLEN - 1 + 1 + KEY_LEN + 4 || name[MC_UID_STRLEN - 1] != '-') return 0;
  if (strcmp(name + n - 4, ".jpg") != 0) return 0;
  for (size_t i = MC_UID_STRLEN; i < n - 4; i++)
    if (!strchr("0123456789abcdef", name[i])) return 0;
  return 1;
}

/* A temp file save_grid() leaves if it's killed: <uid>-<key>.jpg.<8 hex>.tmp */
static int is_grid_tmp(const char *name) {
  size_t n = strlen(name), base = MC_UID_STRLEN + KEY_LEN + 4;
  return n == base + 13 && strcmp(name + n - 4, ".tmp") == 0;
}

typedef struct name_list {
  char **names;
  int count, cap;
  int failed;
} name_list;

static void collect_name(void *user, const char *name, int is_dir) {
  name_list *l = user;
  if (is_dir || l->failed || !(is_grid_name(name) || is_grid_tmp(name))) return;
  if (l->count == l->cap) {
    char **grown = realloc(l->names, (size_t)(l->cap = l->cap ? l->cap * 2 : 16) * sizeof *grown);
    if (!grown) {
      l->failed = 1;
      return;
    }
    l->names = grown;
  }
  if ((l->names[l->count] = mc_strdup(name))) l->count++;
  else l->failed = 1;
}

static void free_names(name_list *l) {
  for (int i = 0; i < l->count; i++) free(l->names[i]);
  free(l->names);
}

static char *join(const char *dir, const char *name) {
  size_t len = strlen(dir) + strlen(name) + 2;
  char *p = malloc(len);
  if (p) snprintf(p, len, "%s%c%s", dir, MC_PATH_SEP, name);
  return p;
}

/* Deletes the playlist's older grids (anything but keep). Failures are ignored: a file
 * that's open elsewhere goes next time. */
static void remove_old_grids(const char *cache_dir, const char *playlist_uid, const char *keep) {
  name_list l = {0};
  mc_list_dir(cache_dir, collect_name, &l);
  const char *keep_name = strrchr(keep, MC_PATH_SEP);
  keep_name = keep_name ? keep_name + 1 : keep;
  for (int i = 0; i < l.count; i++) {
    const char *name = l.names[i];
    if (!is_grid_name(name) || strncmp(name, playlist_uid, MC_UID_STRLEN - 1) != 0 ||
        strcmp(name, keep_name) == 0)
      continue;
    char *path = join(cache_dir, name);
    if (path) mc_remove(path);
    free(path);
  }
  free_names(&l);
}

/* mc_playlist_art(), or for group mc_album_group_art(); playlist_uid is either's uid. */
static mc_status make_art(mc_library *lib, const char *playlist_uid, int group, const char *cache_dir,
                          char **out_path) {
  if (out_path) *out_path = NULL;
  if (!lib || !playlist_uid || strlen(playlist_uid) != MC_UID_STRLEN - 1 || !cache_dir || !*cache_dir ||
      !out_path)
    return MC_ERR_INVALID_ARG;

  cover_row *rows;
  int count, next = 0;
  mc_status st = load_rows(lib, playlist_uid, group, &rows, &count);
  if (st != MC_OK) return st;

  /* The top four covers on this device. */
  char *paths[GRID_COVERS] = {0};
  const char *hashes[GRID_COVERS];
  int found = 0;
  while (found < GRID_COVERS) {
    int i = next_cover(lib, rows, count, &next, &paths[found]);
    if (i < 0) break;
    spend_hash(rows, count, i);
    hashes[found++] = rows[i].hash;
  }

  char *grid_file = NULL;
  unsigned char *grid = NULL;
  if (found == GRID_COVERS && !(grid_file = grid_path(cache_dir, playlist_uid, hashes))) st = MC_ERR_NOMEM;
  if (st == MC_OK && grid_file && !mc_file_exists(grid_file)) {
    /* Make it. A cover that won't decode gives up its place to the next in line. */
    if (!(grid = malloc((size_t)MC_PLAYLIST_ART_SIZE * MC_PLAYLIST_ART_SIZE * 3))) st = MC_ERR_NOMEM;
    int drawn = 0;
    while (st == MC_OK && drawn < GRID_COVERS) {
      if (draw_tile(paths[drawn], grid, drawn % 2, drawn / 2)) {
        drawn++;
        continue;
      }
      /* Shift the later ones up and look for another at the end. */
      free(paths[drawn]);
      memmove(&paths[drawn], &paths[drawn + 1], (size_t)(GRID_COVERS - 1 - drawn) * sizeof *paths);
      memmove(&hashes[drawn], &hashes[drawn + 1], (size_t)(GRID_COVERS - 1 - drawn) * sizeof *hashes);
      int i = next_cover(lib, rows, count, &next, &paths[GRID_COVERS - 1]);
      if (i < 0) break;
      spend_hash(rows, count, i);
      hashes[GRID_COVERS - 1] = rows[i].hash;
    }
    free(grid_file);
    grid_file = NULL;
    if (st == MC_OK && drawn == GRID_COVERS) {
      if (!(grid_file = grid_path(cache_dir, playlist_uid, hashes))) st = MC_ERR_NOMEM;
      else if (!mc_file_exists(grid_file) && (st = mc_mkdirs(cache_dir)) == MC_OK &&
               (st = save_grid(grid, grid_file)) == MC_OK)
        remove_old_grids(cache_dir, playlist_uid, grid_file);
    } else if (st == MC_OK) {
      /* Fewer than four decode: the best one that did, alone, else the best left untried
       * (the frontend's decoder may still manage it). */
      found = paths[0] != NULL;
    }
  }

  if (st == MC_OK) {
    if (grid_file) {
      *out_path = grid_file;
      grid_file = NULL;
    } else if (found > 0) {
      *out_path = paths[0];
      paths[0] = NULL;
    }
  }
  free(grid_file);
  free(grid);
  for (int i = 0; i < GRID_COVERS; i++) free(paths[i]);
  free_rows(rows, count);
  return st;
}

mc_status mc_playlist_art(mc_library *lib, const char *playlist_uid, const char *cache_dir,
                          char **out_path) {
  return make_art(lib, playlist_uid, 0, cache_dir, out_path);
}

mc_status mc_album_group_art(mc_library *lib, const char *group_uid, const char *cache_dir,
                             char **out_path) {
  return make_art(lib, group_uid, 1, cache_dir, out_path);
}

mc_status mc_playlist_art_prune(mc_library *lib, const char *cache_dir) {
  if (!lib || !cache_dir || !*cache_dir) return MC_ERR_INVALID_ARG;
  name_list l = {0};
  mc_list_dir(cache_dir, collect_name, &l); /* a missing folder lists nothing */
  if (l.failed) {
    free_names(&l);
    return MC_ERR_NOMEM;
  }
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db,
                         "SELECT 1 FROM playlists WHERE uid = ?1 UNION ALL SELECT 1 FROM album_groups WHERE uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    free_names(&l);
    return MC_ERR_DB;
  }
  mc_status st = MC_OK;
  int64_t now = (int64_t)mc_wall_ms();
  for (int i = 0; st == MC_OK && i < l.count; i++) {
    const char *name = l.names[i];
    char *path = join(cache_dir, name);
    if (!path) {
      st = MC_ERR_NOMEM;
      break;
    }
    int remove = 0;
    if (is_grid_tmp(name)) {
      int64_t size, mtime = 0;
      remove = mc_file_info(path, &size, &mtime) == MC_OK && now - mtime > STALE_TMP_MS;
    } else {
      sqlite3_reset(stmt);
      sqlite3_bind_text(stmt, 1, name, MC_UID_STRLEN - 1, SQLITE_TRANSIENT);
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) remove = 1;
      else if (rc != SQLITE_ROW) st = MC_ERR_DB;
    }
    if (remove) mc_remove(path);
    free(path);
  }
  sqlite3_finalize(stmt);
  free_names(&l);
  return st;
}
