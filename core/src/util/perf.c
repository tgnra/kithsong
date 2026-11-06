/* The profiler (kithcore/perf.h). Frontends feed it from their UI threads, at most once a
 * frame, so one lock is plenty; while it's off every feed stops at the first line. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */
#endif
#include "kithcore/perf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "config.h"
#include "util/platform.h"

enum {
  MAX_MARKS = 48,
  MAX_NOTES = 24,
  MAX_SERIES = 24,
  MAX_FRAMES = 16384, /* per series: minutes of scrolling */
  MAX_VIEWS = 24,
  MAX_SWITCHES = 64, /* per view */
  NAME_CAP = 48,
  VALUE_CAP = 96,
  PAUSE_US = 500000,
};

typedef struct mark {
  char name[NAME_CAP];
  uint64_t at_us;
} mark;

typedef struct note {
  char key[NAME_CAP];
  char value[VALUE_CAP];
} note;

typedef struct series {
  char name[NAME_CAP];
  float *frame_ms, *work_ms; /* count of each; work_ms only has the frames that gave one */
  int count, work_count, pauses, overflow;
  uint64_t last_us; /* 0: the next frame starts a run */
} series;

typedef struct view {
  char name[NAME_CAP];
  float ms[MAX_SWITCHES];
  int count;
} view;

static struct {
  mc_mutex lock;
  volatile int on, ready;
  volatile int auto_on;
  uint64_t t0_us; /* the process's start, on mc_perf_now_us()'s clock */
  int t0_known;   /* 0: the system wouldn't say, so the profiler's first use stands in */
  double refresh_hz;
  mark marks[MAX_MARKS];
  int nmarks;
  note notes[MAX_NOTES];
  int nnotes;
  series series[MAX_SERIES];
  int nseries;
  view views[MAX_VIEWS];
  int nviews;
  char switching[NAME_CAP]; /* the view switch under way; "" for none */
  uint64_t switch_us;
} P;

/* The lock has no static initialiser on every platform, and mc_perf_set_enabled() comes first
 * and from the main thread, so it's made there. */
static void ensure_ready(void) {
  if (P.ready) return;
  mc_mutex_init(&P.lock);
  P.refresh_hz = 60;
  P.ready = 1;
}

uint64_t mc_perf_now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER freq;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  /* In two steps, so a long uptime doesn't overflow. */
  uint64_t whole = (uint64_t)now.QuadPart / (uint64_t)freq.QuadPart;
  uint64_t rest = (uint64_t)now.QuadPart % (uint64_t)freq.QuadPart;
  return whole * 1000000u + rest * 1000000u / (uint64_t)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

/* How long ago this process started, in microseconds, or 0 if the system won't say. */
static uint64_t process_age_us(void) {
#ifdef _WIN32
  FILETIME created, a, b, c, now;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &a, &b, &c)) return 0;
  GetSystemTimePreciseAsFileTime(&now);
  ULARGE_INTEGER from = {.LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime};
  ULARGE_INTEGER to = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
  return to.QuadPart > from.QuadPart ? (to.QuadPart - from.QuadPart) / 10 : 0;
#else
  /* /proc/self/stat's 22nd field is the start, in clock ticks since boot (so to 10 ms). */
  char buf[1024];
  FILE *f = fopen("/proc/self/stat", "r");
  if (!f) return 0;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = '\0';
  const char *p = strrchr(buf, ')'); /* the name before it may hold spaces */
  if (!p) return 0;
  unsigned long long ticks = 0;
  p++; /* the state, field 3, comes next */
  for (int field = 3; field <= 22; field++) {
    while (*p == ' ') p++;
    if (field == 22) ticks = strtoull(p, NULL, 10);
    while (*p && *p != ' ') p++;
  }
  long hz = sysconf(_SC_CLK_TCK);
  struct timespec ts;
#ifdef CLOCK_BOOTTIME
  if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) return 0;
#else
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
#endif
  if (hz <= 0 || !ticks) return 0;
  uint64_t now_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
  uint64_t start_us = (uint64_t)ticks * 1000000u / (uint64_t)hz;
  return now_us > start_us ? now_us - start_us : 0;
#endif
}

int mc_perf_enabled(void) {
  return P.on;
}

int mc_perf_env_enabled(void) {
  const char *v = getenv("KITHSONG_PERF");
  return v && *v && strcmp(v, "0") != 0;
}

