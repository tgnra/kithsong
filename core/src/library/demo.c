/* The demo library (kithcore/demo.h): six fictional albums, their covers drawn here, their
 * tracks silent FLACs, imported as-is. The data is the store listing's (albums.py and
 * covers.py of the Play Store assets), so the screenshots of every platform agree. */
#include "kithcore/demo.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb_image_write.h>

#include "kithcore/library.h"
#include "kithcore/library_move.h"
#include "kithcore/tagger.h"
#include "util/platform.h"

enum { MAX_TRACKS = 6, IMPORT_TIMEOUT_MS = 120000 };

typedef enum shape_kind { WAVES, SUN, MOONS, BARS, LINES, BLOCKS } shape_kind;

typedef struct demo_album {
  const char *artist, *title, *genre;
  int year;
  unsigned colors[3]; /* the palette: background, then two to draw with */
  shape_kind kind;
  int ntracks;
  const char *tracks[MAX_TRACKS];
} demo_album;

static const demo_album k_albums[MC_DEMO_ALBUMS] = {
  {"Marrowfield Choir", "Salt on the Sill", "Folk", 2021, {0x16324F, 0xE9C46A, 0xF4F1DE}, WAVES, 6,
   {"Salt on the Sill", "The Long Tide Home", "Grey Gulls", "Mending Nets", "Windward", "A Lamp for the Boats"}},
  {"Oona Vesper", "Low Country Radio", "Americana", 2023, {0x7A2E2E, 0xF2A65A, 0xFCEFD9}, SUN, 6,
   {"Low Country Radio", "Dust on the Dial", "Halfway to Morning", "Porchlight", "Cicada Static", "Last Call for Rain"}},
  {"Pale Orchard", "Nightjar Season", "Dream Pop", 2019, {0x1B1B3A, 0x9D8DF1, 0xE6E1FF}, MOONS, 5,
   {"Nightjar Season", "Windfall", "Soft Machinery", "Pollen Count", "Blue Hour Bicycle"}},
  {"Tomas Quillfeather", "Small Hours, Big Rooms", "Jazz", 2022, {0x0F3D3E, 0xE2A03F, 0xF1EFE7}, BARS, 6,
   {"Small Hours", "Big Rooms", "Coat Check Blues", "Third Set", "Elevator to the Lobby", "Goodnight, Mezzanine"}},
  {"Kestrel Bay Ensemble", "Field Notes for Late Trains", "Ambient", 2020, {0x2F5D50, 0xCFE3C4, 0xF7F7EE}, LINES, 5,
   {"Platform Four", "Sleeper Car", "Night Freight", "Field Notes", "Terminus"}},
  {"Ivy Sunderholt", "Copperline", "Indie Rock", 2024, {0xB5432A, 0x1F2A44, 0xFBE9D0}, BLOCKS, 6,
   {"Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running",
    "Copperline (Reprise)"}},
};

/* Opened after the import, in this order, so the last is the newest recent. */
static const int k_opened[] = {2, 0, 5};

const char *mc_demo_album_artist(int i) {
  return i >= 0 && i < MC_DEMO_ALBUMS ? k_albums[i].artist : NULL;
}

const char *mc_demo_album_title(int i) {
  return i >= 0 && i < MC_DEMO_ALBUMS ? k_albums[i].title : NULL;
}

/* --- Numbers --- */

