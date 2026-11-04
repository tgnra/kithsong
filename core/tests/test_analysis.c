/* Local audio analysis (musiccore/analysis.h): loudness against EBU Tech 3341 and 3342's test
 * signals, timbre from synthetic ones, and how results are stored, scanned and synced. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "analysis/scanner.h"
#include "library/delete_internal.h"
#include "library/library_internal.h"
#include "musiccore/delete.h"
#include "musiccore/musiccore.h"
#include "sync/rows.h"
#include "test.h"
#include "util/platform.h"
#include "util/uid.h"

#define PI 3.14159265358979323846
#define WAIT_MS 30000

#define CHECK_NEAR(a, b, tol)                                                      \
  do {                                                                             \
    double a_ = (a), b_ = (b);                                                     \
    if (!(fabs(a_ - b_) <= (tol))) {                                               \
      fprintf(stderr, "%s:%d: %s = %.4f, wanted %.4f +- %g\n", __FILE__, __LINE__, #a, a_, b_, \
              (double)(tol));                                                      \
      test_failures++;                                                             \
    }                                                                              \
  } while (0)

/* --- Signals --- */

static double from_db(double db) {
  return pow(10.0, db / 20.0);
}

/* A sine in every channel for seconds, fed to l through a small buffer. phase counts on. */
static void feed_sine(mc_loudness *l, unsigned rate, unsigned channels, double hz, double db,
                      double seconds) {
  float buf[512 * 8];
  size_t total = (size_t)llround(seconds * rate), done = 0;
  double amp = from_db(db);
  while (done < total) {
    size_t n = total - done < 512 ? total - done : 512;
    for (size_t i = 0; i < n; i++) {
      float v = (float)(amp * sin(2.0 * PI * hz * (double)(done + i) / rate));
      for (unsigned c = 0; c < channels; c++) buf[i * channels + c] = v;
    }
    mc_loudness_feed(l, buf, n);
    done += n;
  }
}

typedef struct segment {
  double db, seconds;
} segment;

/* Stereo 1 kHz at 48 kHz, in steps of level. */
static void measure(const segment *s, int count, double *lufs, double *lra) {
  mc_loudness *l = mc_loudness_new(48000, 2);
  for (int i = 0; i < count; i++) feed_sine(l, 48000, 2, 1000.0, s[i].db, s[i].seconds);
  double peak;
  mc_loudness_finish(l, lufs, lra, &peak);
  mc_loudness_free(l);
}

/* Deterministic noise in [-1, 1]. */
static float noise(uint32_t *state) {
  *state = *state * 1664525u + 1013904223u;
  return (float)((double)(*state >> 8) / 8388608.0 - 1.0);
}

/* A sound with something in it at every rate: harmonics that move, under the rates' Nyquist. */
static double music(double t) {
  double swell = 0.6 + 0.4 * sin(2.0 * PI * 1.3 * t);
  double v = 0.0;
  for (int h = 1; h <= 12; h++) v += sin(2.0 * PI * 220.0 * h * t + h) / h;
  v += 0.2 * sin(2.0 * PI * 6300.0 * t) * sin(2.0 * PI * 3.0 * t);
  return 0.3 * swell * v;
}

/* seconds of music() at rate, in channels, at gain, analysed. */
static void analyse_music(unsigned rate, unsigned channels, double gain, double seconds,
                          mc_analysis *out) {
  mc_analyzer *a = mc_analyzer_new(rate, channels);
  float buf[1000 * 8];
  size_t total = (size_t)(seconds * rate), done = 0;
  while (a && done < total) {
    size_t n = total - done < 1000 ? total - done : 1000;
    for (size_t i = 0; i < n; i++) {
      float v = (float)(gain * music((double)(done + i) / rate));
      for (unsigned c = 0; c < channels; c++) buf[i * channels + c] = v;
    }
    mc_analyzer_feed(a, buf, n);
    done += n;
  }
  if (a) mc_analyzer_finish(a, out);
  mc_analyzer_free(a);
}

/* --- Loudness --- */

/* EBU Tech 3341's minimum requirements, cases 1 to 6: within 0.1 LU. */
static void test_ebu_3341(void) {
  double lufs, lra, peak;
  measure((segment[]){{-23, 20}}, 1, &lufs, &lra);
  CHECK_NEAR(lufs, -23.0, 0.1);
  printf("3341 case 1: %.3f LUFS (-23.0)\n", lufs);
  measure((segment[]){{-33, 20}}, 1, &lufs, &lra);
  CHECK_NEAR(lufs, -33.0, 0.1);
  printf("3341 case 2: %.3f LUFS (-33.0)\n", lufs);
  measure((segment[]){{-36, 10}, {-23, 60}, {-36, 10}}, 3, &lufs, &lra);
  CHECK_NEAR(lufs, -23.0, 0.1);
  printf("3341 case 3: %.3f LUFS (-23.0)\n", lufs);
  measure((segment[]){{-72, 10}, {-36, 10}, {-23, 60}, {-36, 10}, {-72, 10}}, 5, &lufs, &lra);
  CHECK_NEAR(lufs, -23.0, 0.1);
  printf("3341 case 4: %.3f LUFS (-23.0)\n", lufs);
  measure((segment[]){{-26, 20}, {-20, 20.1}, {-26, 20}}, 3, &lufs, &lra);
  CHECK_NEAR(lufs, -23.0, 0.1);
  printf("3341 case 5: %.3f LUFS (-23.0)\n", lufs);

  /* Case 6: 5.0, L and R -28, C -24, surrounds -30. */
  mc_loudness *l = mc_loudness_new(48000, 5);
  const double levels[5] = {-28, -28, -24, -30, -30};
  float buf[5];
  for (int i = 0; i < 48000 * 20; i++) {
    double s = sin(2.0 * PI * 1000.0 * i / 48000.0);
    for (int c = 0; c < 5; c++) buf[c] = (float)(from_db(levels[c]) * s);
    mc_loudness_feed(l, buf, 1);
  }
  mc_loudness_finish(l, &lufs, &lra, &peak);
  mc_loudness_free(l);
  CHECK_NEAR(lufs, -23.0, 0.1);
  printf("3341 case 6: %.3f LUFS (-23.0)\n", lufs);
}

/* BS.1770: a 997 Hz sine at 0 dBFS in one channel reads -3.01 LKFS. At other rates too, which
 * is what working the filter out per rate is for. */
