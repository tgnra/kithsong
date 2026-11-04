/* Analysing a file in one decode, the feature BLOB, track_analysis and the distance. See
 * "Audio analysis" in docs/schema.md. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "playback/decoder.h"
#include "util/uid.h"

#define READ_FRAMES 4096 /* a tenth of a second or so between looks at cancel */
#define MIN_RATE 1000    /* under this a header is lying, whatever it says */

/* --- One decode --- */

struct mc_analyzer {
  mc_loudness *loudness;
  mc_timbre *timbre;
  unsigned rate, channels;
  uint64_t frames;
  float *clean, *mono;
};

mc_analyzer *mc_analyzer_new(unsigned rate, unsigned channels) {
  if (rate < MIN_RATE || !channels) return NULL;
  mc_analyzer *a = calloc(1, sizeof *a);
  if (!a) return NULL;
  a->rate = rate;
  a->channels = channels;
  a->loudness = mc_loudness_new(rate, channels);
  a->timbre = mc_timbre_new(rate);
  a->clean = malloc((size_t)READ_FRAMES * channels * sizeof *a->clean);
  a->mono = malloc(READ_FRAMES * sizeof *a->mono);
  if (!a->loudness || !a->timbre || !a->clean || !a->mono) {
    mc_analyzer_free(a);
    return NULL;
  }
  return a;
}

void mc_analyzer_free(mc_analyzer *a) {
  if (!a) return;
  mc_loudness_free(a->loudness);
  mc_timbre_free(a->timbre);
  free(a->clean);
  free(a->mono);
  free(a);
}

void mc_analyzer_feed(mc_analyzer *a, const float *in, size_t frames) {
  a->frames += frames;
  while (frames) {
    size_t n = frames < READ_FRAMES ? frames : READ_FRAMES;
    size_t samples = n * a->channels;
    for (size_t i = 0; i < samples; i++) a->clean[i] = isfinite(in[i]) ? in[i] : 0.0f;
    float scale = 1.0f / (float)a->channels;
    for (size_t i = 0; i < n; i++) {
      float sum = 0.0f;
      for (unsigned c = 0; c < a->channels; c++) sum += a->clean[i * a->channels + c];
      a->mono[i] = sum * scale;
    }
    mc_loudness_feed(a->loudness, a->clean, n);
    mc_timbre_feed(a->timbre, a->mono, n);
    in += samples;
    frames -= n;
  }
}

void mc_analyzer_finish(mc_analyzer *a, mc_analysis *out) {
  memset(out, 0, sizeof *out);
  out->version = MC_ANALYSIS_VERSION;
  out->seconds = (double)a->frames / a->rate;
  mc_loudness_finish(a->loudness, &out->lufs, &out->lra, &out->true_peak);
  mc_timbre_finish(a->timbre, out);
}

mc_status mc_analysis_file(const char *path, mc_analysis_cancel_fn cancel, void *user,
                           mc_analysis *out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  mc_decoder *d;
  mc_status st = mc_decoder_open(path, &d);
  if (st != MC_OK) return st;
  unsigned channels = mc_decoder_channels(d), rate = mc_decoder_rate(d);
  mc_analyzer *a = mc_analyzer_new(rate, channels);
  float *buf = malloc((size_t)READ_FRAMES * channels * sizeof *buf);
  if (rate < MIN_RATE) st = MC_ERR_FORMAT;
  else if (!a || !buf) st = MC_ERR_NOMEM;
  uint64_t total = 0;
  while (st == MC_OK) {
    if (cancel && cancel(user)) {
      st = MC_ERR_IN_USE;
      break;
    }
    uint64_t got = mc_decoder_read(d, buf, READ_FRAMES);
    if (!got) break;
    mc_analyzer_feed(a, buf, (size_t)got);
    total += got;
  }
  if (st == MC_OK && !total) st = MC_ERR_FORMAT; /* it opened, but nothing in it decodes */
  if (st == MC_OK) mc_analyzer_finish(a, out);
  free(buf);
  mc_analyzer_free(a);
  mc_decoder_close(d);
  return st;
}