static uint64_t next(uint64_t *state) { /* splitmix64 */
  uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/* lo ..= hi */
static int between(uint64_t *state, int lo, int hi) {
  return lo + (int)(next(state) % (uint64_t)(hi - lo + 1));
}

/* Track t of album a's length in seconds, 148 to 292 as in the listing's files. */
static int track_seconds(int a, int t) {
  uint64_t s = 0x64656D6Full + (uint64_t)(a * 16 + t);
  return between(&s, 148, 292);
}

/* --- Covers --- */

/* Drawn in the 1000 x 1000 space covers.py's SVGs use, sampled at twice the size and halved. */
typedef struct canvas {
  unsigned char *px; /* RGB */
  int n;             /* pixels a side */
  double unit;       /* pixels per SVG unit */
} canvas;

typedef enum op { RECT, CIRCLE, RING, CAPSULE, WAVE } op;

typedef struct shape {
  op op;
  double a, b, c, d, e; /* RECT x y w h; CIRCLE cx cy r; RING cx cy r width; CAPSULE x0 y0 x1 y1 width;
                         * WAVE y width */
} shape;

static int inside(const shape *s, double x, double y) {
  switch (s->op) {
    case RECT: return x >= s->a && x < s->a + s->c && y >= s->b && y < s->b + s->d;
    case CIRCLE: return (x - s->a) * (x - s->a) + (y - s->b) * (y - s->b) <= s->c * s->c;
    case RING: {
      double r = sqrt((x - s->a) * (x - s->a) + (y - s->b) * (y - s->b));
      return fabs(r - s->c) <= s->d / 2;
    }
    case CAPSULE: { /* distance to the segment */
      double dx = s->c - s->a, dy = s->d - s->b, len2 = dx * dx + dy * dy;
      double t = len2 > 0 ? ((x - s->a) * dx + (y - s->b) * dy) / len2 : 0;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      double px = s->a + t * dx - x, py = s->b + t * dy - y;
      return px * px + py * py <= s->e * s->e / 4;
    }
    case WAVE: {
      /* "M-50,y q125,-70 250,0 t250,0 ...": arcs 250 wide, a parabola each, up then down. */
      double u = (x + 50) / 250;
      if (u < 0 || u >= 5) return 0;
      int k = (int)u;
      double t = u - k, sign = k % 2 ? 1 : -1;
      double cy = s->a + sign * 140 * t * (1 - t), slope = sign * 140 * (1 - 2 * t) / 250;
      return fabs(y - cy) / sqrt(1 + slope * slope) <= s->b / 2;
    }
  }
  return 0;
}

/* The shape's bounds in SVG units: x0 y0 x1 y1. */
static void bounds(const shape *s, double out[4]) {
  switch (s->op) {
    case RECT: out[0] = s->a, out[1] = s->b, out[2] = s->a + s->c, out[3] = s->b + s->d; return;
    case CIRCLE: out[0] = s->a - s->c, out[1] = s->b - s->c, out[2] = s->a + s->c, out[3] = s->b + s->c; return;
    case RING: {
      double r = s->c + s->d;
      out[0] = s->a - r, out[1] = s->b - r, out[2] = s->a + r, out[3] = s->b + r;
      return;
    }
    case CAPSULE: {
      double w = s->e;
      out[0] = (s->a < s->c ? s->a : s->c) - w, out[1] = (s->b < s->d ? s->b : s->d) - w;
      out[2] = (s->a > s->c ? s->a : s->c) + w, out[3] = (s->b > s->d ? s->b : s->d) + w;
      return;
    }
    case WAVE: out[0] = 0, out[1] = s->a - 40 - s->b, out[2] = 1000, out[3] = s->a + 40 + s->b; return;
  }
}

static void paint(canvas *cv, shape s, unsigned color, double opacity) {
  double b[4];
  bounds(&s, b);
  int x0 = (int)floor(b[0] * cv->unit), y0 = (int)floor(b[1] * cv->unit);
  int x1 = (int)ceil(b[2] * cv->unit), y1 = (int)ceil(b[3] * cv->unit);
  x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
  x1 = x1 > cv->n ? cv->n : x1, y1 = y1 > cv->n ? cv->n : y1;
  int rgb[3] = {(int)(color >> 16) & 255, (int)(color >> 8) & 255, (int)color & 255};
  int alpha = (int)(opacity * 256 + 0.5);
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      if (!inside(&s, (x + 0.5) / cv->unit, (y + 0.5) / cv->unit)) continue;
      unsigned char *p = cv->px + ((size_t)y * (size_t)cv->n + (size_t)x) * 3;
      for (int i = 0; i < 3; i++) p[i] = (unsigned char)((rgb[i] * alpha + p[i] * (256 - alpha)) >> 8);
    }
}

static shape rect(double x, double y, double w, double h) {
  return (shape){RECT, x, y, w, h, 0};
}
static shape circle(double cx, double cy, double r) {
  return (shape){CIRCLE, cx, cy, r, 0, 0};
}

