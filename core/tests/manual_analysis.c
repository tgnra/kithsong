/* Analyses a file or a folder of them and prints what came out, for a person to judge:
 *   build/<preset>/core/tests/manual_analysis <file or folder> [neighbours]
 * Each file's loudness, range, peak and brightness, and its nearest neighbours among the rest
 * (5 by default), by the distance a library of just these files would give. Then the cost, as
 * CPU seconds per hour of audio, which only means anything from a release build. Reads the
 * files and writes nothing. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "kithcore/analysis.h"
#include "playback/decoder.h"
#include "util/platform.h"

typedef struct entry {
  char *path;
  mc_analysis a;
} entry;

typedef struct list {
  entry *items;
  int count;
} list;

static double cpu_seconds(void) {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
  ULARGE_INTEGER k = {.u = {kernel.dwLowDateTime, kernel.dwHighDateTime}};
  ULARGE_INTEGER u = {.u = {user.dwLowDateTime, user.dwHighDateTime}};
  return (double)(k.QuadPart + u.QuadPart) / 1e7;
#else
  struct timespec t;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) slash = back;
  return slash ? slash + 1 : path;
}

static int on_file(void *user, const char *path) {
  list *l = user;
  if (!mc_decoder_supports(path)) return 0;
  entry *more = realloc(l->items, (size_t)(l->count + 1) * sizeof *more);
  if (!more) return 1;
  l->items = more;
  l->items[l->count].path = mc_strdup(path);
  l->count++;
  return 0;
}

static void print_features(const entry *e) {
  const mc_analysis *a = &e->a;
  printf("%s\n", base_name(e->path));
  printf("  %.1f s  %.2f LUFS  LRA %.2f LU  peak %.2f dBTP  centroid %.0f Hz (sd %.0f)\n", a->seconds,
         a->lufs, a->lra, a->true_peak, a->centroid_mean, sqrt(a->centroid_var));
  printf("  mfcc mean");
  for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) printf(" %.1f", a->mfcc_mean[c]);
  printf("\n  mfcc sd  ");
  for (int c = 0; c < MC_ANALYSIS_MFCCS; c++) printf(" %.1f", sqrt(a->mfcc_var[c]));
  printf("\n");
}

typedef struct near_one {
  int index;
  double distance;
} near_one;

static int by_distance(const void *a, const void *b) {
  double x = ((const near_one *)a)->distance, y = ((const near_one *)b)->distance;
  return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file or folder> [neighbours]\n", argv[0]);
    return 2;
  }
  int neighbours = argc > 2 ? atoi(argv[2]) : 5;
  if (mc_init() != MC_OK) return 1;
  list found = {NULL, 0};
  mc_walk(argv[1], on_file, &found);

  list done = {calloc((size_t)found.count + 1, sizeof(entry)), 0};
  double audio = 0.0, start = cpu_seconds();
  for (int i = 0; i < found.count; i++) {
    entry e = found.items[i];
    mc_status st = mc_analysis_file(e.path, NULL, NULL, &e.a);
    if (st != MC_OK) {
      printf("%s: %s\n", base_name(e.path), mc_status_str(st));
      continue;
    }
    audio += e.a.seconds;
    done.items[done.count++] = e;
    print_features(&e);
  }
  double cpu = cpu_seconds() - start;

  mc_analysis *results = calloc((size_t)done.count + 1, sizeof *results);
  for (int i = 0; i < done.count; i++) results[i] = done.items[i].a;
  mc_analysis_stats stats;
  mc_analysis_stats_from(results, done.count, &stats);
  near_one *closest = calloc((size_t)done.count + 1, sizeof *closest);
  if (done.count > 1 && neighbours > 0) printf("\nnearest neighbours\n");
  for (int i = 0; i < done.count && done.count > 1 && neighbours > 0; i++) {
    int n = 0;
    for (int j = 0; j < done.count; j++)
      if (j != i) closest[n++] = (near_one){j, mc_analysis_distance(&results[i], &results[j], &stats)};
    qsort(closest, (size_t)n, sizeof *closest, by_distance);
    printf("%s\n", base_name(done.items[i].path));
    for (int k = 0; k < n && k < neighbours; k++)
      printf("  %.3f  %s\n", closest[k].distance, base_name(done.items[closest[k].index].path));
  }

  if (audio > 0.0)
    printf("\n%d files, %.1f s of audio, %.2f s of CPU: %.1f s of CPU per hour of audio, %.0fx real"
           " time\n",
           done.count, audio, cpu, cpu / audio * 3600.0, audio / (cpu > 0.0 ? cpu : 1e-9));
  mc_shutdown();
  return 0;
}