/* --- The BLOB --- */

static void put_f32(unsigned char *p, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(bits >> (8 * i));
}

static float get_f32(const unsigned char *p) {
  uint32_t bits = 0;
  for (int i = 0; i < 4; i++) bits |= (uint32_t)p[i] << (8 * i);
  float v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

void mc_analysis_pack(const mc_analysis *a, unsigned char out[MC_ANALYSIS_BLOB_SIZE]) {
  memset(out, 0, MC_ANALYSIS_BLOB_SIZE);
  out[0] = MC_ANALYSIS_LAYOUT;
  out[1] = MC_ANALYSIS_MFCCS;
  unsigned char *p = out + 4;
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++, p += 4) put_f32(p, a->mfcc_mean[i]);
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++, p += 4) put_f32(p, a->mfcc_var[i]);
  put_f32(p, a->centroid_mean);
  put_f32(p + 4, a->centroid_var);
}

int mc_analysis_unpack(const void *blob, int len, mc_analysis *a) {
  const unsigned char *p = blob;
  if (!p || len != MC_ANALYSIS_BLOB_SIZE || p[0] != MC_ANALYSIS_LAYOUT || p[1] != MC_ANALYSIS_MFCCS)
    return 0;
  p += 4;
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++, p += 4) a->mfcc_mean[i] = get_f32(p);
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++, p += 4) a->mfcc_var[i] = get_f32(p);
  a->centroid_mean = get_f32(p);
  a->centroid_var = get_f32(p + 4);
  return 1;
}

/* --- track_analysis --- */

static void bind_real(sqlite3_stmt *stmt, int col, double v) {
  if (isnan(v))
    sqlite3_bind_null(stmt, col);
  else
    sqlite3_bind_double(stmt, col, v);
}

mc_status mc_analysis_store(mc_library *lib, const char *track_uid, const char *file_rev,
                            mc_analysis_source source, const mc_analysis *a) {
  if (!lib || !track_uid || !a) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_derive(track_uid, (int)source, uid);
  if (st != MC_OK) return st;
  unsigned char blob[MC_ANALYSIS_BLOB_SIZE];
  mc_analysis_pack(a, blob);
  if ((st = mc_library_begin_write(lib)) != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "INSERT INTO track_analysis (uid, track_uid, source, file_rev,"
                         " analysis_version, features, lufs, lra, true_peak)"
                         " SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9 WHERE EXISTS"
                         " (SELECT 1 FROM tracks WHERE uid = ?2 AND file_rev IS ?4)"
                         " ON CONFLICT (uid) DO UPDATE SET file_rev = excluded.file_rev,"
                         " analysis_version = excluded.analysis_version,"
                         " features = excluded.features, lufs = excluded.lufs,"
                         " lra = excluded.lra, true_peak = excluded.true_peak"
                         " WHERE excluded.analysis_version >= track_analysis.analysis_version",
                         -1, &stmt, NULL) != SQLITE_OK) {
    mc_library_rollback(lib);
    return MC_ERR_DB;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, track_uid, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 3, (int)source);
  if (file_rev) sqlite3_bind_text(stmt, 4, file_rev, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 5, a->version);
  sqlite3_bind_blob(stmt, 6, blob, sizeof blob, SQLITE_STATIC);
  bind_real(stmt, 7, a->lufs);
  bind_real(stmt, 8, a->lra);
  bind_real(stmt, 9, a->true_peak);
  st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* A track's rows, best first (mc_analysis_get()). */
#define RESULT_COLS \
  "a.track_uid, a.source, a.analysis_version, a.features, a.lufs, a.lra, a.true_peak"