static void test_997(void) {
  static const unsigned rates[] = {44100, 48000, 96000, 192000, 32000, 22050};
  for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
    mc_loudness *l = mc_loudness_new(rates[i], 1);
    feed_sine(l, rates[i], 1, 997.0, 0.0, 10.0);
    double lufs, lra, peak;
    mc_loudness_finish(l, &lufs, &lra, &peak);
    mc_loudness_free(l);
    CHECK_NEAR(lufs, -3.01, 0.05);
    printf("997 Hz 0 dBFS mono at %u: %.3f LUFS (-3.01)\n", rates[i], lufs);
  }
}

/* EBU Tech 3342's four synthetic cases: within 1 LU. */
static void test_ebu_3342(void) {
  double lufs, lra;
  measure((segment[]){{-20, 20}, {-30, 20}}, 2, &lufs, &lra);
  CHECK_NEAR(lra, 10.0, 1.0);
  printf("3342 case 1: LRA %.2f (10)\n", lra);
  measure((segment[]){{-20, 20}, {-15, 20}}, 2, &lufs, &lra);
  CHECK_NEAR(lra, 5.0, 1.0);
  printf("3342 case 2: LRA %.2f (5)\n", lra);
  measure((segment[]){{-40, 20}, {-20, 20}}, 2, &lufs, &lra);
  CHECK_NEAR(lra, 20.0, 1.0);
  printf("3342 case 3: LRA %.2f (20)\n", lra);
  measure((segment[]){{-50, 20}, {-35, 20}, {-20, 20}, {-35, 20}, {-50, 20}}, 5, &lufs, &lra);
  CHECK_NEAR(lra, 15.0, 1.0);
  printf("3342 case 4: LRA %.2f (15)\n", lra);
}

static void test_gating(void) {
  double alone, with_silence, lra;
  measure((segment[]){{-20, 10}}, 1, &alone, &lra);
  /* Silence on both sides is gated out, whatever its length. */
  mc_loudness *l = mc_loudness_new(48000, 2);
  feed_sine(l, 48000, 2, 1000.0, -200.0, 30.0);
  feed_sine(l, 48000, 2, 1000.0, -20.0, 10.0);
  feed_sine(l, 48000, 2, 1000.0, -200.0, 60.0);
  double peak;
  mc_loudness_finish(l, &with_silence, &lra, &peak);
  mc_loudness_free(l);
  /* Exactly what BS.1770 has: of the blocks over the edges, those at 3/4, 1/2 and 1/4 of the
   * tone pass the relative gate too, so 97 + 6 blocks hold 100 blocks' energy. */
  CHECK_NEAR(with_silence, alone + 10.0 * log10(100.0 / 103.0), 0.01);

  /* All silence has no loudness and no peak; digital silence goes through the same way. */
  l = mc_loudness_new(44100, 2);
  float zeros[2048] = {0};
  for (int i = 0; i < 100; i++) mc_loudness_feed(l, zeros, 1024);
  mc_loudness_finish(l, &with_silence, &lra, &peak);
  mc_loudness_free(l);
  CHECK(isnan(with_silence));
  CHECK(isnan(peak));

  /* Shorter than a block has none either, nor anything under 3 s a range. */
  l = mc_loudness_new(48000, 2);
  feed_sine(l, 48000, 2, 1000.0, -20.0, 0.3);
  mc_loudness_finish(l, &with_silence, &lra, &peak);
  mc_loudness_free(l);
  CHECK(isnan(with_silence));
  CHECK(isnan(lra));
  CHECK_NEAR(peak, -20.0, 0.1);
  measure((segment[]){{-20, 2}}, 1, &with_silence, &lra);
  CHECK_NEAR(with_silence, -20.0, 0.1);
  CHECK(isnan(lra));
}

/* The peak between samples: a sine at a quarter of the rate, 45 degrees off, has all its samples
 * 3 dB under its crests. EBU Tech 3341 cases 15 to 19, +0.2/-0.4 dB. The tone fades in: starting
 * dead on a sample that's far from zero is a step, and the true peak of a step really is over
 * the tone's. */
static double peak_of(unsigned rate, double fraction, double phase_deg, double amp) {
  mc_loudness *l = mc_loudness_new(rate, 2);
  float buf[2];
  for (unsigned i = 0; i < rate; i++) {
    double fade = i < 4800 ? i / 4800.0 : 1.0;
    buf[0] = buf[1] = (float)(fade * amp * sin(2.0 * PI * fraction * i + phase_deg * PI / 180.0));
    mc_loudness_feed(l, buf, 1);
  }
  double lufs, lra, peak;
  mc_loudness_finish(l, &lufs, &lra, &peak);
  mc_loudness_free(l);
  return peak;
}

static void test_true_peak(void) {
  static const struct {
    int ebu_case;
    double fraction, phase, amp, want;
  } cases[] = {
    {15, 1.0 / 4, 0.0, 0.5, -6.0},
    {16, 1.0 / 4, 45.0, 0.5, -6.0},
    {17, 1.0 / 6, 60.0, 0.5, -6.0},
    {18, 1.0 / 8, 67.5, 0.5, -6.0},
    {19, 1.0 / 4, 45.0, 1.41, 3.0},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    double peak = peak_of(48000, cases[i].fraction, cases[i].phase, cases[i].amp);
    printf("3341 case %d: %.3f dBTP (%.1f)\n", cases[i].ebu_case, peak, cases[i].want);
    CHECK(peak >= cases[i].want - 0.4 && peak <= cases[i].want + 0.2);
  }
  /* Its samples all at 0.7071: only the interpolation finds the other 3 dB. */
  CHECK_NEAR(peak_of(44100, 0.25, 45.0, 1.0), 0.0, 0.2);
  /* From 96 kHz up it interpolates less; at 192 kHz not at all. */
  CHECK_NEAR(peak_of(96000, 1.0 / 16, 11.25, 1.0), 0.0, 0.3);
  CHECK_NEAR(peak_of(192000, 1.0 / 32, 0.0, 1.0), 0.0, 0.3);

  /* Skipping quiet runs changes nothing: one crest late in a track is still found. */
  mc_loudness *l = mc_loudness_new(48000, 1);
  float buf[1];
  for (int i = 0; i < 48000; i++) {
    double a = i >= 40000 && i < 40064 ? 1.0 : 0.05;
    buf[0] = (float)(a * sin(2.0 * PI * 0.25 * i + PI / 4));
    mc_loudness_feed(l, buf, 1);
  }
  double lufs, lra, peak;
  mc_loudness_finish(l, &lufs, &lra, &peak);
  mc_loudness_free(l);
  CHECK(peak > -0.5 && peak < 0.5);
}

/* --- Timbre --- */

static void test_fft(void) {
  enum { N = 256 };
  float x[N], power[N / 2 + 1];
  uint32_t state = 1;
  for (int i = 0; i < N; i++) x[i] = noise(&state);
  mc_analysis_power_spectrum(x, N, power);
  for (int k = 0; k <= N / 2; k++) {
    double re = 0.0, im = 0.0;
    for (int i = 0; i < N; i++) {
      re += x[i] * cos(2.0 * PI * k * i / N);
      im -= x[i] * sin(2.0 * PI * k * i / N);
    }
    CHECK_NEAR(power[k], re * re + im * im, 1e-2 * (1.0 + re * re + im * im));
  }
}

static void analyse_buffer(unsigned rate, const float *mono, size_t frames, mc_analysis *out) {
  mc_analyzer *a = mc_analyzer_new(rate, 1);
  mc_analyzer_feed(a, mono, frames);
  mc_analyzer_finish(a, out);
  mc_analyzer_free(a);
}

/* The mean difference over the coefficients past 0, and over all their variances. */
static double mfcc_gap(const mc_analysis *a, const mc_analysis *b) {
  double sum = 0.0;
  for (int c = 1; c < MC_ANALYSIS_MFCCS; c++) sum += fabs(a->mfcc_mean[c] - b->mfcc_mean[c]);
  return sum / (MC_ANALYSIS_MFCCS - 1);
}

static void test_noise_and_sine(void) {
  enum { RATE = 44100, N = RATE * 5 };
  float *x = malloc(N * sizeof *x);
  mc_analysis sine, white, other;
  for (int i = 0; i < N; i++) x[i] = (float)(0.5 * sin(2.0 * PI * 997.0 * i / RATE));
  analyse_buffer(RATE, x, N, &sine);
  uint32_t state = 7;
  for (int i = 0; i < N; i++) x[i] = 0.3f * noise(&state);
  analyse_buffer(RATE, x, N, &white);
  for (int i = 0; i < N; i++) x[i] = 0.3f * noise(&state);
  analyse_buffer(RATE, x, N, &other);
  free(x);

  printf("centroid: sine %.0f Hz, noise %.0f Hz; MFCC gap %.1f dB\n", sine.centroid_mean,
         white.centroid_mean, mfcc_gap(&sine, &white));
  CHECK_NEAR(sine.centroid_mean, 997.0, 60.0);
  CHECK(sine.centroid_var < 1.0); /* a steady sine stays where it is */
  /* White noise's centroid is the middle of the band the resampler leaves. */
  CHECK(white.centroid_mean > 4500.0 && white.centroid_mean < 6000.0);
  CHECK(mfcc_gap(&sine, &white) > 10.0);
  /* Two different stretches of the same noise are alike. */
  CHECK(mfcc_gap(&white, &other) < 0.5);
  CHECK_NEAR(white.centroid_mean, other.centroid_mean, 50.0);
}

/* The same sound from a file at any of these rates gives the same features. */
static void test_rates(void) {
  mc_analysis ref;
  analyse_music(44100, 1, 1.0, 6.0, &ref);
  CHECK(ref.centroid_mean > 300.0);
  static const unsigned rates[] = {22050, 32000, 48000, 88200, 96000, 192000, 44056};
  for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
    mc_analysis a;
    analyse_music(rates[i], 1, 1.0, 6.0, &a);
    double var_gap = 0.0, worst = 0.0;
    for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) {
      double d = fabs(a.mfcc_mean[c] - ref.mfcc_mean[c]);
      worst = d > worst ? d : worst;
      var_gap += fabs(sqrt(a.mfcc_var[c]) - sqrt(ref.mfcc_var[c]));
    }
    printf("%6u Hz vs 44100: worst MFCC mean %.3f dB, mean std gap %.3f dB, centroid %.1f vs %.1f,"
           " LUFS %.3f vs %.3f\n",
           rates[i], worst, var_gap / MC_ANALYSIS_MFCCS, a.centroid_mean, ref.centroid_mean, a.lufs,
           ref.lufs);
    CHECK(worst < 0.5);
    CHECK(var_gap / MC_ANALYSIS_MFCCS < 0.1);
    CHECK_NEAR(a.centroid_mean, ref.centroid_mean, ref.centroid_mean * 0.01);
    CHECK_NEAR(a.lufs, ref.lufs, 0.05);
  }
}

/* A gain moves c0 and nothing else of the timbre; a stereo file is its mono mix. */
static void test_invariants(void) {
  mc_analysis loud, quiet, stereo;
  analyse_music(44100, 1, 1.0, 5.0, &loud);
  analyse_music(44100, 1, 0.1, 5.0, &quiet);
  analyse_music(44100, 2, 1.0, 5.0, &stereo);
  CHECK(mfcc_gap(&loud, &quiet) < 0.02);
  CHECK_NEAR(loud.mfcc_mean[0] - quiet.mfcc_mean[0], 20.0 * sqrt(MC_ANALYSIS_MELS), 0.1);
  CHECK_NEAR(sqrt(loud.mfcc_var[0]), sqrt(quiet.mfcc_var[0]), 0.02);
  CHECK_NEAR(loud.centroid_mean, quiet.centroid_mean, 1.0);
  CHECK_NEAR(loud.lufs - quiet.lufs, 20.0, 0.02);
  CHECK(mfcc_gap(&loud, &stereo) < 0.001);
  CHECK_NEAR(stereo.lufs - loud.lufs, 3.01, 0.02);
  CHECK(mc_analysis_distance(&loud, &quiet, NULL) < 0.8); /* only loudness differs */

  /* Silence before and after a sound isn't its timbre. */
  enum { RATE = 44100, N = RATE * 3 };
  float *x = calloc(N * 4, sizeof *x);
  for (int i = 0; i < N; i++) {
    /* Faded at both ends: a sound cut off dead is a click, and that is timbre. */
    double edge = (i < N - i ? i : N - i) / 2000.0;
    x[N + i] = (float)((edge < 1.0 ? edge : 1.0) * music((double)i / RATE));
  }
  mc_analysis padded, bare;
  analyse_buffer(RATE, x, N * 4, &padded);
  analyse_buffer(RATE, x + N, N, &bare);
  free(x);
  printf("silence around a sound: MFCC gap %.3f dB, LUFS %.3f vs %.3f\n", mfcc_gap(&padded, &bare),
         padded.lufs, bare.lufs);
  CHECK(mfcc_gap(&padded, &bare) < 0.5);
  CHECK_NEAR(padded.lufs, bare.lufs, 0.25); /* the edge blocks, as in test_gating() */

  /* A float file with holes in it reads them as silence. */
  float bad[4096];
  for (int i = 0; i < 4096; i++) bad[i] = i % 7 ? 0.1f * (float)sin(i * 0.1) : NAN;
  bad[100] = INFINITY;
  mc_analysis a;
  analyse_buffer(RATE, bad, 4096, &a);
  CHECK(isfinite(a.centroid_mean) && isfinite(a.mfcc_mean[1]) && isfinite(a.true_peak));
}

static void test_silent_and_short(void) {
  float zeros[8192] = {0};
  mc_analysis a;
  analyse_buffer(44100, zeros, 8192, &a);
  CHECK(isnan(a.lufs) && isnan(a.true_peak));
  CHECK(a.centroid_mean == 0.0f && a.mfcc_mean[0] == 0.0f);
  CHECK(isfinite(mc_analysis_distance(&a, &a, NULL)));
  /* Shorter than a frame. */
  float blip[300];
  for (int i = 0; i < 300; i++) blip[i] = 0.5f * (float)sin(i * 0.3);
  analyse_buffer(44100, blip, 300, &a);
  CHECK(a.centroid_mean > 0.0f);
  CHECK(mc_analyzer_new(0, 2) == NULL);
  CHECK(mc_analyzer_new(44100, 0) == NULL);
}

/* --- Distance --- */

static void test_distance(void) {
  mc_analysis a, b;
  analyse_music(44100, 2, 1.0, 3.0, &a);
  CHECK(mc_analysis_distance(&a, &a, NULL) == 0.0);
  float *x = malloc(44100 * 3 * sizeof *x);
  uint32_t state = 3;
  for (int i = 0; i < 44100 * 3; i++) x[i] = 0.2f * noise(&state);
  analyse_buffer(44100, x, 44100 * 3, &b);
  free(x);
  double apart = mc_analysis_distance(&a, &b, NULL);
  CHECK(apart > 1.0);
  CHECK(mc_analysis_distance(&b, &a, NULL) == apart);

  /* Scaled by a library: the z-scores of a spread of results make random pairs about 1 apart. */
  enum { COUNT = 200 };
  static mc_analysis many[COUNT];
  for (int i = 0; i < COUNT; i++) {
    memset(&many[i], 0, sizeof many[i]);
    many[i].version = MC_ANALYSIS_VERSION;
    for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) {
      many[i].mfcc_mean[c] = 40.0f * noise(&state);
      many[i].mfcc_var[c] = 50.0f + 40.0f * noise(&state);
    }
    many[i].centroid_mean = 2000.0f + 1500.0f * noise(&state);
    many[i].centroid_var = 500000.0f + 400000.0f * noise(&state);
    many[i].lufs = -14.0 + 8.0 * noise(&state);
    many[i].lra = 8.0 + 6.0 * noise(&state);
  }
  mc_analysis_stats stats;
  mc_analysis_stats_from(many, COUNT, &stats);
  CHECK(stats.count == COUNT);
  double sum = 0.0;
  for (int i = 0; i + 1 < COUNT; i += 2) sum += pow(mc_analysis_distance(&many[i], &many[i + 1], &stats), 2);
  double rms = sqrt(sum / (COUNT / 2));
  printf("random pairs, by the library's own stats: rms distance %.3f\n", rms);
  CHECK_NEAR(rms, 1.0, 0.25);

  /* A track with no loudness compares on the rest. */
  mc_analysis no_loudness = many[0];
  no_loudness.lufs = NAN;
  no_loudness.lra = NAN;
  double d = mc_analysis_distance(&many[0], &no_loudness, &stats);
  CHECK(d == 0.0);
  /* And results of another version don't compare at all, nor count. */
  mc_analysis old = many[0];
  old.version = MC_ANALYSIS_VERSION + 1;
  CHECK(mc_analysis_distance(&many[0], &old, &stats) == -1.0);
  mc_analysis_stats_from(&old, 1, &stats);
  CHECK(stats.count == 0);
  for (int i = 0; i < MC_ANALYSIS_DIMS; i++) CHECK(stats.scale[i] > 0.0f);
}

/* --- The BLOB --- */

static void test_blob(void) {
  mc_analysis a, b;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) {
    a.mfcc_mean[c] = (float)c - 2.5f;
    a.mfcc_var[c] = (float)c * 3.25f;
  }
  a.mfcc_mean[0] = 1.0f;
  a.centroid_mean = 1234.5f;
  a.centroid_var = 0.125f;
  unsigned char blob[MC_ANALYSIS_BLOB_SIZE];
  CHECK(sizeof blob == 116);
  mc_analysis_pack(&a, blob);
  /* As docs/schema.md has it, on any machine: */
  static const unsigned char head[] = {1, 13, 0, 0, 0x00, 0x00, 0x80, 0x3f};
  CHECK(!memcmp(blob, head, sizeof head));
  static const unsigned char tail[] = {0x00, 0x00, 0x00, 0x3e}; /* 0.125f, last */
  CHECK(!memcmp(blob + sizeof blob - 4, tail, 4));
  CHECK(mc_analysis_unpack(blob, sizeof blob, &b));
  CHECK(!memcmp(a.mfcc_mean, b.mfcc_mean, sizeof a.mfcc_mean));
  CHECK(!memcmp(a.mfcc_var, b.mfcc_var, sizeof a.mfcc_var));
  CHECK(a.centroid_mean == b.centroid_mean && a.centroid_var == b.centroid_var);
  CHECK(!mc_analysis_unpack(blob, sizeof blob - 1, &b));
  blob[0] = 2; /* a layout from the future */
  CHECK(!mc_analysis_unpack(blob, sizeof blob, &b));
  CHECK(!mc_analysis_unpack(NULL, 0, &b));
}

/* --- Files --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* seconds of music() as a 16-bit stereo WAV. */
static int write_wav(const char *path, unsigned rate, double seconds, double gain) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = (uint32_t)(seconds * rate), bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 2);
  put32(f, rate);
  put32(f, rate * 4);
  put16(f, 4);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) {
    unsigned v = (unsigned)(uint16_t)(int16_t)(gain * music((double)i / rate) * 32767);
    put16(f, v);
    put16(f, v);
  }
  return fclose(f) == 0;
}

static void write_junk(const char *path) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  fputs("this is no audio file, whatever its name says", f);
  fclose(f);
}

static int give_up(void *user) {
  int *left = user;
  return --*left < 0;
}

