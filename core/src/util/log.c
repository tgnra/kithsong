#include "util/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kithcore/kithcore.h"

/* Set before any thread logs (mc_log_set_handler()'s contract), so read without a lock. */
static mc_log_fn g_fn;
static void *g_user;

void mc_log_set_handler(mc_log_fn fn, void *user) {
  g_user = user;
  g_fn = fn;
}

void mc_log(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  mc_log_fn fn = g_fn;
  if (!fn) {
    fputs(line, stderr);
    return;
  }
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
  fn(g_user, line);
}
