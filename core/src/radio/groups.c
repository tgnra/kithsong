/* See groups.h, and "Radio stations" in docs/schema.md for the reasoning.
 *
 * It takes four steps: leave out the tracks too alone to say anything, divide what's left into
 * two groups, three and so on with k-medoids, keep the count that divides it most clearly, then
 * give every track of the library to the nearest centre. The first three work on a sample, so
 * what they cost doesn't grow with the library; the last is one pass over all of it. */
#include "radio/groups.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

enum {
  SAMPLE = 1024,  /* tracks compared with each other: the matrix is 4 MB */
  NEIGHBOURS = 8, /* how a track's company is measured: the distance to this many */
  ROUNDS = 12,    /* of k-medoids, which has settled long before this on anything real */
  /* A group smaller than this isn't a station: about an album, which it'd soon run out of. */
  MIN_TRACKS = 12,
  /* Nor is one smaller than 1 / this of the library, an odd album in a big one being no kind of
   * music of its own. */
  MIN_SHARE = 50,
  MOST_TRIED = 32 /* the most groups a library is ever divided into, to see */
};

/* A track further from its neighbours than this many times the usual is too alone to tell
 * anything: real libraries have a few, an intro or a noise track, and being far from everything
 * they'd each pull a group to themselves. */
#define LONELY 1.5
/* But this near, on the library's scales, is company whatever's usual: otherwise a loose genre
 * beside a tight one would be left out whole. */
#define COMPANY 0.7

/* Two centres closer than this aren't different kinds of music. It's on the fixed scales, as the
 * library's own put any library's tracks about 1 apart, however alike they all sound. In a real
 * library of 1,026 tracks two tracks of an album were 0.51 apart at the median and any two 0.68, so
 * this asks for about what two tracks picked at random from a varied library have. */
#define MIN_APART 0.6

/* How clearly a library must divide to count as more than one kind, as a mean silhouette: for each
 * track, how much nearer it is to its own group than to the next, from 0 for no nearer to 1. In a
 * real library of two plain kinds it was 0.25, and each kind alone divided at 0.12 and 0.17. */
#define MIN_SILHOUETTE 0.2

/* A track further than this share of the way to the next centre sits between two groups. */
#define BETWEEN 0.8

typedef struct by_hash {
  unsigned hash;
  int index;
} by_hash;

static int by_hash_cmp(const void *a, const void *b) {
  const by_hash *x = a, *y = b;
  if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
  return x->index - y->index;
}

/* Up to SAMPLE results by hash, into sample. How many, 0 if out of memory. */
static int draw_sample(const unsigned *hashes, int count, int *sample) {
  by_hash *order = malloc((size_t)count * sizeof *order);
  if (!order) return 0;
  for (int i = 0; i < count; i++) order[i] = (by_hash){hashes[i], i};
  qsort(order, (size_t)count, sizeof *order, by_hash_cmp);
  int n = count < SAMPLE ? count : SAMPLE;
  for (int i = 0; i < n; i++) sample[i] = order[i].index;
  free(order);
  return n;
}