static void test_file(const char *scratch) {
  char path[1024], junk[1024];
  snprintf(path, sizeof path, "%s/test_analysis_file.wav", scratch);
  snprintf(junk, sizeof junk, "%s/test_analysis_junk.flac", scratch);
  CHECK(write_wav(path, 44100, 4.0, 1.0));
  write_junk(junk);

  mc_analysis a, direct;
  CHECK(mc_analysis_file(path, NULL, NULL, &a) == MC_OK);
  analyse_music(44100, 2, 1.0, 4.0, &direct);
  CHECK_NEAR(a.seconds, 4.0, 0.01);
  CHECK_NEAR(a.lufs, direct.lufs, 0.01);
  CHECK(mfcc_gap(&a, &direct) < 0.05);
  CHECK(a.version == MC_ANALYSIS_VERSION);

  CHECK(mc_analysis_file(junk, NULL, NULL, &a) != MC_OK);
  snprintf(junk, sizeof junk, "%s/test_analysis_none.wav", scratch);
  CHECK(mc_analysis_file(junk, NULL, NULL, &a) == MC_ERR_IO);
  snprintf(junk, sizeof junk, "%s/test_analysis.txt", scratch);
  CHECK(mc_analysis_file(junk, NULL, NULL, &a) == MC_ERR_FORMAT);
  int left = 3;
  CHECK(mc_analysis_file(path, give_up, &left, &a) == MC_ERR_IN_USE);
  mc_remove(path);
}

/* --- Library --- */

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                               MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void exec_ok(mc_library *lib, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
    test_failures++;
  }
  sqlite3_free(err);
}

static mc_library *open_library(const char *scratch, const char *name, char *dir, size_t cap) {
  snprintf(dir, cap, "%s/%s", scratch, name);
  mc_mkdirs(dir);
  remove_db(dir);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  return lib;
}

/* A track, and a file for it if rel isn't NULL: an original, or a transcode received from a
 * peer. Its uid comes back in uid. */
static void add_track(mc_library *lib, const char *rel, int transcode, char uid[MC_UID_STRLEN]) {
  char file[MC_UID_STRLEN], sql[512];
  mc_uid_new(uid);
  mc_uid_new(file);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO tracks (uid, title) VALUES ('%s', 'A Track')", uid);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
  if (!rel) return;
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms, source_file_uid)"
           " VALUES ('%s', '%s', '%s', 'wav', 1000, 2000, %s)",
           file, uid, rel, transcode ? "'orig'" : "NULL");
  exec_ok(lib, sql);
}

/* Scans until nothing's left. counts[] by mc_scan_step. */
static void scan_all(mc_library *lib, int counts[5]) {
  char cursor[MC_UID_STRLEN] = "";
  memset(counts, 0, 5 * sizeof *counts);
  for (;;) {
    mc_scan_step step = MC_SCAN_CANCELLED;
    CHECK(mc_analysis_scan_one(lib, cursor, NULL, NULL, &step) == MC_OK);
    counts[step]++;
    if (step == MC_SCAN_NONE || counts[step] > 100) break;
  }
}

static int rows_of(mc_library *lib, const char *track, const char *where) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_analysis WHERE track_uid = '%s' AND %s",
           track, where);
  return query_int(lib, sql);
}

static void test_scan(const char *scratch) {
  char dir[1024], path[1200];
  mc_library *lib = open_library(scratch, "test_analysis_scan.d", dir, sizeof dir);
  if (!lib) return;
  CHECK(query_int(lib, "SELECT count(*) FROM track_analysis") == 0);
  snprintf(path, sizeof path, "%s/not-there.wav", dir);
  mc_remove(path); /* left by the last run */
  snprintf(path, sizeof path, "%s/good.wav", dir);
  CHECK(write_wav(path, 44100, 1.0, 1.0));
  snprintf(path, sizeof path, "%s/quiet.wav", dir);
  CHECK(write_wav(path, 44100, 1.0, 0.1));
  snprintf(path, sizeof path, "%s/copy.wav", dir);
  CHECK(write_wav(path, 48000, 1.0, 1.0));
  snprintf(path, sizeof path, "%s/bad.wav", dir);
  write_junk(path);

  char good[MC_UID_STRLEN], copy[MC_UID_STRLEN], bad[MC_UID_STRLEN], elsewhere[MC_UID_STRLEN];
  char missing[MC_UID_STRLEN], both[MC_UID_STRLEN];
  add_track(lib, "good.wav", 0, good);
  add_track(lib, "copy.wav", 1, copy);
  add_track(lib, "bad.wav", 0, bad);
  add_track(lib, NULL, 0, elsewhere); /* no file on this device */
  add_track(lib, "not-there.wav", 0, missing);
  add_track(lib, "quiet.wav", 0, both);
  /* A transcode beside the original: the original is what's read. */
  char sql[512], u[MC_UID_STRLEN];
  mc_uid_new(u);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, source_file_uid)"
           " VALUES ('%s', '%s', 'copy.wav.bak', 'wav', 'orig')", u, both);
  exec_ok(lib, sql);

  int counts[5];
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 3);
  CHECK(counts[MC_SCAN_FAILED] == 1);
  CHECK(counts[MC_SCAN_SKIPPED] == 1);
  CHECK(rows_of(lib, good, "source = 0 AND analysis_version = " MC_SQL_NUM(MC_ANALYSIS_VERSION)) == 1);
  CHECK(rows_of(lib, copy, "source = 1") == 1);
  CHECK(rows_of(lib, both, "source = 0") == 1);
  CHECK(rows_of(lib, both, "source = 1") == 0);
  CHECK(rows_of(lib, bad, "1") == 0);
  CHECK(rows_of(lib, elsewhere, "1") == 0);
  CHECK(rows_of(lib, missing, "1") == 0);
  /* The undecodable file is recorded, here only; the missing one isn't. */
  CHECK(query_int(lib, "SELECT count(*) FROM analysis_failed") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'analysis_failed'") == 0);
  /* Every field of a result is logged for sync, the BLOB as a BLOB. */
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'track_analysis'"
                       " AND field = 'features' AND typeof(value) = 'blob'") == 3);

  mc_analysis a, q;
  CHECK(mc_analysis_get(lib, good, &a) == MC_OK);
  CHECK(a.source == MC_ANALYSIS_ORIGINAL);
  CHECK(a.seconds == 0.0);
  CHECK(mc_analysis_get(lib, both, &q) == MC_OK);
  CHECK_NEAR(a.lufs - q.lufs, 20.0, 0.1);
  CHECK(mc_analysis_get(lib, copy, &q) == MC_OK);
  CHECK(q.source == MC_ANALYSIS_LOSSY_COPY);
  CHECK(mc_analysis_get(lib, elsewhere, &q) == MC_ERR_NOT_FOUND);

  /* Nothing to do the second time, but the missing file, which is looked for again. */
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 0);
  CHECK(counts[MC_SCAN_FAILED] == 0);
  CHECK(counts[MC_SCAN_SKIPPED] == 1);

  /* The missing file comes back. */
  snprintf(path, sizeof path, "%s/not-there.wav", dir);
  CHECK(write_wav(path, 44100, 1.0, 0.5));
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 1);
  CHECK(rows_of(lib, missing, "source = 0") == 1);

  /* A file that changes is tried again. */
  exec_ok(lib, "UPDATE files SET mtime_ms = 3000 WHERE path = 'bad.wav'");
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_FAILED] == 1);
  CHECK(counts[MC_SCAN_DONE] == 0);

  /* A file replaced by a better one: the old result is out of date, and stands until there's a
   * new one. A copy made from the old file isn't read for it. */
  char rev[MC_UID_STRLEN];
  mc_uid_new(rev);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "UPDATE tracks SET file_rev = '%s' WHERE uid = '%s'", rev, good);
  exec_ok(lib, sql);
  snprintf(sql, sizeof sql, "UPDATE tracks SET file_rev = '%s' WHERE uid = '%s'", rev, copy);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 0);
  CHECK(mc_analysis_get(lib, good, &q) == MC_OK);
  snprintf(sql, sizeof sql, "UPDATE files SET file_rev = '%s' WHERE track_uid = '%s'", rev, good);
  exec_ok(lib, sql);
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_analysis WHERE track_uid = '%s'"
                            " AND file_rev = '%s'", good, rev);
  CHECK(query_int(lib, sql) == 1);
  CHECK(rows_of(lib, good, "1") == 1); /* the same row, written again */

  /* A better algorithm does everything again; one newer than this build's stands. */
  exec_ok(lib, "UPDATE track_analysis SET analysis_version = 0");
  exec_ok(lib, "UPDATE analysis_failed SET analysis_version = 0");
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 3); /* the one with the replaced file has no current file */
  CHECK(counts[MC_SCAN_FAILED] == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM track_analysis WHERE analysis_version = 0") == 1);
  exec_ok(lib, "UPDATE track_analysis SET analysis_version = 99 WHERE analysis_version <> 0");
  scan_all(lib, counts);
  CHECK(counts[MC_SCAN_DONE] == 0);

  /* Results that came and went: a track deleted, and with it its results. */
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "DELETE FROM tracks WHERE uid = '%s'", both);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
  CHECK(rows_of(lib, both, "1") == 0);
  /* Writing for a track that went meanwhile does nothing, and no harm. */
  CHECK(mc_analysis_store(lib, both, NULL, MC_ANALYSIS_ORIGINAL, &a) == MC_OK);
  CHECK(rows_of(lib, both, "1") == 0);
  /* And for a file replaced meanwhile. */
  CHECK(mc_analysis_store(lib, missing, "another-rev", MC_ANALYSIS_ORIGINAL, &a) == MC_OK);
  CHECK(rows_of(lib, missing, "file_rev IS NOT NULL") == 0);

  mc_analysis_stats stats;
  CHECK(mc_analysis_stats_load(lib, &stats) == MC_OK);
  CHECK(stats.count == 0); /* only those of this version count, and there are none now */
  exec_ok(lib, "UPDATE track_analysis SET analysis_version = " MC_SQL_NUM(MC_ANALYSIS_VERSION));
  CHECK(mc_analysis_stats_load(lib, &stats) == MC_OK);
  CHECK(stats.count == query_int(lib, "SELECT count(DISTINCT track_uid) FROM track_analysis"));
  mc_library_close(lib);
}

