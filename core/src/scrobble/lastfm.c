#include "scrobble/lastfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md5.h>
#include <yyjson.h>

#include "util/platform.h"

mc_lastfm_post_fn mc_lastfm_post = mc_http_post_form;

/* --- Strings --- */

typedef struct strbuf {
  char *s;
  size_t len, cap;
  int failed;
} strbuf;

static void sb_add(strbuf *b, const char *s, size_t n) {
  if (b->failed) return;
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + n + 1) cap *= 2;
    char *grown = realloc(b->s, cap);
    if (!grown) {
      b->failed = 1;
      return;
    }
    b->s = grown;
    b->cap = cap;
  }
  memcpy(b->s + b->len, s, n);
  b->len += n;
  b->s[b->len] = '\0';
}

static void sb_str(strbuf *b, const char *s) {
  sb_add(b, s, strlen(s));
}

static void sb_escaped(strbuf *b, const char *s) {
  char *e = mc_url_escape(s);
  if (!e) b->failed = 1;
  else sb_str(b, e);
  free(e);
}

/* --- Signed calls --- */

static int by_name(const void *a, const void *b) {
  return strcmp(((const mc_lastfm_param *)a)->name, ((const mc_lastfm_param *)b)->name);
}

void mc_lastfm_sign(const mc_lastfm_param *params, int count, const char *secret,
                    char out[MC_LASTFM_SIG_STRLEN]) {
  out[0] = '\0';
  mc_lastfm_param *sorted = malloc((size_t)(count ? count : 1) * sizeof *sorted);
  if (!sorted) return;
  memcpy(sorted, params, (size_t)count * sizeof *sorted);
  qsort(sorted, (size_t)count, sizeof *sorted, by_name);
  strbuf b = {0};
  for (int i = 0; i < count; i++) {
    sb_str(&b, sorted[i].name);
    sb_str(&b, sorted[i].value);
  }
  sb_str(&b, secret);
  free(sorted);
  unsigned char md5[16];
  if (!b.failed && mbedtls_md5((const unsigned char *)b.s, b.len, md5) == 0) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
      out[i * 2] = hex[md5[i] >> 4];
      out[i * 2 + 1] = hex[md5[i] & 15];
    }
    out[32] = '\0';
  }
  mc_lastfm_wipe(b.s, b.cap); /* it ends with the secret */
  free(b.s);
}

/* What an API error code means for the caller; see lastfm.h. */
static mc_status error_status(int code) {
  switch (code) {
    case 4:  /* authentication failed */
    case 9:  /* invalid session key */
    case 10: /* invalid API key */
    case 13: /* invalid signature */
    case 14: /* token not yet authorised */
    case 15: /* token expired */
    case 26: /* API key suspended */
      return MC_ERR_AUTH;
    case 8:  /* operation failed: try again */
    case 11: /* service offline */
    case 16: /* temporarily unavailable */
    case 29: /* rate limit exceeded */
      return MC_ERR_NET;
    default:
      return MC_ERR_FORMAT;
  }
}

/* Codes come as numbers from Last.fm and sometimes as strings from others. */
static int int_of(yyjson_val *v) {
  if (yyjson_is_int(v)) return (int)yyjson_get_int(v);
  if (yyjson_is_str(v)) return atoi(yyjson_get_str(v));
  return 0;
}

/* Calls method with params, adding api_key, sk (with a session), api_sig and format. On
 * MC_OK, *out is the answer (free with yyjson_doc_free()). */
