/* MusicBrainz web service client (https://musicbrainz.org/doc/MusicBrainz_API).
 * No API key; the requirements are a descriptive User-Agent and at most 1 request/second. */
#include "tagger/musicbrainz.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "musiccore/tagger.h"
#include "net/http.h"
#include "net/socket.h"
#include "tagger/search_keys.h"
#include "util/platform.h"

#define MB_HOST "musicbrainz.org"
#define MB_API "https://" MB_HOST "/ws/2"

/* Radiohead: a stable, well-known artist for the connectivity check. */
#define MB_PING_ARTIST "a74b1b7f-71a5-4011-9441-d0b5e4122711"

/* Shared by every thread in the process: the limit is per client, not per request. */
static mc_ratelimit g_mb_rate;

/* Set by tests (mb_set_fake). */
static const mb_fake *g_fake;

void mb_set_fake(const mb_fake *fake) {
  g_fake = fake;
}

int mb_tracing(void) {
  static int on = -1; /* the environment doesn't change, so a race only reads it twice */
  if (on < 0) {
    const char *v = getenv("KITHSONG_TAGGER_TRACE");
    on = v && *v && strcmp(v, "0") != 0;
  }
  return on;
}

void mb_trace(const char *fmt, ...) {
  if (!mb_tracing()) return;
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fprintf(stderr, "[tagger %6.1f] %s\n", (double)(mc_mono_ms() % 1000000) / 1000.0, line);
  fflush(stderr);
}

void mc_musicbrainz_global_init(void) {
  mc_ratelimit_init(&g_mb_rate, 1100); /* a little over 1 s for clock slack */
}

/* A request that fails for a reason that passes (a dropped connection, a timeout, or
 * MusicBrainz busy: 502, 503, 504) is tried again: after 2 s, doubling up to 30 s. Its
 * servers drop a good share of new connections at times, so giving up at the first failure
 * would put albums in review that only needed another try. Every third failure in a row
 * checks whether MusicBrainz can be reached at all and stops if it can't, so an offline
 * import is told so instead; and after 10 minutes of failures it stops anyway. */
#define RETRY_FIRST_MS 2000u
#define RETRY_MAX_MS 30000u
#define RETRY_GIVE_UP_MS (10u * 60u * 1000u)
#define RETRY_CHECK_EVERY 3
#define RETRY_CHECK_MS 8000

static int call_cancelled(const mc_http_cancel *cancel) {
  return cancel && cancel->cancelled && cancel->cancelled(cancel->user);
}

/* Waits ms, or less if cancel says so. Returns nonzero if cancelled. */
static int wait_cancel(uint32_t ms, const mc_http_cancel *cancel) {
  for (uint32_t waited = 0; waited < ms; waited += 100) {
    if (call_cancelled(cancel)) return 1;
    mc_sleep_ms(ms - waited < 100 ? ms - waited : 100);
  }
  return call_cancelled(cancel);
}

