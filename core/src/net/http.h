/* Minimal HTTPS client over libcurl + mbedTLS, using the embedded CA bundle.
 * Requires mc_init() to have been called. */
#ifndef MC_NET_HTTP_H
#define MC_NET_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"
#include "util/platform.h"

typedef struct mc_buffer {
  char *data; /* NUL-terminated */
  size_t len;
} mc_buffer;

void mc_buffer_free(mc_buffer *buf);

/* Percent-encodes s for use in a URL query (RFC 3986 unreserved characters pass through).
 * Returns a malloc'd string, or NULL on allocation failure. */
char *mc_url_escape(const char *s);

/* A way to give up on a request in progress from another thread: cancelled(user) is asked
 * at least once a second while the request runs (name lookup included), and once it returns
 * non-zero the request stops and fails with MC_ERR_NET, as if offline. It's called on the
 * requesting thread, so it must be safe there while other threads change what it reads. */
typedef struct mc_http_cancel {
  int (*cancelled)(void *user);
  void *user;
} mc_http_cancel;

/* GET url into *body. *http_status receives the HTTP status code (may be NULL).
 * Transport failures return MC_ERR_NET; any HTTP status counts as success. */
mc_status mc_http_get(const char *url, mc_buffer *body, long *http_status);

/* POSTs form (application/x-www-form-urlencoded, already escaped) to url; otherwise as
 * mc_http_get(). cancel may be NULL. */
mc_status mc_http_post_form(const char *url, const char *form, mc_buffer *body, long *http_status,
                            const mc_http_cancel *cancel);

/* Enforces a minimum interval between requests to one service, across threads.
 * MusicBrainz allows 1 request/second per client. */
typedef struct mc_ratelimit {
  mc_mutex lock;
  uint64_t next_ms;
  uint32_t interval_ms;
} mc_ratelimit;

void mc_ratelimit_init(mc_ratelimit *rl, uint32_t interval_ms);
/* Blocks until the caller may send its request. */
void mc_ratelimit_wait(mc_ratelimit *rl);

#endif
