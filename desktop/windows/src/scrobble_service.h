/* The Scrobbling service dialog, opened by Settings' "Use a different service…": Last.fm (the
 * default), Libre.fm, or another service that speaks Last.fm's API, entered by hand (API
 * address, sign-in page, key and secret; addresses must start http:// or https://). The Linux
 * counterpart is desktop/linux/src/settings_view.c's service dialog.
 *
 * The dialog's template is built in memory, so it needs no resources.
 */
#ifndef KITHSONG_SCROBBLE_SERVICE_H
#define KITHSONG_SCROBBLE_SERVICE_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* What OK closed the dialog with, for mc_scrobbler_set_service(): malloc'd UTF-8, all NULL
 * for Last.fm. */
typedef struct scrobble_service_choice {
  char *api_url, *auth_url, *api_key, *api_secret;
} scrobble_service_choice;

/* Shows the dialog, modal over owner, starting from the service in use (current, with its
 * addresses, which fill the boxes for another service; either may be NULL). TRUE with *out
 * filled if closed with OK; free it with scrobble_service_choice_clear(). */
BOOL scrobble_service_show(HWND owner, mc_scrobbler_service current, const char *api_url, const char *auth_url,
                           scrobble_service_choice *out);
void scrobble_service_choice_clear(scrobble_service_choice *c);

#endif