/* call may be NULL (see mb_call). */
static mc_status mb_get_json(const char *path, yyjson_doc **out, mb_call *call) {
  const mc_http_cancel *cancel = call ? call->cancel : NULL;
  char *why = call ? call->why : NULL;
  size_t why_len = call ? sizeof call->why : 0;
  size_t len = strlen(MB_API) + strlen(path) + 16;
  char *url = malloc(len);
  if (!url) return MC_ERR_NOMEM;
  snprintf(url, len, MB_API "%s%cfmt=json", path, strchr(path, '?') ? '&' : '?');

  mc_buffer body;
  long http;
  mc_status st;
  uint64_t first_failure = 0;
  uint32_t delay = RETRY_FIRST_MS;
  for (int failures = 0;;) {
    if (mc_ratelimit_wait_cancel(&g_mb_rate, cancel)) {
      free(url);
      return MC_ERR_NET;
    }
    http = 0;
    uint64_t t0 = mc_mono_ms();
    st = mc_http_get_cancel(url, &body, &http, cancel, why, why_len);
    mb_trace("GET %s -> %s %ld (%llu ms)", url, st == MC_OK ? "HTTP" : "failed", http,
             (unsigned long long)(mc_mono_ms() - t0));
    int busy = st == MC_OK && (http == 502 || http == 503 || http == 504);
    if (call_cancelled(cancel) || (st != MC_ERR_NET && !busy)) break;

    if (busy) {
      mc_buffer_free(&body);
      if (why_len) snprintf(why, why_len, "HTTP %ld, MusicBrainz is busy", http);
    }
    uint64_t now = mc_mono_ms();
    if (failures++ == 0) first_failure = now;
    int give_up = now - first_failure >= RETRY_GIVE_UP_MS;
    /* MusicBrainz can't be reached at all: why says so now. */
    if (!give_up && failures % RETRY_CHECK_EVERY == 0)
      give_up = mb_reachable_why(RETRY_CHECK_MS, cancel, why, why_len) != MC_OK;
    if (give_up) {
      mb_trace("giving up on %s after %d failures: %s", url, failures, why ? why : "");
      free(url);
      return MC_ERR_NET;
    }
    mb_trace("retrying in %u ms (failure %d)", delay, failures);
    if (wait_cancel(delay, cancel)) {
      free(url);
      return MC_ERR_NET;
    }
    delay = delay * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : delay * 2;
  }
  if (st == MC_OK && call_cancelled(cancel)) {
    mc_buffer_free(&body);
    st = MC_ERR_NET;
  }
  if (st == MC_OK && http != 200 && http != 404)
    fprintf(stderr, "musiccore: MusicBrainz returned HTTP %ld for %s\n", http, url);
  free(url);
  if (st != MC_OK) return st;

  if (http == 404) st = MC_ERR_NOT_FOUND;
  else if (http != 200) {
    st = MC_ERR_NET;
    if (why_len) snprintf(why, why_len, "HTTP %ld", http);
  }
  else if (!(*out = yyjson_read(body.data, body.len, 0))) st = MC_ERR_FORMAT;
  mc_buffer_free(&body);
  return st;
}

mc_status mc_musicbrainz_ping(char **out_name) {
  if (!out_name) return MC_ERR_INVALID_ARG;
  *out_name = NULL;

  yyjson_doc *doc;
  mc_status st = mb_get_json("/artist/" MB_PING_ARTIST, &doc, NULL);
  if (st != MC_OK) return st;

  const char *name = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "name"));
  if (!name) st = MC_ERR_FORMAT;
  else if (!(*out_name = mc_strdup(name))) st = MC_ERR_NOMEM;
  yyjson_doc_free(doc);
  return st;
}

mc_status mc_musicbrainz_reachable(int timeout_ms) {
  return mb_reachable_why(timeout_ms, NULL, NULL, 0);
}

mc_status mb_reachable_why(int timeout_ms, const mc_http_cancel *cancel, char *why, size_t why_len) {
  if (g_fake) return MC_OK;
  char host[256];
  uint16_t port = 443;
  snprintf(host, sizeof host, "%s", MB_HOST);
  /* host:port; the last colon, so an IPv6 literal needs brackets left off and a port given. */
  const char *probe = getenv("KITHSONG_MUSICBRAINZ_PROBE");
  const char *colon = probe ? strrchr(probe, ':') : NULL;
  if (colon && colon > probe && (size_t)(colon - probe) < sizeof host) {
    snprintf(host, sizeof host, "%.*s", (int)(colon - probe), probe);
    port = (uint16_t)atoi(colon + 1);
  }
  mc_status st = mc_socket_probe(host, port, timeout_ms, cancel ? cancel->cancelled : NULL,
                                 cancel ? cancel->user : NULL, why, why_len);
  return st == MC_OK ? MC_OK : MC_ERR_NET;
}

/* --- JSON helpers --- */

static char *json_strdup(yyjson_val *obj, const char *key) {
  const char *s = yyjson_get_str(yyjson_obj_get(obj, key));
  return s && *s ? mc_strdup(s) : NULL;
}

static int json_int(yyjson_val *obj, const char *key) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  return yyjson_is_num(v) ? (int)yyjson_get_num(v) : 0;
}

/* "2003-05-01" -> 2003. */
static int json_year(yyjson_val *obj, const char *key) {
  const char *s = yyjson_get_str(yyjson_obj_get(obj, key));
  return s ? atoi(s) : 0;
}

typedef struct credit {
  char *text;       /* "A feat. B" */
  char *first_name; /* the first credited artist */
  char *first_id;
  char *first_sort;
  int count;
  mb_credit parts;
} credit;

static char *credit_string(yyjson_val *credits);

void mb_credit_clear(mb_credit *c) {
  if (!c) return;
  for (int i = 0; i < c->count; i++) {
    free(c->parts[i].name);
    free(c->parts[i].id);
    free(c->parts[i].sort_name);
    free(c->parts[i].join);
    free(c->parts[i].artist_name);
  }
  free(c->parts);
  c->parts = NULL;
  c->count = 0;
}

