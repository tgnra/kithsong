/* Rhythm: how the onset strength of each frame repeats. Not a beat tracker: what's kept is a
 * summary of the periodicities, which two tracks can be compared by, and a best guess at the
 * tempo on top of it.
 *
 * The strengths go through a window of about 12 s, half of it new each time, so memory stays
 * the same for a track hours long. Each window's autocorrelation is scaled by its own energy and
 * added to a sum, so a quiet verse says as much as a loud chorus, and a tempo that drifts only
 * smears as far as it drifts in a window.
 *
 * What would tie results to the file rather than the music:
 *  - Strengths are differences of dB, so a gain doesn't move them, and they're in units that
 *    mean something: a window under a fraction of a dB has no rhythm, however regular.
 *  - Silence around a sound leaves no mark. A gap counts as time only once sound comes back, and
 *    only up to MAX_GAP: past that it's a hidden track, and what follows is spliced on. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"

#define FRAMES_PER_SECOND ((double)MC_ANALYSIS_RATE / MC_ANALYSIS_HOP)

#define WINDOW 512     /* 11.9 s: a few bars at any tempo */
#define STEP 256       /* windows overlap by half, so a beat near an edge counts */
#define MIN_FRAMES 128 /* 3 s: less has too few beats to repeat */
#define MAX_GAP 86     /* 2 s: longer than any beat */
#define LOCAL_MEAN 21  /* frames each side, half a second: slow swells aren't onsets */
#define LANES 8        /* the sums are padded to a multiple of this, for the compiler to vectorise */

/* Lags in frames: 186 ms to 3 s, or 323 to 20 beats a minute, four octaves of them. */
#define MIN_LAG 8
#define MAX_LAG 128
#define BIN_REACH 1.5 /* a lag's share of the bins near it, so tempos 5% apart stay close */

/* Squared dB, added under the energy of a window. A steady tone's strengths wobble by a hundredth
 * of a dB with perfect regularity, which isn't a pulse. */
#define ENERGY_FLOOR 0.01f
#define ONSET_DB 1.5f /* over the local mean: under it a noise's own flicker */
#define ONSET_GAP 3   /* frames: the same attack seen twice, as frames overlap */

#define MIN_BPM 40.0
#define BPM_CENTRE 140.0 /* tempos are heard near here, so it settles a choice between octaves */
#define MIN_PULSE 0.1f   /* a clearer pulse than this has a tempo */

struct mc_rhythm {
  float strength[WINDOW];
  int fill;
  int fresh; /* strengths in no window yet */
  int gap;   /* silent frames since the last that sounded */
  int sounded;
  double acf[MAX_LAG + 1]; /* sums over windows of the autocorrelation at each lag */
  int acf_windows[MAX_LAG + 1];
  double pulse, onsets; /* sums over windows */
  int windows;
};

mc_rhythm *mc_rhythm_new(void) {
  return calloc(1, sizeof(mc_rhythm));
}

void mc_rhythm_free(mc_rhythm *r) {
  free(r);
}

static float product(const float *a, const float *b, int n) {
  float lane[LANES] = {0};
  for (int i = 0; i < n; i += LANES)
    for (int j = 0; j < LANES; j++) lane[j] += a[i + j] * b[i + j];
  return ((lane[0] + lane[4]) + (lane[1] + lane[5])) + ((lane[2] + lane[6]) + (lane[3] + lane[7]));
}

/* Attacks in x, which is what stands over the local mean. */
static int count_onsets(const float *x, int n) {
  int count = 0, last = -ONSET_GAP;
  for (int i = 1; i + 1 < n; i++) {
    if (x[i] < ONSET_DB || x[i] < x[i - 1] || x[i] < x[i + 1] || i - last < ONSET_GAP) continue;
    count++;
    last = i;
  }
  return count;
}

/* The first n strengths, n being WINDOW but for the last of a track. */
static void take_window(mc_rhythm *r, int n) {
  float x[WINDOW + MAX_LAG + LANES] = {0};
  double running[WINDOW + 1];
  running[0] = 0.0;
  for (int i = 0; i < n; i++) running[i + 1] = running[i] + r->strength[i];
  double mean = 0.0;
  for (int i = 0; i < n; i++) {
    int from = i > LOCAL_MEAN ? i - LOCAL_MEAN : 0, to = i + LOCAL_MEAN + 1 < n ? i + LOCAL_MEAN + 1 : n;
    float over = r->strength[i] - (float)((running[to] - running[from]) / (to - from));
    x[i] = over > 0.0f ? over : 0.0f;
    mean += x[i];
  }
  r->onsets += count_onsets(x, n) * FRAMES_PER_SECOND / n;
  mean /= n;
  for (int i = 0; i < n; i++) x[i] -= (float)mean;

  int padded = (n + LANES - 1) / LANES * LANES, top = n / 2 < MAX_LAG ? n / 2 : MAX_LAG;
  float energy = product(x, x, padded) / (float)n + ENERGY_FLOOR, best = 0.0f;
  for (int lag = 1; lag <= top; lag++) {
    float v = product(x, x + lag, padded) / (float)(n - lag) / energy;
    r->acf[lag] += v;
    r->acf_windows[lag]++;
    if (lag >= MIN_LAG && v > best) best = v;
  }
  r->pulse += best;
  r->windows++;
}

