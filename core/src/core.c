#include "musiccore/musiccore.h"

#include <stdlib.h>

#include <curl/curl.h>

#include "config.h"
#include "tagger/coverart.h"
#include "tagger/musicbrainz.h"

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
  }
  return mc_str(MC_STR_STATUS_UNKNOWN);
}

mc_status mc_init(void) {
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return MC_ERR_NET;
  mc_musicbrainz_global_init();
  mc_coverart_global_init();
  return MC_OK;
}

void mc_shutdown(void) {
  curl_global_cleanup();
}

void mc_free(void *ptr) {
  free(ptr);
}
