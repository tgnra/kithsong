/* Timbre: a mono signal is resampled to MC_ANALYSIS_RATE and cut into Hann-windowed frames,
 * and each frame gives a spectral centroid and, through 40 mel bands in dB and a DCT, 13
 * MFCCs. Means and variances over the frames are what's kept, and how fast the MFCCs move.
 * Straight from the power bins, as mel bands can't resolve the low end, come each frame's share
 * of energy by band and how flat each band is. What rose since the last frame goes to rhythm.c.
 *
 * What would tie results to the file rather than the music is taken out here:
 *  - The resampler has the same cutoff from every rate above the internal one.
 *  - Frames under SILENCE_DB are left out, so a long fade or a gap before a hidden track
 *    doesn't count as timbre.
 *  - The floor under the mel bands is relative to the frame's own power, so a band with nothing
 *    in it reads the same at any level, and rounding noise in an empty band never shows. A gain
 *    then moves every band alike, which the orthonormal DCT puts in coefficient 0 alone: the
 *    other 12 don't change with level.
 *  - Band shares and flatness are ratios of a frame's own power, and the onset strength and the
 *    MFCCs' changes are differences of dB, so none of them moves with a gain either. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"

#define PI 3.14159265358979323846
#define FRAME MC_ANALYSIS_FRAME
#define BINS (FRAME / 2 + 1)
#define MELS MC_ANALYSIS_MELS
#define MFCCS MC_ANALYSIS_MFCCS

#define MEL_LOW_HZ 20.0
#define MEL_HIGH_HZ (MC_ANALYSIS_RATE / 2.0)
#define SILENCE_DB (-70.0) /* a frame's level in dBFS, under which it doesn't count */
#define FLOOR_DB (-100.0)  /* under the frame's power: as low as a mel band reads */
#define ONSET_RANGE_DB 60.0f /* under the frame's top band: lower bands say nothing of an onset */
#define ONSET_MOST_DB 15.0f  /* a sound out of silence: as strong as an onset reads */
#define DELTA_FRAMES 2       /* apart, which is frames sharing no samples */

/* Bands in bins of 21.5 Hz, for the low end's sake straight from the spectrum. A bin reaches half
 * a bin either side. Sub stops where a kick's body starts and starts above bin 1, which holds what
 * a DC offset leaks. Bass is kicks and bass lines. Low mid is the body of voices and guitars, mid
 * up to 2 kHz their harmonics, and high is presence and air, stopping under the resampler's edge.
 * The edges are about the octaves that production talks in. */
static const struct {
  int first, last;
} k_bands[MC_ANALYSIS_BANDS] = {
  {2, 3},    /* 32 to 75 Hz */
  {4, 11},   /* 75 to 248 Hz */
  {12, 23},  /* 248 to 506 Hz */
  {24, 95},  /* 506 Hz to 2.1 kHz */
  {96, 495}, /* 2.1 to 10.7 kHz */
};

/* Flatness goes block by block, as across a wide band it'd only measure the spectrum's tilt. Blocks
 * are 260 to 340 Hz, a harmonic or so of a voice, except low down where a band is all there is. */
static const struct {
  int first, blocks, size;
} k_flatness[MC_ANALYSIS_FLATNESS] = {
  {2, 1, 10},
  {12, 1, 12},
  {24, 6, 12},
  {96, 25, 16},
};
#define BAND_FLOOR 1e-6 /* -60 dB of the frame: a band under this is empty, whatever's in it */
#define BIN_FLOOR 1e-6f /* of a block's mean: a deep notch says no more than this */

/* The resampler's windowed sinc: 16 zero crossings each side and a Kaiser window down 60 dB,
 * cutting off at 97% of the lower of the two Nyquist frequencies. Phases are exact for the
 * usual rates (147 from 48 kHz, 1 from 44.1 kHz); an odd rate takes the nearest of 4096, an
 * error far under what a mel band can tell. */
#define SINC_CROSSINGS 16
#define SINC_BETA 6.0
#define SINC_CUTOFF 0.97
#define MAX_PHASES 4096
#define MAX_STEP 64 /* the highest rate taken, as a multiple of the internal one */
#define LANES 8     /* taps are padded to a multiple of this, for the compiler to vectorise */
#define IN_CHUNK 8192

typedef struct mel_band {
  int first, count;
  float weight[BINS];
} mel_band;

