/* The Last.fm 2.0 API, as Last.fm and compatible services (Libre.fm) speak it: signed POSTs
 * of form fields, JSON answers. Only what scrobbling needs. Internal to core. */
#ifndef MC_SCROBBLE_LASTFM_H
#define MC_SCROBBLE_LASTFM_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "net/http.h"

#define MC_LASTFM_API_URL "https://ws.audioscrobbler.com/2.0/"
#define MC_LASTFM_AUTH_URL "https://www.last.fm/api/auth/"
/* Scrobbles per track.scrobble call: the API's limit. */
#define MC_LASTFM_BATCH 50
/* Hex MD5 and its NUL. */
#define MC_LASTFM_SIG_STRLEN 33

typedef mc_status (*mc_lastfm_post_fn)(const char *url, const char *form, mc_buffer *body,
                                       long *http_status);
/* How calls are sent: mc_http_post_form(), unless a test swaps it before making any. */
extern mc_lastfm_post_fn mc_lastfm_post;

typedef struct mc_lastfm_service {
  const char *api_url;
  const char *api_key;     /* NULL: the build's (mc_lastfm_build_key()) */
  const char *api_secret;  /* NULL: the build's, decoded only while a call is signed */
  const char *session_key; /* NULL for the auth calls */
} mc_lastfm_service;

/* The build's own Last.fm API account (lastfm_key.c), kept obscured in the binary. Room for
 * either, with its NUL. */
#define MC_LASTFM_KEY_MAX 128
/* Whether the build has a key and secret. */
int mc_lastfm_build_key_present(void);
/* Decode the build's key or secret into out: 1, or 0 with out "" if there's none or it
 * doesn't fit. Wipe out with mc_lastfm_wipe() as soon as it's no longer needed. */
int mc_lastfm_build_key(char *out, size_t cap);
int mc_lastfm_build_secret(char *out, size_t cap);
/* Zeroes buf in a way the compiler can't leave out. */
void mc_lastfm_wipe(void *buf, size_t len);

typedef struct mc_lastfm_param {
  const char *name;
  const char *value;
} mc_lastfm_param;

/* api_sig: the MD5, in hex, of every name and value (sorted by name, in byte order)
 * concatenated, then the secret. */
void mc_lastfm_sign(const mc_lastfm_param *params, int count, const char *secret,
                    char out[MC_LASTFM_SIG_STRLEN]);

/* One play. Strings other than artist and track may be NULL, numbers 0 when unknown. */
typedef struct mc_scrobble {
  int64_t played_at_s; /* when it began, Unix time */
  const char *artist;
  const char *track;
  const char *album;
  const char *album_artist;
  const char *mb_trackid;
  int track_no;
  int duration_s;
} mc_scrobble;

/* Each call sets *api_error to the service's error code, 0 when it gave none (success, or
 * it couldn't be reached). Its status says what to do about it: MC_ERR_AUTH for a refused
 * key, session or token (including one the user hasn't approved yet), MC_ERR_NET for a
 * failure that may pass (offline, the service down or rate limiting), MC_ERR_FORMAT for a
 * request it won't take as it is. */

/* auth.getToken: a token for the user to approve at the auth URL. */
mc_status mc_lastfm_get_token(const mc_lastfm_service *svc, char **token, int *api_error);
/* auth.getSession: the session key and user name once the token has been approved. */
mc_status mc_lastfm_get_session(const mc_lastfm_service *svc, const char *token,
                                char **session_key, char **username, int *api_error);
/* track.updateNowPlaying. */
mc_status mc_lastfm_now_playing(const mc_lastfm_service *svc, const mc_scrobble *s,
                                int *api_error);
/* track.scrobble of count plays (at most MC_LASTFM_BATCH). On MC_OK, done[i] is 1 once play
 * i needn't be sent again: accepted, or ignored for good (say, too old); 0 if it was ignored
 * for now (over the daily limit). */
mc_status mc_lastfm_scrobble(const mc_lastfm_service *svc, const mc_scrobble *plays, int count,
                             int *done, int *api_error);

#endif