void mc_perf_set_enabled(int on) {
  ensure_ready();
  mc_mutex_lock(&P.lock);
  if (on && !P.t0_us) {
    uint64_t age = process_age_us(), now = mc_perf_now_us();
    P.t0_known = age > 0;
    P.t0_us = now > age ? now - age : 1;
  }
  P.on = on != 0;
  mc_mutex_unlock(&P.lock);
}

void mc_perf_set_refresh_hz(double hz) {
  ensure_ready();
  if (hz >= 10 && hz <= 1000) P.refresh_hz = hz;
}

void mc_perf_mark_at(const char *name, uint64_t at_us) {
  if (!P.on || !name) return;
  mc_mutex_lock(&P.lock);
  int seen = 0;
  for (int i = 0; i < P.nmarks && !seen; i++) seen = !strcmp(P.marks[i].name, name);
  if (!seen && P.nmarks < MAX_MARKS) {
    snprintf(P.marks[P.nmarks].name, NAME_CAP, "%s", name);
    P.marks[P.nmarks++].at_us = at_us;
  }
  mc_mutex_unlock(&P.lock);
}

void mc_perf_mark(const char *name) {
  if (P.on) mc_perf_mark_at(name, mc_perf_now_us());
}

void mc_perf_note(const char *key, const char *value) {
  if (!P.on || !key || !value) return;
  mc_mutex_lock(&P.lock);
  int i = 0;
  while (i < P.nnotes && strcmp(P.notes[i].key, key)) i++;
  if (i < MAX_NOTES) {
    if (i == P.nnotes) {
      P.nnotes++;
      snprintf(P.notes[i].key, NAME_CAP, "%s", key);
    }
    snprintf(P.notes[i].value, VALUE_CAP, "%s", value);
  }
  mc_mutex_unlock(&P.lock);
}

void mc_perf_note_int(const char *key, int64_t value) {
  char text[32];
  snprintf(text, sizeof text, "%lld", (long long)value);
  mc_perf_note(key, text);
}

/* The series called name, made if need be; NULL if there's no room. Under the lock. */
static series *find_series(const char *name) {
  for (int i = 0; i < P.nseries; i++)
    if (!strcmp(P.series[i].name, name)) return &P.series[i];
  if (P.nseries == MAX_SERIES) return NULL;
  series *s = &P.series[P.nseries];
  memset(s, 0, sizeof *s);
  s->frame_ms = malloc(MAX_FRAMES * sizeof *s->frame_ms);
  s->work_ms = malloc(MAX_FRAMES * sizeof *s->work_ms);
  if (!s->frame_ms || !s->work_ms) {
    free(s->frame_ms);
    free(s->work_ms);
    return NULL;
  }
  snprintf(s->name, NAME_CAP, "%s", name);
  P.nseries++;
  return s;
}

/* A frame's series: its own, or its test's while one runs. */
static void series_name(char *out, size_t cap, const char *name) {
  snprintf(out, cap, "%s%s", P.auto_on ? "auto_" : "", name);
}

void mc_perf_frame(const char *name, uint64_t work_us) {
  if (!P.on || !name) return;
  uint64_t now = mc_perf_now_us();
  char full[NAME_CAP];
  mc_mutex_lock(&P.lock);
  series_name(full, sizeof full, name);
  series *s = find_series(full);
  if (s) {
    uint64_t gap = s->last_us ? now - s->last_us : 0;
    if (s->last_us && gap >= PAUSE_US && !P.auto_on) {
      s->pauses++;
    } else if (s->last_us) {
      if (s->count < MAX_FRAMES) s->frame_ms[s->count++] = (float)((double)gap / 1000.0);
      else s->overflow++;
    }
    if (work_us && s->work_count < MAX_FRAMES) s->work_ms[s->work_count++] = (float)((double)work_us / 1000.0);
    s->last_us = now;
  }
  mc_mutex_unlock(&P.lock);
}

void mc_perf_frame_break(const char *name) {
  if (!P.on || !name) return;
  char full[NAME_CAP];
  mc_mutex_lock(&P.lock);
  series_name(full, sizeof full, name);
  for (int i = 0; i < P.nseries; i++)
    if (!strcmp(P.series[i].name, full)) P.series[i].last_us = 0;
  mc_mutex_unlock(&P.lock);
}