static mc_status call(const mc_lastfm_service *svc, const char *method,
                      const mc_lastfm_param *params, int count, yyjson_doc **out,
                      int *api_error) {
  *out = NULL;
  *api_error = 0;
  /* The build's key and secret, if the service has none of its own: decoded here, on the
   * stack, and wiped once the call is signed. */
  char build_key[MC_LASTFM_KEY_MAX], build_secret[MC_LASTFM_KEY_MAX];
  build_key[0] = build_secret[0] = '\0';
  const char *key = svc->api_key, *secret = svc->api_secret;
  if (!key && mc_lastfm_build_key(build_key, sizeof build_key)) key = build_key;
  if (!secret && mc_lastfm_build_secret(build_secret, sizeof build_secret)) secret = build_secret;
  if (!svc->api_url || !*svc->api_url || !key || !*key || !secret || !*secret) {
    mc_lastfm_wipe(build_key, sizeof build_key);
    mc_lastfm_wipe(build_secret, sizeof build_secret);
    return MC_ERR_AUTH; /* no API account: a build without one, on its default service */
  }

  mc_lastfm_param *all = malloc((size_t)(count + 3) * sizeof *all);
  if (!all) {
    mc_lastfm_wipe(build_key, sizeof build_key);
    mc_lastfm_wipe(build_secret, sizeof build_secret);
    return MC_ERR_NOMEM;
  }
  if (count) memcpy(all, params, (size_t)count * sizeof *all);
  int n = count;
  all[n++] = (mc_lastfm_param){"method", method};
  all[n++] = (mc_lastfm_param){"api_key", key};
  if (svc->session_key) all[n++] = (mc_lastfm_param){"sk", svc->session_key};
  char sig[MC_LASTFM_SIG_STRLEN];
  mc_lastfm_sign(all, n, secret, sig);
  mc_lastfm_wipe(build_secret, sizeof build_secret);

  strbuf form = {0};
  for (int i = 0; i < n; i++) {
    sb_escaped(&form, all[i].name);
    sb_str(&form, "=");
    sb_escaped(&form, all[i].value);
    sb_str(&form, "&");
  }
  sb_str(&form, "api_sig=");
  sb_str(&form, sig);
  sb_str(&form, "&format=json"); /* not signed: the API leaves format out of the signature */
  free(all);
  mc_lastfm_wipe(build_key, sizeof build_key);
  if (form.failed || !sig[0]) {
    free(form.s);
    return MC_ERR_NOMEM;
  }

  mc_buffer body = {0};
  long http = 0;
  mc_status st = mc_lastfm_post(svc->api_url, form.s, &body, &http, svc->cancel);
  free(form.s);
  if (st != MC_OK) return st;

  yyjson_doc *doc = body.data ? yyjson_read(body.data, body.len, 0) : NULL;
  mc_buffer_free(&body);
  yyjson_val *root = yyjson_doc_get_root(doc);
  yyjson_val *err = yyjson_obj_get(root, "error");
  if (err) {
    *api_error = int_of(err);
    yyjson_doc_free(doc);
    return error_status(*api_error);
  }
  if (!yyjson_is_obj(root) || http >= 400) { /* a proxy's error page, say */
    yyjson_doc_free(doc);
    return http >= 500 || !doc ? MC_ERR_NET : MC_ERR_FORMAT;
  }
  *out = doc;
  return MC_OK;
}

/* A copy of the string at root.a.b, or NULL. */
static char *dup_at(yyjson_doc *doc, const char *a, const char *b) {
  yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(doc), a);
  if (b) v = yyjson_obj_get(v, b);
  const char *s = yyjson_get_str(v);
  return s && *s ? mc_strdup(s) : NULL;
}

mc_status mc_lastfm_get_token(const mc_lastfm_service *svc, char **token, int *api_error) {
  *token = NULL;
  yyjson_doc *doc;
  mc_status st = call(svc, "auth.getToken", NULL, 0, &doc, api_error);
  if (st != MC_OK) return st;
  *token = dup_at(doc, "token", NULL);
  yyjson_doc_free(doc);
  return *token ? MC_OK : MC_ERR_FORMAT;
}

mc_status mc_lastfm_get_session(const mc_lastfm_service *svc, const char *token,
                                char **session_key, char **username, int *api_error) {
  *session_key = *username = NULL;
  mc_lastfm_param p[] = {{"token", token}};
  yyjson_doc *doc;
  mc_status st = call(svc, "auth.getSession", p, 1, &doc, api_error);
  if (st != MC_OK) return st;
  *session_key = dup_at(doc, "session", "key");
  *username = dup_at(doc, "session", "name");
  yyjson_doc_free(doc);
  if (*session_key && *username) return MC_OK;
  free(*session_key);
  free(*username);
  *session_key = *username = NULL;
  return MC_ERR_FORMAT;
}