/* e^(-2 pi i k / FRAME), k < FRAME / 2. */
typedef struct twiddles {
  float cos[FRAME / 2], sin[FRAME / 2];
} twiddles;

struct mc_timbre {
  /* Resampling: output n is at input position n * step_num / step_den. */
  uint32_t step_num, step_den, phases;
  int taps, half;
  float *kernel; /* phases * taps */
  float *in;     /* input not yet used, the first of it `half` samples before the position */
  size_t in_len, in_cap, pos;
  uint32_t rem;

  float frame[FRAME];
  int fill;
  float window[FRAME];
  mel_band *mel;
  float dct[MFCCS][MELS];
  twiddles tw;

  /* What came before, back to the last silent frame. */
  float last_db[MELS];
  int has_last;
  float past[DELTA_FRAMES][MFCCS];
  uint64_t past_count;
  mc_rhythm *rhythm;

  /* Sums over the frames that count. */
  uint64_t frames;
  double sum[MFCCS], sum_sq[MFCCS];
  double centroid_sum, centroid_sq;
  double delta_sq[MFCCS];
  uint64_t deltas;
  double share[MC_ANALYSIS_BANDS];
  double flat_sum[MC_ANALYSIS_FLATNESS], flat_weight[MC_ANALYSIS_FLATNESS];
  int any; /* a frame was looked at, loud enough or not */
};

/* --- The FFT ---
 *
 * n real samples as n/2 complex ones, a radix-2 FFT of those, then the spectra of the even and
 * odd samples pulled apart and joined. */

static void make_twiddles(twiddles *tw) {
  for (int k = 0; k < FRAME / 2; k++) {
    tw->cos[k] = (float)cos(2.0 * PI * k / FRAME);
    tw->sin[k] = (float)-sin(2.0 * PI * k / FRAME);
  }
}

/* In place on n points, n at most FRAME / 2. */
static void fft(const twiddles *tw, float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    int half = len >> 1, stride = FRAME / len;
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < half; k++) {
        float wr = tw->cos[k * stride], wi = tw->sin[k * stride];
        int a = i + k, b = a + half;
        float xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
  }
}

static void power_spectrum(const twiddles *tw, const float *x, int n, float *power) {
  float re[FRAME / 2], im[FRAME / 2];
  int half = n / 2, stride = FRAME / n;
  for (int i = 0; i < half; i++) {
    re[i] = x[2 * i];
    im[i] = x[2 * i + 1];
  }
  fft(tw, re, im, half);
  power[0] = (re[0] + im[0]) * (re[0] + im[0]);
  power[half] = (re[0] - im[0]) * (re[0] - im[0]);
  for (int k = 1; k < half; k++) {
    int m = half - k;
    float er = 0.5f * (re[k] + re[m]), ei = 0.5f * (im[k] - im[m]); /* the even samples' */
    float dr = 0.5f * (im[k] + im[m]), di = 0.5f * (re[m] - re[k]); /* the odd samples' */
    float wr = tw->cos[k * stride], wi = tw->sin[k * stride];
    float xr = er + dr * wr - di * wi, xi = ei + dr * wi + di * wr;
    power[k] = xr * xr + xi * xi;
  }
}

void mc_analysis_power_spectrum(const float *x, int n, float *power) {
  twiddles tw;
  make_twiddles(&tw);
  power_spectrum(&tw, x, n, power);
}

/* --- Mel bands and frames --- */

static double to_mel(double hz) {
  return 2595.0 * log10(1.0 + hz / 700.0);
}

static double from_mel(double mel) {
  return 700.0 * (pow(10.0, mel / 2595.0) - 1.0);
}

/* Triangles, each of area one, so a flat spectrum reads the same in every band. */
static void mel_bands(mel_band *bands) {
  double low = to_mel(MEL_LOW_HZ), high = to_mel(MEL_HIGH_HZ);
  double bin_hz = (double)MC_ANALYSIS_RATE / FRAME;
  for (int m = 0; m < MELS; m++) {
    double left = from_mel(low + (high - low) * m / (MELS + 1));
    double mid = from_mel(low + (high - low) * (m + 1) / (MELS + 1));
    double right = from_mel(low + (high - low) * (m + 2) / (MELS + 1));
    mel_band *b = &bands[m];
    b->first = -1;
    double area = 0.0;
    for (int k = 0; k < BINS; k++) {
      double hz = k * bin_hz;
      double w = hz <= left || hz >= right ? 0.0
                 : hz <= mid               ? (hz - left) / (mid - left)
                                           : (right - hz) / (right - mid);
      if (w <= 0.0) continue;
      if (b->first < 0) b->first = k;
      b->weight[k - b->first] = (float)w;
      b->count = k - b->first + 1;
      area += w;
    }
    for (int i = 0; i < b->count; i++) b->weight[i] = (float)(b->weight[i] / area);
    if (b->first < 0) b->first = 0;
  }
}