/* The array as it is, entry by entry; never re-split. */
static mb_credit credit_parts(yyjson_val *credits) {
  mb_credit c = {0};
  size_t n = yyjson_arr_size(credits), idx, max;
  if (!n || !(c.parts = calloc(n, sizeof *c.parts))) return c;
  yyjson_val *v;
  yyjson_arr_foreach(credits, idx, max, v) {
    yyjson_val *a = yyjson_obj_get(v, "artist");
    mb_credit_part *p = &c.parts[c.count];
    p->name = json_strdup(v, "name");
    if (!p->name) p->name = json_strdup(a, "name");
    if (!p->name) continue; /* nothing to show for it */
    p->id = json_strdup(a, "id");
    p->sort_name = json_strdup(a, "sort-name");
    p->join = json_strdup(v, "joinphrase");
    const char *own = yyjson_get_str(yyjson_obj_get(a, "name"));
    if (own && strcmp(own, p->name) != 0) p->artist_name = mc_strdup(own);
    c.count++;
  }
  if (c.count) {
    /* Nothing follows the last one. */
    free(c.parts[c.count - 1].join);
    c.parts[c.count - 1].join = NULL;
  } else {
    free(c.parts);
    c.parts = NULL;
  }
  return c;
}

static mb_credit credit_copy(const mb_credit *from) {
  mb_credit c = {0};
  if (!from->count || !(c.parts = calloc((size_t)from->count, sizeof *c.parts))) return c;
  for (int i = 0; i < from->count; i++) {
    const mb_credit_part *f = &from->parts[i];
    c.parts[i] = (mb_credit_part){mc_strdup(f->name), mc_strdup(f->id), mc_strdup(f->sort_name),
                                  mc_strdup(f->join), mc_strdup(f->artist_name)};
  }
  c.count = from->count;
  return c;
}

static credit parse_credit(yyjson_val *credits) {
  credit c = {0};
  yyjson_val *first = yyjson_obj_get(yyjson_arr_get_first(credits), "artist");
  c.text = credit_string(credits);
  c.first_name = json_strdup(first, "name");
  c.first_id = json_strdup(first, "id");
  c.first_sort = json_strdup(first, "sort-name");
  c.count = (int)yyjson_arr_size(credits);
  c.parts = credit_parts(credits);
  return c;
}

/* Fills a release's artist fields, taking ownership of the credit's strings. */
static void set_release_artist(mb_release *rel, credit c) {
  rel->artist = c.text;
  rel->primary_artist = c.first_name ? c.first_name : mc_strdup(c.text);
  rel->primary_artist_id = c.first_id;
  rel->primary_artist_sort = c.first_sort;
  rel->credit = c.parts;
  if (c.count == 1) {
    rel->artist_id = mc_strdup(c.first_id);
    rel->artist_sort = mc_strdup(c.first_sort);
  }
}

static void set_track_artist(mb_track *t, credit c) {
  t->artist = c.text;
  t->artist_id = c.count == 1 ? c.first_id : NULL;
  t->credit = c.parts;
  if (c.count != 1) free(c.first_id);
  free(c.first_name);
  free(c.first_sort);
}

/* Joins an artist-credit array into the credited string, e.g. "A feat. B". */
static char *credit_string(yyjson_val *credits) {
  size_t len = 1;
  size_t idx, max;
  yyjson_val *c;
  yyjson_arr_foreach(credits, idx, max, c) {
    const char *name = yyjson_get_str(yyjson_obj_get(c, "name"));
    const char *join = yyjson_get_str(yyjson_obj_get(c, "joinphrase"));
    len += (name ? strlen(name) : 0) + (join ? strlen(join) : 0);
  }
  char *out = malloc(len);
  if (!out) return NULL;
  out[0] = '\0';
  yyjson_arr_foreach(credits, idx, max, c) {
    const char *name = yyjson_get_str(yyjson_obj_get(c, "name"));
    const char *join = yyjson_get_str(yyjson_obj_get(c, "joinphrase"));
    if (name) strcat(out, name);
    if (join) strcat(out, join);
  }
  if (!*out) {
    free(out);
    return NULL;
  }
  return out;
}