/* Field values of one play, numbered for track.scrobble (index >= 0) or not. Its strings
 * live in names and values, which hold up to PLAY_FIELDS each. */
enum { PLAY_FIELDS = 8, NAME_LEN = 24, VALUE_LEN = 24 };

static int play_params(const mc_scrobble *s, int index, mc_lastfm_param *out,
                       char names[][NAME_LEN], char values[][VALUE_LEN]) {
  int n = 0;
#define ADD(field, v)                                                              \
  do {                                                                             \
    if (index >= 0) snprintf(names[n], NAME_LEN, "%s[%d]", field, index);          \
    else snprintf(names[n], NAME_LEN, "%s", field);                                \
    out[n].name = names[n];                                                        \
    out[n].value = (v);                                                            \
    n++;                                                                           \
  } while (0)
  ADD("artist", s->artist);
  ADD("track", s->track);
  if (index >= 0) {
    snprintf(values[n], VALUE_LEN, "%lld", (long long)s->played_at_s);
    ADD("timestamp", values[n]);
  }
  if (s->album && *s->album) ADD("album", s->album);
  if (s->album_artist && *s->album_artist) ADD("albumArtist", s->album_artist);
  if (s->mb_trackid && *s->mb_trackid) ADD("mbid", s->mb_trackid);
  if (s->track_no > 0) {
    snprintf(values[n], VALUE_LEN, "%d", s->track_no);
    ADD("trackNumber", values[n]);
  }
  if (s->duration_s > 0) {
    snprintf(values[n], VALUE_LEN, "%d", s->duration_s);
    ADD("duration", values[n]);
  }
#undef ADD
  return n;
}

mc_status mc_lastfm_now_playing(const mc_lastfm_service *svc, const mc_scrobble *s,
                                int *api_error) {
  mc_lastfm_param p[PLAY_FIELDS];
  char names[PLAY_FIELDS][NAME_LEN], values[PLAY_FIELDS][VALUE_LEN];
  int n = play_params(s, -1, p, names, values);
  yyjson_doc *doc;
  mc_status st = call(svc, "track.updateNowPlaying", p, n, &doc, api_error);
  yyjson_doc_free(doc);
  return st;
}

mc_status mc_lastfm_scrobble(const mc_lastfm_service *svc, const mc_scrobble *plays, int count,
                             int *done, int *api_error) {
  if (count < 1 || count > MC_LASTFM_BATCH) return MC_ERR_INVALID_ARG;
  size_t cap = (size_t)count * PLAY_FIELDS;
  mc_lastfm_param *p = malloc(cap * sizeof *p);
  char (*names)[NAME_LEN] = malloc(cap * sizeof *names);
  char (*values)[VALUE_LEN] = malloc(cap * sizeof *values);
  mc_status st = MC_ERR_NOMEM;
  yyjson_doc *doc = NULL;
  if (p && names && values) {
    int n = 0;
    for (int i = 0; i < count; i++) n += play_params(&plays[i], i, p + n, names + n, values + n);
    st = call(svc, "track.scrobble", p, n, &doc, api_error);
  }
  free(p);
  free(names);
  free(values);
  if (st != MC_OK) return st;

  /* {"scrobbles": {"scrobble": [...], "@attr": {...}}}, in the order sent; a lone scrobble
   * comes as an object rather than an array. */
  yyjson_val *list = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(doc), "scrobbles"), "scrobble");
  int items = yyjson_is_arr(list) ? (int)yyjson_arr_size(list) : yyjson_is_obj(list) ? 1 : 0;
  if (items != count) {
    yyjson_doc_free(doc);
    return MC_ERR_FORMAT;
  }
  for (int i = 0; i < count; i++) {
    yyjson_val *item = yyjson_is_arr(list) ? yyjson_arr_get(list, (size_t)i) : list;
    int code = int_of(yyjson_obj_get(yyjson_obj_get(item, "ignoredMessage"), "code"));
    done[i] = code != 5; /* 5: over the daily limit; the rest (1-4) won't change */
  }
  yyjson_doc_free(doc);
  return MC_OK;
}
