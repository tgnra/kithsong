/* Art colours and the backdrop's glows (artcolors.h): Android's ArtColors and glowColors()
 * (android/.../ui/theme/ArtTheme.kt and ArtBackdrop.kt), on Material's colour science.
 *
 * The colour science (HCT on CAM16, the Wu and WSMeans quantizers and Score) is ported from
 * Google's material-color-utilities (https://github.com/material-foundation/
 * material-color-utilities, Java), Copyright 2021-2022 Google LLC, under the Apache License
 * 2.0 (http://www.apache.org/licenses/LICENSE-2.0; see LICENSES.md), which Android reaches
 * through MaterialKolor's Kotlin port. Changed from the original: written in C, in one file;
 * WSMeans looks at every cluster for each point rather than skipping by the clusters'
 * distances to each other (the same nearest cluster, without its matrices); pixels that
 * aren't opaque are left out by both quantizers; and HCT's solver (a colour from hue, chroma
 * and tone) is a bisection on CAM16's J and chroma rather than the original's closed form,
 * which agrees to within rounding for the toned tints this needs. */
#include "kithcore/artcolors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- Colour utilities --- */

static const double SRGB_TO_XYZ[3][3] = {
    {0.41233895, 0.35762064, 0.18051042},
    {0.2126, 0.7152, 0.0722},
    {0.01932141, 0.11916382, 0.95034478},
};
static const double XYZ_TO_SRGB[3][3] = {
    {3.2413774792388685, -1.5376652402851851, -0.49885366846268053},
    {-0.9691452513005321, 1.8758853451067872, 0.04156585616912061},
    {0.05562093689691305, -0.20395524564742123, 1.0571799111220335},
};
static const double WHITE_D65[3] = {95.047, 100.0, 108.883};

static double sanitize_degrees(double d) {
  d = fmod(d, 360.0);
  return d < 0 ? d + 360.0 : d;
}
static int sanitize_degrees_int(int d) {
  d %= 360;
  return d < 0 ? d + 360 : d;
}
static double difference_degrees(double a, double b) {
  return 180.0 - fabs(fabs(a - b) - 180.0);
}

/* An sRGB component (0-255) as linear light, 0-100. */
static double linearized(int c) {
  double n = c / 255.0;
  return n <= 0.040449936 ? n / 12.92 * 100.0 : pow((n + 0.055) / 1.055, 2.4) * 100.0;
}
static int delinearized(double c) {
  double n = c / 100.0;
  double d = n <= 0.0031308 ? n * 12.92 : 1.055 * pow(n, 1.0 / 2.4) - 0.055;
  int v = (int)lround(d * 255.0);
  return v < 0 ? 0 : v > 255 ? 255 : v;
}
static uint32_t argb_from_rgb(int r, int g, int b) {
  return 0xFF000000u | ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
}
static int red_of(uint32_t c) { return (int)(c >> 16 & 255); }
static int green_of(uint32_t c) { return (int)(c >> 8 & 255); }
static int blue_of(uint32_t c) { return (int)(c & 255); }

static void xyz_from_argb(uint32_t c, double xyz[3]) {
  double r = linearized(red_of(c)), g = linearized(green_of(c)), b = linearized(blue_of(c));
  for (int i = 0; i < 3; i++) xyz[i] = SRGB_TO_XYZ[i][0] * r + SRGB_TO_XYZ[i][1] * g + SRGB_TO_XYZ[i][2] * b;
}
static void linear_rgb_from_xyz(const double xyz[3], double rgb[3]) {
  for (int i = 0; i < 3; i++)
    rgb[i] = XYZ_TO_SRGB[i][0] * xyz[0] + XYZ_TO_SRGB[i][1] * xyz[1] + XYZ_TO_SRGB[i][2] * xyz[2];
}
static uint32_t argb_from_xyz(const double xyz[3]) {
  double rgb[3];
  linear_rgb_from_xyz(xyz, rgb);
  return argb_from_rgb(delinearized(rgb[0]), delinearized(rgb[1]), delinearized(rgb[2]));
}

static double lab_f(double t) {
  const double e = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
  return t > e ? cbrt(t) : (kappa * t + 16.0) / 116.0;
}
static double lab_invf(double ft) {
  const double e = 216.0 / 24389.0, kappa = 24389.0 / 27.0;
  double ft3 = ft * ft * ft;
  return ft3 > e ? ft3 : (116.0 * ft - 16.0) / kappa;
}
static double y_from_lstar(double lstar) { return 100.0 * lab_invf((lstar + 16.0) / 116.0); }
static double lstar_from_y(double y) { return lab_f(y / 100.0) * 116.0 - 16.0; }
static double lstar_from_argb(uint32_t c) {
  double xyz[3];
  xyz_from_argb(c, xyz);
  return lstar_from_y(xyz[1]);
}
static uint32_t argb_from_lstar(double lstar) {
  int v = delinearized(y_from_lstar(lstar));
  return argb_from_rgb(v, v, v);
}

