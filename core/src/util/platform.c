#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* realpath */
#endif

#include "util/platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32

uint64_t mc_wall_ms(void) {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return t100ns / 10000u - UINT64_C(11644473600000); /* 1601 -> 1970 */
}

uint64_t mc_mono_ms(void) {
  return GetTickCount64();
}

void mc_sleep_ms(uint32_t ms) {
  Sleep(ms);
}

int mc_cpu_count(void) {
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

mc_status mc_random_bytes(void *buf, size_t len) {
  if (len > ULONG_MAX) return MC_ERR_INVALID_ARG;
  NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return st == 0 ? MC_OK : MC_ERR_IO;
}

void mc_mutex_init(mc_mutex *mutex) {
  InitializeSRWLock((PSRWLOCK)&mutex->srw);
}

void mc_mutex_destroy(mc_mutex *mutex) {
  (void)mutex; /* SRW locks need no cleanup */
}

void mc_mutex_lock(mc_mutex *mutex) {
  AcquireSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

void mc_mutex_unlock(mc_mutex *mutex) {
  ReleaseSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

void mc_cond_init(mc_cond *cond) {
  InitializeConditionVariable((PCONDITION_VARIABLE)&cond->cv);
}

void mc_cond_destroy(mc_cond *cond) {
  (void)cond;
}

void mc_cond_wait(mc_cond *cond, mc_mutex *mutex) {
  SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->cv, (PSRWLOCK)&mutex->srw, INFINITE, 0);
}

void mc_cond_wait_ms(mc_cond *cond, mc_mutex *mutex, uint32_t ms) {
  SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->cv, (PSRWLOCK)&mutex->srw, ms, 0);
}

void mc_cond_broadcast(mc_cond *cond) {
  WakeAllConditionVariable((PCONDITION_VARIABLE)&cond->cv);
}

typedef struct thread_start {
  void (*fn)(void *);
  void *arg;
} thread_start;

static unsigned __stdcall thread_trampoline(void *p) {
  thread_start s = *(thread_start *)p;
  free(p);
  s.fn(s.arg);
  return 0;
}

mc_status mc_thread_start(mc_thread *thread, void (*fn)(void *), void *arg) {
  thread_start *s = malloc(sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  s->fn = fn;
  s->arg = arg;
  uintptr_t h = _beginthreadex(NULL, 0, thread_trampoline, s, 0, NULL);
  if (!h) {
    free(s);
    return MC_ERR_NOMEM;
  }
  thread->handle = (void *)h;
  return MC_OK;
}

void mc_thread_join(mc_thread *thread) {
  WaitForSingleObject(thread->handle, INFINITE);
  CloseHandle(thread->handle);
}

/* UTF-8 -> UTF-16, malloc'd. */
static wchar_t *widen(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
  if (n <= 0) return NULL;
  wchar_t *w = malloc((size_t)n * sizeof *w);
  if (w) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n);
  return w;
}

static char *narrow(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (n <= 0) return NULL;
  char *s = malloc((size_t)n);
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

/* Returns non-zero if fn asked to stop. */
static int walk_dir(const wchar_t *dir, mc_walk_fn fn, void *user) {
  size_t len = wcslen(dir);
  wchar_t *pattern = malloc((len + 3) * sizeof *pattern);
  if (!pattern) return 0;
  memcpy(pattern, dir, len * sizeof *pattern);
  wcscpy(pattern + len, L"\\*");

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                              FIND_FIRST_EX_LARGE_FETCH);
  free(pattern);
  if (h == INVALID_HANDLE_VALUE) return 0;

  int stop = 0;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
    size_t name_len = wcslen(fd.cFileName);
    wchar_t *full = malloc((len + name_len + 2) * sizeof *full);
    if (!full) break;
    memcpy(full, dir, len * sizeof *full);
    full[len] = L'\\';
    memcpy(full + len + 1, fd.cFileName, (name_len + 1) * sizeof *full);

    /* Don't follow junctions or symlinks: they can loop. */
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      stop = walk_dir(full, fn, user);
    } else {
      char *utf8 = narrow(full);
      if (utf8) stop = fn(user, utf8);
      free(utf8);
    }
    free(full);
  } while (!stop && FindNextFileW(h, &fd));
  FindClose(h);
  return stop;
}