/* log2 of a positive normal float to about 1e-3, far under what a band's flatness can tell, at a
 * fraction of log2f()'s cost: there are 500 of these to a frame. */
static float quick_log2(float x) {
  uint32_t bits;
  memcpy(&bits, &x, sizeof bits);
  float e = (float)((int)(bits >> 23) - 127);
  bits = (bits & 0x007fffffu) | 0x3f800000u;
  float m;
  memcpy(&m, &bits, sizeof m);
  return e + (-2.153620718f + m * (3.047884161f + m * (-1.051875031f + m * 0.1582487046f)));
}

/* The frame's share of energy in each band, and how flat they are. */
static void bands_of(mc_timbre *t, const float *power) {
  double band[MC_ANALYSIS_BANDS], total = 0.0;
  for (int b = 0; b < MC_ANALYSIS_BANDS; b++) {
    float e = 0.0f;
    for (int k = k_bands[b].first; k <= k_bands[b].last; k++) e += power[k];
    band[b] = e;
    total += e;
  }
  if (total <= 0.0) return;
  for (int b = 0; b < MC_ANALYSIS_BANDS; b++) t->share[b] += band[b] / total;
  for (int f = 0; f < MC_ANALYSIS_FLATNESS; f++) {
    const float *block = power + k_flatness[f].first;
    int size = k_flatness[f].size;
    double energy = 0.0, flat = 0.0;
    for (int n = 0; n < k_flatness[f].blocks; n++, block += size) {
      float sum = 0.0f;
      for (int k = 0; k < size; k++) sum += block[k];
      if (sum < 1e-30f) continue;
      float mean = sum / (float)size, least = mean * BIN_FLOOR, logs = 0.0f;
      for (int k = 0; k < size; k++) logs += quick_log2(block[k] > least ? block[k] : least);
      energy += sum;
      flat += (double)sum * (logs / (float)size - quick_log2(mean));
    }
    double weight = energy / total;
    if (weight < BAND_FLOOR) continue;
    t->flat_sum[f] += weight * (flat / energy) * 3.0102999566; /* in dB */
    t->flat_weight[f] += weight;
  }
}

/* How much rose since the last frame, as a mean over the bands. After a gap there's nothing to
 * rise from but the floor. */
static float onset_strength(mc_timbre *t, const float *db) {
  float top = db[0];
  for (int m = 1; m < MELS; m++)
    if (db[m] > top) top = db[m];
  float floor_db = top - ONSET_RANGE_DB, sum = 0.0f;
  for (int m = 0; m < MELS; m++) {
    float now = db[m] > floor_db ? db[m] : floor_db;
    float was = t->has_last && t->last_db[m] > floor_db ? t->last_db[m] : floor_db;
    if (now > was) sum += now - was;
    t->last_db[m] = db[m];
  }
  t->has_last = 1;
  sum /= MELS;
  return sum < ONSET_MOST_DB ? sum : ONSET_MOST_DB;
}