static void lab_from_argb(uint32_t c, double lab[3]) {
  double xyz[3];
  xyz_from_argb(c, xyz);
  double fx = lab_f(xyz[0] / WHITE_D65[0]), fy = lab_f(xyz[1] / WHITE_D65[1]), fz = lab_f(xyz[2] / WHITE_D65[2]);
  lab[0] = 116.0 * fy - 16.0;
  lab[1] = 500.0 * (fx - fy);
  lab[2] = 200.0 * (fy - fz);
}
static uint32_t argb_from_lab(const double lab[3]) {
  double fy = (lab[0] + 16.0) / 116.0, fx = lab[1] / 500.0 + fy, fz = fy - lab[2] / 200.0;
  double xyz[3] = {lab_invf(fx) * WHITE_D65[0], lab_invf(fy) * WHITE_D65[1], lab_invf(fz) * WHITE_D65[2]};
  return argb_from_xyz(xyz);
}

/* --- CAM16, in the default viewing conditions (an sRGB display in average surroundings) --- */

typedef struct viewing {
  double n, aw, nbb, ncb, c, nc, fl, fl_root, z;
  double rgb_d[3];
} viewing;

static viewing default_viewing(void) {
  static const double M[3][3] = {
      {0.401288, 0.650173, -0.051461},
      {-0.250268, 1.204414, 0.045854},
      {-0.002079, 0.048952, 0.953127},
  };
  viewing v;
  double la = (200.0 / M_PI) * y_from_lstar(50.0) / 100.0, background_lstar = 50.0, surround = 2.0;
  double rgb_w[3];
  for (int i = 0; i < 3; i++) rgb_w[i] = M[i][0] * WHITE_D65[0] + M[i][1] * WHITE_D65[1] + M[i][2] * WHITE_D65[2];
  double f = 0.8 + surround / 10.0;
  v.c = f >= 0.9 ? 0.59 + (0.69 - 0.59) * ((f - 0.9) * 10.0) : 0.525 + (0.59 - 0.525) * ((f - 0.8) * 10.0);
  double d = f * (1.0 - (1.0 / 3.6) * exp((-la - 42.0) / 92.0));
  d = d > 1.0 ? 1.0 : d < 0.0 ? 0.0 : d;
  v.nc = f;
  for (int i = 0; i < 3; i++) v.rgb_d[i] = d * (100.0 / rgb_w[i]) + 1.0 - d;
  double k = 1.0 / (5.0 * la + 1.0), k4 = k * k * k * k, k4f = 1.0 - k4;
  v.fl = k4 * la + 0.1 * k4f * k4f * cbrt(5.0 * la);
  v.n = y_from_lstar(background_lstar) / WHITE_D65[1];
  v.z = 1.48 + sqrt(v.n);
  v.nbb = 0.725 / pow(v.n, 0.2);
  v.ncb = v.nbb;
  double rgb_a[3];
  for (int i = 0; i < 3; i++) {
    double af = pow(v.fl * v.rgb_d[i] * rgb_w[i] / 100.0, 0.42);
    rgb_a[i] = 400.0 * af / (af + 27.13);
  }
  v.aw = (2.0 * rgb_a[0] + rgb_a[1] + 0.05 * rgb_a[2]) * v.nbb;
  v.fl_root = pow(v.fl, 0.25);
  return v;
}

/* A colour's hue (degrees), chroma and tone (L*). */
typedef struct hct {
  double hue, chroma, tone;
} hct;

static hct hct_from_argb(const viewing *v, uint32_t argb) {
  double xyz[3];
  xyz_from_argb(argb, xyz);
  double x = xyz[0], y = xyz[1], z = xyz[2];
  double rc = 0.401288 * x + 0.650173 * y - 0.051461 * z;
  double gc = -0.250268 * x + 1.204414 * y + 0.045854 * z;
  double bc = -0.002079 * x + 0.048952 * y + 0.953127 * z;
  double cone[3] = {v->rgb_d[0] * rc, v->rgb_d[1] * gc, v->rgb_d[2] * bc}, adapted[3];
  for (int i = 0; i < 3; i++) {
    double af = pow(v->fl * fabs(cone[i]) / 100.0, 0.42);
    adapted[i] = (cone[i] < 0 ? -1.0 : cone[i] > 0 ? 1.0 : 0.0) * 400.0 * af / (af + 27.13);
  }
  double ra = adapted[0], ga = adapted[1], ba = adapted[2];
  double a = (11.0 * ra + -12.0 * ga + ba) / 11.0;
  double b = (ra + ga - 2.0 * ba) / 9.0;
  double u = (20.0 * ra + 20.0 * ga + 21.0 * ba) / 20.0;
  double p2 = (40.0 * ra + 20.0 * ga + ba) / 20.0;
  double hue = atan2(b, a) * 180.0 / M_PI;
  hue = hue < 0 ? hue + 360.0 : hue >= 360.0 ? hue - 360.0 : hue;
  double ac = p2 * v->nbb;
  double j = 100.0 * pow(ac / v->aw, v->c * v->z);
  double hue_prime = hue < 20.14 ? hue + 360.0 : hue;
  double e_hue = 0.25 * (cos(hue_prime * M_PI / 180.0 + 2.0) + 3.8);
  double p1 = 50000.0 / 13.0 * e_hue * v->nc * v->ncb;
  double t = p1 * hypot(a, b) / (u + 0.305);
  double alpha = pow(1.64 - pow(0.29, v->n), 0.73) * pow(t, 0.9);
  hct out = {hue, alpha * sqrt(j / 100.0), lstar_from_argb(argb)};
  return out;
}

