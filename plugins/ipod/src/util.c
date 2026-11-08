#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <sys/stat.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

int ipod_seterr(ipod_error *err, int code, const char *fmt, ...) {
  if (err) {
    va_list ap;
    va_start(ap, fmt);
    err->code = code;
    vsnprintf(err->message, sizeof err->message, fmt, ap);
    va_end(ap);
  }
  return code;
}

/* --- Time ---------------------------------------------------------------------- */

int64_t ipod_mac_to_unix(uint32_t mac, int32_t tz_offset) {
  if (mac == 0) return 0;
  return (int64_t)mac - (int64_t)IPOD_MAC_EPOCH_OFFSET - tz_offset;
}

uint32_t ipod_unix_to_mac(int64_t unix_time, int32_t tz_offset) {
  if (unix_time == 0) return 0;
  int64_t v = unix_time + tz_offset + (int64_t)IPOD_MAC_EPOCH_OFFSET;
  if (v < 0) return 0;
  if (v > (int64_t)UINT32_MAX) return UINT32_MAX;
  return (uint32_t)v;
}

int32_t ipod_local_tz_offset(void) {
  time_t now = time(NULL);
  struct tm g, l;
#ifdef _WIN32
  gmtime_s(&g, &now);
  localtime_s(&l, &now);
#else
  gmtime_r(&now, &g);
  localtime_r(&now, &l);
#endif
  /* Seconds since an arbitrary epoch for both broken-down times; days via a simple count
   * that only has to agree with itself for dates a day apart. */
  long dl = (long)(l.tm_year - g.tm_year) * 366 + (l.tm_yday - g.tm_yday);
  if (dl > 1) dl = -1; /* year wrapped: local is behind */
  if (dl < -1) dl = 1;
  long secs = dl * 86400L + (long)(l.tm_hour - g.tm_hour) * 3600L + (long)(l.tm_min - g.tm_min) * 60L +
              (l.tm_sec - g.tm_sec);
  return (int32_t)secs;
}

/* --- Buffers -------------------------------------------------------------------- */

void ipod_buf_free(ipod_buf *b) {
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

uint8_t *ipod_buf_grow(ipod_buf *b, size_t n) {
  if (b->oom) return NULL;
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n) cap *= 2;
    uint8_t *p = realloc(b->data, cap);
    if (!p) {
      b->oom = 1;
      return NULL;
    }
    b->data = p;
    b->cap = cap;
  }
  uint8_t *at = b->data + b->len;
  memset(at, 0, n);
  b->len += n;
  return at;
}

void ipod_buf_put(ipod_buf *b, const void *p, size_t n) {
  if (!n) return;
  uint8_t *at = ipod_buf_grow(b, n);
  if (at) memcpy(at, p, n);
}
void ipod_buf_zero(ipod_buf *b, size_t n) { (void)ipod_buf_grow(b, n); }
void ipod_buf_u8(ipod_buf *b, uint8_t v) { ipod_buf_put(b, &v, 1); }
void ipod_buf_le16(ipod_buf *b, uint16_t v) {
  uint8_t *p = ipod_buf_grow(b, 2);
  if (p) ipod_wr16(p, v);
}
void ipod_buf_le32(ipod_buf *b, uint32_t v) {
  uint8_t *p = ipod_buf_grow(b, 4);
  if (p) ipod_wr32(p, v);
}
void ipod_buf_le64(ipod_buf *b, uint64_t v) {
  uint8_t *p = ipod_buf_grow(b, 8);
  if (p) ipod_wr64(p, v);
}
void ipod_buf_be16(ipod_buf *b, uint16_t v) {
  uint8_t *p = ipod_buf_grow(b, 2);
  if (p) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
  }
}
void ipod_buf_be32(ipod_buf *b, uint32_t v) {
  uint8_t *p = ipod_buf_grow(b, 4);
  if (p) ipod_wrbe32(p, v);
}
void ipod_buf_be64(ipod_buf *b, uint64_t v) {
  ipod_buf_be32(b, (uint32_t)(v >> 32));
  ipod_buf_be32(b, (uint32_t)v);
}
void ipod_buf_tag(ipod_buf *b, const char tag[4]) { ipod_buf_put(b, tag, 4); }
void ipod_buf_set_le32(ipod_buf *b, size_t at, uint32_t v) {
  if (!b->oom && at + 4 <= b->len) ipod_wr32(b->data + at, v);
}
void ipod_buf_set_be32(ipod_buf *b, size_t at, uint32_t v) {
  if (!b->oom && at + 4 <= b->len) ipod_wrbe32(b->data + at, v);
}

/* --- Strings --------------------------------------------------------------------- */

char *ipod_strdup(const char *s) {
  if (!s) return NULL;
  return ipod_strndup(s, strlen(s));
}

char *ipod_strndup(const char *s, size_t n) {
  char *d = malloc(n + 1);
  if (!d) return NULL;
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

char *ipod_asprintf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) return NULL;
  char *s = malloc((size_t)n + 1);
  if (!s) return NULL;
  va_start(ap, fmt);
  vsnprintf(s, (size_t)n + 1, fmt, ap);
  va_end(ap);
  return s;
}