#define RESULT_JOIN " FROM track_analysis a JOIN tracks t ON t.uid = a.track_uid"
#define RESULT_ORDER \
  "(a.file_rev IS t.file_rev) DESC, a.source, a.analysis_version DESC, a.uid"

static double column_real(sqlite3_stmt *stmt, int col) {
  return sqlite3_column_type(stmt, col) == SQLITE_NULL ? NAN : sqlite3_column_double(stmt, col);
}

/* The row stmt is on, if it's of a layout this build reads. */
static int read_result(sqlite3_stmt *stmt, mc_analysis *out) {
  memset(out, 0, sizeof *out);
  out->source = sqlite3_column_int(stmt, 1) ? MC_ANALYSIS_LOSSY_COPY : MC_ANALYSIS_ORIGINAL;
  out->version = sqlite3_column_int(stmt, 2);
  out->lufs = column_real(stmt, 4);
  out->lra = column_real(stmt, 5);
  out->true_peak = column_real(stmt, 6);
  return mc_analysis_unpack(sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3), out);
}

mc_status mc_analysis_get(mc_library *lib, const char *track_uid, mc_analysis *out) {
  if (!lib || !track_uid || !out) return MC_ERR_INVALID_ARG;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT " RESULT_COLS RESULT_JOIN " WHERE a.track_uid = ?1"
                         " ORDER BY " RESULT_ORDER,
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, track_uid, -1, SQLITE_STATIC);
  mc_status st = MC_ERR_NOT_FOUND;
  int rc;
  while (st == MC_ERR_NOT_FOUND && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    if (read_result(stmt, out)) st = MC_OK;
  if (st == MC_ERR_NOT_FOUND && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* --- Distance ---
 *
 * What's compared, as a vector of MC_ANALYSIS_DIMS, in four groups:
 *  - shape:      the means of MFCCs 1 to 12, the spectrum's outline. Not 0, which is the level:
 *                the same recording mastered louder is the same timbre.
 *  - texture:    the standard deviations of all 13, how much the sound moves.
 *  - brightness: the centroid, in octaves as hearing goes, and how much it moves as a fraction
 *                of itself.
 *  - energy:     integrated loudness and loudness range.
 * Each dimension is a z-score against the library's own spread, as the raw numbers are in
 * unlike units with spreads an order of magnitude apart. Within a group a squared difference is
 * averaged, so a group's weight doesn't depend on how many dimensions it has, and the weights
 * sum to one. Two random tracks differ by 2 in a squared z-score on average, so halving makes
 * that a distance of 1. */

enum { SHAPE = 0, TEXTURE = 12, BRIGHTNESS = 25, ENERGY = 27 };

static const struct {
  int first, count;
  double weight;
} k_groups[] = {
  {SHAPE, 12, 0.45},
  {TEXTURE, 13, 0.25},
  {BRIGHTNESS, 2, 0.20},
  {ENERGY, 2, 0.10},
};

/* Typical means and spreads, for a library too small to say, or none. Rough, and meant to be:
 * they only need to be the right size. */
static const float k_prior_mean[MC_ANALYSIS_DIMS] = {
  60, 0, 5, 0, 2, 0, 1, 0, 0, 0, 0, 0,                   /* shape */
  30, 12, 7, 5, 4, 4, 3, 3, 3, 3, 3, 3, 3,               /* texture */
  10.5f, 0.5f,                                           /* brightness */
  -14, 7,                                                /* energy */
};
static const float k_prior_scale[MC_ANALYSIS_DIMS] = {
  25, 12, 8, 6, 5, 4, 4, 3, 3, 3, 3, 3,                  /* shape */
  8, 4, 2.5f, 2, 1.5f, 1.5f, 1, 1, 1, 1, 1, 1, 1,        /* texture */
  0.7f, 0.2f,                                            /* brightness */
  4, 4,                                                  /* energy */
};
/* How many results the fixed scales count for, against the library's. */
#define PRIOR_WEIGHT 16.0

static void features_of(const mc_analysis *a, float *v) {
  int d = 0;
  for (int c = 1; c < MC_ANALYSIS_MFCCS; c++) v[d++] = a->mfcc_mean[c];
  for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) v[d++] = sqrtf(a->mfcc_var[c]);
  float centroid = a->centroid_mean > 1.0f ? a->centroid_mean : 1.0f;
  v[d++] = log2f(centroid);
  v[d++] = sqrtf(a->centroid_var) / centroid;
  v[d++] = (float)a->lufs; /* NaN if there's none: no difference is counted for it */
  v[d++] = (float)a->lra;
}

void mc_analysis_stats_from(const mc_analysis *results, int count, mc_analysis_stats *out) {
  double sum[MC_ANALYSIS_DIMS] = {0}, sum_sq[MC_ANALYSIS_DIMS] = {0};
  int n[MC_ANALYSIS_DIMS] = {0};
  memset(out, 0, sizeof *out);
  for (int i = 0; i < count; i++) {
    if (results[i].version != MC_ANALYSIS_VERSION) continue;
    float v[MC_ANALYSIS_DIMS];
    features_of(&results[i], v);
    for (int d = 0; d < MC_ANALYSIS_DIMS; d++) {
      if (isnan(v[d])) continue;
      sum[d] += v[d];
      sum_sq[d] += (double)v[d] * v[d];
      n[d]++;
    }
    out->count++;
  }
  for (int d = 0; d < MC_ANALYSIS_DIMS; d++) {
    double mean = n[d] ? sum[d] / n[d] : k_prior_mean[d];
    double var = n[d] ? sum_sq[d] / n[d] - mean * mean : 0.0;
    double prior = (double)k_prior_scale[d] * k_prior_scale[d];
    out->mean[d] = (float)mean;
    out->scale[d] = (float)sqrt(((var > 0.0 ? var : 0.0) * n[d] + prior * PRIOR_WEIGHT) /
                                (n[d] + PRIOR_WEIGHT));
  }
}

mc_status mc_analysis_stats_load(mc_library *lib, mc_analysis_stats *out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT " RESULT_COLS RESULT_JOIN " ORDER BY a.track_uid, " RESULT_ORDER,
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_analysis *all = NULL;
  int count = 0, cap = 0, rc;
  mc_status st = MC_OK;
  char last[MC_UID_STRLEN] = "";
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *track = (const char *)sqlite3_column_text(stmt, 0);
    if (!track || !strcmp(track, last)) continue; /* only each track's best */
    if (count == cap) {
      int grown = cap ? cap * 2 : 256;
      mc_analysis *more = realloc(all, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      all = more;
      cap = grown;
    }
    if (read_result(stmt, &all[count])) {
      count++;
      memcpy(last, track, sizeof last - 1);
    }
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st == MC_OK) mc_analysis_stats_from(all, count, out);
  free(all);
  return st;
}

double mc_analysis_distance(const mc_analysis *a, const mc_analysis *b,
                            const mc_analysis_stats *stats) {
  if (!a || !b || a->version != MC_ANALYSIS_VERSION || b->version != MC_ANALYSIS_VERSION)
    return -1.0;
  float x[MC_ANALYSIS_DIMS], y[MC_ANALYSIS_DIMS];
  features_of(a, x);
  features_of(b, y);
  const float *scale = stats ? stats->scale : k_prior_scale;
  double total = 0.0;
  for (size_t g = 0; g < sizeof k_groups / sizeof k_groups[0]; g++) {
    double sum = 0.0;
    for (int i = 0; i < k_groups[g].count; i++) {
      int d = k_groups[g].first + i;
      if (isnan(x[d]) || isnan(y[d])) continue;
      double z = (double)(x[d] - y[d]) / scale[d];
      sum += z * z;
    }
    total += k_groups[g].weight * sum / k_groups[g].count;
  }
  return sqrt(total / 2.0);
}
