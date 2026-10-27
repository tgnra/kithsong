/* musiccore: shared library behind every musicapp frontend.
 *
 * Conventions:
 *  - Every function returns mc_status (or a value plus an out-param) unless noted.
 *  - Objects are opaque handles: mc_xxx_new/open() to create, mc_xxx_free/close() to destroy.
 *  - All strings are UTF-8, including file paths on Windows.
 *  - Strings returned through out-params are owned by the caller; free with mc_free().
 *  - Handles are not thread-safe unless their header says so.
 */
#ifndef MUSICCORE_H
#define MUSICCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_status {
  MC_OK = 0,
  MC_ERR_UNIMPLEMENTED = -1,
  MC_ERR_INVALID_ARG = -2,
  MC_ERR_NOMEM = -3,
  MC_ERR_IO = -4,
  MC_ERR_DB = -5,
  MC_ERR_NET = -6,
  MC_ERR_FORMAT = -7,
  MC_ERR_NOT_FOUND = -8,
  MC_ERR_LOCKED = -9,           /* e.g. an archive device's sync profile */
  MC_ERR_IN_GROUP = -10,        /* the device must leave its sync group first */
  MC_ERR_LIBRARY_MISMATCH = -11 /* a different library than the sync group's */
} mc_status;

/* Length of a textual UUID including the terminating NUL. */
#define MC_UID_STRLEN 37

/* Library version as "major.minor.patch". Never fails, safe to call before mc_init(). */
const char *mc_version(void);
/* A short reason for status, in the UI language and lower case, to go inside a message:
 * "out of memory". */
const char *mc_status_str(mc_status status);

/* Process-wide setup/teardown (network stack). Call once from the main thread. */
mc_status mc_init(void);
void mc_shutdown(void);

/* Frees any string or buffer returned by musiccore. NULL is a no-op. */
void mc_free(void *ptr);

/* Writes a new time-ordered (v7) UUID such as "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e". */
mc_status mc_uid_new(char out[MC_UID_STRLEN]);

#ifdef __cplusplus
}
#endif

#include "musiccore/library.h"
#include "musiccore/settings.h"
#include "musiccore/playlist.h"
#include "musiccore/smartlist.h"
#include "musiccore/playback.h"
#include "musiccore/tagger.h"
#include "musiccore/transcode.h"
#include "musiccore/discovery.h"
#include "musiccore/sync.h"
#include "musiccore/i18n.h"

#endif