/* One way of a sync, as test_merge.c does it. */
static void send(mc_library *from, mc_library *to) {
  mc_origin_hlc *vv;
  int nvv;
  CHECK(mc_changelog_version_vector(mc_library_db(to), &vv, &nvv) == MC_OK);
  mc_rows_reader *rd = NULL;
  CHECK(mc_rows_reader_open(mc_library_db(from), vv, nvv, 0, &rd) == MC_OK);
  free(vv);
  if (!rd) return;
  mc_rows_target tg;
  mc_rows_target_library(to, &tg);
  mc_rows_applier *a = NULL;
  CHECK(mc_rows_applier_new(&tg, &a) == MC_OK);
  static unsigned char buf[1 << 18];
  mc_rows_writer w;
  mc_row r;
  int got = 1;
  while (got && a) {
    mc_rows_writer_init(&w, buf, sizeof buf, MC_ROWS_LIBRARY);
    while (w.count < 500 && mc_rows_reader_next(rd, &r, &got) == MC_OK && got)
      CHECK(mc_rows_writer_add(&w, &r) == 1);
    size_t len = mc_rows_writer_finish(&w);
    int db, n;
    mc_row *rows = NULL;
    CHECK(mc_rows_decode(buf, len, &db, &rows, &n) == MC_OK);
    if (n) CHECK(mc_rows_apply(a, rows, n, NULL) == MC_OK);
    free(rows);
  }
  mc_rows_applier_free(a);
  mc_rows_reader_close(rd);
}

static mc_analysis fake_result(float brightness, double lufs) {
  mc_analysis a;
  memset(&a, 0, sizeof a);
  a.version = MC_ANALYSIS_VERSION;
  for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) {
    a.mfcc_mean[c] = brightness + (float)c;
    a.mfcc_var[c] = 2.0f * (float)c;
  }
  a.centroid_mean = brightness;
  a.centroid_var = 10.0f;
  a.lufs = lufs;
  a.lra = 7.5;
  a.true_peak = -1.25;
  return a;
}

/* Results reach a device that scans nothing, whole; and a transcode's result, however late,
 * doesn't take the place of an original's. */
