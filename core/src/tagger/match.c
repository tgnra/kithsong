#include "tagger/match.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "util/text.h"

/* --- String distance --- */

/* Appends the comparable form of [s, end) to out; returns the new length. Letters and digits
 * are kept (folded, see mc_text_fold), everything else is dropped. */
static size_t normalize(const char *s, const char *end, uint32_t *out) {
  size_t n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (p < (const unsigned char *)end && *p) {
    int k = mc_text_fold(mc_utf8_next(&p), out + n);
    if (k > 0) n += (size_t)k;
  }
  return n;
}

static size_t levenshtein(const uint32_t *a, size_t n, const uint32_t *b, size_t m) {
  size_t *row = malloc((m + 1) * sizeof *row);
  if (!row) return n > m ? n : m;
  for (size_t j = 0; j <= m; j++) row[j] = j;
  for (size_t i = 1; i <= n; i++) {
    size_t diag = row[0];
    row[0] = i;
    for (size_t j = 1; j <= m; j++) {
      size_t up = row[j];
      size_t best = diag + (a[i - 1] != b[j - 1]);
      if (up + 1 < best) best = up + 1;
      if (row[j - 1] + 1 < best) best = row[j - 1] + 1;
      row[j] = best;
      diag = up;
    }
  }
  size_t d = row[m];
  free(row);
  return d;
}

/* Skips a leading "the ". */
static const char *skip_article(const char *s) {
  if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'H' || s[1] == 'h') &&
      (s[2] == 'E' || s[2] == 'e') && s[3] == ' ')
    return s + 4;
  return s;
}

/* Where a "(Remastered)", "[Live]" or " - 2011 Remaster" suffix starts, or the end. */
static const char *suffix_start(const char *s) {
  const char *end = s + strlen(s);
  for (const char *p = s + 1; *p; p++) {
    if (*p == '(' || *p == '[') return p;
    if (p[0] == ' ' && p[1] == '-' && p[2] == ' ') return p;
  }
  return end;
}

static double distance_range(const char *a, const char *a_end, const char *b, const char *b_end) {
  /* Each byte yields at most three code points ("&" -> "and"). */
  uint32_t *na = malloc(((size_t)(a_end - a) * 3 + 1) * sizeof *na);
  uint32_t *nb = malloc(((size_t)(b_end - b) * 3 + 1) * sizeof *nb);
  double d = 1.0;
  if (na && nb) {
    size_t n = normalize(a, a_end, na), m = normalize(b, b_end, nb);
    size_t longest = n > m ? n : m;
    if (longest == 0) {
      /* Nothing comparable left (all punctuation): fall back to exact comparison. */
      d = (a_end - a == b_end - b && memcmp(a, b, (size_t)(a_end - a)) == 0) ? 0.0 : 1.0;
    } else {
      d = (double)levenshtein(na, n, nb, m) / (double)longest;
    }
  }
  free(na);
  free(nb);
  return d;
}

double mc_string_distance(const char *a, const char *b) {
  if (!a || !b || !*a || !*b) return -1.0;
  a = skip_article(a);
  b = skip_article(b);
  const char *a_end = a + strlen(a), *b_end = b + strlen(b);
  double d = distance_range(a, a_end, b, b_end);

  /* "Song (2011 Remaster)" vs "Song": close, but not identical. */
  const char *as = suffix_start(a), *bs = suffix_start(b);
  if (d > 0 && (as != a_end || bs != b_end)) {
    double stripped = distance_range(a, as, b, bs) + 0.1;
    if (stripped < d) d = stripped;
  }
  return d;
}

/* --- Tracks --- */

#define LENGTH_GRACE_MS 10000
#define LENGTH_MAX_MS 30000

typedef struct weighted {
  double sum, weight;
} weighted;

/* Adds a component; a negative distance means unknown and is skipped. */
static void add(weighted *w, double distance, double weight) {
  if (distance < 0) return;
  w->sum += distance * weight;
  w->weight += weight;
}

static double length_distance(int a_ms, int b_ms) {
  if (a_ms <= 0 || b_ms <= 0) return -1.0;
  int diff = abs(a_ms - b_ms) - LENGTH_GRACE_MS;
  if (diff <= 0) return 0.0;
  return diff >= LENGTH_MAX_MS - LENGTH_GRACE_MS
           ? 1.0
           : (double)diff / (LENGTH_MAX_MS - LENGTH_GRACE_MS);
}

double mc_track_distance(const mc_tags *file, const mb_track *track, int abs_index) {
  weighted w = {0, 0};
  add(&w, mc_string_distance(file->title, track->title), 3.0);
  add(&w, length_distance(file->duration_ms, track->length_ms), 2.0);
  if (file->track > 0) {
    /* Multi-disc albums are numbered either per disc or straight through. */
    int same = (file->track == track->number && (!file->disc || file->disc == track->disc)) ||
               file->track == abs_index + 1;
    add(&w, same ? 0.0 : 1.0, 1.0);
  }
  add(&w, mc_string_distance(file->artist, track->artist), 1.0);
  return w.weight > 0 ? w.sum / w.weight : 0.5;
}

/* --- Assignment --- */

/* Hungarian algorithm with potentials, O(n^2 m), for n <= m. a is n x m, row-major.
 * col_of_row[i] receives the column for row i. */
