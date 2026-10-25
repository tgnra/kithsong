#include "tagger/organize.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_COMPONENT 100 /* bytes; keeps full paths well under Windows' 260 */

static int is_device_name(const char *s, size_t len) {
  static const char *const names[] = {"con", "prn", "aux", "nul", NULL};
  /* Windows reserves these with any extension, e.g. "nul.txt". */
  const char *dot = memchr(s, '.', len);
  size_t base = dot ? (size_t)(dot - s) : len;
  char lower[5] = {0};
  if (base < 3 || base > 4) return 0;
  for (size_t i = 0; i < base; i++) lower[i] = (char)tolower((unsigned char)s[i]);
  for (const char *const *n = names; *n; n++)
    if (base == 3 && memcmp(lower, *n, 3) == 0) return 1;
  return base == 4 && (memcmp(lower, "com", 3) == 0 || memcmp(lower, "lpt", 3) == 0) &&
         lower[3] >= '1' && lower[3] <= '9';
}

char *mc_path_component(const char *s, const char *fallback) {
  size_t cap = (s ? strlen(s) : 0) + 2;
  char *out = malloc(cap > 4 ? cap : 4);
  if (!out) return NULL;
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    if (*p < 0x20 || strchr("<>:\"/\\|?*", *p)) out[n++] = '_';
    else if (n == 0 && (*p == '.' || *p == ' ')) continue; /* leading dots hide files */
    else out[n++] = (char)*p;
  }
  /* Truncate on a UTF-8 character boundary. */
  if (n > MAX_COMPONENT) {
    n = MAX_COMPONENT;
    while (n > 0 && ((unsigned char)out[n] & 0xC0) == 0x80) n--;
  }
  while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) n--; /* Windows strips these */
  out[n] = '\0';

  if (n == 0) {
    free(out);
    if (fallback) return mc_path_component(fallback, NULL);
    if ((out = malloc(2))) strcpy(out, "_");
    return out;
  }
  if (is_device_name(out, n)) {
    out[n++] = '_';
    out[n] = '\0';
  }
  return out;
}

char *mc_album_dir(const char *album_artist, const char *album, int year) {
  char *artist = mc_path_component(album_artist, "Unknown Artist");
  char *name;
  if (year > 0 && album) {
    size_t len = strlen(album) + 16;
    char *with_year = malloc(len);
    if (with_year) snprintf(with_year, len, "%s (%d)", album, year);
    name = with_year ? mc_path_component(with_year, "Unknown Album") : NULL;
    free(with_year);
  } else {
    name = mc_path_component(album, "Unknown Album");
  }
  char *out = NULL;
  if (artist && name) {
    size_t len = strlen(artist) + strlen(name) + 2;
    if ((out = malloc(len))) snprintf(out, len, "%s/%s", artist, name);
  }
  free(artist);
  free(name);
  return out;
}

char *mc_track_file(const char *title, int disc, int disc_total, int track, const char *ext) {
  char prefix[24] = "";
  if (track > 0 && disc_total > 1 && disc > 0) snprintf(prefix, sizeof prefix, "%d-%02d ", disc, track);
  else if (track > 0) snprintf(prefix, sizeof prefix, "%02d ", track);

  size_t len = strlen(prefix) + (title ? strlen(title) : 0) + 1;
  char *joined = malloc(len);
  if (!joined) return NULL;
  snprintf(joined, len, "%s%s", prefix, title ? title : "");
  char *name = mc_path_component(joined, "Untitled");
  free(joined);
  if (!name || !ext || !*ext) return name;

  size_t out_len = strlen(name) + strlen(ext) + 2;
  char *out = malloc(out_len);
  if (out) snprintf(out, out_len, "%s.%s", name, ext);
  free(name);
  return out;
}