void mc_perf_auto_begin(void) {
  if (!P.on) return;
  mc_mutex_lock(&P.lock);
  for (int i = 0; i < P.nseries; i++) {
    series *s = &P.series[i];
    if (!strncmp(s->name, "auto_", 5)) {
      s->count = s->work_count = s->pauses = s->overflow = 0;
      s->last_us = 0;
    }
  }
  P.auto_on = 1;
  mc_mutex_unlock(&P.lock);
}

void mc_perf_auto_end(void) {
  if (!P.ready) return;
  mc_mutex_lock(&P.lock);
  P.auto_on = 0;
  mc_mutex_unlock(&P.lock);
}

int mc_perf_auto_running(void) {
  return P.auto_on;
}

void mc_perf_view_begin(const char *name) {
  if (!P.on || !name) return;
  uint64_t now = mc_perf_now_us();
  mc_mutex_lock(&P.lock);
  snprintf(P.switching, NAME_CAP, "%s", name);
  P.switch_us = now;
  mc_mutex_unlock(&P.lock);
}

int mc_perf_view_pending(void) {
  return P.on && P.switching[0];
}

void mc_perf_view_end(void) {
  if (!P.on) return;
  uint64_t now = mc_perf_now_us();
  mc_mutex_lock(&P.lock);
  if (P.switching[0]) {
    int i = 0;
    while (i < P.nviews && strcmp(P.views[i].name, P.switching)) i++;
    if (i == P.nviews && P.nviews < MAX_VIEWS) {
      memset(&P.views[i], 0, sizeof P.views[i]);
      snprintf(P.views[i].name, NAME_CAP, "%s", P.switching);
      P.nviews++;
    }
    if (i < P.nviews) {
      view *v = &P.views[i];
      if (v->count == MAX_SWITCHES) { /* full: the oldest goes, the newest say more */
        memmove(v->ms, v->ms + 1, sizeof v->ms - sizeof v->ms[0]);
        v->count--;
      }
      v->ms[v->count++] = (float)((double)(now - P.switch_us) / 1000.0);
    }
    P.switching[0] = '\0';
  }
  mc_mutex_unlock(&P.lock);
}

void mc_perf_clear(void) {
  if (!P.ready) return;
  mc_mutex_lock(&P.lock);
  for (int i = 0; i < P.nseries; i++) {
    free(P.series[i].frame_ms);
    free(P.series[i].work_ms);
  }
  P.nseries = 0;
  P.nviews = 0;
  P.switching[0] = '\0';
  mc_mutex_unlock(&P.lock);
}

/* --- The report --- */

typedef struct text {
  char *s;
  size_t len, cap;
  int failed;
} text;

static void add(text *t, const char *fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  if (n < 0 || t->failed) return;
  if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
  if (t->len + (size_t)n + 1 > t->cap) {
    size_t cap = t->cap ? t->cap * 2 : 4096;
    char *s = realloc(t->s, cap);
    if (!s) {
      t->failed = 1;
      return;
    }
    t->s = s;
    t->cap = cap;
  }
  memcpy(t->s + t->len, line, (size_t)n);
  t->len += (size_t)n;
  t->s[t->len] = '\0';
}