static mc_status hungarian(const double *a, int n, int m, int *col_of_row) {
  size_t sz = (size_t)m + 1;
  double *u = calloc((size_t)n + 1, sizeof *u), *v = calloc(sz, sizeof *v);
  double *minv = malloc(sz * sizeof *minv);
  int *p = calloc(sz, sizeof *p), *way = calloc(sz, sizeof *way);
  char *used = malloc(sz);
  mc_status st = MC_ERR_NOMEM;
  if (!u || !v || !minv || !p || !way || !used) goto done;

  for (int i = 1; i <= n; i++) {
    p[0] = i;
    int j0 = 0;
    for (int j = 0; j <= m; j++) minv[j] = DBL_MAX, used[j] = 0;
    do {
      used[j0] = 1;
      int i0 = p[j0], j1 = 0;
      double delta = DBL_MAX;
      for (int j = 1; j <= m; j++) {
        if (used[j]) continue;
        double cur = a[(size_t)(i0 - 1) * (size_t)m + (size_t)(j - 1)] - u[i0] - v[j];
        if (cur < minv[j]) minv[j] = cur, way[j] = j0;
        if (minv[j] < delta) delta = minv[j], j1 = j;
      }
      for (int j = 0; j <= m; j++) {
        if (used[j]) u[p[j]] += delta, v[j] -= delta;
        else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      int j1 = way[j0];
      p[j0] = p[j1];
      j0 = j1;
    } while (j0);
  }
  for (int j = 1; j <= m; j++)
    if (p[j]) col_of_row[p[j] - 1] = j - 1;
  st = MC_OK;

done:
  free(u);
  free(v);
  free(minv);
  free(p);
  free(way);
  free(used);
  return st;
}

mc_status mc_assign(const double *cost, int rows, int cols, int *out) {
  for (int i = 0; i < rows; i++) out[i] = -1;
  if (rows == 0 || cols == 0) return MC_OK;
  if (rows <= cols) return hungarian(cost, rows, cols, out);

  /* More rows than columns: solve the transpose, then invert the result. */
  double *t = malloc((size_t)rows * (size_t)cols * sizeof *t);
  int *row_of_col = malloc((size_t)cols * sizeof *row_of_col);
  mc_status st = MC_ERR_NOMEM;
  if (t && row_of_col) {
    for (int i = 0; i < rows; i++)
      for (int j = 0; j < cols; j++)
        t[(size_t)j * (size_t)rows + (size_t)i] = cost[(size_t)i * (size_t)cols + (size_t)j];
    st = hungarian(t, cols, rows, row_of_col);
    if (st == MC_OK)
      for (int j = 0; j < cols; j++) out[row_of_col[j]] = j;
  }
  free(t);
  free(row_of_col);
  return st;
}

/* --- Releases --- */

/* Above this, a file and a track are treated as unrelated even if the assignment paired them,
 * so a wrong title is never written to a file just to fill a slot. */
#define TRACK_MATCH_MAX 0.6

static int is_various(const char *artist) {
  return artist && mc_string_distance(artist, "Various Artists") == 0.0;
}

/* Most common non-zero year among the files, or 0. */
static int files_year(const mc_tags *files, int n) {
  int best = 0, best_count = 0;
  for (int i = 0; i < n; i++) {
    if (!files[i].year) continue;
    int count = 0;
    for (int j = 0; j < n; j++) count += files[j].year == files[i].year;
    if (count > best_count) best = files[i].year, best_count = count;
  }
  return best;
}

static double year_distance(int file_year, const mb_release *rel) {
  if (!file_year || (!rel->year && !rel->original_year)) return -1.0;
  if (file_year == rel->year || file_year == rel->original_year) return 0.0;
  int diff = abs(file_year - (rel->original_year ? rel->original_year : rel->year));
  return diff >= 10 ? 1.0 : diff / 10.0;
}

mc_status mc_score_release(const mb_release *rel, const mc_tags *files, int file_count,
                           const char *album, const char *artist, int *map,
                           mc_release_score *out) {
  int n = file_count, m = rel->track_count;
  out->distance = 1.0;
  out->missing = m;
  out->extra = n;
  for (int i = 0; i < n; i++) map[i] = -1;
  if (n == 0 || m == 0) return MC_OK;

  double *cost = malloc((size_t)n * (size_t)m * sizeof *cost);
  if (!cost) return MC_ERR_NOMEM;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < m; j++)
      cost[(size_t)i * (size_t)m + (size_t)j] = mc_track_distance(&files[i], &rel->tracks[j], j);
  mc_status st = mc_assign(cost, n, m, map);
  if (st != MC_OK) {
    free(cost);
    return st;
  }

  int matched = 0;
  double track_sum = 0;
  for (int i = 0; i < n; i++) {
    if (map[i] < 0) continue;
    double d = cost[(size_t)i * (size_t)m + (size_t)map[i]];
    if (d > TRACK_MATCH_MAX) {
      map[i] = -1;
      continue;
    }
    track_sum += d;
    matched++;
  }
  free(cost);

  out->missing = m - matched;
  out->extra = n - matched;

  weighted w = {0, 0};
  /* Heavy enough that a wrong title or artist alone drops below the default threshold. */
  add(&w, mc_string_distance(album, rel->title), 5.0);
  if (!(is_various(rel->artist) && (!artist || is_various(artist))))
    add(&w, mc_string_distance(artist, rel->artist), 5.0);
  add(&w, matched ? track_sum / matched : 1.0, 6.0);
  add(&w, (double)out->missing / m, 10.0);
  add(&w, (double)out->extra / n, 6.0);
  add(&w, year_distance(files_year(files, n), rel), 0.5);
  out->distance = w.sum / w.weight;
  return MC_OK;
}

double mc_score_recording(const mb_release *rec, const mc_tags *file) {
  if (rec->track_count < 1) return 1.0;
  const mb_track *t = &rec->tracks[0];
  weighted w = {0, 0};
  double title = mc_string_distance(file->title, t->title);
  add(&w, title < 0 ? 1.0 : title, 3.0);
  add(&w, mc_string_distance(file->artist, t->artist), 3.0);
  add(&w, length_distance(file->duration_ms, t->length_ms), 2.0);
  return w.sum / w.weight;
}