/* CAM16 j, chroma, hue back to XYZ. */
static void xyz_from_jch(const viewing *v, double j, double chroma, double hue, double xyz[3]) {
  double alpha = chroma == 0.0 || j == 0.0 ? 0.0 : chroma / sqrt(j / 100.0);
  double t = pow(alpha / pow(1.64 - pow(0.29, v->n), 0.73), 1.0 / 0.9);
  double h_rad = hue * M_PI / 180.0;
  double e_hue = 0.25 * (cos(h_rad + 2.0) + 3.8);
  double ac = v->aw * pow(j / 100.0, 1.0 / v->c / v->z);
  double p1 = e_hue * (50000.0 / 13.0) * v->nc * v->ncb;
  double p2 = ac / v->nbb;
  double h_sin = sin(h_rad), h_cos = cos(h_rad);
  double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * h_cos + 108.0 * t * h_sin);
  double a = gamma * h_cos, b = gamma * h_sin;
  double adapted[3] = {
      (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0,
      (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0,
      (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0,
  };
  double f[3];
  for (int i = 0; i < 3; i++) {
    double base = fmax(0.0, 27.13 * fabs(adapted[i]) / (400.0 - fabs(adapted[i])));
    double cone = (adapted[i] < 0 ? -1.0 : adapted[i] > 0 ? 1.0 : 0.0) * (100.0 / v->fl) * pow(base, 1.0 / 0.42);
    f[i] = cone / v->rgb_d[i];
  }
  xyz[0] = 1.86206786 * f[0] - 1.01125463 * f[1] + 0.14918677 * f[2];
  xyz[1] = 0.38752654 * f[0] + 0.62144744 * f[1] - 0.00897398 * f[2];
  xyz[2] = -0.01584150 * f[0] - 0.03412294 * f[1] + 1.04996444 * f[2];
}

/* The J at chroma and hue whose Y is y, and whether that colour is in sRGB's gamut. Y rises
 * with J at a fixed chroma and hue, so a bisection finds it. */
static int solve_j(const viewing *v, double chroma, double hue, double y, double xyz[3]) {
  double lo = 0.0, hi = 100.0;
  for (int i = 0; i < 40; i++) {
    double mid = (lo + hi) / 2.0;
    xyz_from_jch(v, mid, chroma, hue, xyz);
    if (xyz[1] < y) lo = mid;
    else hi = mid;
  }
  xyz_from_jch(v, (lo + hi) / 2.0, chroma, hue, xyz);
  double rgb[3];
  linear_rgb_from_xyz(xyz, rgb);
  for (int i = 0; i < 3; i++)
    if (rgb[i] < -0.01 || rgb[i] > 100.01 || rgb[i] != rgb[i]) return 0;
  return 1;
}

/* The colour of hue and tone with the most chroma up to chroma that sRGB can show. */
static uint32_t argb_from_hct(const viewing *v, double hue, double chroma, double tone) {
  if (chroma < 0.0001 || tone < 0.0001 || tone > 99.9999) return argb_from_lstar(tone);
  hue = sanitize_degrees(hue);
  double y = y_from_lstar(tone), xyz[3];
  if (solve_j(v, chroma, hue, y, xyz)) return argb_from_xyz(xyz);
  double lo = 0.0, hi = chroma, best[3];
  solve_j(v, 0.0, hue, y, best);
  for (int i = 0; i < 30; i++) {
    double mid = (lo + hi) / 2.0;
    if (solve_j(v, mid, hue, y, xyz)) {
      lo = mid;
      memcpy(best, xyz, sizeof best);
    } else {
      hi = mid;
    }
  }
  return argb_from_xyz(best);
}

/* --- Quantizers --- */

enum { WU_BITS = 5, WU_SIDE = 33, WU_TOTAL = WU_SIDE * WU_SIDE * WU_SIDE };

static int wu_index(int r, int g, int b) { return (r << (WU_BITS * 2)) + (r << (WU_BITS + 1)) + r + (g << WU_BITS) + g + b; }

typedef struct wu_box {
  int r0, r1, g0, g1, b0, b1, vol;
} wu_box;

typedef struct wu {
  double *weights, *mr, *mg, *mb, *moments;
  wu_box *cubes;
} wu;

static double wu_volume(const wu_box *c, const double *m) {
  return m[wu_index(c->r1, c->g1, c->b1)] - m[wu_index(c->r1, c->g1, c->b0)] - m[wu_index(c->r1, c->g0, c->b1)] +
         m[wu_index(c->r1, c->g0, c->b0)] - m[wu_index(c->r0, c->g1, c->b1)] + m[wu_index(c->r0, c->g1, c->b0)] +
         m[wu_index(c->r0, c->g0, c->b1)] - m[wu_index(c->r0, c->g0, c->b0)];
}

enum { RED, GREEN, BLUE };

static double wu_bottom(const wu_box *c, int dir, const double *m) {
  switch (dir) {
    case RED:
      return -m[wu_index(c->r0, c->g1, c->b1)] + m[wu_index(c->r0, c->g1, c->b0)] + m[wu_index(c->r0, c->g0, c->b1)] -
             m[wu_index(c->r0, c->g0, c->b0)];
    case GREEN:
      return -m[wu_index(c->r1, c->g0, c->b1)] + m[wu_index(c->r1, c->g0, c->b0)] + m[wu_index(c->r0, c->g0, c->b1)] -
             m[wu_index(c->r0, c->g0, c->b0)];
    default:
      return -m[wu_index(c->r1, c->g1, c->b0)] + m[wu_index(c->r1, c->g0, c->b0)] + m[wu_index(c->r0, c->g1, c->b0)] -
             m[wu_index(c->r0, c->g0, c->b0)];
  }
}

static double wu_top(const wu_box *c, int dir, int p, const double *m) {
  switch (dir) {
    case RED:
      return m[wu_index(p, c->g1, c->b1)] - m[wu_index(p, c->g1, c->b0)] - m[wu_index(p, c->g0, c->b1)] +
             m[wu_index(p, c->g0, c->b0)];
    case GREEN:
      return m[wu_index(c->r1, p, c->b1)] - m[wu_index(c->r1, p, c->b0)] - m[wu_index(c->r0, p, c->b1)] +
             m[wu_index(c->r0, p, c->b0)];
    default:
      return m[wu_index(c->r1, c->g1, p)] - m[wu_index(c->r1, c->g0, p)] - m[wu_index(c->r0, c->g1, p)] +
             m[wu_index(c->r0, c->g0, p)];
  }
}

static double wu_variance(const wu *w, const wu_box *c) {
  double dr = wu_volume(c, w->mr), dg = wu_volume(c, w->mg), db = wu_volume(c, w->mb);
  double xx = wu_volume(c, w->moments);
  return xx - (dr * dr + dg * dg + db * db) / wu_volume(c, w->weights);
}

static double wu_maximize(const wu *w, const wu_box *c, int dir, int first, int last, const double whole[4], int *cut) {
  double br = wu_bottom(c, dir, w->mr), bg = wu_bottom(c, dir, w->mg), bb = wu_bottom(c, dir, w->mb);
  double bw = wu_bottom(c, dir, w->weights);
  double max = 0.0;
  *cut = -1;
  for (int i = first; i < last; i++) {
    double hr = br + wu_top(c, dir, i, w->mr), hg = bg + wu_top(c, dir, i, w->mg);
    double hb = bb + wu_top(c, dir, i, w->mb), hw = bw + wu_top(c, dir, i, w->weights);
    if (hw == 0) continue;
    double temp = (hr * hr + hg * hg + hb * hb) / hw;
    hr = whole[0] - hr;
    hg = whole[1] - hg;
    hb = whole[2] - hb;
    hw = whole[3] - hw;
    if (hw == 0) continue;
    temp += (hr * hr + hg * hg + hb * hb) / hw;
    if (temp > max) {
      max = temp;
      *cut = i;
    }
  }
  return max;
}

static int wu_cut(const wu *w, wu_box *one, wu_box *two) {
  double whole[4] = {wu_volume(one, w->mr), wu_volume(one, w->mg), wu_volume(one, w->mb), wu_volume(one, w->weights)};
  int cut_r, cut_g, cut_b, dir;
  double max_r = wu_maximize(w, one, RED, one->r0 + 1, one->r1, whole, &cut_r);
  double max_g = wu_maximize(w, one, GREEN, one->g0 + 1, one->g1, whole, &cut_g);
  double max_b = wu_maximize(w, one, BLUE, one->b0 + 1, one->b1, whole, &cut_b);
  if (max_r >= max_g && max_r >= max_b) {
    if (cut_r < 0) return 0;
    dir = RED;
  } else if (max_g >= max_r && max_g >= max_b) {
    dir = GREEN;
  } else {
    dir = BLUE;
  }
  two->r1 = one->r1;
  two->g1 = one->g1;
  two->b1 = one->b1;
  if (dir == RED) {
    one->r1 = cut_r;
    two->r0 = one->r1;
    two->g0 = one->g0;
    two->b0 = one->b0;
  } else if (dir == GREEN) {
    one->g1 = cut_g;
    two->r0 = one->r0;
    two->g0 = one->g1;
    two->b0 = one->b0;
  } else {
    one->b1 = cut_b;
    two->r0 = one->r0;
    two->g0 = one->g0;
    two->b0 = one->b1;
  }
  one->vol = (one->r1 - one->r0) * (one->g1 - one->g0) * (one->b1 - one->b0);
  two->vol = (two->r1 - two->r0) * (two->g1 - two->g0) * (two->b1 - two->b0);
  return 1;
}

/* Up to max colours by Wu's quantizer, into out; returns how many. */
static int quantize_wu(const uint32_t *pixels, const int *counts, int n, int max, uint32_t *out) {
  wu w = {0};
  w.weights = calloc(WU_TOTAL, sizeof(double));
  w.mr = calloc(WU_TOTAL, sizeof(double));
  w.mg = calloc(WU_TOTAL, sizeof(double));
  w.mb = calloc(WU_TOTAL, sizeof(double));
  w.moments = calloc(WU_TOTAL, sizeof(double));
  w.cubes = calloc((size_t)max, sizeof(wu_box));
  double *variance = calloc((size_t)max, sizeof(double));
  int found = 0;
  if (!w.weights || !w.mr || !w.mg || !w.mb || !w.moments || !w.cubes || !variance) goto done;

  /* The histogram. */
  for (int i = 0; i < n; i++) {
    int r = red_of(pixels[i]), g = green_of(pixels[i]), b = blue_of(pixels[i]);
    int index = wu_index((r >> 3) + 1, (g >> 3) + 1, (b >> 3) + 1);
    double count = counts[i];
    w.weights[index] += count;
    w.mr[index] += r * count;
    w.mg[index] += g * count;
    w.mb[index] += b * count;
    w.moments[index] += count * (r * r + g * g + b * b);
  }
  /* Its moments, cumulative. */
  for (int r = 1; r < WU_SIDE; r++) {
    double area[WU_SIDE] = {0}, area_r[WU_SIDE] = {0}, area_g[WU_SIDE] = {0}, area_b[WU_SIDE] = {0}, area2[WU_SIDE] = {0};
    for (int g = 1; g < WU_SIDE; g++) {
      double line = 0, line_r = 0, line_g = 0, line_b = 0, line2 = 0;
      for (int b = 1; b < WU_SIDE; b++) {
        int index = wu_index(r, g, b);
        line += w.weights[index];
        line_r += w.mr[index];
        line_g += w.mg[index];
        line_b += w.mb[index];
        line2 += w.moments[index];
        area[b] += line;
        area_r[b] += line_r;
        area_g[b] += line_g;
        area_b[b] += line_b;
        area2[b] += line2;
        int prev = wu_index(r - 1, g, b);
        w.weights[index] = w.weights[prev] + area[b];
        w.mr[index] = w.mr[prev] + area_r[b];
        w.mg[index] = w.mg[prev] + area_g[b];
        w.mb[index] = w.mb[prev] + area_b[b];
        w.moments[index] = w.moments[prev] + area2[b];
      }
    }
  }
  /* The boxes. */
  w.cubes[0].r1 = w.cubes[0].g1 = w.cubes[0].b1 = WU_SIDE - 1;
  int generated = max, next = 0;
  for (int i = 1; i < max; i++) {
    if (wu_cut(&w, &w.cubes[next], &w.cubes[i])) {
      variance[next] = w.cubes[next].vol > 1 ? wu_variance(&w, &w.cubes[next]) : 0.0;
      variance[i] = w.cubes[i].vol > 1 ? wu_variance(&w, &w.cubes[i]) : 0.0;
    } else {
      variance[next] = 0.0;
      i--;
    }
    next = 0;
    double temp = variance[0];
    for (int j = 1; j <= i; j++)
      if (variance[j] > temp) {
        temp = variance[j];
        next = j;
      }
    if (temp <= 0.0) {
      generated = i + 1;
      break;
    }
  }
  for (int i = 0; i < generated; i++) {
    double weight = wu_volume(&w.cubes[i], w.weights);
    if (weight <= 0) continue;
    int r = (int)(wu_volume(&w.cubes[i], w.mr) / weight), g = (int)(wu_volume(&w.cubes[i], w.mg) / weight);
    int b = (int)(wu_volume(&w.cubes[i], w.mb) / weight);
    out[found++] = argb_from_rgb(r, g, b);
  }
done:
  free(w.weights);
  free(w.mr);
  free(w.mg);
  free(w.mb);
  free(w.moments);
  free(w.cubes);
  free(variance);
  return found;
}

/* java.util.Random, for WSMeans' starting assignments to come out as the original's. */
typedef struct jrandom {
  uint64_t seed;
} jrandom;

static jrandom jrandom_new(uint64_t seed) {
  jrandom r = {(seed ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1)};
  return r;
}
static int32_t jrandom_next(jrandom *r, int bits) {
  r->seed = (r->seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
  return (int32_t)(r->seed >> (48 - bits));
}
static int jrandom_int(jrandom *r, int bound) {
  if ((bound & -bound) == bound) return (int)(((int64_t)bound * jrandom_next(r, 31)) >> 31);
  int32_t bits, val;
  do {
    bits = jrandom_next(r, 31);
    val = bits % bound;
  } while ((int64_t)bits - val + (bound - 1) > INT32_MAX);
  return val;
}

static double lab_distance(const double a[3], const double b[3]) {
  double d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
  return d0 * d0 + d1 * d1 + d2 * d2;
}

/* WSMeans from the Wu colours start, over the n distinct pixels (with their counts): the
 * colours and how many pixels each has, into out and out_counts; returns how many. */
static int quantize_wsmeans(const uint32_t *pixels, const int *counts, int n, const uint32_t *start, int nstart,
                            int max, uint32_t *out, int *out_counts) {
  int k = n < max ? n : max;
  if (nstart) k = k < nstart ? k : nstart;
  if (k <= 0) return 0;
  double(*points)[3] = malloc((size_t)n * sizeof *points);
  double(*clusters)[3] = calloc((size_t)k, sizeof *clusters);
  int *assigned = malloc((size_t)n * sizeof(int));
  double *sums = calloc((size_t)k * 3, sizeof(double));
  int *population = calloc((size_t)k, sizeof(int));
  int found = 0;
  if (!points || !clusters || !assigned || !sums || !population) goto done;
  for (int i = 0; i < n; i++) lab_from_argb(pixels[i], points[i]);
  for (int i = 0; i < k; i++) lab_from_argb(start[i], clusters[i]);
  jrandom random = jrandom_new(0x42688);
  for (int i = 0; i < n; i++) assigned[i] = jrandom_int(&random, k);

  for (int iteration = 0; iteration < 10; iteration++) {
    int moved = 0;
    for (int i = 0; i < n; i++) {
      double previous = lab_distance(points[i], clusters[assigned[i]]), nearest = previous;
      int to = -1;
      for (int j = 0; j < k; j++) {
        double d = lab_distance(points[i], clusters[j]);
        if (d < nearest) {
          nearest = d;
          to = j;
        }
      }
      if (to != -1 && fabs(sqrt(nearest) - sqrt(previous)) > 3.0) {
        moved++;
        assigned[i] = to;
      }
    }
    if (moved == 0 && iteration != 0) break;
    memset(sums, 0, (size_t)k * 3 * sizeof(double));
    memset(population, 0, (size_t)k * sizeof(int));
    for (int i = 0; i < n; i++) {
      int c = assigned[i];
      population[c] += counts[i];
      for (int d = 0; d < 3; d++) sums[c * 3 + d] += points[i][d] * counts[i];
    }
    for (int c = 0; c < k; c++)
      for (int d = 0; d < 3; d++) clusters[c][d] = population[c] ? sums[c * 3 + d] / population[c] : 0.0;
  }
  for (int c = 0; c < k; c++) {
    if (!population[c]) continue;
    uint32_t argb = argb_from_lab(clusters[c]);
    int dup = 0;
    for (int i = 0; i < found && !dup; i++) dup = out[i] == argb;
    if (dup) continue;
    out[found] = argb;
    out_counts[found++] = population[c];
  }
done:
  free(points);
  free(clusters);
  free(assigned);
  free(sums);
  free(population);
  return found;
}

/* --- Score --- */

typedef struct scored {
  uint32_t argb;
  hct hct;
  double score;
} scored;

/* Up to desired colours fit for a theme, from the quantized colours and their populations,
 * best first, as Score.score(..., filter = true) gives them; none without a fit one. */
static int score_colors(const uint32_t *colors, const hct *hcts, const int *population, int n, int desired, uint32_t *out) {
  double hue_population[360] = {0}, excited[360] = {0}, sum = 0;
  for (int i = 0; i < n; i++) {
    int hue = sanitize_degrees_int((int)floor(hcts[i].hue));
    hue_population[hue] += population[i];
    sum += population[i];
  }
  if (sum <= 0) return 0;
  for (int hue = 0; hue < 360; hue++) {
    double proportion = hue_population[hue] / sum;
    for (int i = hue - 14; i < hue + 16; i++) excited[sanitize_degrees_int(i)] += proportion;
  }
  scored *s = malloc((size_t)n * sizeof *s);
  if (!s) return 0;
  int ns = 0;
  for (int i = 0; i < n; i++) {
    double proportion = excited[sanitize_degrees_int((int)lround(hcts[i].hue))];
    if (hcts[i].chroma < 5.0 || proportion <= 0.01) continue;
    double chroma_weight = hcts[i].chroma < 48.0 ? 0.1 : 0.3;
    scored e = {colors[i], hcts[i], proportion * 100.0 * 0.7 + (hcts[i].chroma - 48.0) * chroma_weight};
    /* Insertion, so equal scores keep their order, as the original's stable sort does. */
    int at = ns;
    while (at > 0 && s[at - 1].score < e.score) {
      s[at] = s[at - 1];
      at--;
    }
    s[at] = e;
    ns++;
  }
  /* The best that are far enough apart in hue, starting a quarter turn apart and closing in
   * until there are enough. */
  int chosen = 0;
  double hues[MC_ART_COLORS_MAX];
  if (desired > MC_ART_COLORS_MAX) desired = MC_ART_COLORS_MAX;
  for (int apart = 90; apart >= 15; apart--) {
    chosen = 0;
    for (int i = 0; i < ns && chosen < desired; i++) {
      int dup = 0;
      for (int j = 0; j < chosen && !dup; j++) dup = difference_degrees(s[i].hct.hue, hues[j]) < apart;
      if (dup) continue;
      hues[chosen] = s[i].hct.hue;
      out[chosen++] = s[i].argb;
    }
    if (chosen >= desired) break;
  }
  free(s);
  return chosen;
}

/* --- Art colours --- */

/* Chroma below which a colour reads as grey rather than as a hue: Score's cut-off for a
 * colour to count at all, raised a little, since art's darks and lights are often barely
 * tinted. */
#define NEUTRAL_CHROMA 8.0
#define NEAR_HUE 30.0     /* how near in hue a pixel's colour is to count towards a colour's share */
#define NEUTRAL_SHARE 0.2 /* how much of the art has to be grey for a neutral colour */
#define QUANTIZE_MAX 128

int mc_art_colors(const uint8_t *rgba, int width, int height, size_t stride, mc_art_color out[MC_ART_COLORS_MAX]) {
  if (!rgba || !out || width <= 0 || height <= 0 || stride < (size_t)width * 4) return 0;
  /* The distinct opaque pixels and how many of each, in the order they first come. */
  size_t total = (size_t)width * (size_t)height;
  uint32_t *pixels = malloc(total * sizeof(uint32_t));
  int *counts = malloc(total * sizeof(int));
  size_t cap = 1;
  while (cap < total * 2) cap <<= 1;
  int32_t *table = malloc(cap * sizeof(int32_t)); /* open addressing: index into pixels, or -1 */
  uint32_t wu_colors[QUANTIZE_MAX], quantized[QUANTIZE_MAX];
  int quantized_counts[QUANTIZE_MAX];
  hct hcts[QUANTIZE_MAX];
  int found = 0;
  if (!pixels || !counts || !table) goto done;
  memset(table, 0xFF, cap * sizeof(int32_t));
  int n = 0;
  for (int y = 0; y < height; y++) {
    const uint8_t *row = rgba + (size_t)y * stride;
    for (int x = 0; x < width; x++) {
      const uint8_t *p = row + (size_t)x * 4;
      if (p[3] < 255) continue;
      uint32_t argb = argb_from_rgb(p[0], p[1], p[2]);
      size_t h = (argb * 2654435761u) & (cap - 1);
      while (table[h] >= 0 && pixels[table[h]] != argb) h = (h + 1) & (cap - 1);
      if (table[h] < 0) {
        table[h] = n;
        pixels[n] = argb;
        counts[n++] = 0;
      }
      counts[table[h]]++;
    }
  }
  if (!n) goto done;

  /* Celebi: Wu's colours as WSMeans' start. */
  int nwu = quantize_wu(pixels, counts, n, QUANTIZE_MAX, wu_colors);
  int nq = quantize_wsmeans(pixels, counts, n, wu_colors, nwu, QUANTIZE_MAX, quantized, quantized_counts);
  viewing v = default_viewing();
  for (int i = 0; i < nq; i++) hcts[i] = hct_from_argb(&v, quantized[i]);
  uint32_t best[4];
  int nbest = score_colors(quantized, hcts, quantized_counts, nq, 4, best);
  if (!nbest) goto done;

  /* Their shares: each quantized colour counts towards the scored one nearest its hue, if
   * one is near enough, and grey ones towards the neutral colour. */
  double hues[4], shares[4] = {0}, all = 0, neutral_count = 0;
  for (int i = 0; i < nbest; i++) hues[i] = hct_from_argb(&v, best[i]).hue;
  uint32_t neutral = 0;
  int neutral_most = 0, have_neutral = 0;
  for (int i = 0; i < nq; i++) {
    all += quantized_counts[i];
    if (hcts[i].chroma < NEUTRAL_CHROMA) {
      neutral_count += quantized_counts[i];
      if (quantized_counts[i] > neutral_most) {
        neutral = quantized[i];
        neutral_most = quantized_counts[i];
        have_neutral = 1;
      }
      continue;
    }
    int nearest = 0;
    for (int j = 1; j < nbest; j++)
      if (difference_degrees(hues[j], hcts[i].hue) < difference_degrees(hues[nearest], hcts[i].hue)) nearest = j;
    if (difference_degrees(hues[nearest], hcts[i].hue) <= NEAR_HUE) shares[nearest] += quantized_counts[i];
  }
  if (all < 1) all = 1;
  for (int i = 0; i < nbest; i++) {
    out[found].argb = best[i];
    out[found].share = (float)(shares[i] / all);
    out[found++].neutral = 0;
  }
  if (have_neutral && neutral_count / all >= NEUTRAL_SHARE) {
    out[found].argb = neutral;
    out[found].share = (float)(neutral_count / all);
    out[found++].neutral = 1;
  }
done:
  free(pixels);
  free(counts);
  free(table);
  return found;
}

/* --- Glows --- */

static const float CORNERS[MC_ART_GLOWS][2] = {{0.1f, 0.05f}, {0.9f, 0.2f}, {0.15f, 0.75f}, {0.9f, 0.95f}};

/* Tones that secondary text still reads on at well over 4.5:1, before the glows thin out
 * towards the surface. */
#define DARK_TONE 30.0
#define LIGHT_TONE 86.0
#define MAX_CHROMA 40.0   /* art's loudest colours still shout at those tones above this */
#define REPEAT_CHROMA 0.6 /* how much of its chroma a colour keeps each time it's used again */
#define SAME_HUE 25.0     /* hues closer than this count as one colour */
#define MAX_CORNERS 2     /* the most corners one colour gets while there are others */
#define MIN_SHARE 0.05f   /* the share of all the colours one needs to get a corner at all */
#define STRENGTH 0.8      /* the glows' alpha at their centres */

void mc_art_glow_center(int corner, float *x, float *y) {
  if (corner < 0 || corner >= MC_ART_GLOWS) corner = 0;
  if (x) *x = CORNERS[corner][0];
  if (y) *y = CORNERS[corner][1];
}

/* color at the tone the background uses, its chroma tamed so it stays a tint. repeat counts
 * how many times it's been used already: each time a little further from the text's tone and
 * quieter, so a colour with corners to itself doesn't shout. */
static uint32_t toned(const viewing *v, uint32_t color, int dark, int repeat) {
  hct h = hct_from_argb(v, color);
  double tone = dark ? DARK_TONE - repeat * 5.0 : LIGHT_TONE + repeat * 3.0;
  double chroma = (h.chroma < MAX_CHROMA ? h.chroma : MAX_CHROMA) * pow(REPEAT_CHROMA, repeat);
  return (argb_from_hct(v, h.hue, chroma, tone) & 0x00FFFFFFu) | ((uint32_t)lround(STRENGTH * 255.0) << 24);
}

typedef struct group {
  uint32_t color;
  double hue;
  int neutral;
  float share;
  int corners;
} group;

static float owed(const group *g, float whole) { return g->share / whole * MC_ART_GLOWS - (float)g->corners; }

int mc_art_glows(const mc_art_color *colors, int count, int dark, uint32_t out[MC_ART_GLOWS]) {
  if (!colors || count <= 0 || !out) return 0;
  viewing v = default_viewing();
  group *groups = calloc((size_t)count, sizeof *groups);
  if (!groups) return 0;
  int ngroups = 0;
  for (int i = 0; i < count; i++) {
    double hue = hct_from_argb(&v, colors[i].argb).hue;
    int neutral = colors[i].neutral, g = 0;
    while (g < ngroups &&
           !(groups[g].neutral == neutral && (neutral || difference_degrees(groups[g].hue, hue) < SAME_HUE)))
      g++;
    if (g == ngroups) {
      groups[g].color = colors[i].argb;
      groups[g].hue = hue;
      groups[g].neutral = neutral;
      ngroups++;
    }
    groups[g].share += colors[i].share;
  }
  /* Art whose colours are all too faint to have shares still has colours. */
  int any_share = 0;
  for (int g = 0; g < ngroups; g++) any_share |= groups[g].share != 0.0f;
  if (!any_share)
    for (int g = 0; g < ngroups; g++) groups[g].share = 1.0f;
  float whole = 0;
  int biggest = 0;
  for (int g = 0; g < ngroups; g++) {
    whole += groups[g].share;
    if (groups[g].share > groups[biggest].share) biggest = g;
  }
  /* Each corner to the colour furthest short of its share of them, if one is; otherwise to
   * the biggest, a hue rather than grey if there's one. Grey only gets a corner, and only
   * one, when there's more of it than of any hue, since it adds no colour. */
  for (int corner = 0; corner < MC_ART_GLOWS; corner++) {
    int pick = -1;
    for (int g = 0; g < ngroups; g++) {
      const group *gr = &groups[g];
      int may = gr->neutral ? g == biggest && gr->corners == 0
                : gr->corners == 0 ? gr->share / whole >= MIN_SHARE
                                   : gr->corners < MAX_CORNERS && owed(gr, whole) > 0.0f;
      if (may && (pick < 0 || owed(gr, whole) > owed(&groups[pick], whole))) pick = g;
    }
    if (pick < 0) {
      pick = 0;
      for (int g = 1; g < ngroups; g++) {
        const group *a = &groups[g], *b = &groups[pick];
        if (!a->neutral > !b->neutral || (!a->neutral == !b->neutral && a->share > b->share)) pick = g;
      }
    }
    groups[pick].corners++;
  }
  int filled[MC_ART_GLOWS] = {0};
  for (int g = 0; g < ngroups; g++) {
    int first = -1;
    for (int n = 0; n < groups[g].corners; n++) {
      int across = first < 0 ? -1 : MC_ART_GLOWS - 1 - first, corner = 0;
      if (across >= 0 && !filled[across]) corner = across;
      else
        while (filled[corner]) corner++;
      if (first < 0) first = corner;
      filled[corner] = 1;
      out[corner] = toned(&v, groups[g].color, dark, n);
    }
  }
  free(groups);
  return MC_ART_GLOWS;
}

/* --- Accent --- */

/* Tones Material's Content scheme gives a seed's primary colour, near which the accent
 * starts: behind the accent's white text, and as text on the theme's background. */
#define ACCENT_BG_TONE 40.0
#define ACCENT_LIGHT_TEXT_TONE 35.0
#define ACCENT_DARK_TEXT_TONE 75.0
#define ACCENT_CONTRAST 4.5 /* WCAG AA for body text */
/* The backgrounds accent text is checked against: the darkest light one (Adwaita's light
 * sidebar, about #EBEBED) and the lightest dark one (its dark sidebar, about #303034). */
#define LIGHT_BACKGROUND 0xFFEBEBEDu
#define DARK_BACKGROUND 0xFF303034u

static double luminance(uint32_t c) {
  double xyz[3];
  xyz_from_argb(c, xyz);
  return xyz[1] / 100.0;
}

static double contrast(uint32_t a, uint32_t b) {
  double la = luminance(a), lb = luminance(b);
  return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/* h's hue and chroma at the first tone from start, stepping by step, with ACCENT_CONTRAST
 * against against. */
static uint32_t accent_at(const viewing *v, hct h, double start, double step, uint32_t against) {
  uint32_t c = 0;
  for (double tone = start; tone >= 0.0 && tone <= 100.0; tone += step) {
    c = argb_from_hct(v, h.hue, h.chroma, tone);
    if (contrast(c, against) >= ACCENT_CONTRAST) break;
  }
  return c | 0xFF000000u;
}

int mc_art_accent(const mc_art_color *colors, int count, int dark, uint32_t *bg, uint32_t *text) {
  if (!colors || count <= 0) return 0;
  viewing v = default_viewing();
  for (int i = 0; i < count; i++) {
    if (colors[i].neutral) continue;
    hct h = hct_from_argb(&v, colors[i].argb);
    if (h.chroma < NEUTRAL_CHROMA) continue;
    if (bg) *bg = accent_at(&v, h, ACCENT_BG_TONE, -1.0, 0xFFFFFFFFu);
    if (text)
      *text = dark ? accent_at(&v, h, ACCENT_DARK_TEXT_TONE, 1.0, DARK_BACKGROUND)
                   : accent_at(&v, h, ACCENT_LIGHT_TEXT_TONE, -1.0, LIGHT_BACKGROUND);
    return 1;
  }
  return 0;
}
