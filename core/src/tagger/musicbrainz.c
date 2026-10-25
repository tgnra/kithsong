/* MusicBrainz web service client (https://musicbrainz.org/doc/MusicBrainz_API).
 * No API key; the requirements are a descriptive User-Agent and at most 1 request/second. */
#include "tagger/musicbrainz.h"

#include <stdio.h>
#include <string.h>

#include <yyjson.h>

#include "musiccore/tagger.h"
#include "net/http.h"
#include "util/platform.h"

#define MB_API "https://musicbrainz.org/ws/2"

/* Radiohead: a stable, well-known artist for the connectivity check. */
#define MB_PING_ARTIST "a74b1b7f-71a5-4011-9441-d0b5e4122711"

/* Shared by every thread in the process: the limit is per client, not per request. */
static mc_ratelimit g_mb_rate;

void mc_musicbrainz_global_init(void) {
  mc_ratelimit_init(&g_mb_rate, 1100); /* a little over 1 s for clock slack */
}

static mc_status mb_get_json(const char *path, yyjson_doc **out) {
  char url[512];
  snprintf(url, sizeof url, MB_API "%s%cfmt=json", path, strchr(path, '?') ? '&' : '?');

  mc_ratelimit_wait(&g_mb_rate);

  mc_buffer body;
  long http = 0;
  mc_status st = mc_http_get(url, &body, &http);
  if (st != MC_OK) return st;

  if (http == 404) st = MC_ERR_NOT_FOUND;
  else if (http != 200) st = MC_ERR_NET;
  else if (!(*out = yyjson_read(body.data, body.len, 0))) st = MC_ERR_FORMAT;
  mc_buffer_free(&body);
  return st;
}

mc_status mc_musicbrainz_ping(char **out_name) {
  if (!out_name) return MC_ERR_INVALID_ARG;
  *out_name = NULL;

  yyjson_doc *doc;
  mc_status st = mb_get_json("/artist/" MB_PING_ARTIST, &doc);
  if (st != MC_OK) return st;

  const char *name = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "name"));
  if (!name) st = MC_ERR_FORMAT;
  else if (!(*out_name = mc_strdup(name))) st = MC_ERR_NOMEM;
  yyjson_doc_free(doc);
  return st;
}
