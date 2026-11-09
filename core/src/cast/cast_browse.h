/* Finding Google Cast devices: browsing for "_googlecast._tcp.local." on a thread of its own,
 * over the mDNS sockets Kithsong's own discovery uses (discovery/msock.h). It only asks and
 * listens; it answers nothing. Internal to core. */
#ifndef MC_CAST_CAST_BROWSE_H
#define MC_CAST_CAST_BROWSE_H

#include <stddef.h>

#include "kithcore/cast.h"

typedef struct cast_browser cast_browser;

/* Called on the browser's thread with every device known, whenever that changes: one
 * appeared, changed (name, address, port) or went. The array is valid during the call. */
typedef void (*cast_browse_cb)(void *user, const mc_cast_device *devices, int count);

mc_status cast_browser_start(cast_browse_cb cb, void *user, cast_browser **out);
/* Stops the thread and frees b, reporting nothing more. NULL is a no-op. */
void cast_browser_stop(cast_browser *b);

/* What a device announces in its TXT record, for tests: fills id, name and model from
 * "key=value" entries (key and value as given, not NUL-terminated). */
void cast_browse_txt(mc_cast_device *device, const char *key, size_t key_len, const char *value,
                     size_t value_len);

#endif