static void draw(canvas *cv, int album) {
  const demo_album *al = &k_albums[album];
  unsigned a = al->colors[0], b = al->colors[1], c = al->colors[2];
  uint64_t r = 0x636F766572ull + (uint64_t)album; /* "cover" */
  paint(cv, rect(0, 0, 1000, 1000), a, 1);
  switch (al->kind) {
    case WAVES:
      paint(cv, circle(720, 300, 130), b, 1);
      for (int i = 0; i < 7; i++) paint(cv, (shape){WAVE, 520 + i * 72, 14, 0, 0, 0}, c, 0.9 - i * 0.1);
      break;
    case SUN:
      paint(cv, rect(0, 640, 1000, 360), b, 0.35);
      paint(cv, circle(500, 640, 280), b, 1);
      for (int i = 0; i < 5; i++) paint(cv, rect(0, 660 + i * 62, 1000, 10 + i * 7), a, 1);
      break;
    case MOONS:
      for (int i = 0; i < 5; i++) {
        double x = 140 + i * 180;
        paint(cv, circle(x, 500, 82), c, 1);
        paint(cv, circle(x + (i - 2) * 36, 500, 82), a, 1);
      }
      paint(cv, (shape){RING, 500, 500, 330, 10, 0}, b, 1);
      break;
    case BARS:
      for (int i = 0; i < 14; i++) {
        int h = between(&r, 180, 760);
        double x = 70 + i * 63 + 17, top = (1000 - h) / 2 + 17, bottom = (1000 - h) / 2 + h - 17;
        paint(cv, (shape){CAPSULE, x, top, x, bottom, 34}, i % 3 ? b : c, 1);
      }
      break;
    case LINES:
      for (int i = 0; i < 22; i++) {
        double y = 130 + i * 34, x0 = between(&r, 90, 330), x1 = between(&r, 640, 910);
        paint(cv, (shape){CAPSULE, x0, y, x1, y, 7}, i % 4 ? c : b, 1);
      }
      break;
    case BLOCKS:
      paint(cv, rect(120, 120, 480, 760), b, 1);
      paint(cv, rect(400, 300, 480, 400), c, 1);
      paint(cv, circle(640, 500, 110), a, 1);
      break;
  }
}

typedef struct buffer {
  unsigned char *data;
  size_t len, cap;
  int failed;
} buffer;

static void put(buffer *b, const void *data, size_t n) {
  if (b->failed) return;
  if (b->len + n > b->cap) {
    size_t cap = (b->len + n) * 2 + 256;
    unsigned char *p = realloc(b->data, cap);
    if (!p) {
      b->failed = 1;
      return;
    }
    b->data = p;
    b->cap = cap;
  }
  memcpy(b->data + b->len, data, n);
  b->len += n;
}

static void jpeg_write(void *ctx, void *data, int size) {
  put(ctx, data, (size_t)size);
}

mc_status mc_demo_cover_jpeg(int i, int size, unsigned char **data, size_t *len) {
  if (data) *data = NULL;
  if (len) *len = 0;
  if (i < 0 || i >= MC_DEMO_ALBUMS || size < 64 || size > 2000 || !data || !len) return MC_ERR_INVALID_ARG;
  canvas cv = {malloc((size_t)size * size * 12), size * 2, size * 2 / 1000.0};
  unsigned char *out = malloc((size_t)size * size * 3);
  if (!cv.px || !out) {
    free(cv.px);
    free(out);
    return MC_ERR_NOMEM;
  }
  draw(&cv, i);
  for (int y = 0; y < size; y++) /* each pixel the mean of its four samples */
    for (int x = 0; x < size; x++)
      for (int k = 0; k < 3; k++) {
        const unsigned char *p = cv.px + ((size_t)(2 * y) * (size_t)cv.n + (size_t)(2 * x)) * 3 + k;
        size_t row = (size_t)cv.n * 3;
        out[((size_t)y * size + x) * 3 + k] = (unsigned char)((p[0] + p[3] + p[row] + p[row + 3] + 2) / 4);
      }
  free(cv.px);
  buffer b = {0};
  int ok = stbi_write_jpg_to_func(jpeg_write, &b, size, size, 3, out, 90);
  free(out);
  if (!ok || b.failed || !b.len) {
    free(b.data);
    return MC_ERR_NOMEM;
  }
  *data = b.data;
  *len = b.len;
  return MC_OK;
}

/* --- FLAC --- */

/* Silence costs a few bytes a frame: a CONSTANT subframe of zero. Fixed blocks of 4096 samples,
 * 44.1 kHz, 16-bit mono; STREAMINFO with no frame sizes or MD5, which FLAC allows. */
enum { FLAC_RATE = 44100, FLAC_BLOCK = 4096 };

static unsigned char crc8(const unsigned char *p, size_t n) {
  unsigned crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = crc & 0x80 ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
  }
  return (unsigned char)crc;
}

static unsigned crc16(const unsigned char *p, size_t n) {
  unsigned crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (unsigned)p[i] << 8;
    for (int k = 0; k < 8; k++) crc = crc & 0x8000 ? ((crc << 1) ^ 0x8005) & 0xFFFF : (crc << 1) & 0xFFFF;
  }
  return crc;
}

mc_status mc_demo_silent_flac(uint64_t n_samples, unsigned char **data, size_t *len) {
  if (data) *data = NULL;
  if (len) *len = 0;
  if (!data || !len || n_samples == 0 || n_samples >= (1ull << 36)) return MC_ERR_INVALID_ARG;
  buffer b = {0};
  unsigned char head[42] = {'f', 'L', 'a', 'C', 0x80 /* last block, STREAMINFO */, 0, 0, 34};
  unsigned char *si = head + 8;
  si[0] = FLAC_BLOCK >> 8, si[1] = FLAC_BLOCK & 255, si[2] = FLAC_BLOCK >> 8, si[3] = FLAC_BLOCK & 255;
  /* frame sizes (6 bytes) unknown: zero */
  uint64_t v = ((uint64_t)FLAC_RATE << 44) | (0ull << 41) | (15ull << 36) | n_samples;
  for (int i = 0; i < 8; i++) si[10 + i] = (unsigned char)(v >> (56 - 8 * i));
  /* MD5 (16 bytes) unknown: zero */
  put(&b, head, sizeof head);
  uint64_t frames = (n_samples + FLAC_BLOCK - 1) / FLAC_BLOCK;
  for (uint64_t f = 0; f < frames; f++) {
    unsigned char fr[24];
    size_t n = 0;
    uint64_t samples = f + 1 < frames ? FLAC_BLOCK : n_samples - f * FLAC_BLOCK;
    int full = samples == FLAC_BLOCK;
    fr[n++] = 0xFF;
    fr[n++] = 0xF8;                                   /* sync, fixed block size */
    fr[n++] = (unsigned char)((full ? 0xC : 0x7) << 4 | 0x9); /* 4096 or 16-bit size after; 44.1 kHz */
    fr[n++] = 0x08;                                   /* mono, 16 bits */
    if (f < 0x80) { /* the frame number, UTF-8 style; frames < 2^36 / 4096 fit in 4 bytes */
      fr[n++] = (unsigned char)f;
    } else if (f < 0x800) {
      fr[n++] = (unsigned char)(0xC0 | (f >> 6));
      fr[n++] = (unsigned char)(0x80 | (f & 0x3F));
    } else if (f < 0x10000) {
      fr[n++] = (unsigned char)(0xE0 | (f >> 12));
      fr[n++] = (unsigned char)(0x80 | ((f >> 6) & 0x3F));
      fr[n++] = (unsigned char)(0x80 | (f & 0x3F));
    } else {
      fr[n++] = (unsigned char)(0xF0 | (f >> 18));
      fr[n++] = (unsigned char)(0x80 | ((f >> 12) & 0x3F));
      fr[n++] = (unsigned char)(0x80 | ((f >> 6) & 0x3F));
      fr[n++] = (unsigned char)(0x80 | (f & 0x3F));
    }
    if (!full) {
      fr[n++] = (unsigned char)((samples - 1) >> 8);
      fr[n++] = (unsigned char)((samples - 1) & 255);
    }
    fr[n] = crc8(fr, n);
    n++;
    fr[n++] = 0x00; /* subframe: CONSTANT, no wasted bits */
    fr[n++] = 0x00;
    fr[n++] = 0x00; /* the value, 16 bits */
    unsigned crc = crc16(fr, n);
    fr[n++] = (unsigned char)(crc >> 8);
    fr[n++] = (unsigned char)(crc & 255);
    put(&b, fr, n);
  }
  if (b.failed) {
    free(b.data);
    return MC_ERR_NOMEM;
  }
  *data = b.data;
  *len = b.len;
  return MC_OK;
}

/* --- Files --- */

static mc_status write_file(const char *path, const void *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return MC_ERR_IO;
  int ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok ? MC_OK : MC_ERR_IO;
}

static char *join(const char *a, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c%s", a, MC_PATH_SEP, b);
  return p;
}

/* name without the characters a file name can't have on Windows. */
static void safe_name(const char *name, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = name; *p && n + 1 < cap; p++)
    if (!strchr("\\/:*?\"<>|", *p)) out[n++] = *p;
  out[n] = '\0';
}

