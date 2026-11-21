#include "log_file.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "kithcore/kithcore.h"
#include "wstr.h"

#define LOG_FILE_MAX (1024 * 1024)

static struct {
  CRITICAL_SECTION lock;
  wchar_t path[MAX_PATH], old[MAX_PATH]; /* "" for none */
  HANDLE file; /* NULL until a line opens it */
  LONGLONG size;
} g_log;

static HANDLE open_log(void) {
  HANDLE f = CreateFileW(g_log.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (f == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER size = {0};
  g_log.size = GetFileSizeEx(f, &size) ? size.QuadPart : 0;
  return f;
}

static void write_line(const char *text, size_t len) {
  if (!g_log.path[0]) return;
  if (g_log.file && g_log.size + (LONGLONG)len > LOG_FILE_MAX) {
    CloseHandle(g_log.file);
    MoveFileExW(g_log.path, g_log.old, MOVEFILE_REPLACE_EXISTING);
    g_log.file = NULL;
  }
  if (!g_log.file) g_log.file = open_log();
  if (!g_log.file) return; /* tried again with the next line */
  DWORD wrote = 0;
  if (WriteFile(g_log.file, text, (DWORD)len, &wrote, NULL)) g_log.size += wrote;
}

static void on_log(void *user, const char *line) {
  (void)user;
  SYSTEMTIME t;
  GetLocalTime(&t);
  char text[2048];
  int n = snprintf(text, sizeof text, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                   t.wMinute, t.wSecond, t.wMilliseconds, line ? line : "");
  if (n < 0) return;
  if ((size_t)n >= sizeof text) { /* cut short, still ending the line */
    n = (int)sizeof text - 1;
    text[n - 2] = '\r';
    text[n - 1] = '\n';
    text[n] = '\0';
  }
  wchar_t wide[2048];
  wstr_from_utf8(wide, sizeof wide / sizeof *wide, text);
  OutputDebugStringW(wide);
  EnterCriticalSection(&g_log.lock);
  write_line(text, (size_t)n);
  LeaveCriticalSection(&g_log.lock);
}

void log_file_start(const char *config_dir) {
  InitializeCriticalSection(&g_log.lock);
  if (config_dir) {
    wchar_t dir[MAX_PATH];
    wstr_from_utf8(dir, MAX_PATH, config_dir);
    if (swprintf(g_log.path, MAX_PATH, L"%ls\\kithsong.log", dir) < 0 ||
        swprintf(g_log.old, MAX_PATH, L"%ls\\kithsong.1.log", dir) < 0)
      g_log.path[0] = L'\0';
  }
  mc_log_set_handler(on_log, NULL);
}
