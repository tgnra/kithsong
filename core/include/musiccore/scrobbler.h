/* Scrobbling: telling Last.fm, or a service with the same API such as Libre.fm, what's
 * being played.
 *
 * The account is the sync group's: signing in on one device signs in every member once they
 * sync, and signing out signs them all out. It's kept in sync.db (with the service it's on),
 * and stays when the device leaves its group. Each device scrobbles its own plays. They're
 * queued in sync.db, so plays made offline are sent when the service can be reached again;
 * plays made while signed out aren't kept.
 *
 * The player says when a play counts (MC_PLAYER_EVENT_PLAYED); the frontend passes it on
 * with mc_scrobbler_scrobble(), and each new track with mc_scrobbler_now_playing(). A worker
 * thread looks the track up and does the rest. Plays of tracks under 30 seconds, or with no
 * artist, aren't scrobbled, as Last.fm asks.
 *
 * Unlike most handles, a scrobbler may be used from any thread.
 */
#ifndef MUSICCORE_SCROBBLER_H
#define MUSICCORE_SCROBBLER_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_scrobbler mc_scrobbler;

/* Libre.fm's addresses, for mc_scrobbler_set_service(). It takes any API key. */
#define MC_SCROBBLER_LIBREFM_API_URL "https://libre.fm/2.0/"
#define MC_SCROBBLER_LIBREFM_AUTH_URL "https://libre.fm/api/auth/"

/* Starts scrobbling plays from library lib for this device, whose sync.db is in config_dir
 * (the per-device config dir, as for mc_sync_new()). lib must stay open while the scrobbler
 * exists, but is only read here: the scrobbler opens its own connections. */
mc_status mc_scrobbler_new(mc_library *lib, const char *config_dir, mc_scrobbler **out);
/* Queues plays not yet queued, then stops; waits for a call to the service in progress to
 * finish or time out. NULL is a no-op. */
void mc_scrobbler_free(mc_scrobbler *sc);

/* track_uid has started playing. Best effort: sent once, if signed in and online. */
mc_status mc_scrobbler_now_playing(mc_scrobbler *sc, const char *track_uid);
/* A play of track_uid counts (see mc_player_take_played()); started_ms is when it began,
 * in wall clock ms. Queued and sent when it can be. */
mc_status mc_scrobbler_scrobble(mc_scrobbler *sc, const char *track_uid, int64_t started_ms);

/* Signing in, in the user's browser. Both block on the network, so call them off the UI
 * thread. mc_scrobbler_auth_begin() gets a token from the service and returns the page
 * (free with mc_free()) where the user approves it. mc_scrobbler_auth_finish() then turns
 * the token into a session: MC_ERR_AUTH until the user has approved it, so poll it every few
 * seconds until MC_OK, or give up (the token lasts an hour). On MC_OK the account is saved,
 * and reaches the rest of the group when it next syncs. MC_ERR_AUTH from begin means the
 * service refused the API key, or the build has none for Last.fm. */
mc_status mc_scrobbler_auth_begin(mc_scrobbler *sc, char **out_url);
mc_status mc_scrobbler_auth_finish(mc_scrobbler *sc);
/* Signs out, here and across the group. Queued plays wait for the next sign-in. */
mc_status mc_scrobbler_sign_out(mc_scrobbler *sc);

/* Which service to scrobble to. NULLs mean Last.fm's addresses and the build's Last.fm API
 * key and secret; a service that wants its own key takes it here. Changing it signs out,
 * since a session belongs to the service and key that made it. Synced like the account. */
mc_status mc_scrobbler_set_service(mc_scrobbler *sc, const char *api_url, const char *auth_url,
                                   const char *api_key, const char *api_secret);

typedef struct mc_scrobbler_status {
  char *username;     /* NULL when signed out */
  char *api_url;      /* the service's */
  int pending;        /* plays queued here and not yet sent */
  mc_status last_error; /* of the last call to the service: MC_ERR_AUTH when it refused the
                         * session (sign in again), MC_ERR_NET when it couldn't be reached */
} mc_scrobbler_status;

/* A snapshot; cheap. Release with mc_scrobbler_status_clear(). */
mc_status mc_scrobbler_status_get(mc_scrobbler *sc, mc_scrobbler_status *out);
void mc_scrobbler_status_clear(mc_scrobbler_status *status);

#ifdef __cplusplus
}
#endif

#endif