mc_status mc_walk(const char *path, mc_walk_fn fn, void *user) {
  wchar_t *w = widen(path);
  if (!w) return MC_ERR_INVALID_ARG;
  size_t len = wcslen(w);
  while (len > 3 && (w[len - 1] == L'\\' || w[len - 1] == L'/')) w[--len] = L'\0';

  mc_status st = MC_OK;
  DWORD attrs = GetFileAttributesW(w);
  if (attrs == INVALID_FILE_ATTRIBUTES) st = MC_ERR_NOT_FOUND;
  else if (attrs & FILE_ATTRIBUTE_DIRECTORY) walk_dir(w, fn, user);
  else fn(user, path);
  free(w);
  return st;
}

FILE *mc_fopen(const char *path, const char *mode) {
  wchar_t *wpath = widen(path), *wmode = widen(mode);
  FILE *f = wpath && wmode ? _wfopen(wpath, wmode) : NULL;
  free(wpath);
  free(wmode);
  return f;
}

mc_status mc_mkdirs(const char *path) {
  wchar_t *w = widen(path);
  if (!w) return MC_ERR_INVALID_ARG;
  /* Create each prefix in turn; failures on existing parents are expected. */
  for (wchar_t *p = w + 1; *p; p++) {
    if (*p != L'\\' && *p != L'/') continue;
    wchar_t c = *p;
    *p = L'\0';
    CreateDirectoryW(w, NULL);
    *p = c;
  }
  BOOL ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
  free(w);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_list_dir(const char *dir, mc_list_fn fn, void *user) {
  wchar_t *w = widen(dir);
  if (!w) return MC_ERR_INVALID_ARG;
  size_t len = wcslen(w);
  wchar_t *pattern = malloc((len + 3) * sizeof *pattern);
  if (!pattern) {
    free(w);
    return MC_ERR_NOMEM;
  }
  memcpy(pattern, w, len * sizeof *pattern);
  wcscpy(pattern + len, L"\\*");
  free(w);

  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
  free(pattern);
  if (h == INVALID_HANDLE_VALUE) return MC_ERR_NOT_FOUND;
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
    char *name = narrow(fd.cFileName);
    if (name) fn(user, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    free(name);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return MC_OK;
}

int mc_file_exists(const char *path) {
  wchar_t *w = widen(path);
  if (!w) return 0;
  DWORD attrs = GetFileAttributesW(w);
  free(w);
  return attrs != INVALID_FILE_ATTRIBUTES;
}

int mc_is_link(const char *path) {
  wchar_t *w = widen(path);
  if (!w) return 0;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return 0;
  FindClose(h);
  /* Other reparse points (cloud placeholders, dedup) keep the file where it is. */
  DWORD tag = fd.dwReserved0;
  return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

mc_status mc_copy_file(const char *src, const char *dst, int overwrite) {
  wchar_t *ws = widen(src), *wd = widen(dst);
  BOOL ok = ws && wd && CopyFileW(ws, wd, !overwrite);
  free(ws);
  free(wd);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_rename(const char *src, const char *dst, int replace) {
  wchar_t *ws = widen(src), *wd = widen(dst);
  DWORD flags = MOVEFILE_COPY_ALLOWED | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
  BOOL ok = ws && wd && MoveFileExW(ws, wd, flags);
  free(ws);
  free(wd);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_remove(const char *path) {
  wchar_t *w = widen(path);
  BOOL ok = w && DeleteFileW(w);
  free(w);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_rmdir(const char *path) {
  wchar_t *w = widen(path);
  BOOL ok = w && RemoveDirectoryW(w);
  free(w);
  return ok ? MC_OK : MC_ERR_IO;
}

int mc_same_path(const char *a, const char *b) {
  wchar_t *wa = widen(a), *wb = widen(b);
  int same = wa && wb && CompareStringOrdinal(wa, -1, wb, -1, TRUE) == CSTR_EQUAL;
  free(wa);
  free(wb);
  return same;
}

mc_status mc_rename_in_place(const char *src, const char *dst) {
  wchar_t *ws = widen(src), *wd = widen(dst);
  BOOL ok = ws && wd && MoveFileExW(ws, wd, 0); /* no MOVEFILE_COPY_ALLOWED */
  free(ws);
  free(wd);
  return ok ? MC_OK : MC_ERR_IO;
}

int mc_same_volume(const char *a, const char *b) {
  wchar_t *wa = widen(a), *wb = widen(b), va[MAX_PATH], vb[MAX_PATH];
  int same = wa && wb && GetVolumePathNameW(wa, va, MAX_PATH) && GetVolumePathNameW(wb, vb, MAX_PATH) &&
             CompareStringOrdinal(va, -1, vb, -1, TRUE) == CSTR_EQUAL;
  free(wa);
  free(wb);
  return same;
}

char *mc_full_path(const char *path) {
  wchar_t *w = widen(path);
  if (!w) return NULL;
  DWORD n = GetFullPathNameW(w, 0, NULL, NULL);
  wchar_t *full = n ? malloc(n * sizeof *full) : NULL;
  char *out = NULL;
  if (full && GetFullPathNameW(w, n, full, NULL) < n) {
    size_t len = wcslen(full);
    while (len > 3 && full[len - 1] == L'\\') full[--len] = L'\0'; /* but keep "C:\" */
    out = narrow(full);
  }
  free(full);
  free(w);
  return out;
}

mc_status mc_file_info(const char *path, int64_t *size, int64_t *mtime_ms) {
  wchar_t *w = widen(path);
  if (!w) return MC_ERR_INVALID_ARG;
  WIN32_FILE_ATTRIBUTE_DATA fad;
  BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &fad);
  free(w);
  if (!ok) return MC_ERR_NOT_FOUND;
  *size = (int64_t)(((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow);
  uint64_t t100ns = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                    fad.ftLastWriteTime.dwLowDateTime;
  *mtime_ms = (int64_t)(t100ns / 10000u - UINT64_C(11644473600000));
  return MC_OK;
}

mc_status mc_touch(const char *path) {
  wchar_t *w = widen(path);
  if (!w) return MC_ERR_INVALID_ARG;
  HANDLE h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return MC_ERR_NOT_FOUND;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  BOOL ok = SetFileTime(h, NULL, NULL, &now);
  CloseHandle(h);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_free_space(const char *path, int64_t *out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  wchar_t *w = widen(path);
  if (!w) return MC_ERR_INVALID_ARG;
  ULARGE_INTEGER avail;
  BOOL ok = GetDiskFreeSpaceExW(w, &avail, NULL, NULL);
  free(w);
  if (!ok) return MC_ERR_IO;
  *out = (int64_t)avail.QuadPart;
  return MC_OK;
}

mc_status mc_hostname(char *out, size_t cap) {
  if (!out || !cap) return MC_ERR_INVALID_ARG;
  out[0] = '\0';
  wchar_t name[256];
  DWORD n = sizeof name / sizeof name[0];
  if (!GetComputerNameExW(ComputerNameDnsHostname, name, &n)) return MC_ERR_IO;
  char *s = narrow(name);
  if (!s) return MC_ERR_NOMEM;
  size_t len = strlen(s);
  if (len >= cap) len = cap - 1;
  memcpy(out, s, len);
  out[len] = '\0';
  free(s);
  return MC_OK;
}

#else

static uint64_t timespec_ms(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t mc_wall_ms(void) {
  return timespec_ms(CLOCK_REALTIME);
}

uint64_t mc_mono_ms(void) {
  return timespec_ms(CLOCK_MONOTONIC);
}

void mc_sleep_ms(uint32_t ms) {
  struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

int mc_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int)n : 1;
}

mc_status mc_random_bytes(void *buf, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return MC_ERR_IO;
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close(fd);
      return MC_ERR_IO;
    }
    p += n;
    len -= (size_t)n;
  }
  close(fd);
  return MC_OK;
}

void mc_mutex_init(mc_mutex *mutex) {
  pthread_mutex_init(&mutex->m, NULL);
}

void mc_mutex_destroy(mc_mutex *mutex) {
  pthread_mutex_destroy(&mutex->m);
}

void mc_mutex_lock(mc_mutex *mutex) {
  pthread_mutex_lock(&mutex->m);
}

void mc_mutex_unlock(mc_mutex *mutex) {
  pthread_mutex_unlock(&mutex->m);
}

void mc_cond_init(mc_cond *cond) {
  pthread_cond_init(&cond->c, NULL);
}

void mc_cond_destroy(mc_cond *cond) {
  pthread_cond_destroy(&cond->c);
}

void mc_cond_wait(mc_cond *cond, mc_mutex *mutex) {
  pthread_cond_wait(&cond->c, &mutex->m);
}

void mc_cond_wait_ms(mc_cond *cond, mc_mutex *mutex, uint32_t ms) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += ms / 1000;
  ts.tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L) {
    ts.tv_sec++;
    ts.tv_nsec -= 1000000000L;
  }
  pthread_cond_timedwait(&cond->c, &mutex->m, &ts);
}

void mc_cond_broadcast(mc_cond *cond) {
  pthread_cond_broadcast(&cond->c);
}

typedef struct thread_start {
  void (*fn)(void *);
  void *arg;
} thread_start;

static void *thread_trampoline(void *p) {
  thread_start s = *(thread_start *)p;
  free(p);
  s.fn(s.arg);
  return NULL;
}

mc_status mc_thread_start(mc_thread *thread, void (*fn)(void *), void *arg) {
  thread_start *s = malloc(sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  s->fn = fn;
  s->arg = arg;
  if (pthread_create(&thread->t, NULL, thread_trampoline, s) != 0) {
    free(s);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

void mc_thread_join(mc_thread *thread) {
  pthread_join(thread->t, NULL);
}

static int walk_dir(const char *dir, mc_walk_fn fn, void *user) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  size_t len = strlen(dir);
  int stop = 0;
  struct dirent *e;
  while (!stop && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue; /* ".", ".." and hidden files */
    size_t name_len = strlen(e->d_name);
    char *full = malloc(len + name_len + 2);
    if (!full) break;
    memcpy(full, dir, len);
    full[len] = '/';
    memcpy(full + len + 1, e->d_name, name_len + 1);

    struct stat st; /* lstat: don't follow symlinks, they can loop */
    if (lstat(full, &st) == 0) {
      if (S_ISDIR(st.st_mode)) stop = walk_dir(full, fn, user);
      else if (S_ISREG(st.st_mode)) stop = fn(user, full);
    }
    free(full);
  }
  closedir(d);
  return stop;
}

mc_status mc_walk(const char *path, mc_walk_fn fn, void *user) {
  struct stat st;
  if (stat(path, &st) != 0) return MC_ERR_NOT_FOUND;
  if (!S_ISDIR(st.st_mode)) {
    fn(user, path);
    return MC_OK;
  }
  char *dir = mc_strdup(path);
  if (!dir) return MC_ERR_NOMEM;
  size_t len = strlen(dir);
  while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
  walk_dir(dir, fn, user);
  free(dir);
  return MC_OK;
}

FILE *mc_fopen(const char *path, const char *mode) {
  return fopen(path, mode);
}

mc_status mc_mkdirs(const char *path) {
  char *p = mc_strdup(path);
  if (!p) return MC_ERR_NOMEM;
  for (char *s = p + 1; *s; s++) {
    if (*s != '/') continue;
    *s = '\0';
    mkdir(p, 0755);
    *s = '/';
  }
  int ok = mkdir(p, 0755) == 0 || errno == EEXIST;
  free(p);
  return ok ? MC_OK : MC_ERR_IO;
}

mc_status mc_file_info(const char *path, int64_t *size, int64_t *mtime_ms) {
  struct stat st;
  if (stat(path, &st) != 0) return MC_ERR_NOT_FOUND;
  *size = (int64_t)st.st_size;
  *mtime_ms = (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
  return MC_OK;
}

mc_status mc_touch(const char *path) {
  if (utimensat(AT_FDCWD, path, NULL, 0) == 0) return MC_OK;
  return errno == ENOENT ? MC_ERR_NOT_FOUND : MC_ERR_IO;
}

mc_status mc_list_dir(const char *dir, mc_list_fn fn, void *user) {
  DIR *d = opendir(dir);
  if (!d) return MC_ERR_NOT_FOUND;
  size_t len = strlen(dir);
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    size_t name_len = strlen(e->d_name);
    char *full = malloc(len + name_len + 2);
    if (!full) break;
    snprintf(full, len + name_len + 2, "%s/%s", dir, e->d_name);
    struct stat st;
    int is_dir = lstat(full, &st) == 0 && S_ISDIR(st.st_mode);
    free(full);
    fn(user, e->d_name, is_dir);
  }
  closedir(d);
  return MC_OK;
}

int mc_file_exists(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0;
}

int mc_is_link(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

mc_status mc_copy_file(const char *src, const char *dst, int overwrite) {
  int in = open(src, O_RDONLY | O_CLOEXEC);
  if (in < 0) return MC_ERR_IO;
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (overwrite ? 0 : O_EXCL), 0644);
  if (out < 0) {
    close(in);
    return MC_ERR_IO;
  }
  char buf[1 << 16];
  mc_status st = MC_OK;
  for (;;) {
    ssize_t n = read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) st = MC_ERR_IO;
    if (n <= 0) break;
    for (ssize_t off = 0; off < n;) {
      ssize_t w = write(out, buf + off, (size_t)(n - off));
      if (w < 0 && errno == EINTR) continue;
      if (w < 0) {
        st = MC_ERR_IO;
        break;
      }
      off += w;
    }
    if (st != MC_OK) break;
  }
  close(in);
  if (close(out) != 0) st = MC_ERR_IO;
  if (st != MC_OK) unlink(dst);
  return st;
}

mc_status mc_rename(const char *src, const char *dst, int replace) {
  if (!replace && mc_file_exists(dst)) return MC_ERR_IO;
  if (rename(src, dst) == 0) return MC_OK;
  if (errno != EXDEV) return MC_ERR_IO;
  /* Different file systems: copy, then remove the original. */
  mc_status st = mc_copy_file(src, dst, replace);
  if (st == MC_OK && unlink(src) != 0) st = MC_ERR_IO;
  return st;
}

mc_status mc_remove(const char *path) {
  return unlink(path) == 0 ? MC_OK : MC_ERR_IO;
}

mc_status mc_rmdir(const char *path) {
  return rmdir(path) == 0 ? MC_OK : MC_ERR_IO;
}

int mc_same_path(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

mc_status mc_rename_in_place(const char *src, const char *dst) {
  if (mc_file_exists(dst)) return MC_ERR_IO;
  return rename(src, dst) == 0 ? MC_OK : MC_ERR_IO;
}

int mc_same_volume(const char *a, const char *b) {
  struct stat sa, sb;
  return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev;
}

char *mc_full_path(const char *path) {
  if (!path || !*path) return NULL;
  char *abs;
  if (path[0] == '/') {
    abs = mc_strdup(path);
  } else {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return NULL;
    size_t n = strlen(cwd) + strlen(path) + 2;
    if ((abs = malloc(n))) snprintf(abs, n, "%s/%s", cwd, path);
  }
  if (!abs) return NULL;
  /* Resolve the longest part that exists, then add the rest back on. */
  size_t len = strlen(abs);
  while (len > 1 && abs[len - 1] == '/') abs[--len] = '\0';
  char *cut = abs + len, real[PATH_MAX];
  for (;;) {
    char saved = *cut;
    *cut = '\0';
    int ok = realpath(cut == abs ? "/" : abs, real) != NULL;
    *cut = saved;
    if (ok) break;
    while (cut > abs && *--cut != '/') {}
  }
  const char *rest = cut;
  size_t n = strlen(real) + strlen(rest) + 1;
  char *out = malloc(n);
  if (out) snprintf(out, n, "%s%s", strcmp(real, "/") || !*rest ? real : "", rest);
  free(abs);
  return out;
}

mc_status mc_free_space(const char *path, int64_t *out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  struct statvfs sv;
  if (statvfs(path, &sv) != 0) return MC_ERR_IO;
  *out = (int64_t)sv.f_bavail * (int64_t)sv.f_frsize;
  return MC_OK;
}

mc_status mc_hostname(char *out, size_t cap) {
  if (!out || !cap) return MC_ERR_INVALID_ARG;
  char name[256];
  out[0] = '\0';
  if (gethostname(name, sizeof name) != 0) return MC_ERR_IO;
  name[sizeof name - 1] = '\0';
  size_t len = strlen(name);
  if (len >= cap) len = cap - 1;
  memcpy(out, name, len);
  out[len] = '\0';
  return MC_OK;
}

#endif

char *mc_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (copy) memcpy(copy, s, n);
  return copy;
}
