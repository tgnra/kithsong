/* Stress test libraries (musiccore/stress.h). Rows go straight into the tables, as the importer
 * would have left them, with the triggers' logging off: nothing here ever syncs, and writing the
 * changelog as well would take most of the time. Generated from a fixed seed, uids included, so
 * a preset is the same library every time. */
#include "musiccore/stress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stb_image_write.h>

#include "library/library_internal.h"
#include "musiccore/library_move.h"
#include "util/fracindex.h"
#include "util/platform.h"

#define STRESS_META "stress_library" /* in meta: the preset's name */
#define BASE_MS 1700000000000LL      /* "now", fixed so that a preset never differs */

enum {
  COVERS = 24,        /* different pictures; each album gets a file of its own */
  COVER_SIZE = 400,   /* pixels; decoding them is part of what a grid costs */
  WAV_RATE = 8000,    /* half a second of silence, as small as plays everywhere */
  WAV_SAMPLES = 4000,
  BATCH_TRACKS = 4000, /* per transaction */
  LONG_NAME = 240,    /* bytes, in a name meant to be too long */
  NAME_CAP = 512,
};

static const char *const k_preset_names[MC_STRESS_PRESETS] = {"1k", "10k", "100k"};
static const int k_preset_tracks[MC_STRESS_PRESETS] = {1000, 10000, 100000};

static const char *const k_words_a[] = {
  "Crystal", "Velvet", "Hollow", "Golden", "Silent", "Electric", "Paper", "Northern", "Lucid", "Broken",
  "Amber", "Distant", "Midnight", "Copper", "Wandering", "Pale", "Burning", "Quiet", "Scarlet", "Winter",
  "Glass", "Neon", "Salt", "Ivory", "Fading", "Restless", "Hidden", "Slow", "Cobalt", "Wild",
};
static const char *const k_words_b[] = {
  "Harbor", "Echoes", "Gardens", "Machines", "Rivers", "Parade", "Horizon", "Satellites", "Shadows", "Orchard",
  "Mirrors", "Avenue", "Lanterns", "Thunder", "Daughters", "Radio", "Meridian", "Static", "Tides", "Ghosts",
  "Mountains", "Sparrows", "Cathedral", "Signals", "Bloom", "Letters", "Coast", "Engines", "Fields", "Summer",
};
/* What breaks a layout or a sort: other scripts, right-to-left text, combining marks, emoji. */
static const char *const k_awkward[] = {
  "東京事変 — 群青日和",
  "빛나는 밤의 노래",
  "Кино и группа крови",
  "فيروز وأغنية الصباح",
  "שיר השירים הארוך",
  "Μουσική της θάλασσας",
  "Sigur Rós – Ágætis byrjun",
  "Björk & Motörhead Ünïcödé",
  "Z̸a̷l̵g̶o̴ ̷M̸a̵r̶k̸s̷",
  "🎸🔥 Emoji Parade 🎹✨",
  "ไทย เพลงยามเช้า",
  "हिन्दी गीतों का संग्रह",
};
/* What breaks formatting: quotes, markup, mnemonics, printf and placeholder signs. */
static const char *const k_symbols[] = {
  "\"Quoted\" & <b>Tagged</b>",
  "100% of %s and {0}",
  "_Under_score &Ampersand",
  "AC/DC: Back\\Slash?",
  "...",
  "x",
};
static const char *const k_genres[] = {
  "Rock", "Electronic", "Jazz", "Classical", "Hip Hop", "Folk", "Ambient", "Metal", "Pop", "Soul",
  "Reggae", "Punk", "Blues", "Country", "House", "Techno", "Shoegaze", "Post-Rock", "Funk", "World",
};

struct mc_stress_job {
  char *dir;
  mc_stress_preset preset;
  mc_thread thread;
  volatile int percent, done, cancel;
  volatile mc_status status;
};

/* --- Random numbers and uids --- */

typedef struct rng {
  uint64_t state;
  uint64_t uids; /* how many uids so far */
} rng;