/* Album i's folder, made, under dir: malloc'd into *out. */
static mc_status album_folder(const char *dir, int i, char **out) {
  char artist[128], title[128];
  safe_name(k_albums[i].artist, artist, sizeof artist);
  safe_name(k_albums[i].title, title, sizeof title);
  char *a = join(dir, artist), *folder = a ? join(a, title) : NULL;
  free(a);
  if (!folder) return MC_ERR_NOMEM;
  mc_status st = mc_mkdirs(folder);
  if (st != MC_OK) {
    free(folder);
    return st;
  }
  *out = folder;
  return MC_OK;
}

static mc_status write_album(const char *dir, int i) {
  const demo_album *al = &k_albums[i];
  char *folder = NULL;
  mc_status st = album_folder(dir, i, &folder);
  if (st != MC_OK) return st;
  unsigned char *data = NULL;
  size_t len = 0;
  char *path = join(folder, "cover.jpg");
  st = path ? mc_demo_cover_jpeg(i, 800, &data, &len) : MC_ERR_NOMEM;
  if (st == MC_OK) st = write_file(path, data, len);
  free(data);
  free(path);
  for (int t = 0; t < al->ntracks && st == MC_OK; t++) {
    char title[128], name[160];
    safe_name(al->tracks[t], title, sizeof title);
    snprintf(name, sizeof name, "%02d %s.flac", t + 1, title);
    if (!(path = join(folder, name))) {
      st = MC_ERR_NOMEM;
      break;
    }
    st = mc_demo_silent_flac((uint64_t)track_seconds(i, t) * FLAC_RATE, &data, &len);
    if (st == MC_OK) st = write_file(path, data, len);
    free(data);
    data = NULL;
    if (st == MC_OK) {
      mc_tags tags = {
        .title = (char *)al->tracks[t],
        .artist = (char *)al->artist,
        .album = (char *)al->title,
        .album_artist = (char *)al->artist,
        .genre = (char *)al->genre,
        .year = al->year,
        .original_year = al->year,
        .track = t + 1,
        .track_total = al->ntracks,
        .disc = 1,
        .disc_total = 1,
      };
      st = mc_tags_write(path, &tags);
    }
    free(path);
  }
  free(folder);
  return st;
}

mc_status mc_demo_write_folder(const char *dir) {
  if (!dir || !*dir) return MC_ERR_INVALID_ARG;
  if (!mc_folder_is_empty(dir)) return MC_ERR_EXISTS;
  mc_status st = mc_mkdirs(dir);
  for (int i = 0; i < MC_DEMO_ALBUMS && st == MC_OK; i++) st = write_album(dir, i);
  return st;
}

/* --- The library --- */

static char *marker_path(const char *dir) {
  return join(dir, MC_DEMO_MARKER);
}

static int marker_says(const char *dir, const char *what) {
  char *path = dir ? marker_path(dir) : NULL;
  FILE *f = path ? mc_fopen(path, "rb") : NULL;
  free(path);
  if (!f) return 0;
  char text[512];
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = '\0';
  return what ? strstr(text, what) != NULL : 1;
}

static mc_status write_marker(const char *dir, int complete) {
  char *path = marker_path(dir);
  if (!path) return MC_ERR_NOMEM;
  char text[256];
  int n = snprintf(text, sizeof text,
                   "A demo library made by Kithsong, for screenshots. Its music is made up, and silent.\nstatus %s\n",
                   complete ? "complete" : "generating");
  mc_status st = write_file(path, text, (size_t)n);
  free(path);
  return st;
}

int mc_demo_is_complete(const char *dir) {
  return marker_says(dir, "status complete");
}