static int cmp_float(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

static int cmp_series(const void *a, const void *b) {
  return strcmp((*(series *const *)a)->name, (*(series *const *)b)->name);
}

static int cmp_view(const void *a, const void *b) {
  return strcmp((*(view *const *)a)->name, (*(view *const *)b)->name);
}

static int cmp_mark(const void *a, const void *b) {
  uint64_t x = (*(mark *const *)a)->at_us, y = (*(mark *const *)b)->at_us;
  return x < y ? -1 : x > y;
}

/* The value at percentile pct of n sorted values, nearest rank. */
static double percentile(const float *sorted, int n, int pct) {
  int rank = (n * pct + 99) / 100;
  return sorted[rank > 0 ? rank - 1 : 0];
}

static const char *platform_name(void) {
#if defined(__ANDROID__)
  return "android";
#elif defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

/* values sorted into a copy, or NULL. Free with free(). */
static float *sorted_copy(const float *values, int n) {
  float *sorted = malloc((size_t)n * sizeof *sorted);
  if (!sorted) return NULL;
  memcpy(sorted, values, (size_t)n * sizeof *sorted);
  qsort(sorted, (size_t)n, sizeof *sorted, cmp_float);
  return sorted;
}

static void add_series(text *t, const series *s, double frame_ms) {
  add(t, "\n[scroll %s]\n", s->name);
  add(t, "frames %d\n", s->count);
  float *sorted = s->count ? sorted_copy(s->frame_ms, s->count) : NULL;
  if (sorted) {
    /* A frame twice as long as it should be missed one refresh, and so on. */
    long dropped = 0;
    for (int i = 0; i < s->count; i++) {
      long missed = (long)((double)sorted[i] / frame_ms + 0.5) - 1;
      if (missed > 0) dropped += missed;
    }
    add(t, "median_ms %.2f\n", percentile(sorted, s->count, 50));
    add(t, "p95_ms %.2f\n", percentile(sorted, s->count, 95));
    add(t, "p99_ms %.2f\n", percentile(sorted, s->count, 99));
    add(t, "worst_ms %.2f\n", (double)sorted[s->count - 1]);
    add(t, "dropped %ld\n", dropped);
    free(sorted);
  }
  sorted = s->work_count ? sorted_copy(s->work_ms, s->work_count) : NULL;
  if (sorted) {
    add(t, "paint_median_ms %.2f\n", percentile(sorted, s->work_count, 50));
    add(t, "paint_p95_ms %.2f\n", percentile(sorted, s->work_count, 95));
    add(t, "paint_worst_ms %.2f\n", (double)sorted[s->work_count - 1]);
    free(sorted);
  }
  if (s->pauses) add(t, "pauses %d\n", s->pauses);
  if (s->overflow) add(t, "frames_not_kept %d\n", s->overflow);
}

char *mc_perf_report(void) {
  ensure_ready();
  text t = {0};
  mc_mutex_lock(&P.lock);
  add(&t, "# Kithsong profile\n\n[run]\n");
  add(&t, "version %s\n", MC_VERSION_STRING);
  add(&t, "platform %s\n", platform_name());
#ifdef NDEBUG
  add(&t, "build release\n");
#else
  add(&t, "build debug\n");
#endif
  add(&t, "cpus %d\n", mc_cpu_count());
  add(&t, "refresh_hz %.0f\n", P.refresh_hz);
  for (int i = 0; i < P.nnotes; i++) add(&t, "%s %s\n", P.notes[i].key, P.notes[i].value);

  add(&t, "\n[startup]\n# phase  ms_since_process_start  ms_since_last_phase\n");
  if (!P.t0_known) add(&t, "# the system didn't say when the process started\n");
  mark *order[MAX_MARKS];
  for (int i = 0; i < P.nmarks; i++) order[i] = &P.marks[i];
  qsort(order, (size_t)P.nmarks, sizeof *order, cmp_mark);
  uint64_t last = P.t0_us;
  for (int i = 0; i < P.nmarks; i++) {
    uint64_t at = order[i]->at_us;
    add(&t, "%s %.1f %.1f\n", order[i]->name, (double)(at - P.t0_us) / 1000.0, (double)(at - last) / 1000.0);
    last = at;
  }

  series *ss[MAX_SERIES];
  for (int i = 0; i < P.nseries; i++) ss[i] = &P.series[i];
  qsort(ss, (size_t)P.nseries, sizeof *ss, cmp_series);
  for (int i = 0; i < P.nseries; i++) add_series(&t, ss[i], 1000.0 / P.refresh_hz);

  if (P.nviews) add(&t, "\n[views]\n# view  switches  median_ms  worst_ms\n");
  view *vs[MAX_VIEWS];
  for (int i = 0; i < P.nviews; i++) vs[i] = &P.views[i];
  qsort(vs, (size_t)P.nviews, sizeof *vs, cmp_view);
  for (int i = 0; i < P.nviews; i++) {
    float *sorted = sorted_copy(vs[i]->ms, vs[i]->count);
    if (!sorted) continue;
    add(&t, "%s %d %.1f %.1f\n", vs[i]->name, vs[i]->count, percentile(sorted, vs[i]->count, 50),
        (double)sorted[vs[i]->count - 1]);
    free(sorted);
  }
  mc_mutex_unlock(&P.lock);
  if (t.failed) {
    free(t.s);
    return NULL;
  }
  return t.s;
}

mc_status mc_perf_save(const char *path) {
  if (!path) return MC_ERR_INVALID_ARG;
  char *report = mc_perf_report();
  if (!report) return MC_ERR_NOMEM;
  FILE *f = mc_fopen(path, "wb");
  mc_status st = MC_ERR_IO;
  if (f) {
    size_t n = strlen(report);
    int ok = fwrite(report, 1, n, f) == n;
    if (fclose(f) == 0 && ok) st = MC_OK;
  }
  free(report);
  return st;
}