static void push(mc_rhythm *r, float strength) {
  r->strength[r->fill++] = strength;
  r->fresh++;
  if (r->fill < WINDOW) return;
  take_window(r, WINDOW);
  memmove(r->strength, r->strength + STEP, (WINDOW - STEP) * sizeof *r->strength);
  r->fill = WINDOW - STEP;
  r->fresh = 0;
}

void mc_rhythm_onset(mc_rhythm *r, float strength) {
  if (r->sounded && r->gap <= MAX_GAP)
    for (int i = 0; i < r->gap; i++) push(r, 0.0f);
  r->sounded = 1;
  r->gap = 0;
  push(r, strength);
}

void mc_rhythm_silence(mc_rhythm *r) {
  r->gap++;
}

/* Where a peak really is, between frames, from the lags either side of it. */
static double peak_at(const float *p, int lag) {
  if (lag <= 1 || lag >= MAX_LAG) return lag;
  double a = p[lag - 1], b = p[lag], c = p[lag + 1], curve = a - 2.0 * b + c;
  return curve < 0.0 ? lag + 0.5 * (a - c) / curve : lag;
}

/* A beat's multiples are lags that repeat too. They say nothing new, and they are what keeps two
 * tempos an octave apart from looking alike, so a lag keeps only what its fractions don't explain:
 * a click at 170 and one at 85 are each left with one peak, at the same place in the octave. */
static void take_out_multiples(const float *p, float *left) {
  for (int lag = MIN_LAG; lag <= MAX_LAG; lag++) {
    float explained = 0.0f;
    for (int k = 2; lag / k >= MIN_LAG; k++) {
      int low = lag / k, high = low + (lag % k != 0);
      float v = p[low] > p[high] ? p[low] : p[high]; /* it may fall between two lags */
      if (v > explained) explained = v;
    }
    left[lag] = p[lag] > explained ? p[lag] - explained : 0.0f;
  }
}

static void fill_profile(const float *left, float *profile) {
  for (int lag = MIN_LAG; lag <= MAX_LAG; lag++) {
    double at = MC_ANALYSIS_LAGS_PER_OCTAVE * log2((double)lag / MIN_LAG);
    for (int b = 0; b < MC_ANALYSIS_LAGS; b++) {
      double near = 1.0 - fabs(at - b) / BIN_REACH;
      float v = (float)near * left[lag];
      if (near > 0.0 && v > profile[b]) profile[b] = v;
    }
  }
}

/* The strongest lag with the likeliest tempos favoured, made exact from where its multiples fall:
 * they're further out, where a frame is a smaller share of the lag. 0 if nothing repeats. */
static double best_lag(const float *p) {
  int top = (int)(FRAMES_PER_SECOND * 60.0 / MIN_BPM), best = 0;
  double best_score = 0.0;
  for (int lag = MIN_LAG; lag <= top; lag++) {
    if (p[lag] < p[lag - 1] || p[lag] < p[lag + 1]) continue;
    double octaves = log2(FRAMES_PER_SECOND * 60.0 / lag / BPM_CENTRE);
    double score = p[lag] * exp(-0.5 * octaves * octaves);
    if (score > best_score) {
      best_score = score;
      best = lag;
    }
  }
  if (!best) return 0.0;
  double lag = peak_at(p, best);
  for (int times = 2; times * (lag + 1.0) < MAX_LAG; times *= 2) {
    int centre = (int)(lag * times + 0.5), peak = centre;
    for (int l = centre - times; l <= centre + times; l++)
      if (p[l] > p[peak]) peak = l;
    if (p[peak] < 0.5f * p[best]) break;
    lag = peak_at(p, peak) / times;
  }
  return lag;
}

void mc_rhythm_finish(mc_rhythm *r, mc_analysis *out) {
  /* What's left of a track past its last whole window, if it's worth one. */
  if (r->windows ? r->fresh >= STEP / 2 : r->fill >= MIN_FRAMES) take_window(r, r->fill);
  if (!r->windows) return;
  float p[MAX_LAG + 2] = {0}, left[MAX_LAG + 2] = {0};
  for (int lag = 1; lag <= MAX_LAG; lag++) {
    double v = r->acf_windows[lag] ? r->acf[lag] / r->acf_windows[lag] : 0.0;
    p[lag] = v > 0.0 ? (float)v : 0.0f;
  }
  take_out_multiples(p, left);
  fill_profile(left, out->rhythm);
  out->pulse = (float)(r->pulse / r->windows);
  out->onset_rate = (float)(r->onsets / r->windows);
  double lag = out->pulse >= MIN_PULSE ? best_lag(p) : 0.0;
  out->bpm = lag > 0.0 ? (float)(FRAMES_PER_SECOND * 60.0 / lag) : 0.0f;
}