static void frame_done(mc_timbre *t) {
  t->any = 1;
  double sq = 0.0;
  for (int i = 0; i < FRAME; i++) sq += (double)t->frame[i] * t->frame[i];
  if (sq <= 0.0 || 10.0 * log10(sq / FRAME) < SILENCE_DB) {
    /* What comes after a gap isn't a change from what came before it. */
    t->has_last = 0;
    t->past_count = 0;
    mc_rhythm_silence(t->rhythm);
    return;
  }

  float x[FRAME], power[BINS];
  for (int i = 0; i < FRAME; i++) x[i] = t->frame[i] * t->window[i];
  power_spectrum(&t->tw, x, FRAME, power);

  double total = 0.0, moment = 0.0, all_power = 0.0;
  for (int k = 0; k < BINS; k++) {
    double magnitude = sqrt(power[k]);
    total += magnitude;
    moment += magnitude * k;
    all_power += power[k];
  }
  double centroid = total > 0.0 ? moment / total * MC_ANALYSIS_RATE / FRAME : 0.0;

  float db[MELS];
  float floor_power = (float)(all_power * pow(10.0, FLOOR_DB / 10.0));
  for (int m = 0; m < MELS; m++) {
    const mel_band *b = &t->mel[m];
    float e = 0.0f;
    for (int i = 0; i < b->count; i++) e += b->weight[i] * power[b->first + i];
    db[m] = 10.0f * log10f(e > floor_power ? e : floor_power);
  }
  float *past = t->past[t->past_count % DELTA_FRAMES];
  int moved = t->past_count >= DELTA_FRAMES;
  for (int c = 0; c < MFCCS; c++) {
    float v = 0.0f;
    for (int m = 0; m < MELS; m++) v += t->dct[c][m] * db[m];
    t->sum[c] += v;
    t->sum_sq[c] += (double)v * v;
    if (moved) t->delta_sq[c] += (double)(v - past[c]) * (v - past[c]);
    past[c] = v;
  }
  t->past_count++;
  t->deltas += (uint64_t)moved;
  bands_of(t, power);
  mc_rhythm_onset(t->rhythm, onset_strength(t, db));
  t->centroid_sum += centroid;
  t->centroid_sq += centroid * centroid;
  t->frames++;
}

/* --- Resampling --- */

static double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 32; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