static void test_sync(const char *scratch) {
  char da[1024], db[1024];
  mc_library *a = open_library(scratch, "test_analysis_sync_a.d", da, sizeof da);
  mc_library *b = open_library(scratch, "test_analysis_sync_b.d", db, sizeof db);
  if (!a || !b) return;
  char track[MC_UID_STRLEN];
  add_track(a, NULL, 0, track);
  send(a, b);

  mc_analysis original = fake_result(1500.0f, -12.5);
  CHECK(mc_analysis_store(a, track, NULL, MC_ANALYSIS_ORIGINAL, &original) == MC_OK);
  send(a, b);
  mc_analysis got;
  CHECK(mc_analysis_get(b, track, &got) == MC_OK);
  CHECK(got.source == MC_ANALYSIS_ORIGINAL);
  CHECK(!memcmp(got.mfcc_mean, original.mfcc_mean, sizeof got.mfcc_mean));
  CHECK(!memcmp(got.mfcc_var, original.mfcc_var, sizeof got.mfcc_var));
  CHECK(got.centroid_mean == 1500.0f);
  CHECK(got.lufs == -12.5 && got.lra == 7.5 && got.true_peak == -1.25);

  /* b, with only a transcode, had analysed it before hearing of a's. Its row is written later
   * by any clock, and syncs on, and still loses to the original's on every device. */
  mc_analysis lossy = fake_result(900.0f, -30.0);
  CHECK(mc_analysis_store(b, track, NULL, MC_ANALYSIS_LOSSY_COPY, &lossy) == MC_OK);
  send(b, a);
  send(a, b);
  CHECK(query_int(a, "SELECT count(*) FROM track_analysis") == 2);
  CHECK(query_int(b, "SELECT count(*) FROM track_analysis") == 2);
  CHECK(mc_analysis_get(a, track, &got) == MC_OK);
  CHECK(got.source == MC_ANALYSIS_ORIGINAL && got.lufs == -12.5);
  CHECK(mc_analysis_get(b, track, &got) == MC_OK);
  CHECK(got.source == MC_ANALYSIS_ORIGINAL && got.lufs == -12.5);

  /* Two devices with the original write one row, not two. */
  mc_analysis again = fake_result(1500.0f, -12.4);
  CHECK(mc_analysis_store(b, track, NULL, MC_ANALYSIS_ORIGINAL, &again) == MC_OK);
  send(b, a);
  CHECK(query_int(a, "SELECT count(*) FROM track_analysis WHERE source = 0") == 1);
  CHECK(mc_analysis_get(a, track, &got) == MC_OK);
  CHECK(got.lufs == -12.4);

  /* A delete syncs too. */
  CHECK(mc_library_begin_write(a) == MC_OK);
  char sql[160];
  snprintf(sql, sizeof sql, "DELETE FROM tracks WHERE uid = '%s'", track);
  exec_ok(a, sql);
  CHECK(mc_library_commit(a) == MC_OK);
  send(a, b);
  CHECK(query_int(b, "SELECT count(*) FROM track_analysis") == 0);
  mc_library_close(a);
  mc_library_close(b);
}

/* Duplicates merging: results follow the track that survives. */
static void test_merge(const char *scratch) {
  char da[1024], db[1024], sql[256];
  mc_library *a = open_library(scratch, "test_analysis_merge_a.d", da, sizeof da);
  mc_library *b = open_library(scratch, "test_analysis_merge_b.d", db, sizeof db);
  if (!a || !b) return;
  char ta[MC_UID_STRLEN], tb[MC_UID_STRLEN];
  add_track(a, NULL, 0, ta);
  add_track(b, NULL, 0, tb);
  for (int i = 0; i < 2; i++) {
    mc_library *l = i ? b : a;
    CHECK(mc_library_begin_write(l) == MC_OK);
    snprintf(sql, sizeof sql, "UPDATE tracks SET mb_releasetrackid = 'same' WHERE uid = '%s'",
             i ? tb : ta);
    exec_ok(l, sql);
    CHECK(mc_library_commit(l) == MC_OK);
  }
  /* Only the one that'll go has a result. */
  const char *canonical = strcmp(ta, tb) < 0 ? ta : tb, *loser = canonical == ta ? tb : ta;
  mc_library *has = canonical == ta ? b : a;
  mc_analysis r = fake_result(2000.0f, -9.0);
  CHECK(mc_analysis_store(has, loser, NULL, MC_ANALYSIS_ORIGINAL, &r) == MC_OK);
  send(a, b);
  send(b, a);
  mc_analysis got;
  for (int i = 0; i < 2; i++) {
    mc_library *l = i ? b : a;
    CHECK(mc_analysis_get(l, canonical, &got) == MC_OK);
    CHECK(got.lufs == -9.0);
    CHECK(query_int(l, "SELECT count(*) FROM tracks") == 1);
    CHECK(query_int(l, "SELECT count(*) FROM track_analysis") == 1);
  }
  /* The survivor gets a result of its own: two rows of one source, which reads as one. */
  mc_analysis own = fake_result(2000.0f, -8.0);
  CHECK(mc_analysis_store(a, canonical, NULL, MC_ANALYSIS_ORIGINAL, &own) == MC_OK);
  send(a, b);
  CHECK(mc_analysis_get(b, canonical, &got) == MC_OK);
  mc_analysis_stats stats;
  CHECK(mc_analysis_stats_load(b, &stats) == MC_OK);
  CHECK(stats.count == 1);
  mc_library_close(a);
  mc_library_close(b);
}

/* A device upgrading late has rows newer peers logged, unknown to it then. */
static void test_newly_known(const char *scratch) {
  char dir[1024], sql[900];
  mc_library *lib = open_library(scratch, "test_analysis_known.d", dir, sizeof dir);
  if (!lib) return;
  char track[MC_UID_STRLEN], row[MC_UID_STRLEN];
  add_track(lib, NULL, 0, track);
  mc_uid_derive(track, 0, row);
  lib->applying_remote = 1;
  unsigned char blob[MC_ANALYSIS_BLOB_SIZE];
  mc_analysis r = fake_result(1000.0f, -10.0);
  mc_analysis_pack(&r, blob);
  char hex[2 * MC_ANALYSIS_BLOB_SIZE + 1];
  for (int i = 0; i < MC_ANALYSIS_BLOB_SIZE; i++) snprintf(hex + 2 * i, 3, "%02X", blob[i]);
  exec_ok(lib, "DROP TABLE track_analysis; DROP TABLE analysis_failed;");
  static const char *const fields[] = {"track_uid", "source", "analysis_version", "features", "lufs"};
  for (int i = 0; i < 5; i++) {
    char value[300];
    if (i == 0) snprintf(value, sizeof value, "'%s'", track);
    else if (i == 1) snprintf(value, sizeof value, "0");
    else if (i == 2) snprintf(value, sizeof value, "%d", MC_ANALYSIS_VERSION);
    else if (i == 3) snprintf(value, sizeof value, "X'%s'", hex);
    else snprintf(value, sizeof value, "-10.0");
    snprintf(sql, sizeof sql,
             "INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value) VALUES"
             " (%d, 'peer0000-0000-7000-8000-000000000000', 'track_analysis', '%s', '%s', %s)",
             1000 + i, row, fields[i], value);
    exec_ok(lib, sql);
  }
  exec_ok(lib, "PRAGMA user_version = 17");
  lib->applying_remote = 0;
  mc_library_close(lib);

  lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  CHECK(mc_library_schema_version(lib) == mc_library_schema_latest());
  mc_analysis got;
  CHECK(mc_analysis_get(lib, track, &got) == MC_OK);
  CHECK(got.lufs == -10.0 && got.centroid_mean == 1000.0f);
  mc_library_close(lib);
}

