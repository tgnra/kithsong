/* How fast Opus transcoding runs here. Not a test: build Release, with a
 * FETCHCONTENT_BASE_DIR of its own (a shared one lets a debug build's libopus objects in).
 *
 *   manual_opus_bench info
 *   manual_opus_bench resq
 *   manual_opus_bench stages <audio|synth:silence|synth:noise|synth:tones> <kbps> <complexity> <reps>
 *   manual_opus_bench sweep  <audio> <reps>
 *   manual_opus_bench pool   <dir of tNN/track.*> <threads> <kbps> <scratch cache> <passes>
 *   manual_opus_bench prof   <audio> <kbps> <complexity> <rip file>
 *
 * Every number comes with thread or process CPU time beside the wall time, since other work
 * on the machine stretches the second and hardly touches the first. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime() under -std=c11 */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "musiccore/transcode.h"
#include "opusenc.h"
#include "playback/decoder.h"
#include "tagger/coverart.h"
#include "tagger/tag_io.h"
#include "util/platform.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* libopusenc's own Speex resampler, by the prefix third_party/CMakeLists.txt gives it. */
void *libopusenc_resampler_init(uint32_t channels, uint32_t in, uint32_t out, int quality,
                                int *err);
int libopusenc_resampler_process_interleaved_float(void *st, const float *in, uint32_t *in_len,
                                                   float *out, uint32_t *out_len);
void libopusenc_resampler_destroy(void *st);

enum { CHUNK = 4096, MAX_FILES = 256 };

/* --- Clocks --- */