/* "CD", "2×CD", "CD + DVD": consecutive media of the same format are counted. */
static char *media_formats(yyjson_val *media) {
  char buf[256] = "";
  size_t idx, max;
  yyjson_val *m;
  const char *prev = NULL;
  int count = 0;
  for (idx = 0, max = yyjson_arr_size(media); idx <= max; idx++) {
    const char *fmt = NULL;
    if (idx < max) {
      m = yyjson_arr_get(media, idx);
      fmt = yyjson_get_str(yyjson_obj_get(m, "format"));
      if (!fmt) fmt = "Unknown";
      if (prev && strcmp(prev, fmt) == 0) {
        count++;
        continue;
      }
    }
    if (prev) {
      size_t used = strlen(buf);
      if (count > 1)
        snprintf(buf + used, sizeof buf - used, "%s%d\xC3\x97%s", used ? " + " : "", count, prev);
      else
        snprintf(buf + used, sizeof buf - used, "%s%s", used ? " + " : "", prev);
    }
    prev = fmt;
    count = 1;
  }
  return *buf ? mc_strdup(buf) : NULL;
}

/* Medium formats that hold video (https://musicbrainz.org/doc/Release/Format): "DVD-Video",
 * "DVD-R Video", the DVD-Video sides of DVDplus and DualDisc, Blu-ray, tapes and the rest. A
 * plain "DVD" can be either, so release_from goes by its recordings' flags; so do CDV and
 * Enhanced CD, which carry audio tracks beside a video. */
static int video_format(const char *fmt) {
  static const char *const formats[] = {"Blu-ray", "Blu-ray-R", "HD-DVD", "VCD",     "SVCD",
                                        "UMD",     "VHS",       "VHD",    "Betamax", "CED"};
  if (!fmt) return 0;
  if (strstr(fmt, "Video")) return 1;
  size_t n = strlen(fmt);
  if (n >= 9 && strcmp(fmt + n - 9, "LaserDisc") == 0) return 1; /* also 8" and 12" */
  for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++)
    if (strcmp(fmt, formats[i]) == 0) return 1;
  return 0;
}

static int recording_is_video(yyjson_val *track) {
  return yyjson_get_bool(yyjson_obj_get(yyjson_obj_get(track, "recording"), "video"));
}

void mb_release_clear(mb_release *rel) {
  if (!rel) return;
  for (int i = 0; i < rel->track_count; i++) {
    mb_track *t = &rel->tracks[i];
    free(t->id);
    free(t->recording_id);
    free(t->title);
    free(t->artist);
    free(t->artist_id);
    mb_credit_clear(&t->credit);
  }
  free(rel->tracks);
  mb_credit_clear(&rel->credit);
  for (int i = 0; i < rel->label_count; i++) free(rel->labels[i]);
  free(rel->labels);
  char *strs[] = {rel->id,           rel->title,   rel->artist,  rel->artist_id,
                  rel->artist_sort,  rel->primary_artist,      rel->primary_artist_id,
                  rel->primary_artist_sort,       rel->releasegroup_id,
                  rel->release_type, rel->country, rel->label,   rel->catalog, rel->format};
  for (size_t i = 0; i < sizeof strs / sizeof strs[0]; i++) free(strs[i]);
  memset(rel, 0, sizeof *rel);
}

/* Escapes Lucene query syntax so user text is searched literally. */
static char *lucene_escape(const char *s) {
  static const char special[] = "+-&|!(){}[]^\"~*?:\\/";
  char *out = malloc(strlen(s) * 2 + 1);
  if (!out) return NULL;
  char *p = out;
  for (; *s; s++) {
    if (strchr(special, *s)) *p++ = '\\';
    *p++ = *s;
  }
  *p = '\0';
  return out;
}

/* Builds "<path>?query=<url-escaped lucene>&limit=<n>" from field:(value) pairs. */
static char *search_path(const char *entity, const char *field1, const char *value1,
                         const char *field2, const char *value2, int limit) {
  char *v1 = lucene_escape(value1);
  char *v2 = value2 && *value2 ? lucene_escape(value2) : NULL;
  char *path = NULL, *esc = NULL;
  size_t qlen = (v1 ? strlen(v1) : 0) + (v2 ? strlen(v2) : 0) + 64;
  char *query = malloc(qlen);
  if (!v1 || (value2 && *value2 && !v2) || !query) goto done;

  if (v2) snprintf(query, qlen, "%s:(%s) AND %s:(%s)", field1, v1, field2, v2);
  else snprintf(query, qlen, "%s:(%s)", field1, v1);
  if (!(esc = mc_url_escape(query))) goto done;

  size_t plen = strlen(entity) + strlen(esc) + 32;
  if ((path = malloc(plen))) snprintf(path, plen, "/%s?query=%s&limit=%d", entity, esc, limit);

done:
  free(v1);
  free(v2);
  free(query);
  free(esc);
  return path;
}