/* Imports the album folders under src as-is and waits until each item has settled. */
static mc_status import_all(mc_library *lib, const char *src) {
  char *paths[MC_DEMO_ALBUMS] = {0};
  mc_status st = MC_OK;
  for (int i = 0; i < MC_DEMO_ALBUMS && st == MC_OK; i++) st = album_folder(src, i, &paths[i]);
  mc_import *imp = NULL;
  mc_import_options opts = {.skip_tagging = 1};
  if (st == MC_OK) st = mc_import_start(lib, (const char *const *)paths, MC_DEMO_ALBUMS, &opts, NULL, NULL, &imp);
  if (st == MC_OK) {
    uint64_t deadline = mc_mono_ms() + IMPORT_TIMEOUT_MS;
    int settled = 0, done = 0;
    while (!settled && mc_mono_ms() < deadline) {
      mc_sleep_ms(20);
      int n = mc_import_item_count(imp);
      settled = n >= MC_DEMO_ALBUMS;
      done = 0;
      for (int i = 0; i < n && settled; i++) {
        mc_import_item *it = NULL;
        if (mc_import_item_get(imp, i, &it) != MC_OK) {
          settled = 0;
          break;
        }
        if (it->state == MC_IMPORT_DONE) done++;
        else if (it->state != MC_IMPORT_SKIPPED && it->state != MC_IMPORT_FAILED) settled = 0;
        mc_import_item_free(it);
      }
    }
    st = !settled ? MC_ERR_IO : done == MC_DEMO_ALBUMS ? MC_OK : MC_ERR_FORMAT;
    mc_import_free(imp);
  }
  for (int i = 0; i < MC_DEMO_ALBUMS; i++) free(paths[i]);
  return st;
}

/* Plays of track t of album a: a few tracks played a few times, for Most Played. */
static int track_plays(int a, int t) {
  int n = (a * 5 + t * 3) % 7;
  return n > 3 ? n - 3 : 0;
}

static mc_status open_some(mc_library *lib) {
  mc_album_list *list = NULL;
  mc_status st = mc_library_list_albums(lib, &list);
  for (int i = 0; st == MC_OK && i < list->count; i++) {
    int a = 0;
    while (a < MC_DEMO_ALBUMS && strcmp(k_albums[a].title, list->items[i].title ? list->items[i].title : "")) a++;
    mc_track_list *tracks = NULL;
    if (a == MC_DEMO_ALBUMS || mc_library_list_album_tracks(lib, list->items[i].uid, &tracks) != MC_OK) continue;
    for (int t = 0; t < tracks->count && st == MC_OK; t++)
      for (int p = track_plays(a, t); p > 0 && st == MC_OK; p--) st = mc_library_record_play(lib, tracks->items[t].uid);
    mc_track_list_free(tracks);
  }
  for (size_t k = 0; k < sizeof k_opened / sizeof *k_opened && st == MC_OK; k++) {
    const demo_album *al = &k_albums[k_opened[k]];
    st = MC_ERR_NOT_FOUND;
    for (int i = 0; i < list->count; i++)
      if (list->items[i].title && !strcmp(list->items[i].title, al->title)) {
        st = mc_library_record_opened(lib, MC_RECENT_ALBUM, list->items[i].uid);
        mc_sleep_ms(2); /* each a later time, so the order holds */
        break;
      }
  }
  mc_album_list_free(list);
  return st;
}

mc_status mc_demo_create(const char *library_dir, const char *staging_dir) {
  if (!library_dir || !*library_dir) return MC_ERR_INVALID_ARG;
  if (marker_says(library_dir, NULL) && !mc_demo_is_complete(library_dir)) {
    mc_status st = mc_remove_tree(library_dir); /* an earlier run, cut off */
    if (st != MC_OK) return st;
  }
  if (!mc_folder_is_empty(library_dir)) return MC_ERR_EXISTS;

  char *tmp = staging_dir ? NULL : mc_temp_dir();
  const char *base = staging_dir ? staging_dir : tmp;
  unsigned char rnd[6];
  char name[48];
  mc_status st = base ? mc_random_bytes(rnd, sizeof rnd) : MC_ERR_IO;
  snprintf(name, sizeof name, "kithsong-demo-%02x%02x%02x%02x%02x%02x", rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
  char *src = st == MC_OK ? join(base, name) : NULL;
  if (st == MC_OK && !src) st = MC_ERR_NOMEM;
  free(tmp);
  if (st == MC_OK) st = mc_demo_write_folder(src);

  mc_library *lib = NULL;
  if (st == MC_OK) st = mc_mkdirs(library_dir);
  if (st == MC_OK) st = write_marker(library_dir, 0);
  if (st == MC_OK) st = mc_library_open(library_dir, &lib);
  if (st == MC_OK) st = import_all(lib, src);
  if (st == MC_OK) st = open_some(lib);
  mc_library_close(lib);
  if (st == MC_OK) st = write_marker(library_dir, 1);
  if (src) mc_remove_tree(src);
  free(src);
  if (st != MC_OK && marker_says(library_dir, NULL)) mc_remove_tree(library_dir);
  return st;
}
