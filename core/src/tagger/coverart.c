#include "tagger/coverart.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>
#include <yyjson.h>

#include "tagger/match.h"
#include "tagger/tag_io.h"
#include "util/platform.h"

#define CAA_API "https://coverartarchive.org"
#define ITUNES_API "https://itunes.apple.com/search"
#define MAX_IMAGE_BYTES (16u * 1024u * 1024u)

/* Apple doesn't publish a limit; about 20 requests a minute is the commonly cited one. */
static mc_ratelimit g_itunes_rate;

void mc_coverart_global_init(void) {
  mc_ratelimit_init(&g_itunes_rate, 3000);
}

static const char *image_ext(const mc_buffer *b) {
  const unsigned char *p = (const unsigned char *)b->data;
  if (b->len > 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "jpg";
  if (b->len > 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return "png";
  return NULL;
}

/* GETs url and keeps the body only if it is a reasonably sized JPEG or PNG. */
static mc_status get_image(const char *url, mc_buffer *img, const char **ext) {
  long http = 0;
  mc_status st = mc_http_get(url, img, &http);
  if (st != MC_OK) return st;
  if (http == 200 && img->len <= MAX_IMAGE_BYTES && (*ext = image_ext(img))) return MC_OK;
  mc_buffer_free(img);
  return http == 404 || http == 200 ? MC_ERR_NOT_FOUND : MC_ERR_NET;
}

static mc_status from_caa(const char *entity, const char *id, mc_buffer *img, const char **ext) {
  char url[160];
  /* 1200 px thumbnails are JPEG, unlike the original upload, which can be anything. */
  snprintf(url, sizeof url, CAA_API "/%s/%s/front-1200", entity, id);
  return get_image(url, img, ext);
}

/* Best iTunes album for artist + album, or MC_ERR_NOT_FOUND if none is close enough. */
static mc_status from_itunes(const char *artist, const char *album, mc_buffer *img,
                             const char **ext) {
  size_t tlen = strlen(artist) + strlen(album) + 2;
  char *term = malloc(tlen);
  if (!term) return MC_ERR_NOMEM;
  snprintf(term, tlen, "%s %s", artist, album);
  char *esc = mc_url_escape(term);
  free(term);
  if (!esc) return MC_ERR_NOMEM;

  size_t ulen = strlen(esc) + 96;
  char *url = malloc(ulen);
  if (!url) {
    free(esc);
    return MC_ERR_NOMEM;
  }
  snprintf(url, ulen, ITUNES_API "?term=%s&media=music&entity=album&limit=10", esc);
  free(esc);

  mc_ratelimit_wait(&g_itunes_rate);
  mc_buffer body;
  long http = 0;
  mc_status st = mc_http_get(url, &body, &http);
  free(url);
  if (st != MC_OK) return st;
  yyjson_doc *doc = http == 200 ? yyjson_read(body.data, body.len, 0) : NULL;
  mc_buffer_free(&body);
  if (!doc) return http == 200 ? MC_ERR_FORMAT : MC_ERR_NET;

  const char *best = NULL;
  double best_d = 0.25; /* combined distance must beat this */
  size_t idx, max;
  yyjson_val *r;
  yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "results"), idx, max, r) {
    const char *art = yyjson_get_str(yyjson_obj_get(r, "artworkUrl100"));
    double da = mc_string_distance(album, yyjson_get_str(yyjson_obj_get(r, "collectionName")));
    double dr = mc_string_distance(artist, yyjson_get_str(yyjson_obj_get(r, "artistName")));
    if (!art || da < 0 || dr < 0) continue;
    double d = 0.6 * da + 0.4 * dr;
    if (d < best_d) best = art, best_d = d;
  }

  st = MC_ERR_NOT_FOUND;
  if (best) {
    /* artworkUrl100 ends in ".../100x100bb.jpg"; the CDN serves other sizes on request. */
    char art[1024];
    const char *size = strstr(best, "100x100bb");
    if (size && strlen(best) < sizeof art - 8) {
      snprintf(art, sizeof art, "%.*s1200x1200bb%s", (int)(size - best), best, size + 9);
      st = get_image(art, img, ext);
    }
  }
  yyjson_doc_free(doc);
  return st;
}