/* Ids come from file tags, so check them before they go into a URL. */
static int is_mbid(const char *id) {
  if (!id || strlen(id) != MC_UID_STRLEN - 1) return 0;
  for (int i = 0; i < MC_UID_STRLEN - 1; i++) {
    char c = id[i];
    int dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return 1;
}

/* --- Releases --- */

static int earlier(yyjson_val *a, yyjson_val *b);

/* Search-result order: right track count, then higher score, then earlier date. */
static int better_hit(yyjson_val *a, yyjson_val *b, int track_count) {
  if (track_count > 0) {
    int ca = json_int(a, "track-count") == track_count, cb = json_int(b, "track-count") == track_count;
    if (ca != cb) return ca;
  }
  int sa = json_int(a, "score"), sb = json_int(b, "score");
  if (sa != sb) return sa > sb;
  return earlier(a, b);
}

/* Runs a release search and fills ids with the hits, ranked (see better_hit). */
static mc_status release_ids_from(const char *path, int track_count, char (*ids)[MC_UID_STRLEN],
                                  int max, int *n, mb_call *call) {
  yyjson_doc *doc;
  mc_status st = mb_get_json(path, &doc, call);
  if (st != MC_OK) return st;

  /* Rank: expected track count first, then search score, then earliest date, so that among
   * equally good hits the original release beats later reissues. */
  yyjson_val *releases = yyjson_obj_get(yyjson_doc_get_root(doc), "releases");
  size_t count = yyjson_arr_size(releases);
  yyjson_val **hits = malloc((count + 1) * sizeof *hits);
  if (!hits) {
    yyjson_doc_free(doc);
    return MC_ERR_NOMEM;
  }
  for (size_t i = 0; i < count; i++) hits[i] = yyjson_arr_get(releases, i);
  for (size_t i = 1; i < count; i++) {
    yyjson_val *v = hits[i];
    size_t j = i;
    while (j > 0 && better_hit(v, hits[j - 1], track_count)) {
      hits[j] = hits[j - 1];
      j--;
    }
    hits[j] = v;
  }
  for (size_t i = 0; i < count && *n < max; i++) {
    const char *id = yyjson_get_str(yyjson_obj_get(hits[i], "id"));
    if (is_mbid(id)) memcpy(ids[(*n)++], id, MC_UID_STRLEN);
  }
  free(hits);
  yyjson_doc_free(doc);
  return MC_OK;
}

mc_status mb_search_releases(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call) {
  *n = 0;
  if (!album || !*album) return MC_ERR_INVALID_ARG;
  /* "(Promo CDr)" and the like only narrow the search; the scoring still sees them. */
  char *terms = mc_search_title(album);
  if (!terms) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  if (g_fake) {
    if (g_fake->search_releases) st = g_fake->search_releases(artist, terms, track_count, ids, max, n);
  } else {
    char *path = search_path("release", "release", terms, "artist", artist, 15);
    st = path ? release_ids_from(path, track_count, ids, max, n, call) : MC_ERR_NOMEM;
    free(path);
  }
  free(terms);
  return st;
}

/* Appends " AND field:(v1 OR v2 ...)" (without the AND for the first clause) to q. */
static int append_clause(char *q, size_t cap, const char *field, const char *const *values,
                         int count) {
  if (count <= 0) return 1;
  size_t used = strlen(q);
  int w = snprintf(q + used, cap - used, "%s%s:(", used ? " AND " : "", field);
  if (w < 0 || (size_t)w >= cap - used) return 0;
  used += (size_t)w;
  for (int i = 0; i < count; i++) {
    char *v = lucene_escape(values[i]);
    if (!v) return 0;
    w = snprintf(q + used, cap - used, "%s%s", i ? " OR " : "", v);
    free(v);
    if (w < 0 || (size_t)w >= cap - used) return 0;
    used += (size_t)w;
  }
  if (used + 2 > cap) return 0;
  q[used++] = ')';
  q[used] = '\0';
  return 1;
}

static mc_status search_releases_by(const mb_release_query *q, int track_count,
                                    char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call);

mc_status mb_search_releases_by(const mb_release_query *q, int track_count,
                                char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call) {
  *n = 0;
  if (!q) return MC_ERR_INVALID_ARG;
  /* As in mb_search_releases: the title without its edition notes. */
  mb_release_query terms = *q;
  char *album = q->album ? mc_search_title(q->album) : NULL;
  if (q->album && !album) return MC_ERR_NOMEM;
  terms.album = album;
  mc_status st = search_releases_by(&terms, track_count, ids, max, n, call);
  free(album);
  return st;
}

static mc_status search_releases_by(const mb_release_query *q, int track_count,
                                    char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call) {
  if (g_fake)
    return g_fake->search_releases_by ? g_fake->search_releases_by(q, track_count, ids, max, n) : MC_OK;
  size_t cap = 64;
  if (q->album) cap += strlen(q->album) * 2 + 16;
  if (q->artist) cap += strlen(q->artist) * 2 + 16;
  for (int i = 0; i < q->label_count; i++) cap += strlen(q->labels[i]) * 2 + 16;
  for (int i = 0; i < q->catno_count; i++) cap += strlen(q->catnos[i]) * 2 + 16;
  char *query = malloc(cap);
  if (!query) return MC_ERR_NOMEM;
  query[0] = '\0';
  const char *album[1] = {q->album}, *artist[1] = {q->artist};
  int ok = append_clause(query, cap, "release", album, q->album && *q->album) &&
           append_clause(query, cap, "artist", artist, q->artist && *q->artist) &&
           append_clause(query, cap, "label", q->labels, q->label_count) &&
           append_clause(query, cap, "catno", q->catnos, q->catno_count);
  if (!ok || !*query) {
    free(query);
    return ok ? MC_ERR_INVALID_ARG : MC_ERR_NOMEM;
  }
  char *esc = mc_url_escape(query);
  free(query);
  if (!esc) return MC_ERR_NOMEM;
  size_t plen = strlen(esc) + 48;
  char *path = malloc(plen);
  mc_status st = MC_ERR_NOMEM;
  if (path) {
    snprintf(path, plen, "/release?query=%s&limit=15", esc);
    st = release_ids_from(path, track_count, ids, max, n, call);
  }
  free(path);
  free(esc);
  return st;
}

static mc_status release_from(yyjson_val *root, mb_release *out);

mc_status mb_lookup_release(const char *id, mb_release *out, mb_call *call) {
  memset(out, 0, sizeof *out);
  if (!is_mbid(id)) return MC_ERR_INVALID_ARG;
  if (g_fake) return g_fake->lookup_release ? g_fake->lookup_release(id, out) : MC_ERR_NOT_FOUND;
  char path[128];
  snprintf(path, sizeof path, "/release/%s?inc=recordings+artist-credits+release-groups+labels",
           id);
  yyjson_doc *doc;
  mc_status st = mb_get_json(path, &doc, call);
  if (st != MC_OK) return st;
  st = release_from(yyjson_doc_get_root(doc), out);
  yyjson_doc_free(doc);
  return st;
}

mc_status mb_release_parse(const char *json, size_t len, mb_release *out) {
  memset(out, 0, sizeof *out);
  yyjson_doc *doc = yyjson_read(json, len, 0);
  if (!doc) return MC_ERR_FORMAT;
  mc_status st = release_from(yyjson_doc_get_root(doc), out);
  yyjson_doc_free(doc);
  return st;
}

/* A release lookup's JSON into *out (zeroed); on failure *out is left empty. */
static mc_status release_from(yyjson_val *root, mb_release *out) {
  mc_status st = MC_OK;
  yyjson_val *rg = yyjson_obj_get(root, "release-group");
  yyjson_val *media = yyjson_obj_get(root, "media");

  out->id = json_strdup(root, "id");
  out->title = json_strdup(root, "title");
  set_release_artist(out, parse_credit(yyjson_obj_get(root, "artist-credit")));
  out->releasegroup_id = json_strdup(rg, "id");
  out->release_type = json_strdup(rg, "primary-type");
  out->country = json_strdup(root, "country");
  out->format = media_formats(media);
  out->year = json_year(root, "date");
  out->original_year = json_year(rg, "first-release-date");
  out->disc_total = (int)yyjson_arr_size(media);

  yyjson_val *infos = yyjson_obj_get(root, "label-info");
  yyjson_val *li = yyjson_arr_get_first(infos);
  out->label = json_strdup(yyjson_obj_get(li, "label"), "name");
  out->catalog = json_strdup(li, "catalog-number");
  /* All of them, for scoring: an album artist tag may name any of the release's labels. */
  size_t n_li = yyjson_arr_size(infos), lidx, lmax;
  if (n_li && !(out->labels = calloc(n_li, sizeof *out->labels))) st = MC_ERR_NOMEM;
  yyjson_arr_foreach(infos, lidx, lmax, li) {
    if (st != MC_OK) break;
    char *name = json_strdup(yyjson_obj_get(li, "label"), "name");
    if (name) out->labels[out->label_count++] = name;
  }

  size_t total = 0, mi, mmax, ti, tmax;
  yyjson_val *m, *t;
  yyjson_arr_foreach(media, mi, mmax, m) total += yyjson_arr_size(yyjson_obj_get(m, "tracks"));
  if (total && !(out->tracks = calloc(total, sizeof *out->tracks))) st = MC_ERR_NOMEM;

  yyjson_arr_foreach(media, mi, mmax, m) {
    if (st != MC_OK) break;
    int disc = json_int(m, "position");
    const char *fmt = yyjson_get_str(yyjson_obj_get(m, "format"));
    int video = video_format(fmt);
    if (!video && fmt && strcmp(fmt, "DVD") == 0)
      yyjson_arr_foreach(yyjson_obj_get(m, "tracks"), ti, tmax, t) video |= recording_is_video(t);
    yyjson_arr_foreach(yyjson_obj_get(m, "tracks"), ti, tmax, t) {
      mb_track *tr = &out->tracks[out->track_count++];
      yyjson_val *rec = yyjson_obj_get(t, "recording");
      tr->id = json_strdup(t, "id");
      tr->recording_id = json_strdup(rec, "id");
      tr->title = json_strdup(t, "title");
      set_track_artist(tr, parse_credit(yyjson_obj_get(t, "artist-credit")));
      tr->disc = disc ? disc : (int)mi + 1;
      tr->number = json_int(t, "position");
      tr->length_ms = json_int(t, "length");
      if (!tr->length_ms) tr->length_ms = json_int(rec, "length");
      tr->video = video || recording_is_video(t);
    }
  }

  if (st == MC_OK && (!out->id || !out->title)) st = MC_ERR_FORMAT;
  if (st != MC_OK) mb_release_clear(out);
  return st;
}

/* --- Recordings (loose tracks) --- */

static int str_is(const char *s, const char *want) {
  return s && strcmp(s, want) == 0;
}

/* How well a release represents a recording as a single: an official single beats an
 * official album, and compilations ("Now That's What I Call...", radio promos) come last. */
static int single_rank(yyjson_val *rel) {
  yyjson_val *rg = yyjson_obj_get(rel, "release-group");
  const char *type = yyjson_get_str(yyjson_obj_get(rg, "primary-type"));
  int rank = 0;
  if (str_is(yyjson_get_str(yyjson_obj_get(rel, "status")), "Official")) rank += 4;
  if (str_is(type, "Single")) rank += 2;
  else if (str_is(type, "Album") || str_is(type, "EP")) rank += 1;
  size_t idx, max;
  yyjson_val *st;
  yyjson_arr_foreach(yyjson_obj_get(rg, "secondary-types"), idx, max, st) {
    if (str_is(yyjson_get_str(st), "Compilation")) rank -= 6;
  }
  return rank;
}

/* Lexically smaller date wins; undated releases sort last. */
static int earlier(yyjson_val *a, yyjson_val *b) {
  const char *da = yyjson_get_str(yyjson_obj_get(a, "date"));
  const char *db = yyjson_get_str(yyjson_obj_get(b, "date"));
  if (!da || !*da) return 0;
  if (!db || !*db) return 1;
  return strcmp(da, db) < 0;
}

static mc_status recording_to_release(yyjson_val *rec, mb_release *out) {
  memset(out, 0, sizeof *out);

  /* The release the single is credited to: best ranked, then earliest. */
  yyjson_val *best = NULL, *r;
  int best_rank = 0;
  size_t idx, max;
  yyjson_arr_foreach(yyjson_obj_get(rec, "releases"), idx, max, r) {
    int rank = single_rank(r);
    if (!best || rank > best_rank || (rank == best_rank && earlier(r, best))) {
      best = r;
      best_rank = rank;
    }
  }

  out->is_single = 1;
  set_release_artist(out, parse_credit(yyjson_obj_get(rec, "artist-credit")));
  out->original_year = json_year(rec, "first-release-date");
  if (!(out->tracks = calloc(1, sizeof *out->tracks))) return MC_ERR_NOMEM;
  out->track_count = 1;
  mb_track *t = &out->tracks[0];
  t->recording_id = json_strdup(rec, "id");
  t->title = json_strdup(rec, "title");
  t->artist = mc_strdup(out->artist);
  t->artist_id = mc_strdup(out->artist_id);
  t->credit = credit_copy(&out->credit);
  t->length_ms = json_int(rec, "length");

  if (best) {
    yyjson_val *rg = yyjson_obj_get(best, "release-group");
    yyjson_val *media = yyjson_obj_get(best, "media");
    yyjson_val *m = yyjson_arr_get_first(media);
    yyjson_val *track = yyjson_arr_get_first(yyjson_obj_get(m, "track"));
    out->id = json_strdup(best, "id");
    out->title = json_strdup(best, "title");
    out->releasegroup_id = json_strdup(rg, "id");
    out->release_type = json_strdup(rg, "primary-type");
    out->country = json_strdup(best, "country");
    out->format = media_formats(media);
    out->year = json_year(best, "date");
    t->id = json_strdup(track, "id");
    /* Where it is on that release, for when the release can't be looked up in full. */
    t->disc = json_int(m, "position");
    yyjson_val *offset = yyjson_obj_get(m, "track-offset");
    const char *number = yyjson_get_str(yyjson_obj_get(track, "number"));
    t->number = yyjson_is_num(offset) ? (int)yyjson_get_num(offset) + 1 : number ? atoi(number) : 0;
  }
  if (!out->year) out->year = out->original_year;
  if (!out->original_year) out->original_year = out->year;
  return t->recording_id && t->title ? MC_OK : MC_ERR_FORMAT;
}

static mc_status recordings_from_doc(yyjson_doc *doc, mb_release *out, int max, int *n) {
  mc_status st = MC_OK;
  size_t idx, count;
  yyjson_val *rec;
  yyjson_arr_foreach(yyjson_obj_get(yyjson_doc_get_root(doc), "recordings"), idx, count, rec) {
    if (*n >= max) break;
    mc_status rs = recording_to_release(rec, &out[*n]);
    if (rs == MC_OK) (*n)++;
    else mb_release_clear(&out[*n]);
    if (rs == MC_ERR_NOMEM) {
      st = rs;
      break;
    }
  }
  return st;
}

static mc_status recordings_from(const char *path, mb_release *out, int max, int *n,
                                 mb_call *call) {
  *n = 0;
  yyjson_doc *doc;
  mc_status st = mb_get_json(path, &doc, call);
  if (st != MC_OK) return st;
  st = recordings_from_doc(doc, out, max, n);
  yyjson_doc_free(doc);
  return st;
}

mc_status mb_recordings_parse(const char *json, size_t len, mb_release *out, int max, int *n) {
  *n = 0;
  yyjson_doc *doc = yyjson_read(json, len, 0);
  if (!doc) return MC_ERR_FORMAT;
  mc_status st = recordings_from_doc(doc, out, max, n);
  yyjson_doc_free(doc);
  return st;
}

mc_status mb_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                               int *n, mb_call *call) {
  *n = 0;
  if (!title || !*title) return MC_ERR_INVALID_ARG;
  if (g_fake)
    return g_fake->search_recordings ? g_fake->search_recordings(artist, title, out, max, n) : MC_OK;
  char *path = search_path("recording", "recording", title, "artist", artist, max);
  if (!path) return MC_ERR_NOMEM;
  mc_status st = recordings_from(path, out, max, n, call);
  free(path);
  return st;
}

mc_status mb_lookup_recording(const char *id, mb_release *out, mb_call *call) {
  /* A search by id returns the same shape as a text search, including the release's track
   * entry, which a plain lookup doesn't. */
  memset(out, 0, sizeof *out);
  if (!is_mbid(id)) return MC_ERR_INVALID_ARG;
  if (g_fake) return g_fake->lookup_recording ? g_fake->lookup_recording(id, out) : MC_ERR_NOT_FOUND;
  char path[96];
  snprintf(path, sizeof path, "/recording?query=rid:%s&limit=1", id);
  int n = 0;
  mc_status st = recordings_from(path, out, 1, &n, call);
  if (st == MC_OK && n == 0) st = MC_ERR_NOT_FOUND;
  return st;
}