static uint32_t gcd(uint32_t a, uint32_t b) {
  while (b) {
    uint32_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

static int make_kernel(mc_timbre *t, unsigned rate) {
  uint32_t g = gcd(rate, MC_ANALYSIS_RATE);
  t->step_num = rate / g;
  t->step_den = MC_ANALYSIS_RATE / g;
  t->phases = t->step_den < MAX_PHASES ? t->step_den : MAX_PHASES;
  /* The cutoff as a fraction of the input's Nyquist frequency. */
  double cutoff = SINC_CUTOFF * (rate > MC_ANALYSIS_RATE ? (double)MC_ANALYSIS_RATE / rate : 1.0);
  t->half = (int)ceil(SINC_CROSSINGS / cutoff);
  t->taps = (2 * t->half + LANES - 1) / LANES * LANES;
  t->kernel = calloc((size_t)t->phases * (size_t)t->taps, sizeof *t->kernel);
  double *v = malloc(2 * (size_t)t->half * sizeof *v);
  if (!t->kernel || !v) {
    free(v);
    return 0;
  }
  double i0 = bessel_i0(SINC_BETA);
  for (uint32_t p = 0; p < t->phases; p++) {
    float *h = t->kernel + (size_t)p * (size_t)t->taps;
    double frac = (double)p / t->phases, sum = 0.0;
    for (int k = 0; k < 2 * t->half; k++) {
      double d = (k - t->half + 1) - frac, r = d / t->half, a = PI * cutoff * d;
      double sinc = fabs(a) < 1e-9 ? 1.0 : sin(a) / a;
      v[k] = fabs(r) < 1.0 ? sinc * bessel_i0(SINC_BETA * sqrt(1.0 - r * r)) / i0 : 0.0;
      sum += v[k];
    }
    for (int k = 0; k < 2 * t->half; k++) h[k] = (float)(v[k] / sum);
  }
  free(v);
  return 1;
}

/* One output sample, from taps inputs at x. */
static float dot(const float *x, const float *h, int taps) {
  float lane[LANES] = {0};
  for (int i = 0; i < taps; i += LANES)
    for (int j = 0; j < LANES; j++) lane[j] += x[i + j] * h[i + j];
  return ((lane[0] + lane[4]) + (lane[1] + lane[5])) + ((lane[2] + lane[6]) + (lane[3] + lane[7]));
}

/* Makes every output sample the input so far allows. */
static void drain(mc_timbre *t) {
  size_t taps = (size_t)t->taps;
  while (t->pos + taps <= t->in_len) {
    uint32_t phase = t->phases == t->step_den
                       ? t->rem
                       : (uint32_t)((uint64_t)t->rem * t->phases / t->step_den);
    t->frame[t->fill++] = dot(t->in + t->pos, t->kernel + (size_t)phase * taps, t->taps);
    if (t->fill == FRAME) {
      frame_done(t);
      memmove(t->frame, t->frame + MC_ANALYSIS_HOP, (FRAME - MC_ANALYSIS_HOP) * sizeof *t->frame);
      t->fill = FRAME - MC_ANALYSIS_HOP;
    }
    t->rem += t->step_num;
    t->pos += t->rem / t->step_den;
    t->rem %= t->step_den;
  }
  memmove(t->in, t->in + t->pos, (t->in_len - t->pos) * sizeof *t->in);
  t->in_len -= t->pos;
  t->pos = 0;
}

mc_timbre *mc_timbre_new(unsigned rate) {
  if (!rate || rate > (unsigned)MC_ANALYSIS_RATE * MAX_STEP) return NULL;
  mc_timbre *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->mel = calloc(MELS, sizeof *t->mel);
  t->rhythm = mc_rhythm_new();
  if (!t->mel || !t->rhythm || !make_kernel(t, rate)) {
    mc_timbre_free(t);
    return NULL;
  }
  t->in_cap = (size_t)t->taps + IN_CHUNK;
  t->in = calloc(t->in_cap, sizeof *t->in);
  if (!t->in) {
    mc_timbre_free(t);
    return NULL;
  }
  t->in_len = (size_t)t->half; /* silence before the first sample: the kernel is centred */
  make_twiddles(&t->tw);
  for (int i = 0; i < FRAME; i++) t->window[i] = (float)(0.5 - 0.5 * cos(2.0 * PI * i / FRAME));
  mel_bands(t->mel);
  for (int c = 0; c < MFCCS; c++)
    for (int m = 0; m < MELS; m++)
      t->dct[c][m] = (float)(sqrt((c ? 2.0 : 1.0) / MELS) * cos(PI * c * (m + 0.5) / MELS));
  return t;
}

void mc_timbre_free(mc_timbre *t) {
  if (!t) return;
  free(t->kernel);
  free(t->in);
  free(t->mel);
  mc_rhythm_free(t->rhythm);
  free(t);
}

void mc_timbre_feed(mc_timbre *t, const float *mono, size_t frames) {
  while (frames) {
    size_t room = t->in_cap - t->in_len, n = frames < room ? frames : room;
    memcpy(t->in + t->in_len, mono, n * sizeof *mono);
    t->in_len += n;
    mono += n;
    frames -= n;
    drain(t);
  }
}

void mc_timbre_finish(mc_timbre *t, mc_analysis *out) {
  /* The kernel's reach past the end, so the last samples come out. */
  static const float silence[256];
  for (int left = t->half; left > 0; left -= 256)
    mc_timbre_feed(t, silence, left < 256 ? (size_t)left : 256);
  /* A file shorter than one frame still gets one. */
  if (!t->any && t->fill) {
    memset(t->frame + t->fill, 0, (size_t)(FRAME - t->fill) * sizeof *t->frame);
    frame_done(t);
  }
  double n = t->frames ? (double)t->frames : 1.0;
  for (int c = 0; c < MFCCS; c++) {
    double mean = t->sum[c] / n, var = t->sum_sq[c] / n - mean * mean;
    out->mfcc_mean[c] = (float)mean;
    out->mfcc_var[c] = (float)(var > 0.0 ? var : 0.0);
  }
  double mean = t->centroid_sum / n, var = t->centroid_sq / n - mean * mean;
  out->centroid_mean = (float)mean;
  out->centroid_var = (float)(var > 0.0 ? var : 0.0);
  double deltas = t->deltas ? (double)t->deltas : 1.0;
  for (int c = 0; c < MFCCS; c++) out->mfcc_delta[c] = (float)sqrt(t->delta_sq[c] / deltas);
  for (int b = 0; b < MC_ANALYSIS_BANDS; b++) {
    double share = t->share[b] / n;
    out->band_share[b] = (float)(10.0 * log10(share > BAND_FLOOR ? share : BAND_FLOOR));
  }
  for (int f = 0; f < MC_ANALYSIS_FLATNESS; f++)
    out->band_flatness[f] = t->flat_weight[f] > 0.0 ? (float)(t->flat_sum[f] / t->flat_weight[f]) : NAN;
  mc_rhythm_finish(t->rhythm, out);
}