mc_status mc_coverart_fetch(const char *mb_albumid, const char *mb_releasegroupid,
                            const char *artist, const char *album, mc_buffer *img,
                            const char **ext) {
  img->data = NULL;
  img->len = 0;
  mc_status st = MC_ERR_NOT_FOUND;
  if (mb_albumid) st = from_caa("release", mb_albumid, img, ext);
  if (st != MC_OK && mb_releasegroupid) st = from_caa("release-group", mb_releasegroupid, img, ext);
  if (st != MC_OK && artist && album) st = from_itunes(artist, album, img, ext);
  return st;
}

typedef struct cover_search {
  int best;   /* index into k_cover_names, lower is better */
  char *name; /* the matching entry */
} cover_search;

static const char *const k_cover_names[] = {"cover.jpg",  "cover.jpeg",  "cover.png",
                                            "folder.jpg", "folder.jpeg", "folder.png",
                                            "front.jpg",  "front.jpeg",  "front.png"};

static int equals_nocase(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return !*a && !*b;
}

static void on_cover_entry(void *user, const char *name, int is_dir) {
  cover_search *s = user;
  if (is_dir) return;
  for (int i = 0; i < (int)(sizeof k_cover_names / sizeof k_cover_names[0]) && i < s->best; i++) {
    if (!equals_nocase(name, k_cover_names[i])) continue;
    char *copy = mc_strdup(name);
    if (copy) {
      free(s->name);
      s->name = copy;
      s->best = i;
    }
    break;
  }
}

char *mc_coverart_find(const char *dir) {
  cover_search s = {INT_MAX, NULL};
  mc_list_dir(dir, on_cover_entry, &s);
  if (!s.name) return NULL;
  size_t len = strlen(dir) + strlen(s.name) + 2;
  char *path = malloc(len);
  if (path) snprintf(path, len, "%s%c%s", dir, MC_PATH_SEP, s.name);
  free(s.name);
  return path;
}

mc_status mc_coverart_save(const char *dir, const mc_buffer *img, const char *ext) {
  mc_status st = mc_mkdirs(dir);
  if (st != MC_OK) return st;
  size_t plen = strlen(dir) + 16;
  char *path = malloc(plen), *tmp = malloc(plen + 4);
  if (!path || !tmp) {
    free(path);
    free(tmp);
    return MC_ERR_NOMEM;
  }
  snprintf(path, plen, "%s%ccover.%s", dir, MC_PATH_SEP, ext);
  snprintf(tmp, plen + 4, "%s.tmp", path);

  FILE *f = mc_fopen(tmp, "wb");
  st = MC_ERR_IO;
  if (f) {
    int ok = fwrite(img->data, 1, img->len, f) == img->len;
    if (fclose(f) == 0 && ok) st = mc_rename(tmp, path, 1);
    if (st != MC_OK) mc_remove(tmp);
  }
  free(path);
  free(tmp);
  return st;
}

mc_status mc_coverart_save_embedded(const char *dir, const char *audio_path) {
  unsigned char *data;
  size_t len;
  mc_status st = mc_tags_read_picture(audio_path, &data, &len);
  if (st != MC_OK) return st;
  mc_buffer img = {(char *)data, len};
  const char *ext = len <= MAX_IMAGE_BYTES ? image_ext(&img) : NULL;
  st = ext ? mc_coverart_save(dir, &img, ext) : MC_ERR_FORMAT;
  free(data);
  return st;
}

mc_status mc_coverart_hash(const char *path, char hash[MC_ART_HASH_LEN]) {
  static const char hex[] = "0123456789abcdef";
  FILE *f = mc_fopen(path, "rb");
  if (!f) return MC_ERR_IO;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mc_status st = mbedtls_sha256_starts(&ctx, 0) == 0 ? MC_OK : MC_ERR_IO;
  unsigned char buf[1 << 15];
  size_t n;
  while (st == MC_OK && (n = fread(buf, 1, sizeof buf, f)) > 0)
    if (mbedtls_sha256_update(&ctx, buf, n) != 0) st = MC_ERR_IO;
  if (ferror(f)) st = MC_ERR_IO;
  fclose(f);

  unsigned char digest[32];
  if (st == MC_OK && mbedtls_sha256_finish(&ctx, digest) != 0) st = MC_ERR_IO;
  mbedtls_sha256_free(&ctx);
  if (st != MC_OK) return st;
  for (int i = 0; i < 32; i++) {
    hash[2 * i] = hex[digest[i] >> 4];
    hash[2 * i + 1] = hex[digest[i] & 15];
  }
  hash[64] = '\0';
  return MC_OK;
}