/* A delete taken back out of the recycle bin brings the results back, BLOB and all: its
 * changelog rows go, so nothing else ever would. */
static void test_recycle_bin(const char *scratch) {
  char dir[1024];
  mc_library *lib = open_library(scratch, "test_analysis_bin.d", dir, sizeof dir);
  if (!lib) return;
  char track[MC_UID_STRLEN];
  add_track(lib, NULL, 0, track);
  mc_analysis r = fake_result(1234.0f, -11.0);
  CHECK(mc_analysis_store(lib, track, NULL, MC_ANALYSIS_ORIGINAL, &r) == MC_OK);
  int logged = query_int(lib, "SELECT count(*) FROM changelog");

  const char *uids[] = {track};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids, 1, &plan) == MC_OK);
  mc_delete_result res;
  CHECK(mc_delete_run_to_bin(lib, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM track_analysis") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'track_analysis'"
                       " AND field = '_deleted'") == 1);

  mc_bin_list *list = NULL;
  CHECK(mc_delete_bin_list(lib, &list) == MC_OK);
  int64_t id = list && list->count ? list->items[0].id : -1;
  mc_delete_bin_list_free(list);
  CHECK(id >= 0);
  mc_bin_taken taken;
  memset(&taken, 0, sizeof taken);
  CHECK(mc_delete_bin_take(lib, id, &taken) == MC_OK);
  mc_bin_taken_free(&taken);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog") == logged);
  mc_analysis got;
  CHECK(mc_analysis_get(lib, track, &got) == MC_OK);
  CHECK(got.lufs == -11.0 && got.centroid_mean == 1234.0f);
  CHECK(!memcmp(got.mfcc_mean, r.mfcc_mean, sizeof got.mfcc_mean));
  CHECK(query_int(lib, "SELECT count(*) FROM track_analysis WHERE typeof(features) = 'blob'") == 1);
  mc_library_close(lib);
}

/* --- The thread --- */

static int analysed(mc_library *lib) {
  return query_int(lib, "SELECT count(*) FROM track_analysis");
}

static int wait_for(mc_library *lib, int count) {
  uint64_t until = mc_mono_ms() + WAIT_MS;
  while (mc_mono_ms() < until) {
    if (analysed(lib) >= count) return 1;
    mc_sleep_ms(10);
  }
  return 0;
}

static void test_scanner(const char *scratch) {
  enum { TRACKS = 6 };
  char dir[1024], path[1200], name[32];
  mc_library *lib = open_library(scratch, "test_analysis_thread.d", dir, sizeof dir);
  if (!lib) return;
  for (int i = 0; i < TRACKS; i++) {
    snprintf(name, sizeof name, "t%d.wav", i);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    CHECK(write_wav(path, 44100, 3.0, 0.5));
    char uid[MC_UID_STRLEN];
    add_track(lib, name, 0, uid);
  }
  mc_analysis_scanner_set_timing(0, 20, 0, 0);

  /* Held off, it reads and writes nothing, and a stop still stops it. */
  mc_analysis_hold();
  mc_analysis_scanner *s = NULL;
  CHECK(mc_analysis_scanner_start(dir, &s) == MC_OK);
  mc_sleep_ms(300);
  CHECK(analysed(lib) == 0);
  uint64_t from = mc_mono_ms();
  mc_analysis_scanner_stop(s);
  CHECK(mc_mono_ms() - from < 2000);
  CHECK(analysed(lib) == 0);

  /* Released, it works; stopped part-way, it stops; started again, it goes on from there. */
  CHECK(mc_analysis_scanner_start(dir, &s) == MC_OK);
  mc_analysis_release();
  CHECK(wait_for(lib, 2));
  mc_analysis_scanner_stop(s);
  int part = analysed(lib);
  CHECK(part >= 2);
  int logged = query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'track_analysis'");
  CHECK(mc_analysis_scanner_start(dir, &s) == MC_OK);
  CHECK(wait_for(lib, TRACKS));
  CHECK(query_int(lib, "SELECT count(*) FROM track_analysis WHERE source = 0") == TRACKS);
  /* Nothing done twice: a row each, and each logged once. */
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'track_analysis'"
                       " AND field = 'features'") == TRACKS);
  CHECK(logged > 0);

  /* A new track is found on a later pass, by itself. */
  char uid[MC_UID_STRLEN];
  snprintf(path, sizeof path, "%s/late.wav", dir);
  CHECK(write_wav(path, 44100, 1.0, 0.5));
  add_track(lib, "late.wav", 0, uid);
  CHECK(wait_for(lib, TRACKS + 1));

  /* With a hold up, a row written mid-session can't happen: the count stands still. */
  mc_analysis_hold();
  mc_sleep_ms(100); /* what was in hand when the hold came */
  char other[MC_UID_STRLEN];
  snprintf(path, sizeof path, "%s/held.wav", dir);
  CHECK(write_wav(path, 44100, 1.0, 0.5));
  add_track(lib, "held.wav", 0, other);
  int before = analysed(lib);
  mc_sleep_ms(300);
  CHECK(analysed(lib) == before);
  mc_analysis_release();
  CHECK(wait_for(lib, TRACKS + 2));
  mc_analysis_scanner_stop(s);
  mc_analysis_scanner_stop(NULL);

  /* Resting as it does for real, a hold isn't taken for work to rest from. */
  mc_analysis_scanner_set_timing(0, 20, 1, 0);
  snprintf(path, sizeof path, "%s/rested.wav", dir);
  CHECK(write_wav(path, 44100, 3.0, 0.5));
  add_track(lib, "rested.wav", 0, other);
  mc_analysis_hold();
  CHECK(mc_analysis_scanner_start(dir, &s) == MC_OK);
  mc_sleep_ms(2500);
  mc_analysis_release();
  from = mc_mono_ms();
  CHECK(wait_for(lib, TRACKS + 3));
  printf("after a 2.5 s hold, a 3 s file took %d ms, resting\n", (int)(mc_mono_ms() - from));
  mc_analysis_scanner_stop(s);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_ebu_3341();
  test_997();
  test_ebu_3342();
  test_gating();
  test_true_peak();
  test_fft();
  test_noise_and_sine();
  test_rates();
  test_invariants();
  test_silent_and_short();
  test_distance();
  test_blob();
  test_file(scratch);
  test_scan(scratch);
  test_sync(scratch);
  test_merge(scratch);
  test_newly_known(scratch);
  test_recycle_bin(scratch);
  test_scanner(scratch);
  mc_shutdown();
  return TEST_RESULT();
}