int ipod_streq(const char *a, const char *b) {
  if (!a) a = "";
  if (!b) b = "";
  return strcmp(a, b) == 0;
}

int ipod_is_ascii(const char *s) {
  for (; s && *s; s++)
    if ((unsigned char)*s >= 0x80) return 0;
  return 1;
}

uint32_t ipod_utf8_next(const char **sp) {
  const unsigned char *s = (const unsigned char *)*sp;
  uint32_t c = s[0];
  if (!c) return 0;
  int n = 0;
  if (c < 0x80) {
    *sp += 1;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    n = 1;
    c &= 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    n = 2;
    c &= 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    n = 3;
    c &= 0x07;
  } else {
    *sp += 1;
    return 0xFFFD;
  }
  for (int i = 1; i <= n; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *sp += i;
      return 0xFFFD;
    }
    c = (c << 6) | (s[i] & 0x3F);
  }
  *sp += n + 1;
  if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0xFFFD;
  return c;
}

static void put_utf8(ipod_buf *b, uint32_t c) {
  uint8_t t[4];
  size_t n;
  if (c < 0x80) {
    t[0] = (uint8_t)c;
    n = 1;
  } else if (c < 0x800) {
    t[0] = (uint8_t)(0xC0 | (c >> 6));
    t[1] = (uint8_t)(0x80 | (c & 0x3F));
    n = 2;
  } else if (c < 0x10000) {
    t[0] = (uint8_t)(0xE0 | (c >> 12));
    t[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    t[2] = (uint8_t)(0x80 | (c & 0x3F));
    n = 3;
  } else {
    t[0] = (uint8_t)(0xF0 | (c >> 18));
    t[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
    t[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
    t[3] = (uint8_t)(0x80 | (c & 0x3F));
    n = 4;
  }
  ipod_buf_put(b, t, n);
}

char *ipod_utf16le_to_utf8(const uint8_t *p, size_t len) {
  ipod_buf b = {0};
  size_t n = len / 2;
  for (size_t i = 0; i < n; i++) {
    uint32_t c = ipod_rd16(p + 2 * i);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
      uint32_t lo = ipod_rd16(p + 2 * (i + 1));
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
        i++;
      } else {
        c = 0xFFFD;
      }
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      c = 0xFFFD;
    }
    if (c == 0) break; /* some writers include a terminator */
    put_utf8(&b, c);
  }
  ipod_buf_u8(&b, 0);
  if (b.oom) {
    ipod_buf_free(&b);
    return NULL;
  }
  return (char *)b.data;
}

uint8_t *ipod_utf8_to_utf16le(const char *s, size_t *out_len) {
  ipod_buf b = {0};
  uint32_t c;
  const char *p = s ? s : "";
  while ((c = ipod_utf8_next(&p)) != 0) {
    if (c >= 0x10000) {
      c -= 0x10000;
      ipod_buf_le16(&b, (uint16_t)(0xD800 + (c >> 10)));
      ipod_buf_le16(&b, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      ipod_buf_le16(&b, (uint16_t)c);
    }
  }
  *out_len = b.len;
  ipod_buf_le16(&b, 0); /* so the result is never NULL for "" and is terminated */
  if (b.oom) {
    ipod_buf_free(&b);
    *out_len = 0;
    return NULL;
  }
  return b.data;
}

/* --- Files ------------------------------------------------------------------------ */

#ifdef _WIN32
static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
  if (!w) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}
#endif

FILE *ipod_fopen(const char *path, const char *mode) {
#ifdef _WIN32
  wchar_t *wp = widen(path), *wm = widen(mode);
  FILE *f = NULL;
  if (wp && wm) f = _wfopen(wp, wm);
  free(wp);
  free(wm);
  return f;
#else
  return fopen(path, mode);
#endif
}

int ipod_file_exists(const char *path) {
#ifdef _WIN32
  wchar_t *w = widen(path);
  if (!w) return 0;
  DWORD a = GetFileAttributesW(w);
  free(w);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int ipod_dir_exists(const char *path) {
#ifdef _WIN32
  wchar_t *w = widen(path);
  if (!w) return 0;
  DWORD a = GetFileAttributesW(w);
  free(w);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

int ipod_mkdir(const char *path) {
  if (ipod_dir_exists(path)) return 0;
#ifdef _WIN32
  wchar_t *w = widen(path);
  if (!w) return -1;
  BOOL ok = CreateDirectoryW(w, NULL);
  free(w);
  return ok || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1;
#else
  return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int ipod_mkdirs(const char *path) {
  char *p = ipod_strdup(path);
  if (!p) return -1;
  size_t n = strlen(p);
  for (size_t i = 1; i < n; i++) {
    if (p[i] == '/' || p[i] == '\\') {
      if (i == 2 && p[1] == ':') continue; /* "C:/" */
      char c = p[i];
      p[i] = 0;
      (void)ipod_mkdir(p);
      p[i] = c;
    }
  }
  int r = ipod_mkdir(p);
  free(p);
  return r;
}

int ipod_remove(const char *path) {
#ifdef _WIN32
  wchar_t *w = widen(path);
  if (!w) return -1;
  BOOL ok = DeleteFileW(w);
  DWORD e = ok ? 0 : GetLastError();
  free(w);
  return ok || e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;
#else
  return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
#endif
}

int ipod_rename_replace(const char *src, const char *dst) {
#ifdef _WIN32
  wchar_t *ws = widen(src), *wd = widen(dst);
  BOOL ok = ws && wd && MoveFileExW(ws, wd, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  free(ws);
  free(wd);
  return ok ? 0 : -1;
#else
  return rename(src, dst) == 0 ? 0 : -1;
#endif
}

uint8_t *ipod_read_file(const char *path, size_t *out_len) {
  FILE *f = ipod_fopen(path, "rb");
  if (!f) return NULL;
  ipod_buf b = {0};
  uint8_t tmp[65536];
  size_t n;
  while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) ipod_buf_put(&b, tmp, n);
  int bad = ferror(f) || b.oom;
  fclose(f);
  if (bad) {
    ipod_buf_free(&b);
    return NULL;
  }
  if (!b.data) b.data = calloc(1, 1); /* empty file: still a valid pointer */
  *out_len = b.len;
  return b.data;
}

static int flush_to_disk(FILE *f) {
  if (fflush(f) != 0) return -1;
#ifdef _WIN32
  return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
  return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

int ipod_write_file_atomic(const char *path, const void *data, size_t len, ipod_error *err) {
  char *tmp = ipod_asprintf("%s.tmp", path);
  if (!tmp) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  FILE *f = ipod_fopen(tmp, "wb");
  if (!f) {
    free(tmp);
    return ipod_seterr(err, IPOD_ERR_IO, "can't create %s.tmp", path);
  }
  int ok = (len == 0 || fwrite(data, 1, len, f) == len) && flush_to_disk(f) == 0;
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    ipod_remove(tmp);
    free(tmp);
    return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s (disk full?)", path);
  }
  if (ipod_rename_replace(tmp, path) != 0) {
    ipod_remove(tmp);
    free(tmp);
    return ipod_seterr(err, IPOD_ERR_IO, "can't replace %s", path);
  }
  free(tmp);
  return 0;
}

int ipod_copy_file(const char *src, const char *dst, ipod_error *err) {
  FILE *in = ipod_fopen(src, "rb");
  if (!in) return ipod_seterr(err, IPOD_ERR_IO, "can't open %s", src);
  FILE *out = ipod_fopen(dst, "wb");
  if (!out) {
    fclose(in);
    return ipod_seterr(err, IPOD_ERR_IO, "can't create %s", dst);
  }
  static const size_t chunk = 1 << 20;
  uint8_t *buf = malloc(chunk);
  int ok = buf != NULL;
  size_t n;
  while (ok && (n = fread(buf, 1, chunk, in)) > 0)
    if (fwrite(buf, 1, n, out) != n) ok = 0;
  if (ferror(in)) ok = 0;
  free(buf);
  fclose(in);
  if (ok) ok = flush_to_disk(out) == 0;
  ok = (fclose(out) == 0) && ok;
  if (!ok) {
    ipod_remove(dst);
    return ipod_seterr(err, IPOD_ERR_FULL, "can't copy to %s (disk full?)", dst);
  }
  return 0;
}

int64_t ipod_file_size(const char *path) {
#ifdef _WIN32
  wchar_t *w = widen(path);
  if (!w) return -1;
  WIN32_FILE_ATTRIBUTE_DATA d;
  BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
  free(w);
  if (!ok) return -1;
  return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
#else
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (int64_t)st.st_size;
#endif
}

char *ipod_path_join(const char *a, const char *b) {
  size_t n = strlen(a);
  if (n && (a[n - 1] == '/' || a[n - 1] == '\\')) return ipod_asprintf("%s%s", a, b);
  return ipod_asprintf("%s/%s", a, b);
}

/* --- Randomness ----------------------------------------------------------------------- */

static uint64_t splitmix(uint64_t *s) {
  uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void ipod_random_bytes(void *buf, size_t n) {
#ifdef _WIN32
  if (BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#else
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd >= 0) {
    size_t got = 0;
    while (got < n) {
      ssize_t r = read(fd, (uint8_t *)buf + got, n - got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    close(fd);
    if (got == n) return;
  }
#endif
  static uint64_t state;
  ipod_global_lock();
  if (!state) state = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&state ^ (uint64_t)clock() << 32;
  for (size_t i = 0; i < n; i += 8) {
    uint64_t v = splitmix(&state);
    size_t k = n - i < 8 ? n - i : 8;
    memcpy((uint8_t *)buf + i, &v, k);
  }
  ipod_global_unlock();
}

uint64_t ipod_random_u64(void) {
  uint64_t v;
  ipod_random_bytes(&v, sizeof v);
  return v;
}

/* --- Lock ------------------------------------------------------------------------------- */

#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
void ipod_global_lock(void) { AcquireSRWLockExclusive(&g_lock); }
void ipod_global_unlock(void) { ReleaseSRWLockExclusive(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
void ipod_global_lock(void) { pthread_mutex_lock(&g_lock); }
void ipod_global_unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif
