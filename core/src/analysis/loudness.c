/* Loudness per ITU-R BS.1770-4 and EBU Tech 3341/3342: K-weighting, gated 400 ms blocks for
 * integrated loudness, gated 3 s blocks for loudness range, and oversampled true peak.
 *
 * The K filter's two stages are given in BS.1770 as coefficients at 48 kHz. Here they come from
 * their analogue parameters by the bilinear transform at the file's rate, which gives those
 * very coefficients at 48 kHz and the same curve elsewhere, so no resampling is needed.
 *
 * Everything counts in 100 ms steps: squares are summed per step, a 400 ms block is 4 steps and
 * a 3 s block 30, each starting one step after the last (75% overlap for the first, as BS.1770
 * asks). Blocks' mean squares are kept, 8 bytes per 100 ms each, so gating is exact: a day-long
 * file costs 14 MB. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"

#define PI 3.14159265358979323846
#define BLOCK_STEPS 4
#define TERM_STEPS 30
#define CHUNK 1024 /* frames worked on at once */

#define ABS_GATE (-70.0)
#define BLOCK_GATE (-10.0) /* LU under the loudness of the blocks over ABS_GATE */
#define TERM_GATE (-20.0)
#define LOW_PERCENTILE 0.10
#define HIGH_PERCENTILE 0.95

/* True peak: samples are interpolated at quarters, or at 96 kHz and up, where peaks are nearer
 * to samples, at halves, and not at all from 192 kHz. Each point is 16 taps of a Kaiser
 * windowed sinc. */
#define TP_TAPS 16
#define TP_MAX_OVER 4
#define TP_BETA 5.0
#define TP_RUN 64 /* samples between checks of whether a run could hold a new peak */

typedef struct biquad {
  double b0, b1, b2, a1, a2;
} biquad;

typedef struct series {
  double *v;
  size_t count, cap;
} series;

typedef struct channel {
  double z[4]; /* the two stages' state */
  double weight;
  float *hist; /* TP_TAPS - 1 earlier samples, then the chunk */
} channel;

struct mc_loudness {
  unsigned rate, channels;
  biquad pre, rlb;
  channel *ch;
  uint64_t seen, step, step_end; /* frames so far; the step they're in, and where it ends */
  double step_sum;               /* weighted squares of this step */
  double sums[TERM_STEPS];       /* of the last steps, by step % TERM_STEPS */
  uint64_t sizes[TERM_STEPS];
  series blocks, terms;
  int failed; /* out of memory: what it needed stays undefined */

  int over;
  float fir[TP_MAX_OVER][TP_TAPS];
  float reach; /* the most an interpolated sample can be, as a multiple of its neighbours */
  float peak;
};

/* Stage 1: the head's high shelf. */
static biquad pre_filter(double rate) {
  const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
  double k = tan(PI * f0 / rate), k2 = k * k;
  double vh = pow(10.0, gain_db / 20.0), vb = pow(vh, 0.4996667741545416);
  double a0 = 1.0 + k / q + k2;
  return (biquad){(vh + vb * k / q + k2) / a0, 2.0 * (k2 - vh) / a0, (vh - vb * k / q + k2) / a0,
                  2.0 * (k2 - 1.0) / a0, (1.0 - k / q + k2) / a0};
}

/* Stage 2: the RLB high pass. */
static biquad rlb_filter(double rate) {
  const double f0 = 38.13547087602444, q = 0.5003270373238773;
  double k = tan(PI * f0 / rate), k2 = k * k;
  double a0 = 1.0 + k / q + k2;
  return (biquad){1.0, -2.0, 1.0, 2.0 * (k2 - 1.0) / a0, (1.0 - k / q + k2) / a0};
}

/* The modified Bessel function of order 0, as a Kaiser window needs. */
static double bessel_i0(double x) {
  double sum = 1.0, term = 1.0;
  for (int k = 1; k < 32; k++) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

static void true_peak_filter(mc_loudness *l) {
  l->over = l->rate < 96000 ? 4 : l->rate < 192000 ? 2 : 1;
  double worst = 1.0;
  for (int p = 1; p < l->over; p++) {
    double h[TP_TAPS], sum = 0.0, abs_sum = 0.0;
    for (int k = 0; k < TP_TAPS; k++) {
      double d = (k - (TP_TAPS / 2 - 1)) - (double)p / l->over, r = d / (TP_TAPS / 2);
      double window = fabs(r) < 1.0 ? bessel_i0(TP_BETA * sqrt(1.0 - r * r)) / bessel_i0(TP_BETA) : 0.0;
      h[k] = sin(PI * d) / (PI * d) * window;
      sum += h[k];
    }
    for (int k = 0; k < TP_TAPS; k++) {
      l->fir[p][k] = (float)(h[k] / sum);
      abs_sum += fabs(h[k] / sum);
    }
    if (abs_sum > worst) worst = abs_sum;
  }
  l->reach = (float)worst;
}

mc_loudness *mc_loudness_new(unsigned rate, unsigned channels) {
  if (rate < 10 || !channels) return NULL;
  mc_loudness *l = calloc(1, sizeof *l);
  if (!l) return NULL;
  l->rate = rate;
  l->channels = channels;
  l->pre = pre_filter(rate);
  l->rlb = rlb_filter(rate);
  l->step_end = rate / 10;
  true_peak_filter(l);
  l->ch = calloc(channels, sizeof *l->ch);
  int ok = l->ch != NULL;
  for (unsigned c = 0; ok && c < channels; c++) {
    /* BS.1770's weights, for the usual order: L R C, then the LFE (left out) in 5.1 and up,
     * then the surrounds. Five channels are 5.0, and four are quadraphonic. */
    int lfe = channels >= 6 && c == 3;
    int surround = channels >= 4 && c >= (channels >= 6 ? 4u : channels == 5 ? 3u : 2u);
    l->ch[c].weight = lfe ? 0.0 : surround ? 1.41 : 1.0;
    l->ch[c].hist = calloc(TP_TAPS - 1 + CHUNK, sizeof *l->ch[c].hist);
    ok = l->ch[c].hist != NULL;
  }
  if (!ok) {
    mc_loudness_free(l);
    return NULL;
  }
  return l;
}

void mc_loudness_free(mc_loudness *l) {
  if (!l) return;
  for (unsigned c = 0; l->ch && c < l->channels; c++) free(l->ch[c].hist);
  free(l->ch);
  free(l->blocks.v);
  free(l->terms.v);
  free(l);
}

static void series_add(mc_loudness *l, series *s, double v) {
  if (s->count == s->cap) {
    size_t grown = s->cap ? s->cap * 2 : 1024;
    double *more = realloc(s->v, grown * sizeof *more);
    if (!more) {
      l->failed = 1;
      return;
    }
    s->v = more;
    s->cap = grown;
  }
  s->v[s->count++] = v;
}

/* The mean square of the n steps up to and including step last. */
static double mean_of(const mc_loudness *l, uint64_t last, int n) {
  double sum = 0.0;
  uint64_t size = 0;
  for (int i = 0; i < n; i++) {
    sum += l->sums[(last - (uint64_t)i) % TERM_STEPS];
    size += l->sizes[(last - (uint64_t)i) % TERM_STEPS];
  }
  return size ? sum / (double)size : 0.0;
}

static void end_step(mc_loudness *l) {
  l->sums[l->step % TERM_STEPS] = l->step_sum;
  l->sizes[l->step % TERM_STEPS] = l->seen - l->step * l->rate / 10;
  l->step_sum = 0.0;
  if (l->step + 1 >= BLOCK_STEPS) series_add(l, &l->blocks, mean_of(l, l->step, BLOCK_STEPS));
  if (l->step + 1 >= TERM_STEPS) series_add(l, &l->terms, mean_of(l, l->step, TERM_STEPS));
  l->step++;
  l->step_end = (l->step + 1) * l->rate / 10;
  /* A filter ringing down over digital silence would reach denormals, which are very slow. */
  for (unsigned c = 0; c < l->channels; c++)
    for (int i = 0; i < 4; i++)
      if (fabs(l->ch[c].z[i]) < 1e-30) l->ch[c].z[i] = 0.0;
}

/* The sum of n samples' K-weighted squares. */
static double weigh(const mc_loudness *l, channel *c, const float *x, size_t n) {
  const biquad a = l->pre, b = l->rlb;
  double z0 = c->z[0], z1 = c->z[1], z2 = c->z[2], z3 = c->z[3], sum = 0.0;
  for (size_t i = 0; i < n; i++) {
    double in = x[i];
    double m = a.b0 * in + z0;
    z0 = a.b1 * in - a.a1 * m + z1;
    z1 = a.b2 * in - a.a2 * m;
    double y = b.b0 * m + z2;
    z2 = b.b1 * m - b.a1 * y + z3;
    z3 = b.b2 * m - b.a2 * y;
    sum += y * y;
  }
  c->z[0] = z0;
  c->z[1] = z1;
  c->z[2] = z2;
  c->z[3] = z3;
  return sum;
}

/* Raises the peak to the highest sample or point between samples, of n + TP_TAPS - 1 samples.
 * A run is only interpolated if it could beat the peak so far: no point passes the highest of
 * its neighbours times the filter's reach, and most of a track is well under its peak, so the
 * result is the same for a fraction of the work. */
static void true_peak(mc_loudness *l, const float *buf, size_t n) {
  float peak = l->peak;
  for (size_t at = 0; at < n; at += TP_RUN) {
    size_t run = n - at < TP_RUN ? n - at : TP_RUN;
    float high = 0.0f;
    for (size_t i = 0; i < run + TP_TAPS - 1; i++) {
      float v = fabsf(buf[at + i]);
      high = v > high ? v : high;
    }
    peak = high > peak ? high : peak;
    if (high * l->reach <= peak) continue;
    for (int p = 1; p < l->over; p++) {
      float acc[TP_RUN] = {0};
      for (int k = 0; k < TP_TAPS; k++) {
        float f = l->fir[p][k];
        const float *in = buf + at + (size_t)k;
        for (size_t i = 0; i < run; i++) acc[i] += f * in[i];
      }
      for (size_t i = 0; i < run; i++) {
        float v = fabsf(acc[i]);
        peak = v > peak ? v : peak;
      }
    }
  }
  l->peak = peak;
}

void mc_loudness_feed(mc_loudness *l, const float *in, size_t frames) {
  while (frames) {
    size_t n = frames < CHUNK ? frames : CHUNK;
    if (n > l->step_end - l->seen) n = (size_t)(l->step_end - l->seen);
    for (unsigned c = 0; c < l->channels; c++) {
      channel *ch = &l->ch[c];
      float *x = ch->hist + TP_TAPS - 1;
      for (size_t i = 0; i < n; i++) x[i] = in[i * l->channels + c];
      l->step_sum += ch->weight * weigh(l, ch, x, n);
      true_peak(l, ch->hist, n);
      memmove(ch->hist, ch->hist + n, (TP_TAPS - 1) * sizeof *ch->hist);
    }
    l->seen += n;
    in += n * l->channels;
    frames -= n;
    if (l->seen == l->step_end) end_step(l);
  }
}

static double to_lufs(double mean_square) {
  return -0.691 + 10.0 * log10(mean_square);
}

/* The mean of the blocks over gate (LUFS), and how many there are. */
static double gated_mean(const series *s, double gate, size_t *kept) {
  double sum = 0.0;
  *kept = 0;
  for (size_t i = 0; i < s->count; i++)
    if (s->v[i] > 0.0 && to_lufs(s->v[i]) > gate) {
      sum += s->v[i];
      (*kept)++;
    }
  return *kept ? sum / (double)*kept : 0.0;
}

static double integrated(const mc_loudness *l) {
  size_t kept;
  double ungated = gated_mean(&l->blocks, ABS_GATE, &kept);
  if (!kept) return NAN;
  double mean = gated_mean(&l->blocks, to_lufs(ungated) + BLOCK_GATE, &kept);
  return kept ? to_lufs(mean) : NAN;
}

static int compare_doubles(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* EBU Tech 3342: the spread between the 10th and 95th percentiles of the gated 3 s blocks. */
static double loudness_range(const mc_loudness *l) {
  if (!l->terms.count) return NAN;
  size_t kept;
  double ungated = gated_mean(&l->terms, ABS_GATE, &kept);
  if (!kept) return 0.0; /* long enough, but silent */
  double gate = to_lufs(ungated) + TERM_GATE;
  if (gate < ABS_GATE) gate = ABS_GATE;
  double *v = malloc(kept * sizeof *v);
  if (!v) return NAN;
  size_t n = 0;
  for (size_t i = 0; i < l->terms.count; i++)
    if (l->terms.v[i] > 0.0 && to_lufs(l->terms.v[i]) > gate) v[n++] = to_lufs(l->terms.v[i]);
  double range = 0.0;
  if (n) {
    qsort(v, n, sizeof *v, compare_doubles);
    range = v[(size_t)llround((double)(n - 1) * HIGH_PERCENTILE)] -
            v[(size_t)llround((double)(n - 1) * LOW_PERCENTILE)];
  }
  free(v);
  return range;
}

void mc_loudness_finish(mc_loudness *l, double *lufs, double *lra, double *peak) {
  *lufs = l->failed ? NAN : integrated(l);
  *lra = l->failed ? NAN : loudness_range(l);
  *peak = l->peak > 0.0f ? 20.0 * log10(l->peak) : NAN;
}
