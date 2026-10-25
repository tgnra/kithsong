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
    case MC_OK: return "ok";
    case MC_ERR_UNIMPLEMENTED: return "not implemented";
    case MC_ERR_INVALID_ARG: return "invalid argument";
    case MC_ERR_NOMEM: return "out of memory";
    case MC_ERR_IO: return "I/O error";
    case MC_ERR_DB: return "database error";
    case MC_ERR_NET: return "network error";
    case MC_ERR_FORMAT: return "unsupported or corrupt file";
    case MC_ERR_NOT_FOUND: return "not found";
    case MC_ERR_LOCKED: return "locked";
    case MC_ERR_IN_GROUP: return "already in a sync group";
    case MC_ERR_LIBRARY_MISMATCH: return "not the sync group's library";
  }
  return "unknown error";
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
