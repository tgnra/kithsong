#include "musiccore/musiccore.h"

#include <stdio.h>
#include <stdlib.h>

#include <curl/curl.h>

#include "config.h"
#include "library/edit_internal.h"
#include "net/http.h"
#include "tagger/coverart.h"
#include "tagger/musicbrainz.h"
#include "tagger/refresh.h"

const char *mc_version(void) {
  return MC_VERSION_STRING;
}

const char *mc_status_str(mc_status status) {
  switch (status) {
    case MC_OK: return mc_str(MC_STR_STATUS_OK);
    case MC_ERR_UNIMPLEMENTED: return mc_str(MC_STR_STATUS_UNIMPLEMENTED);
    case MC_ERR_INVALID_ARG: return mc_str(MC_STR_STATUS_INVALID_ARG);
    case MC_ERR_NOMEM: return mc_str(MC_STR_STATUS_NOMEM);
    case MC_ERR_IO: return mc_str(MC_STR_STATUS_IO);
    case MC_ERR_DB: return mc_str(MC_STR_STATUS_DB);
    case MC_ERR_NET: return mc_str(MC_STR_STATUS_NET);
    case MC_ERR_FORMAT: return mc_str(MC_STR_STATUS_FORMAT);
    case MC_ERR_NOT_FOUND: return mc_str(MC_STR_STATUS_NOT_FOUND);
    case MC_ERR_LOCKED: return mc_str(MC_STR_STATUS_LOCKED);
    case MC_ERR_IN_GROUP: return mc_str(MC_STR_STATUS_IN_GROUP);
    case MC_ERR_LIBRARY_MISMATCH: return mc_str(MC_STR_STATUS_LIBRARY_MISMATCH);
    case MC_ERR_AUTH: return mc_str(MC_STR_STATUS_AUTH);
    case MC_ERR_IN_USE: return mc_str(MC_STR_STATUS_IN_USE);
    case MC_ERR_EXISTS: return mc_str(MC_STR_STATUS_EXISTS);
    case MC_ERR_STRESS_LIBRARY: return mc_str(MC_STR_STATUS_STRESS_LIBRARY);
    case MC_ERR_CANCELLED: return mc_str(MC_STR_STATUS_CANCELLED);

  }
  return mc_str(MC_STR_STATUS_UNKNOWN);
}

mc_status mc_init(void) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return MC_ERR_NET;
  mc_http_global_init();
  mc_musicbrainz_global_init();
  mc_coverart_global_init();
  mc_refresh_global_init();
  mc_edit_global_init();
  return MC_OK;
}

void mc_shutdown(void) {
  mc_http_global_cleanup();
  curl_global_cleanup();
}

void mc_free(void *ptr) {
  free(ptr);
}

size_t mc_format_playing_time(char *buf, size_t size, long long ms) {
  if (ms <= 0) {
    if (size) buf[0] = '\0';
    return 0;
  }
  long long minutes = (ms + 30000) / 60000;
  long long hours = (minutes + 30) / 60;
  if (minutes < 1) minutes = 1;
  if (minutes < 60) return mc_format(buf, size, mc_str(MC_STR_DURATION_MINUTES), MC_D(minutes));
  if (hours < 100) return mc_format(buf, size, mc_str(MC_STR_DURATION_HOURS_MINUTES), MC_D(minutes / 60), MC_D(minutes % 60));
  return mc_format(buf, size, mc_str_plural(MC_STR_DURATION_HOURS, (long)hours), MC_D(hours));
}

size_t mc_format_total_time(char *buf, size_t size, long long ms, mc_group_digits_fn group) {
  long long hours = ((ms + 30000) / 60000 + 30) / 60;
  if (hours >= 1000) {
    char digits[32];
    group(hours, digits, sizeof digits);
    return mc_format(buf, size, mc_str_plural(MC_STR_DURATION_HOURS, (long)hours), MC_S(digits));
  }
  if (ms <= 0) return mc_format(buf, size, mc_str(MC_STR_DURATION_MINUTES), MC_D(0));
  return mc_format_playing_time(buf, size, ms);
}

size_t mc_format_size(char *buf, size_t size, long long bytes) {
  static const enum mc_str_id units[] = {MC_STR_SIZE_KB, MC_STR_SIZE_MB, MC_STR_SIZE_GB, MC_STR_SIZE_TB};
  if (bytes < 1024) return mc_format(buf, size, mc_str_plural(MC_STR_SIZE_BYTES, (long)bytes), MC_D(bytes));
  double v = (double)bytes / 1024.0;
  size_t u = 0;
  while (v >= 1024.0 && u + 1 < sizeof units / sizeof units[0]) v /= 1024.0, u++;
  char num[32];
  snprintf(num, sizeof num, v < 100.0 ? "%.1f" : "%.0f", v);
  return mc_format(buf, size, mc_str(units[u]), MC_S(num));
}