static uint64_t next(rng *r) { /* splitmix64 */
  uint64_t z = (r->state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

static int below(rng *r, int n) {
  return (int)(next(r) % (uint64_t)n);
}

/* Shaped like the UUIDv7s the app makes, which also rise in order, as these do. */
static void new_uid(rng *r, char out[MC_UID_STRLEN]) {
  uint64_t t = (uint64_t)BASE_MS + r->uids++, x = next(r);
  snprintf(out, MC_UID_STRLEN, "%08x-%04x-7%03x-%04x-%012llx", (unsigned)(t >> 16), (unsigned)(t & 0xFFFF),
           (unsigned)(x & 0xFFF), (unsigned)(0x8000 | ((x >> 12) & 0x3FFF)),
           (unsigned long long)((x >> 16) & 0xFFFFFFFFFFFFull));
}

/* A name, the ith of its kind. Most are two words; every so often an awkward one. */
static void make_name(rng *r, int i, char out[NAME_CAP]) {
  const char *a = k_words_a[below(r, (int)(sizeof k_words_a / sizeof *k_words_a))];
  const char *b = k_words_b[below(r, (int)(sizeof k_words_b / sizeof *k_words_b))];
  if (i % 61 == 7) {
    size_t len = 0;
    out[0] = '\0';
    while (len < LONG_NAME) {
      int n = snprintf(out + len, NAME_CAP - len, "%s%s %s", len ? " and the " : "", a, b);
      if (n < 0) break;
      len += (size_t)n;
      a = k_words_a[below(r, (int)(sizeof k_words_a / sizeof *k_words_a))];
      b = k_words_b[below(r, (int)(sizeof k_words_b / sizeof *k_words_b))];
    }
  } else if (i % 17 == 5) {
    int k = below(r, (int)(sizeof k_awkward / sizeof *k_awkward));
    snprintf(out, NAME_CAP, "%s", k_awkward[k]);
  } else if (i % 97 == 11) {
    int k = below(r, (int)(sizeof k_symbols / sizeof *k_symbols));
    snprintf(out, NAME_CAP, "%s", k_symbols[k]);
  } else {
    snprintf(out, NAME_CAP, "%s %s", a, b);
  }
}

/* --- Cover art --- */

typedef struct cover {
  unsigned char *data;
  size_t len, cap;
} cover;

static void cover_write(void *ctx, void *data, int size) {
  cover *c = ctx;
  if (c->len + (size_t)size > c->cap) {
    size_t cap = (c->len + (size_t)size) * 2;
    unsigned char *p = realloc(c->data, cap);
    if (!p) return; /* comes out short, and is left unused */
    c->data = p;
    c->cap = cap;
  }
  memcpy(c->data + c->len, data, (size_t)size);
  c->len += (size_t)size;
}

/* A picture for cover k: bands of two colours across a slanted gradient. Plain, but JPEG has to
 * decode it like any other, and each has its own colours for the backdrop to take. */
static int make_cover(int k, rng *r, cover *out) {
  unsigned char *px = malloc((size_t)COVER_SIZE * COVER_SIZE * 3);
  if (!px) return 0;
  int a[3], b[3];
  for (int i = 0; i < 3; i++) {
    a[i] = below(r, 256);
    b[i] = below(r, 256);
  }
  int band = 12 + k * 3;
  for (int y = 0; y < COVER_SIZE; y++)
    for (int x = 0; x < COVER_SIZE; x++) {
      int t = (x + y) * 255 / (2 * COVER_SIZE), on = ((x * 2 + y) / band) & 1;
      unsigned char *p = px + ((size_t)y * COVER_SIZE + (size_t)x) * 3;
      for (int i = 0; i < 3; i++) {
        int v = (a[i] * (255 - t) + b[i] * t) / 255;
        p[i] = (unsigned char)(on ? v : 255 - v / 2);
      }
    }
  int ok = stbi_write_jpg_to_func(cover_write, out, COVER_SIZE, COVER_SIZE, 3, px, 80);
  free(px);
  return ok && out->len;
}

/* --- Files --- */

static void put_le(unsigned char *p, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A WAV of silence: 16-bit mono, zeros after the header. *len is its size. */
static unsigned char *make_wav(size_t *len) {
  uint32_t data = WAV_SAMPLES * 2;
  *len = 44 + data;
  unsigned char *w = calloc(1, *len);
  if (!w) return NULL;
  memcpy(w, "RIFF", 4);
  put_le(w + 4, 36 + data, 4);
  memcpy(w + 8, "WAVEfmt ", 8);
  put_le(w + 16, 16, 4);
  put_le(w + 20, 1, 2); /* PCM */
  put_le(w + 22, 1, 2); /* mono */
  put_le(w + 24, WAV_RATE, 4);
  put_le(w + 28, WAV_RATE * 2, 4);
  put_le(w + 32, 2, 2);
  put_le(w + 34, 16, 2);
  memcpy(w + 36, "data", 4);
  put_le(w + 40, data, 4);
  return w;
}

static mc_status write_file(const char *path, const void *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return MC_ERR_IO;
  int ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok ? MC_OK : MC_ERR_IO;
}

/* dir's marker file's path: malloc'd, or NULL. */
static char *marker_path(const char *dir) {
  size_t n = strlen(dir) + sizeof MC_STRESS_MARKER + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c" MC_STRESS_MARKER, dir, MC_PATH_SEP);
  return p;
}

static mc_status write_marker(const char *dir, mc_stress_preset preset, int complete) {
  char *path = marker_path(dir);
  if (!path) return MC_ERR_NOMEM;
  char text[256];
  int n = snprintf(text, sizeof text,
                   "A stress test library made by Kithsong, for testing the app's speed. It holds no real music.\n"
                   "preset %s\nstatus %s\n",
                   k_preset_names[preset], complete ? "complete" : "generating");
  mc_status st = write_file(path, text, (size_t)n);
  free(path);
  return st;
}

/* --- Rows --- */

/* The statements, prepared once, in the order of the enum. */
enum { S_ARTIST, S_ALBUM, S_ALBUM_ARTIST, S_TRACK, S_TRACK_ARTIST, S_FILE, S_PLAY, S_PLAYLIST, S_ENTRY,
       S_GROUP, S_GROUP_ENTRY, S_RECENT, S_COUNT };

static const char *const k_sql[S_COUNT] = {
  "INSERT INTO artists (uid, name, sort_name) VALUES (?1, ?2, ?3)",
  "INSERT INTO albums (uid, title, artist_uid, year, original_year, disc_total, art_hash, artist_credit)"
  " VALUES (?1, ?2, ?3, ?4, ?4, ?5, ?6, ?7)",
  "INSERT INTO album_artists (uid, album_uid, artist_uid, position, joinphrase)"
  " VALUES (mc_credit_uid(?1, ?3), ?1, ?2, ?3, ?4)",
  "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, disc_no, duration_ms, genre, year,"
  " added_ms, artist_credit) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
  "INSERT INTO track_artists (uid, track_uid, artist_uid, position, joinphrase)"
  " VALUES (mc_credit_uid(?1, ?3), ?1, ?2, ?3, ?4)",
  "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
  " VALUES (?1, ?2, ?3, 'wav', 128, ?4, ?5)",
  "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES (?1, ?2, ?3, ?4, ?5)",
  "INSERT INTO playlists (uid, name) VALUES (?1, ?2)",
  "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position) VALUES (?1, ?2, ?3, ?4)",
  "INSERT INTO album_groups (uid, name) VALUES (?1, ?2)",
  "INSERT INTO album_group_entries (uid, group_uid, album_uid, position) VALUES (?1, ?2, ?3, ?4)",
  "INSERT INTO recents (uid, kind, opened_ms) VALUES (?1, ?2, ?3)",
};

typedef char uid_t_[MC_UID_STRLEN];

typedef struct gen {
  mc_stress_job *job;
  mc_library *lib;
  sqlite3 *db;
  sqlite3_stmt *stmt[S_COUNT];
  rng r;
  uid_t_ *artists;
  char **artist_names;
  int nartists;
  uid_t_ *albums;
  int nalbums, cap_albums;
  uid_t_ *tracks;
  int ntracks, total;
  cover covers[COVERS];
  unsigned char *wav;
  size_t wav_len;
  mc_status st;
} gen;

/* Binds what's left as text, NULL for SQL NULL, and runs it. Text only: SQLite's column
 * affinity makes numbers of the integer columns' digits, so one call serves. */
static void run(gen *g, int which, int n, const char *const *values) {
  if (g->st != MC_OK) return;
  sqlite3_stmt *s = g->stmt[which];
  for (int i = 0; i < n; i++) sqlite3_bind_text(s, i + 1, values[i], -1, SQLITE_STATIC);
  if (sqlite3_step(s) != SQLITE_DONE) g->st = MC_ERR_DB;
  sqlite3_reset(s);
}

static const char *num(char *buf, long long v) {
  snprintf(buf, 24, "%lld", v);
  return buf;
}

static void add_artists(gen *g) {
  g->nartists = g->total / 40 > 10 ? g->total / 40 : 10;
  g->artists = calloc((size_t)g->nartists, sizeof *g->artists);
  g->artist_names = calloc((size_t)g->nartists, sizeof *g->artist_names);
  if (!g->artists || !g->artist_names) {
    g->st = MC_ERR_NOMEM;
    return;
  }
  for (int i = 0; i < g->nartists && g->st == MC_OK; i++) {
    char name[NAME_CAP], sort[NAME_CAP + 8];
    if (i == 0) snprintf(name, sizeof name, "The Prolific Ensemble"); /* has far the most tracks */
    else if (i == 1) snprintf(name, sizeof name, "Various Artists");
    else make_name(&g->r, i, name);
    new_uid(&g->r, g->artists[i]);
    if (!(g->artist_names[i] = mc_strdup(name))) g->st = MC_ERR_NOMEM;
    if (!strncmp(name, "The ", 4)) snprintf(sort, sizeof sort, "%s, The", name + 4);
    else snprintf(sort, sizeof sort, "%s", name);
    const char *v[] = {g->artists[i], name, sort};
    run(g, S_ARTIST, 3, v);
  }
}

/* An artist for the next album: a few have many albums and most have few, as in real life. */
static int pick_artist(gen *g) {
  double u = (double)(next(&g->r) >> 11) / 9007199254740992.0;
  int i = 2 + (int)((double)(g->nartists - 2) * u * u * u);
  return i < g->nartists ? i : g->nartists - 1;
}

/* One track and its rows. first says whether it gets a real file, in folder (relative). */
static void add_track(gen *g, const char *album_uid, int album_artist, int compilation, int year, int no,
                      int disc, int duration_ms, const char *folder, int first) {
  char uid[MC_UID_STRLEN], title[NAME_CAP], credit[2 * NAME_CAP];
  char b1[24], b2[24], b3[24], b4[24], b5[24], path[96];
  rng *r = &g->r;
  new_uid(r, uid);
  memcpy(g->tracks[g->ntracks], uid, MC_UID_STRLEN);
  make_name(r, g->ntracks, title);
  int artist = compilation ? pick_artist(g) : album_artist;
  int feat = g->ntracks % 20 == 9 ? pick_artist(g) : -1;
  if (feat == artist) feat = -1;
  if (feat >= 0)
    snprintf(credit, sizeof credit, "%s feat. %s", g->artist_names[artist], g->artist_names[feat]);
  int genre = below(r, (int)(sizeof k_genres / sizeof *k_genres));
  long long added = BASE_MS - (long long)g->ntracks * 60000;
  const char *v[] = {uid, album_uid, g->artists[artist], title, num(b1, no), num(b2, disc), num(b3, duration_ms),
                     k_genres[genre], num(b4, year), num(b5, added), feat >= 0 ? credit : NULL};
  run(g, S_TRACK, 11, v);
  const char *c0[] = {uid, g->artists[artist], "0", feat >= 0 ? " feat. " : ""};
  run(g, S_TRACK_ARTIST, 4, c0);
  if (feat >= 0) {
    const char *c1[] = {uid, g->artists[feat], "1", ""};
    run(g, S_TRACK_ARTIST, 4, c1);
  }
  char fuid[MC_UID_STRLEN];
  new_uid(r, fuid);
  snprintf(path, sizeof path, "%s/%05d.wav", folder, no + disc * 1000);
  const char *f[] = {fuid, uid, path, num(b1, (long long)g->wav_len), num(b2, BASE_MS)};
  run(g, S_FILE, 5, f);
  if (first && g->st == MC_OK) {
    char *abs = mc_library_abspath(g->lib, path);
    if (!abs) g->st = MC_ERR_NOMEM;
    else g->st = write_file(abs, g->wav, g->wav_len);
    free(abs);
  }
  if (g->ntracks % 5 == 2) { /* played, for the smart lists to sort */
    char puid[MC_UID_STRLEN];
    new_uid(r, puid);
    const char *p[] = {puid, uid, mc_library_node_uid(g->lib), num(b1, 1 + below(r, 40)),
                       num(b2, BASE_MS - (long long)below(r, 1000000) * 1000)};
    run(g, S_PLAY, 5, p);
  }
  g->ntracks++;
}

/* The album's folder, made, with its cover if it has one. */
static void make_folder(gen *g, const char *folder, int cover_k) {
  char *abs = mc_library_abspath(g->lib, folder);
  if (!abs) {
    g->st = MC_ERR_NOMEM;
    return;
  }
  g->st = mc_mkdirs(abs);
  if (g->st == MC_OK && cover_k >= 0) {
    size_t n = strlen(abs) + sizeof "/cover.jpg";
    char *path = malloc(n);
    if (!path) {
      g->st = MC_ERR_NOMEM;
    } else {
      snprintf(path, n, "%s%ccover.jpg", abs, MC_PATH_SEP);
      g->st = write_file(path, g->covers[cover_k].data, g->covers[cover_k].len);
      free(path);
    }
  }
  free(abs);
}

/* One album of n tracks. kind: 0 an album, 1 a compilation, 2 a long mix. */
static void add_album(gen *g, int n, int artist, int kind) {
  rng *r = &g->r;
  int i = g->nalbums;
  if (i == g->cap_albums) {
    g->cap_albums *= 2;
    uid_t_ *more = realloc(g->albums, (size_t)g->cap_albums * sizeof *more);
    if (!more) {
      g->st = MC_ERR_NOMEM;
      return;
    }
    g->albums = more;
  }
  char *uid = g->albums[i], title[NAME_CAP], folder[32], hash[65], credit[2 * NAME_CAP], b1[24], b2[24];
  new_uid(r, uid);
  make_name(r, i, title);
  /* About one in seven has no cover, so the placeholders are in the way as well. */
  int cover_k = i % 7 == 3 ? -1 : below(r, COVERS);
  if (cover_k >= 0) snprintf(hash, sizeof hash, "%064d", cover_k + 1);
  int year = 1960 + below(r, 65), discs = n > 40 ? (n + 39) / 40 : 1;
  int second = kind == 0 && i % 25 == 4 ? pick_artist(g) : -1;
  if (second == artist) second = -1;
  if (second >= 0) snprintf(credit, sizeof credit, "%s & %s", g->artist_names[artist], g->artist_names[second]);
  const char *v[] = {uid, title, g->artists[artist], num(b1, year), num(b2, discs),
                     cover_k >= 0 ? hash : NULL, second >= 0 ? credit : NULL};
  run(g, S_ALBUM, 7, v);
  const char *a0[] = {uid, g->artists[artist], "0", second >= 0 ? " & " : ""};
  run(g, S_ALBUM_ARTIST, 4, a0);
  if (second >= 0) {
    const char *a1[] = {uid, g->artists[second], "1", ""};
    run(g, S_ALBUM_ARTIST, 4, a1);
  }
  snprintf(folder, sizeof folder, "music/b%06d", i);
  if (g->st == MC_OK) make_folder(g, folder, cover_k);
  for (int t = 0; t < n && g->st == MC_OK; t++) {
    int duration = kind == 2 ? 7200000 : 90000 + below(r, 330000);
    add_track(g, uid, artist, kind == 1, year, t % 40 + 1, t / 40 + 1, duration, folder, t == 0);
  }
  g->nalbums++;
}

static void set_percent(gen *g, int percent) {
  if (percent > g->job->percent) g->job->percent = percent;
}

static void exec(gen *g, const char *sql) {
  if (g->st == MC_OK && sqlite3_exec(g->db, sql, NULL, NULL, NULL) != SQLITE_OK) g->st = MC_ERR_DB;
}

static void add_albums(gen *g) {
  int huge = g->total / 20;
  huge = huge < 100 ? 100 : huge > 5000 ? 5000 : huge;
  int batch = 0;
  exec(g, "BEGIN");
  while (g->ntracks < g->total && g->st == MC_OK) {
    int i = g->nalbums, left = g->total - g->ntracks, roll = below(&g->r, 100), n, kind = 0, artist;
    if (i == 0) { /* far bigger than any other */
      n = huge;
      artist = 0;
    } else if (i == 1) { /* one very long track */
      n = 1;
      kind = 2;
      artist = pick_artist(g);
    } else if (roll < 12) { /* a single */
      n = 1;
      artist = pick_artist(g);
    } else if (roll < 16) { /* a compilation */
      n = 15 + below(&g->r, 11);
      kind = 1;
      artist = 1;
    } else {
      n = 8 + below(&g->r, 9);
      artist = i % 20 == 3 ? 0 : pick_artist(g);
    }
    add_album(g, n < left ? n : left, artist, kind);
    if (g->job->cancel) g->st = MC_ERR_NOT_FOUND;
    set_percent(g, (int)((long long)g->ntracks * 85 / g->total));
    if ((batch += n) >= BATCH_TRACKS) {
      batch = 0;
      exec(g, "COMMIT");
      exec(g, "BEGIN");
    }
  }
  exec(g, "COMMIT");
}

/* An entry of each list goes after the last, as appending does. */
static void add_entries(gen *g, int which, const char *list, int n, const uid_t_ *items, int nitems) {
  char *key = NULL;
  for (int i = 0; i < n && g->st == MC_OK; i++) {
    char *after = mc_frac_key_between(key, NULL), uid[MC_UID_STRLEN];
    free(key);
    key = after;
    if (!key) {
      g->st = MC_ERR_NOMEM;
      break;
    }
    new_uid(&g->r, uid);
    const char *v[] = {uid, list, items[below(&g->r, nitems)], key};
    run(g, which, 4, v);
    if ((i & 1023) == 1023 && g->job->cancel) g->st = MC_ERR_NOT_FOUND;
  }
  free(key);
}

static void add_lists(gen *g) {
  exec(g, "BEGIN");
  int playlists = g->total / 500;
  playlists = playlists < 4 ? 4 : playlists > 200 ? 200 : playlists;
  char uid[MC_UID_STRLEN], name[NAME_CAP], b1[24];
  for (int i = 0; i < playlists && g->st == MC_OK; i++) {
    new_uid(&g->r, uid);
    make_name(&g->r, i, name);
    const char *v[] = {uid, name};
    run(g, S_PLAYLIST, 2, v);
    add_entries(g, S_ENTRY, uid, i == 0 ? g->total / 2 : 20 + below(&g->r, 80), g->tracks, g->ntracks);
    if (i < 4) {
      const char *rec[] = {uid, "playlist", num(b1, BASE_MS - i)};
      run(g, S_RECENT, 3, rec);
    }
    set_percent(g, 85 + i * 10 / playlists);
  }
  for (int i = 0; i < 6 && g->st == MC_OK; i++) {
    new_uid(&g->r, uid);
    make_name(&g->r, i, name);
    const char *v[] = {uid, name};
    run(g, S_GROUP, 2, v);
    add_entries(g, S_GROUP_ENTRY, uid, 30, g->albums, g->nalbums);
  }
  for (int i = 2; i < 10 && i < g->nalbums && g->st == MC_OK; i++) {
    const char *rec[] = {g->albums[i], "album", num(b1, BASE_MS - 10 - i)};
    run(g, S_RECENT, 3, rec);
  }
  exec(g, "COMMIT");
}

static void generate(mc_stress_job *job) {
  gen g = {.job = job, .r = {.state = 0x6B697468736F6E67ull + (uint64_t)job->preset}};
  g.total = k_preset_tracks[job->preset];
  g.cap_albums = g.total / 8 + 16;
  g.albums = calloc((size_t)g.cap_albums, sizeof *g.albums);
  g.tracks = calloc((size_t)g.total, sizeof *g.tracks);
  g.wav = make_wav(&g.wav_len);
  if (!g.albums || !g.tracks || !g.wav) g.st = MC_ERR_NOMEM;
  for (int i = 0; i < COVERS && g.st == MC_OK; i++)
    if (!make_cover(i, &g.r, &g.covers[i])) g.st = MC_ERR_NOMEM;

  /* The folder's new, so it's the generator's before there's a library in it. */
  if (g.st == MC_OK) g.st = mc_mkdirs(job->dir);
  if (g.st == MC_OK) g.st = write_marker(job->dir, job->preset, 0);
  if (g.st == MC_OK) g.st = mc_library_open_internal(job->dir, &g.lib);
  if (g.st == MC_OK) {
    g.db = mc_library_db(g.lib);
    /* Nothing logged, nothing checked and no waiting for the disk: a crash here only leaves a
     * folder that's made again. */
    g.lib->applying_remote = 1;
    exec(&g, "PRAGMA foreign_keys = OFF; PRAGMA synchronous = OFF;");
    for (int i = 0; i < S_COUNT && g.st == MC_OK; i++)
      if (sqlite3_prepare_v2(g.db, k_sql[i], -1, &g.stmt[i], NULL) != SQLITE_OK) g.st = MC_ERR_DB;
  }
  if (g.st == MC_OK) g.st = mc_library_meta_set(g.lib, STRESS_META, k_preset_names[job->preset]);
  if (g.st == MC_OK) {
    exec(&g, "BEGIN");
    add_artists(&g);
    exec(&g, "COMMIT");
  }
  if (g.st == MC_OK) add_albums(&g);
  if (g.st == MC_OK) add_lists(&g);

  if (g.st != MC_OK && g.db && sqlite3_get_autocommit(g.db) == 0) sqlite3_exec(g.db, "ROLLBACK", NULL, NULL, NULL);
  for (int i = 0; i < S_COUNT; i++) sqlite3_finalize(g.stmt[i]);
  if (g.lib) {
    g.lib->applying_remote = 0;
    if (g.st == MC_OK) exec(&g, "PRAGMA wal_checkpoint(TRUNCATE);");
    mc_library_close(g.lib);
  }
  if (g.st == MC_OK) g.st = write_marker(job->dir, job->preset, 1);
  /* Ours and unfinished, so it goes; with the marker never written the folder is what it was. */
  if (g.st != MC_OK && mc_stress_is_generated(job->dir)) mc_remove_tree(job->dir);

  for (int i = 0; i < COVERS; i++) free(g.covers[i].data);
  for (int i = 0; g.artist_names && i < g.nartists; i++) free(g.artist_names[i]);
  free(g.artist_names);
  free(g.artists);
  free(g.albums);
  free(g.tracks);
  free(g.wav);
  job->status = g.st;
  if (g.st == MC_OK) job->percent = 100;
}

static void job_main(void *arg) {
  mc_stress_job *job = arg;
  generate(job);
  job->done = 1;
}

/* --- The API --- */

int mc_stress_preset_tracks(mc_stress_preset preset) {
  return (unsigned)preset < MC_STRESS_PRESETS ? k_preset_tracks[preset] : 0;
}

char *mc_stress_dir(const char *config_dir, mc_stress_preset preset) {
  if (!config_dir || (unsigned)preset >= MC_STRESS_PRESETS) return NULL;
  size_t n = strlen(config_dir) + sizeof MC_STRESS_ROOT + 16;
  char *dir = malloc(n);
  if (dir) snprintf(dir, n, "%s%c" MC_STRESS_ROOT "%cstress-%s", config_dir, MC_PATH_SEP, MC_PATH_SEP, k_preset_names[preset]);
  return dir;
}

int mc_stress_is_generated(const char *dir) {
  char *path = dir ? marker_path(dir) : NULL;
  int yes = path && mc_file_exists(path);
  free(path);
  return yes;
}

int mc_stress_is_complete(const char *dir) {
  char *path = dir ? marker_path(dir) : NULL;
  FILE *f = path ? mc_fopen(path, "rb") : NULL;
  free(path);
  if (!f) return 0;
  char text[512];
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = '\0';
  return strstr(text, "status complete") != NULL;
}

int mc_library_is_stress(mc_library *lib) {
  char value[32];
  return lib && mc_library_meta_get(lib, STRESS_META, value, sizeof value) == MC_OK;
}

mc_status mc_stress_start(const char *dir, mc_stress_preset preset, mc_stress_job **out) {
  if (out) *out = NULL;
  if (!dir || !*dir || !out || (unsigned)preset >= MC_STRESS_PRESETS) return MC_ERR_INVALID_ARG;
  if (mc_stress_is_generated(dir) && !mc_stress_is_complete(dir)) {
    mc_status st = mc_remove_tree(dir); /* an earlier run, cut off */
    if (st != MC_OK) return st;
  }
  if (!mc_folder_is_empty(dir)) return MC_ERR_EXISTS;
  mc_stress_job *job = calloc(1, sizeof *job);
  if (!job) return MC_ERR_NOMEM;
  job->dir = mc_strdup(dir);
  job->preset = preset;
  mc_status st = job->dir ? mc_thread_start(&job->thread, job_main, job) : MC_ERR_NOMEM;
  if (st != MC_OK) {
    free(job->dir);
    free(job);
    return st;
  }
  *out = job;
  return MC_OK;
}

int mc_stress_job_percent(const mc_stress_job *job) {
  return job ? job->percent : 0;
}

int mc_stress_job_done(const mc_stress_job *job, mc_status *status) {
  if (!job || !job->done) return 0;
  if (status) *status = job->status;
  return 1;
}

void mc_stress_job_cancel(mc_stress_job *job) {
  if (job) job->cancel = 1;
}

void mc_stress_job_free(mc_stress_job *job) {
  if (!job) return;
  mc_thread_join(&job->thread);
  free(job->dir);
  free(job);
}

mc_status mc_stress_delete(const char *dir, const char *open_dir) {
  if (!dir || !*dir) return MC_ERR_INVALID_ARG;
  if (!mc_stress_is_generated(dir) || mc_is_link(dir)) return MC_ERR_INVALID_ARG;
  if (open_dir && mc_same_path(dir, open_dir)) return MC_ERR_IN_USE;
  return mc_remove_tree(dir);
}

typedef struct root_list {
  const char *root;
  char **dirs;
  int count, cap;
} root_list;

static void collect(void *user, const char *name, int is_dir) {
  root_list *l = user;
  if (!is_dir) return;
  size_t n = strlen(l->root) + strlen(name) + 2;
  char *dir = malloc(n);
  if (!dir) return;
  snprintf(dir, n, "%s%c%s", l->root, MC_PATH_SEP, name);
  if (!mc_stress_is_generated(dir)) {
    free(dir);
    return;
  }
  if (l->count == l->cap) {
    int cap = l->cap ? l->cap * 2 : 8;
    char **more = realloc(l->dirs, (size_t)cap * sizeof *more);
    if (!more) {
      free(dir);
      return;
    }
    l->dirs = more;
    l->cap = cap;
  }
  l->dirs[l->count++] = dir;
}

/* The stress libraries under config_dir, into *l; root is what its root is kept in. */
static mc_status list_stress(const char *config_dir, char **root, root_list *l) {
  memset(l, 0, sizeof *l);
  size_t n = strlen(config_dir) + sizeof MC_STRESS_ROOT + 1;
  *root = malloc(n);
  if (!*root) return MC_ERR_NOMEM;
  snprintf(*root, n, "%s%c" MC_STRESS_ROOT, config_dir, MC_PATH_SEP);
  l->root = *root;
  if (mc_file_exists(*root)) mc_list_dir(*root, collect, l);
  return MC_OK;
}

mc_status mc_stress_delete_all(const char *config_dir, const char *open_dir, int *deleted) {
  if (deleted) *deleted = 0;
  if (!config_dir) return MC_ERR_INVALID_ARG;
  char *root;
  root_list l;
  mc_status st = list_stress(config_dir, &root, &l);
  if (st != MC_OK) return st;
  for (int i = 0; i < l.count; i++) {
    mc_status one = mc_stress_delete(l.dirs[i], open_dir);
    if (one == MC_OK && deleted) ++*deleted;
    else if (one != MC_OK && one != MC_ERR_IN_USE) st = one;
    free(l.dirs[i]);
  }
  free(l.dirs);
  if (mc_folder_is_empty(root)) mc_rmdir(root);
  free(root);
  return st;
}

int mc_stress_count(const char *config_dir) {
  if (!config_dir) return 0;
  char *root;
  root_list l;
  if (list_stress(config_dir, &root, &l) != MC_OK) return 0;
  int n = l.count;
  for (int i = 0; i < l.count; i++) free(l.dirs[i]);
  free(l.dirs);
  free(root);
  return n;
}