static int float_cmp(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

/* Sets company[i] for each track of the sample that has some, from the distances between them
 * (n by n). tracks is the size of what it's a sample of. */
static mc_status find_company(const float *dist, int n, int tracks, char *company) {
  /* In a sample as thin as this, the same neighbours are nearer the front of the queue. Below
   * the third it says little. */
  int nth = (int)((int64_t)NEIGHBOURS * n / tracks);
  if (nth < 3) nth = 3;
  if (nth > n - 1) nth = n - 1;
  float *row = malloc((size_t)n * sizeof *row), *nearest = malloc((size_t)n * sizeof *nearest);
  if (!row || !nearest) {
    free(row);
    free(nearest);
    return MC_ERR_NOMEM;
  }
  for (int i = 0; i < n; i++) {
    memcpy(row, &dist[(size_t)i * n], (size_t)n * sizeof *row);
    qsort(row, (size_t)n, sizeof *row, float_cmp);
    nearest[i] = row[nth]; /* row[0] is the track itself */
  }
  memcpy(row, nearest, (size_t)n * sizeof *row);
  qsort(row, (size_t)n, sizeof *row, float_cmp);
  float limit = row[n / 2] * (float)LONELY;
  if (limit < (float)COMPANY) limit = (float)COMPANY;
  for (int i = 0; i < n; i++) company[i] = nearest[i] <= limit;
  free(row);
  free(nearest);
  return MC_OK;
}

/* What's being divided: m tracks, with the distances between them, m by m. */
typedef struct field {
  int m;
  const int *members; /* each one's index among the results */
  const float *dist;
  int *labels;        /* each one's group */
  float *from;        /* room for m */
} field;

static float dist_of(const field *f, int a, int b) {
  return f->dist[(size_t)a * f->m + b];
}

/* The member of a group with the least total distance to the rest of it, group -1 being
 * everyone. -1 if the group is empty. */
static int central(const field *f, int group) {
  int best = -1;
  double best_total = DBL_MAX;
  for (int i = 0; i < f->m; i++) {
    if (group >= 0 && f->labels[i] != group) continue;
    double total = 0;
    for (int k = 0; k < f->m; k++)
      if (group < 0 || f->labels[k] == group) total += dist_of(f, i, k);
    if (total < best_total) {
      best_total = total;
      best = i;
    }
  }
  return best;
}

/* Farthest-point selection, for somewhere to start from: count members spread as far apart as
 * they go, the most central of all first. It's one list for every count, each adding to the last. */
static void spread(const field *f, int count, int *chosen) {
  chosen[0] = central(f, -1);
  for (int i = 0; i < f->m; i++) f->from[i] = FLT_MAX;
  for (int c = 1; c < count; c++) {
    int far = 0;
    for (int i = 0; i < f->m; i++) {
      float d = dist_of(f, i, chosen[c - 1]);
      if (d < f->from[i]) f->from[i] = d;
      if (f->from[i] > f->from[far]) far = i;
    }
    chosen[c] = far;
  }
}

static void assign(const field *f, const int *centres, int count) {
  for (int i = 0; i < f->m; i++) {
    int best = 0;
    for (int g = 1; g < count; g++)
      if (dist_of(f, i, centres[g]) < dist_of(f, i, centres[best])) best = g;
    f->labels[i] = best;
  }
}

/* k-medoids from what's in centres: each group's most central member becomes its centre, until
 * that changes nothing. It works from distances alone, which is all there is to go on, and a
 * centre is a real track. */
static void divide(const field *f, int *centres, int count) {
  for (int round = 0; round < ROUNDS; round++) {
    int moved = 0;
    assign(f, centres, count);
    for (int g = 0; g < count; g++) {
      int c = central(f, g);
      if (c >= 0 && c != centres[g]) {
        centres[g] = c;
        moved = 1;
      }
    }
    if (!moved) break;
  }
  assign(f, centres, count);
}

/* The mean silhouette of the division in f->labels. */
static double silhouette(const field *f, int count) {
  int sizes[MOST_TRIED] = {0};
  for (int i = 0; i < f->m; i++) sizes[f->labels[i]]++;
  double total = 0;
  for (int i = 0; i < f->m; i++) {
    double sums[MOST_TRIED] = {0};
    for (int k = 0; k < f->m; k++) sums[f->labels[k]] += dist_of(f, i, k);
    int own = f->labels[i];
    if (sizes[own] < 2) continue; /* a group of one says nothing, and counts as 0 */
    double inside = sums[own] / (sizes[own] - 1), outside = DBL_MAX;
    for (int g = 0; g < count; g++)
      if (g != own && sizes[g] && sums[g] / sizes[g] < outside) outside = sums[g] / sizes[g];
    double most = inside > outside ? inside : outside;
    if (outside < DBL_MAX && most > 0) total += (outside - inside) / most;
  }
  return total / f->m;
}

/* The fewest tracks a group of a library that size may have. */
static int smallest_group(int tracks) {
  return tracks / MIN_SHARE > MIN_TRACKS ? tracks / MIN_SHARE : MIN_TRACKS;
}

/* Whether what divide() came to can stand: no two centres that sound alike, nor a group too
 * small, going by its share of the sample. */
static int stands(const field *f, const mc_analysis *results, const int *centres, int count, int tracks) {
  int sizes[MOST_TRIED] = {0};
  for (int i = 0; i < f->m; i++) sizes[f->labels[i]]++;
  for (int g = 0; g < count; g++) {
    if ((int64_t)sizes[g] * tracks < (int64_t)smallest_group(tracks) * f->m) return 0;
    for (int k = 0; k < g; k++)
      if (mc_analysis_distance(&results[f->members[centres[g]]], &results[f->members[centres[k]]], NULL) < MIN_APART)
        return 0;
  }
  return 1;
}

/* Whether f divides clearly into more groups than there can be stations, as a library of many kinds
 * does, whose best into 8 can't score well however plain the kinds: 8 centres can't fit 30 sounds.
 * A library with no such shape only scores worse the more it's cut up. In a real one 12 and up gave
 * under 0.09, and 30 made-up sounds gave 0.48 at 32. chosen has MOST_TRIED members. */
static int many_kinds(const field *f, const int *chosen, int tracks) {
  static const int tries[] = {12, 16, 24, MOST_TRIED};
  for (size_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
    int centres[MOST_TRIED], k = tries[i];
    if (k * MIN_TRACKS > tracks || k > f->m) break;
    memcpy(centres, chosen, (size_t)k * sizeof *centres);
    divide(f, centres, k);
    if (silhouette(f, k) >= MIN_SILHOUETTE) return 1;
  }
  return 0;
}

/* The count that divides f most clearly, its centres in best, or 1 if the library is one kind. */
static int best_division(const field *f, const mc_analysis *results, int tracks, int *best, mc_radio_groups *out) {
  int most = tracks / smallest_group(tracks), chosen[MOST_TRIED], count = 1;
  if (most > MC_RADIO_STATIONS_MAX) most = MC_RADIO_STATIONS_MAX;
  if (most > f->m) most = f->m;
  if (most < 1) most = 1;
  spread(f, f->m < MOST_TRIED ? f->m : MOST_TRIED, chosen);
  best[0] = chosen[0];
  double clearest = -1;
  for (int k = 2; k <= most; k++) {
    int centres[MOST_TRIED];
    memcpy(centres, chosen, (size_t)k * sizeof *centres);
    divide(f, centres, k);
    double score = silhouette(f, k);
    out->silhouette[k] = score;
    if (score <= clearest || !stands(f, results, centres, k, tracks)) continue;
    clearest = score;
    count = k;
    memcpy(best, centres, (size_t)k * sizeof *best);
  }
  if (clearest >= MIN_SILHOUETTE || (count > 1 && most == MC_RADIO_STATIONS_MAX && many_kinds(f, chosen, tracks))) return count;
  best[0] = chosen[0];
  return 1;
}

int mc_radio_group_of(const mc_radio_groups *groups, const mc_analysis *result) {
  int best = -1;
  double nearest = DBL_MAX;
  for (int g = 0; g < groups->count; g++) {
    double d = mc_analysis_distance(result, &groups->centres[g], &groups->stats);
    if (d >= 0 && d < nearest) {
      nearest = d;
      best = g;
    }
  }
  return best;
}

int mc_radio_group_typical(const mc_radio_groups *groups, const mc_analysis *result, int group) {
  double own = mc_analysis_distance(result, &groups->centres[group], &groups->stats);
  if (own < 0 || own > groups->reach[group]) return 0;
  for (int g = 0; g < groups->count; g++)
    if (g != group && own > BETWEEN * mc_analysis_distance(result, &groups->centres[g], &groups->stats)) return 0;
  return 1;
}

/* Counts every track's group, and lets go of any too small after all: its share of the sample
 * can flatter it. Its tracks go to the nearest that's left. */
static void settle_sizes(mc_radio_groups *out, const mc_analysis *results, int count) {
  for (;;) {
    memset(out->tracks, 0, sizeof out->tracks);
    for (int i = 0; i < count; i++) {
      int g = mc_radio_group_of(out, &results[i]);
      if (g >= 0) out->tracks[g]++;
    }
    int least = 0;
    for (int g = 1; g < out->count; g++)
      if (out->tracks[g] < out->tracks[least]) least = g;
    if (out->count < 2 || out->tracks[least] >= smallest_group(count)) return;
    out->count--;
    out->centre[least] = out->centre[out->count];
    out->centres[least] = out->centres[out->count];
  }
}

static int double_cmp(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/* How far from its centre a group's tracks usually are, with some room, from the sample: being
 * out past that is what being alone comes to across the whole library. */
static mc_status find_reach(mc_radio_groups *out, const mc_analysis *results, const int *sample, int n) {
  double *from = malloc((size_t)n * sizeof *from);
  if (!from) return MC_ERR_NOMEM;
  for (int g = 0; g < out->count; g++) {
    int found = 0;
    for (int i = 0; i < n; i++)
      if (mc_radio_group_of(out, &results[sample[i]]) == g)
        from[found++] = mc_analysis_distance(&results[sample[i]], &out->centres[g], &out->stats);
    qsort(from, (size_t)found, sizeof *from, double_cmp);
    out->reach[g] = found ? from[found / 2] * LONELY : 0;
    if (out->reach[g] < MC_RADIO_NEARBY) out->reach[g] = MC_RADIO_NEARBY;
  }
  free(from);
  return MC_OK;
}

/* Divides the m tracks of the sample that have company, whose distances are in dist. */
static mc_status divide_sample(const mc_analysis *results, const int *members, int m, const float *dist, int tracks,
                               mc_radio_groups *out) {
  int *labels = calloc((size_t)m, sizeof *labels);
  float *from = malloc((size_t)m * sizeof *from);
  if (!labels || !from) {
    free(labels);
    free(from);
    return MC_ERR_NOMEM;
  }
  field f = {m, members, dist, labels, from};
  int centres[MC_RADIO_STATIONS_MAX];
  out->count = best_division(&f, results, tracks, centres, out);
  for (int g = 0; g < out->count; g++) {
    out->centre[g] = members[centres[g]];
    out->centres[g] = results[out->centre[g]];
  }
  free(labels);
  free(from);
  return MC_OK;
}

mc_status mc_radio_groups_find(const mc_analysis *results, const unsigned *hashes, int count,
                               const mc_analysis_stats *stats, mc_radio_groups *out) {
  memset(out, 0, sizeof *out);
  out->stats = *stats;
  if (count < 1) return MC_OK;
  int cap = count < SAMPLE ? count : SAMPLE;
  int *sample = malloc((size_t)cap * sizeof *sample);
  int *members = malloc((size_t)cap * sizeof *members);
  float *dist = malloc((size_t)cap * cap * sizeof *dist);
  char *company = malloc((size_t)cap);
  int n = sample ? draw_sample(hashes, count, sample) : 0;
  mc_status st = MC_ERR_NOMEM;
  if (n && members && dist && company) {
    for (int i = 0; i < n; i++) {
      dist[(size_t)i * n + i] = 0;
      for (int k = 0; k < i; k++)
        dist[(size_t)i * n + k] = dist[(size_t)k * n + i] =
          (float)mc_analysis_distance(&results[sample[i]], &results[sample[k]], stats);
    }
    st = find_company(dist, n, count, company);
  }
  if (st == MC_OK) {
    /* Those with company close up in the matrix. Everything moves towards its start, so nothing
     * still to be read is written over. */
    int m = 0;
    for (int i = 0; i < n; i++) m += company[i] != 0;
    int row = 0;
    for (int i = 0; i < n; i++) {
      if (!company[i]) continue;
      int col = 0;
      for (int k = 0; k < n; k++)
        if (company[k]) dist[(size_t)row * m + col++] = dist[(size_t)i * n + k];
      members[row++] = sample[i];
    }
    st = divide_sample(results, members, m, dist, count, out);
  }
  if (st == MC_OK) {
    settle_sizes(out, results, count);
    st = find_reach(out, results, sample, n);
  }
  free(sample);
  free(members);
  free(dist);
  free(company);
  return st;
}
