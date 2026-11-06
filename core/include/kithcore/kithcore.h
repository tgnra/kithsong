/* musiccore: shared library behind every Kithsong frontend.
 *
 * Conventions:
 *  - Every function returns mc_status (or a value plus an out-param) unless noted.
 *  - Objects are opaque handles: mc_xxx_new/open() to create, mc_xxx_free/close() to destroy.
 *  - All strings are UTF-8, including file paths on Windows.
 *  - Strings returned through out-params are owned by the caller; free with mc_free().
 *  - Handles are not thread-safe unless their header says so.
 */
#ifndef KITHCORE_H
#define KITHCORE_H

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
  MC_ERR_LIBRARY_MISMATCH = -11, /* a different library than the sync group's */
  MC_ERR_AUTH = -12,            /* a service refused the sign-in, session or API key */
  MC_ERR_IN_USE = -13,          /* a file is open (being played, sent...): try again once it isn't */
  MC_ERR_EXISTS = -14,          /* it's in the library already, as something else */
  MC_ERR_STRESS_LIBRARY = -15,  /* a stress test library neither syncs nor scrobbles (stress.h) */
  MC_ERR_CANCELLED = -16        /* the caller stopped it: nothing has changed (edit.h) */

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

#include "kithcore/play_log.h"
#include "kithcore/library.h"
#include "kithcore/settings.h"
#include "kithcore/library_move.h"
#include "kithcore/delete.h"
#include "kithcore/playlist.h"
#include "kithcore/smartlist.h"
#include "kithcore/search.h"
#include "kithcore/edit.h"
#include "kithcore/playback.h"
#include "kithcore/tagger.h"
#include "kithcore/transcode.h"
#include "kithcore/discovery.h"
#include "kithcore/sync.h"
#include "kithcore/sync_here.h"
#include "kithcore/scrobbler.h"
#include "kithcore/artcolors.h"
#include "kithcore/perf.h"
#include "kithcore/stress.h"

#include "kithcore/i18n.h"

#endif