#ifdef _WIN32
static double filetime_s(FILETIME t) {
  return (double)(((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime) / 1e7;
}

static double wall_s(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart / (double)f.QuadPart;
}

static double thread_cpu_s(void) {
  FILETIME a, b, k, u;
  GetThreadTimes(GetCurrentThread(), &a, &b, &k, &u);
  return filetime_s(k) + filetime_s(u);
}

static double process_cpu_s(void) {
  FILETIME a, b, k, u;
  GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
  return filetime_s(k) + filetime_s(u);
}

/* Seconds the whole machine's cores were busy and in all, since boot. */
static void machine_s(double *busy, double *total) {
  FILETIME idle, k, u;
  GetSystemTimes(&idle, &k, &u);
  *total = filetime_s(k) + filetime_s(u); /* kernel time includes idle */
  *busy = *total - filetime_s(idle);
}
#else
static double clock_s(clockid_t id) {
  struct timespec t;
  clock_gettime(id, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
static double wall_s(void) { return clock_s(CLOCK_MONOTONIC); }
static double thread_cpu_s(void) { return clock_s(CLOCK_THREAD_CPUTIME_ID); }
static double process_cpu_s(void) { return clock_s(CLOCK_PROCESS_CPUTIME_ID); }
static void machine_s(double *busy, double *total) { *busy = *total = 0; }
#endif

typedef struct span {
  double wall, cpu;
} span;

static span span_start(void) {
  return (span){wall_s(), thread_cpu_s()};
}

static span span_end(span s) {
  return (span){wall_s() - s.wall, thread_cpu_s() - s.cpu};
}

/* --- Audio --- */

typedef struct pcm {
  float *f;
  uint64_t frames;
  unsigned rate, channels;
} pcm;

static int load(const char *path, pcm *p) {
  mc_decoder *d;
  if (mc_decoder_open(path, &d) != MC_OK) return 0;
  p->channels = mc_decoder_channels(d);
  p->rate = mc_decoder_rate(d);
  uint64_t cap = (uint64_t)p->rate * 60;
  p->f = malloc(cap * p->channels * sizeof *p->f);
  p->frames = 0;
  uint64_t got;
  for (;;) {
    if (p->frames + CHUNK > cap) {
      cap *= 2;
      p->f = realloc(p->f, cap * p->channels * sizeof *p->f);
    }
    if (!(got = mc_decoder_read(d, p->f + p->frames * p->channels, CHUNK))) break;
    p->frames += got;
  }
  mc_decoder_close(d);
  return 1;
}

/* A minute at 44.1 kHz, stereo. tones are the shape of mc_transcode_benchmark()'s. */
static void synth(const char *kind, pcm *p) {
  p->rate = 44100;
  p->channels = 2;
  p->frames = 60 * 44100;
  p->f = calloc(p->frames * 2, sizeof *p->f);
  uint32_t r = 0x12345678u;
  for (uint64_t i = 0; i < p->frames && strcmp(kind, "silence"); i++) {
    double w = 2 * 3.14159265358979 * (double)i / 44100.0;
    r = r * 1664525u + 1013904223u;
    float n = (float)(r >> 8) / 16777216.0f - 0.5f;
    r = r * 1664525u + 1013904223u;
    float m = (float)(r >> 8) / 16777216.0f - 0.5f;
    if (!strcmp(kind, "noise")) {
      p->f[i * 2] = n;
      p->f[i * 2 + 1] = m;
    } else {
      p->f[i * 2] = (float)(0.3 * sin(220 * w) + 0.2 * sin(1375 * w)) + 0.05f * n;
      p->f[i * 2 + 1] = (float)(0.3 * sin(277 * w) + 0.2 * sin(990 * w)) - 0.05f * n;
    }
  }
}

static int source(const char *arg, pcm *p) {
  if (!strncmp(arg, "synth:", 6)) {
    synth(arg + 6, p);
    return 1;
  }
  return load(arg, p);
}

/* --- Encoding from memory --- */

typedef struct mem_sink {
  unsigned char *buf; /* kept only if keep */
  size_t len, cap;
  int keep;
} mem_sink;

static int mem_write(void *user, const unsigned char *ptr, opus_int32 len) {
  mem_sink *m = user;
  if (m->keep) {
    if (m->len + (size_t)len > m->cap) {
      m->cap = (m->len + (size_t)len) * 2;
      m->buf = realloc(m->buf, m->cap);
    }
    memcpy(m->buf + m->len, ptr, (size_t)len);
  }
  m->len += (size_t)len;
  return 0;
}

static int mem_close(void *user) {
  (void)user;
  return 0;
}

/* The settings encode() in transcode.c uses, into m. */
static void encode_mem(const pcm *p, int kbps, int complexity, mem_sink *m) {
  static const OpusEncCallbacks callbacks = {mem_write, mem_close};
  OggOpusComments *c = ope_comments_create();
  int err;
  OggOpusEnc *e = ope_encoder_create_callbacks(&callbacks, m, c, (opus_int32)p->rate,
                                               (int)p->channels, 0, &err);
  ope_encoder_ctl(e, OPUS_SET_BITRATE(kbps * 1000));
  ope_encoder_ctl(e, OPUS_SET_VBR(1));
  ope_encoder_ctl(e, OPUS_SET_COMPLEXITY(complexity));
  ope_encoder_ctl(e, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  for (uint64_t at = 0; at < p->frames; at += CHUNK) {
    uint64_t n = p->frames - at < CHUNK ? p->frames - at : CHUNK;
    ope_encoder_write_float(e, p->f + at * p->channels, (int)n);
  }
  ope_encoder_drain(e);
  ope_encoder_destroy(e);
  ope_comments_destroy(c);
}

/* As the encoder's resampler runs: quality 5, to 48 kHz. *out is malloc'd. */
static void resample(const pcm *p, pcm *out) {
  int err;
  void *r = libopusenc_resampler_init(p->channels, p->rate, 48000, 5, &err);
  uint64_t cap = p->frames * 48000 / p->rate + 2 * CHUNK;
  *out = (pcm){malloc(cap * p->channels * sizeof(float)), 0, 48000, p->channels};
  for (uint64_t at = 0; at < p->frames;) {
    uint32_t in = (uint32_t)(p->frames - at < CHUNK ? p->frames - at : CHUNK), got = 2 * CHUNK;
    libopusenc_resampler_process_interleaved_float(r, p->f + at * p->channels, &in,
                                                   out->f + out->frames * p->channels, &got);
    at += in;
    out->frames += got;
  }
  libopusenc_resampler_destroy(r);
}

/* --- info --- */

#if defined(__x86_64__)
int opus_select_arch(void);
#endif

static int cmd_info(void) {
  printf("opus %s, cores %d\n", opus_get_version_string(), mc_cpu_count());
#if defined(__x86_64__)
  printf("opus_select_arch %d (0 none, 1 SSE, 2 SSE2, 3 SSE4.1, 4 AVX2)\n", opus_select_arch());
#endif
  int score = 0;
  mc_transcode_benchmark(&score);
  printf("mc_transcode_benchmark score %d\n", score);
  return 0;
}

/* --- stages --- */

typedef struct stat3 {
  double min, sum, max;
  int n;
} stat3;

static void stat_add(stat3 *s, double v) {
  if (!s->n || v < s->min) s->min = v;
  if (!s->n || v > s->max) s->max = v;
  s->sum += v;
  s->n++;
}

static void stage_row(const char *name, const stat3 *wall, const stat3 *cpu, double audio_s) {
  printf("%-22s wall %8.3f s (%.3f-%.3f)  cpu %8.3f s (%.3f-%.3f)  %7.1fx rt per cpu\n", name,
         wall->sum / wall->n, wall->min, wall->max, cpu->sum / cpu->n, cpu->min, cpu->max,
         audio_s / (cpu->sum / cpu->n));
}

/* What the sink in transcode.c does with the pages: hash and write. */
static void hash_write(const unsigned char *buf, size_t len, const char *path) {
  FILE *f = mc_fopen(path, "wb");
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  for (size_t at = 0; at < len; at += 4000) {
    size_t n = len - at < 4000 ? len - at : 4000;
    fwrite(buf + at, 1, n, f);
    mbedtls_sha256_update(&sha, buf + at, n);
  }
  unsigned char digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  fclose(f);
}

static void tags_and_cover(const char *path) {
  OggOpusComments *c = ope_comments_create();
  mc_tags t;
  if (mc_tags_read(path, &t) == MC_OK) mc_tags_clear(&t);
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", path);
  char *sep = strrchr(dir, '/'), *native = strrchr(dir, '\\');
  if (native > sep) sep = native;
  if (sep) {
    *sep = '\0';
    char *cover = mc_coverart_find(dir);
    char *open_path = cover ? mc_open_path(cover) : NULL;
    if (open_path) ope_comments_add_picture(c, open_path, -1, NULL);
    free(open_path);
    free(cover);
  }
  ope_comments_destroy(c);
}

static int cmd_stages(const char *arg, int kbps, int complexity, int reps) {
  int real = strncmp(arg, "synth:", 6) != 0;
  int64_t in_bytes = 0, mtime = 0;
  if (real) mc_file_info(arg, &in_bytes, &mtime);
  stat3 w[7] = {0}, c[7] = {0};
  pcm p = {0}, up = {0};
  mem_sink out = {0};
  char tmp[1200], uid[32];
  for (int r = 0; r < reps; r++) {
    free(p.f);
    span s = span_start();
    if (!source(arg, &p)) return 1;
    s = span_end(s);
    stat_add(&w[0], s.wall), stat_add(&c[0], s.cpu);

    s = span_start();
    if (real) tags_and_cover(arg);
    s = span_end(s);
    stat_add(&w[1], s.wall), stat_add(&c[1], s.cpu);

    free(up.f);
    s = span_start();
    resample(&p, &up);
    s = span_end(s);
    stat_add(&w[2], s.wall), stat_add(&c[2], s.cpu);

    mem_sink m48 = {0};
    s = span_start();
    encode_mem(&up, kbps, complexity, &m48);
    s = span_end(s);
    stat_add(&w[3], s.wall), stat_add(&c[3], s.cpu);

    free(out.buf);
    out = (mem_sink){.keep = 1};
    s = span_start();
    encode_mem(&p, kbps, complexity, &out);
    s = span_end(s);
    stat_add(&w[4], s.wall), stat_add(&c[4], s.cpu);

    snprintf(tmp, sizeof tmp, "%s.bench-out", real ? arg : "synth");
    s = span_start();
    hash_write(out.buf, out.len, tmp);
    s = span_end(s);
    stat_add(&w[5], s.wall), stat_add(&c[5], s.cpu);
    mc_remove(tmp);

    if (real) {
      snprintf(tmp, sizeof tmp, "%s.bench-cache", arg);
      snprintf(uid, sizeof uid, "u%d", r);
      mc_transcode_opts o = {MC_FORMAT_OPUS, kbps};
      char *path = NULL;
      s = span_start();
      mc_transcode_cached(tmp, uid, arg, &o, &path);
      s = span_end(s);
      stat_add(&w[6], s.wall), stat_add(&c[6], s.cpu);
      mc_free(path);
      mc_transcode_cache_trim(tmp, 0);
    }
  }
  double audio = (double)p.frames / p.rate;
  printf("%s: %.1f s of audio at %u Hz, %u ch, %.2f MB in, %.2f MB out at %d kbps complexity %d, "
         "%d reps\n", arg, audio, p.rate, p.channels, (double)in_bytes / 1e6,
         (double)out.len / 1e6, kbps, complexity, reps);
  stage_row("decode", &w[0], &c[0], audio);
  stage_row("tags+cover", &w[1], &c[1], audio);
  stage_row("resample only", &w[2], &c[2], audio);
  stage_row("encode 48k input", &w[3], &c[3], audio);
  stage_row("resample+encode", &w[4], &c[4], audio);
  stage_row("hash+write", &w[5], &c[5], audio);
  if (w[6].n) {
    stage_row("whole transcode", &w[6], &c[6], audio);
    double cpu = c[6].sum / c[6].n;
    printf("per thread, by cpu: %.2f MB/s in, %.3f MB/s out, %.1fx rt\n",
           (double)in_bytes / 1e6 / cpu, (double)out.len / 1e6 / cpu, audio / cpu);
  }
  return 0;
}

/* --- sweep --- */

static int cmd_sweep(const char *arg, int reps) {
  pcm p = {0};
  if (!source(arg, &p)) return 1;
  double audio = (double)p.frames / p.rate;
  static const int kbps[] = {64, 96, 128, 160, 192, 256};
  printf("kbps cx    cpu_s(min) xrt    out_MB  actual_kbps\n");
  for (size_t k = 0; k < sizeof kbps / sizeof *kbps; k++)
    for (int cx = 10; cx >= 4; cx--) {
      double best = 0;
      size_t len = 0;
      for (int r = 0; r < reps; r++) {
        mem_sink m = {0};
        span s = span_start();
        encode_mem(&p, kbps[k], cx, &m);
        s = span_end(s);
        if (!r || s.cpu < best) best = s.cpu;
        len = m.len;
      }
      printf("%4d %2d %9.3f %7.1f %7.2f %7.1f\n", kbps[k], cx, best, audio / best,
             (double)len / 1e6, (double)len * 8 / audio / 1000);
      fflush(stdout);
    }
  return 0;
}

/* --- resq --- */

/* How clean the resampler's output is: a sine at each frequency through it, then what is left
 * once the best fit of that sine (any gain and phase) is taken away, as dB below the signal.
 * The first and last 4096 frames are left out, for the filter's edges. */
static double residual_db(const pcm *p, double hz) {
  double w = 2 * 3.14159265358979323846 * hz / p->rate, ss = 0, cc = 0, sc = 0, xs = 0, xc = 0;
  uint64_t from = 4096, to = p->frames - 4096;
  for (uint64_t i = from; i < to; i++) {
    double s = sin(w * (double)i), c = cos(w * (double)i), x = p->f[i * 2];
    ss += s * s, cc += c * c, sc += s * c, xs += x * s, xc += x * c;
  }
  double det = ss * cc - sc * sc, a = (xs * cc - xc * sc) / det, b = (xc * ss - xs * sc) / det;
  double sig = 0, err = 0;
  for (uint64_t i = from; i < to; i++) {
    double fit = a * sin(w * (double)i) + b * cos(w * (double)i), e = p->f[i * 2] - fit;
    sig += fit * fit;
    err += e * e;
  }
  return 10 * log10(err / sig);
}

static int cmd_resq(void) {
  static const double hz[] = {100, 1000, 5000, 10000, 15000, 18000};
  printf("hz     residual_dB  gain_dB\n");
  for (size_t k = 0; k < sizeof hz / sizeof *hz; k++) {
    pcm in = {calloc(44100 * 10 * 2, sizeof(float)), 44100 * 10, 44100, 2}, out;
    for (uint64_t i = 0; i < in.frames; i++)
      in.f[i * 2] = in.f[i * 2 + 1] =
        (float)(0.5 * sin(2 * 3.14159265358979323846 * hz[k] * (double)i / 44100.0));
    resample(&in, &out);
    double rms = 0;
    for (uint64_t i = 4096; i < out.frames - 4096; i++) rms += (double)out.f[i * 2] * out.f[i * 2];
    rms = sqrt(rms / (double)(out.frames - 8192));
    printf("%6.0f %10.2f %8.4f\n", hz[k], residual_db(&out, hz[k]),
           20 * log10(rms / (0.5 / sqrt(2.0))));
    free(in.f);
    free(out.f);
  }
  return 0;
}

/* --- pool --- */

typedef struct file {
  char path[1024];
  int64_t size;
  double audio;
} file;

typedef struct pool_run {
  file *files;
  int count, jobs, next, kbps, pass_tag;
  const char *cache;
  mc_mutex lock;
  double out_bytes;
} pool_run;

static void pool_worker(void *arg) {
  pool_run *r = arg;
  double out_bytes = 0;
  for (;;) {
    mc_mutex_lock(&r->lock);
    int job = r->next < r->jobs ? r->next++ : -1;
    mc_mutex_unlock(&r->lock);
    if (job < 0) break;
    char uid[32];
    snprintf(uid, sizeof uid, "j%d-%d", r->pass_tag, job);
    mc_transcode_opts o = {MC_FORMAT_OPUS, r->kbps};
    char *path = NULL;
    if (mc_transcode_cached(r->cache, uid, r->files[job % r->count].path, &o, &path) == MC_OK) {
      int64_t size = 0, mtime = 0;
      mc_file_info(path, &size, &mtime);
      out_bytes += (double)size;
    } else {
      fprintf(stderr, "failed: %s\n", r->files[job % r->count].path);
    }
    mc_free(path);
  }
  mc_mutex_lock(&r->lock);
  r->out_bytes += out_bytes;
  mc_mutex_unlock(&r->lock);
}

static int by_size_down(const void *a, const void *b) {
  const file *x = a, *y = b;
  return (x->size < y->size) - (x->size > y->size);
}

static int cmd_pool(const char *dir, int threads, int kbps, const char *cache, int passes) {
  static file files[MAX_FILES];
  pool_run r = {.files = files, .kbps = kbps, .cache = cache};
  for (int i = 0; i < MAX_FILES; i++) {
    static const char *const exts[] = {"flac", "mp3", "wav"};
    for (size_t e = 0; e < 3; e++) {
      file *f = &files[r.count];
      snprintf(f->path, sizeof f->path, "%s/t%02d/track.%s", dir, i, exts[e]);
      int64_t mtime;
      if (mc_file_info(f->path, &f->size, &mtime) != MC_OK) continue;
      mc_decoder *d;
      if (mc_decoder_open(f->path, &d) != MC_OK) continue;
      f->audio = (double)mc_decoder_length(d) / mc_decoder_rate(d);
      mc_decoder_close(d);
      r.count++;
      break;
    }
  }
  if (!r.count) return 1;
  /* Longest first, so the last job isn't one long file alone: this is the ceiling. */
  qsort(files, (size_t)r.count, sizeof *files, by_size_down);
  r.jobs = r.count * passes;
  r.pass_tag = (int)((uint64_t)(wall_s() * 10) % 100000);
  mc_mutex_init(&r.lock);
  double in = 0, audio = 0;
  for (int j = 0; j < r.jobs; j++) {
    in += (double)files[j % r.count].size;
    audio += files[j % r.count].audio;
  }
  mc_thread *t = malloc((size_t)threads * sizeof *t);
  double busy0, total0, busy1, total1;
  machine_s(&busy0, &total0);
  double p0 = process_cpu_s(), w0 = wall_s();
  for (int i = 0; i < threads; i++) mc_thread_start(&t[i], pool_worker, &r);
  for (int i = 0; i < threads; i++) mc_thread_join(&t[i]);
  double wall = wall_s() - w0, proc = process_cpu_s() - p0;
  machine_s(&busy1, &total1);
  double machine = total1 > total0 ? (busy1 - busy0) / (total1 - total0) : 0;
  /* Everything else on the machine meanwhile, in cores. */
  double other_cores = machine * mc_cpu_count() - proc / wall;
  printf("threads %2d kbps %3d: %d jobs, wall %7.2f s, cpu %8.2f s | %6.2f MB/s in, %6.3f MB/s out,"
         " %6.1fx rt | per cpu-s %5.2f MB in | machine %3.0f%% busy, others %4.1f cores\n",
         threads, kbps, r.jobs, wall, proc, in / 1e6 / wall, r.out_bytes / 1e6 / wall, audio / wall,
         in / 1e6 / proc, machine * 100, other_cores);
  free(t);
  return 0;
}

/* --- prof --- */

#ifdef _WIN32
typedef struct prof_run {
  HANDLE target;
  volatile int stop;
  uint64_t *rips;
  int n, cap;
} prof_run;

static void sampler(void *arg) {
  prof_run *p = arg;
  while (!p->stop && p->n < p->cap) {
    Sleep(1);
    if (SuspendThread(p->target) == (DWORD)-1) break;
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(p->target, &c)) p->rips[p->n++] = c.Rip;
    ResumeThread(p->target);
  }
}

static int cmd_prof(const char *arg, int kbps, int complexity, const char *out_path) {
  pcm p = {0};
  if (!source(arg, &p)) return 1;
  prof_run r = {0};
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &r.target, 0,
                  FALSE, DUPLICATE_SAME_ACCESS);
  r.cap = 1 << 22;
  r.rips = malloc((size_t)r.cap * sizeof *r.rips);
  mc_thread t;
  mc_thread_start(&t, sampler, &r);
  mem_sink m = {0};
  for (int i = 0; i < 3; i++) encode_mem(&p, kbps, complexity, &m);
  r.stop = 1;
  mc_thread_join(&t);
  FILE *f = fopen(out_path, "w");
  fprintf(f, "base %" PRIx64 "\n", (uint64_t)(uintptr_t)GetModuleHandleW(NULL));
  for (int i = 0; i < r.n; i++) fprintf(f, "%" PRIx64 "\n", r.rips[i]);
  fclose(f);
  printf("%d samples\n", r.n);
  return 0;
}
#endif

int main(int argc, char **argv) {
  const char *cmd = argc > 1 ? argv[1] : "";
  if (!strcmp(cmd, "info")) return cmd_info();
  if (!strcmp(cmd, "resq")) return cmd_resq();
  if (!strcmp(cmd, "stages") && argc == 6)
    return cmd_stages(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
  if (!strcmp(cmd, "sweep") && argc == 4) return cmd_sweep(argv[2], atoi(argv[3]));
  if (!strcmp(cmd, "pool") && argc == 7)
    return cmd_pool(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5], atoi(argv[6]));
#ifdef _WIN32
  if (!strcmp(cmd, "prof") && argc == 6)
    return cmd_prof(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
#endif
  fprintf(stderr, "see the top of manual_opus_bench.c\n");
  return 2;
}
