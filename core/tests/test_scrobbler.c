/* The scrobbler against a fake service: the request signature, signing in, choosing the
 * service, what gets queued and sent, batching, backing off, a refused session, and the
 * account reaching another device through group sync. Also the play-counting rule they share with play counts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/md5.h>

#include "config.h"
#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "scrobble/lastfm.h"
#include "sync/rows.h"
#include "sync/sync_db.h"
#include "test.h"
#include "util/platform.h"

#define T_OK "0192f1e4-7c3a-7b21-9d4e-000000000001"
#define T_SHORT "0192f1e4-7c3a-7b21-9d4e-000000000002"
#define T_NO_ARTIST "0192f1e4-7c3a-7b21-9d4e-000000000003"
#define T_SINGLE "0192f1e4-7c3a-7b21-9d4e-000000000004"
#define T_BAD "0192f1e4-7c3a-7b21-9d4e-000000000005"

/* --- The fake service --- */

enum { ANSWER_OK, ANSWER_RATE_LIMIT, ANSWER_BAD_SESSION };

static mc_mutex lock;
static int answer = ANSWER_OK, approved;
static const char *session = "sk1";
static int scrobble_calls, scrobbled, now_playing_calls;
static char last_form[8192];

static int count_of(const char *s, const char *needle) {
  int n = 0;
  for (const char *p = strstr(s, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

static mc_status reply(mc_buffer *body, const char *json) {
  body->data = mc_strdup(json);
  body->len = strlen(json);
  return body->data ? MC_OK : MC_ERR_NOMEM;
}

static int hang, hung; /* calls wait to be cancelled while hang is set; hung counts them */

static mc_status fake_post(const char *url, const char *form, mc_buffer *body, long *http,
                           const mc_http_cancel *cancel) {
  (void)url;
  *http = 200;
  mc_mutex_lock(&lock);
  if (hang) { /* a network that never answers: up to curl's timeout, unless cancelled */
    hung++;
    mc_mutex_unlock(&lock);
    uint64_t end = mc_mono_ms() + 30000;
    while (!(cancel && cancel->cancelled(cancel->user)) && mc_mono_ms() < end) mc_sleep_ms(10);
    return MC_ERR_NET;
  }
  snprintf(last_form, sizeof last_form, "%s", form);
  mc_status st;
  if (strstr(form, "method=auth.getToken")) {
    st = reply(body, "{\"token\":\"tok123\"}");
  } else if (strstr(form, "method=auth.getSession")) {
    char json[160];
    snprintf(json, sizeof json, "{\"session\":{\"name\":\"tania\",\"key\":\"%s\",\"subscriber\":0}}",
             session);
    st = reply(body, approved ? json : "{\"error\":14,\"message\":\"Unauthorized Token\"}");
  } else if (strstr(form, "method=track.updateNowPlaying")) {
    now_playing_calls++;
    st = reply(body, "{\"nowplaying\":{}}");
  } else if (strstr(form, "method=track.scrobble")) {
    scrobble_calls++;
    int n = count_of(form, "timestamp%5B");
    if (answer == ANSWER_RATE_LIMIT) {
      st = reply(body, "{\"error\":29,\"message\":\"Rate Limit Exceeded\"}");
    } else if (answer == ANSWER_BAD_SESSION && !strstr(form, "sk=sk2")) {
      st = reply(body, "{\"error\":9,\"message\":\"Invalid session key\"}");
    } else if (strstr(form, "track%5B") && strstr(form, "=Bad&")) {
      st = reply(body, "{\"error\":6,\"message\":\"Invalid parameters\"}");
    } else {
      scrobbled += n;
      char json[8192] = "{\"scrobbles\":{\"scrobble\":[";
      for (int i = 0; i < n; i++)
        strcat(json, i ? ",{\"ignoredMessage\":{\"code\":\"0\",\"#text\":\"\"}}"
                       : "{\"ignoredMessage\":{\"code\":\"0\",\"#text\":\"\"}}");
      strcat(json, "]}}");
      st = reply(body, json);
    }
  } else {
    st = reply(body, "{\"error\":3,\"message\":\"Invalid Method\"}");
  }
  mc_mutex_unlock(&lock);
  return st;
}

static int get(int *v) {
  mc_mutex_lock(&lock);
  int n = *v;
  mc_mutex_unlock(&lock);
  return n;
}

static void set(int *v, int n) {
  mc_mutex_lock(&lock);
  *v = n;
  mc_mutex_unlock(&lock);
}

/* Waits up to ms for the scrobbler to show pending plays (-1: any) and last_error. */
static int wait_status(mc_scrobbler *sc, int pending, mc_status last_error, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  for (;;) {
    mc_scrobbler_status st;
    if (mc_scrobbler_status_get(sc, &st) != MC_OK) return 0;
    int ok = (pending < 0 || st.pending == pending) && st.last_error == last_error;
    mc_scrobbler_status_clear(&st);
    if (ok) return 1;
    if (mc_mono_ms() > end) return 0;
    mc_sleep_ms(10);
  }
}

static int wait_count(int *v, int n, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  while (get(v) != n) {
    if (mc_mono_ms() > end) return 0;
    mc_sleep_ms(10);
  }
  return 1;
}

static char *username(mc_scrobbler *sc) {
  mc_scrobbler_status st;
  if (mc_scrobbler_status_get(sc, &st) != MC_OK) return NULL;
  char *name = st.username;
  st.username = NULL;
  mc_scrobbler_status_clear(&st);
  return name;
}

static int sign_in(mc_scrobbler *sc) {
  char *url = NULL;
  int ok = mc_scrobbler_auth_begin(sc, &url) == MC_OK;
  mc_free(url);
  set(&approved, 1);
  ok = ok && mc_scrobbler_auth_finish(sc) == MC_OK;
  set(&approved, 0);
  return ok;
}

typedef struct auth_job {
  mc_scrobbler *sc;
  mc_status st;
} auth_job;

static void auth_begin_main(void *arg) {
  auth_job *job = arg;
  char *url = NULL;
  job->st = mc_scrobbler_auth_begin(job->sc, &url);
  mc_free(url);
}

/* --- Tests --- */

static void test_rule(void) {
  CHECK(!mc_play_counts(360000, 179999)); /* six minutes: half */
  CHECK(mc_play_counts(360000, 180000));
  CHECK(!mc_play_counts(1200000, 239999)); /* twenty minutes: four */
  CHECK(mc_play_counts(1200000, 240000));
  CHECK(mc_play_counts(20000, 10000)); /* short tracks count too; only scrobbling skips them */
  CHECK(!mc_play_counts(0, 239999));   /* unknown length: four minutes */
  CHECK(mc_play_counts(0, 240000));
}

static void test_sign(void) {
  /* Sorted by name, concatenated, then the secret. */
  mc_lastfm_param p[] = {{"token", "yyy"}, {"method", "auth.getSession"}, {"api_key", "xxx"}};
  char sig[MC_LASTFM_SIG_STRLEN];
  mc_lastfm_sign(p, 3, "secret", sig);
  static const char plain[] = "api_keyxxxmethodauth.getSessiontokenyyysecret";
  unsigned char md5[16];
  char hex[33];
  CHECK(mbedtls_md5((const unsigned char *)plain, sizeof plain - 1, md5) == 0);
  for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", md5[i]);
  CHECK(strcmp(sig, hex) == 0);
}

static int cancel_after(void *user) {
  return mc_mono_ms() >= *(uint64_t *)user;
}

/* A real request through curl gives up soon after it's cancelled. 10.255.255.1 isn't routed
 * anywhere, so the connect would otherwise wait out its 10 s timeout (on a machine with no
 * route at all it fails at once, which passes too). */
static void test_http_cancel(void) {
  uint64_t at = mc_mono_ms() + 300;
  mc_http_cancel cancel = {cancel_after, &at};
  mc_buffer body = {0};
  long http = 0;
  uint64_t t0 = mc_mono_ms();
  CHECK(mc_http_post_form("https://10.255.255.1/", "a=b", &body, &http, &cancel) == MC_ERR_NET);
  CHECK(mc_mono_ms() - t0 < 2500);
  CHECK(body.data == NULL);
}

static int queue_rows(const char *config) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(config, &sdb) != MC_OK) return -1;
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), "SELECT count(*) FROM scrobble_queue", -1, &stmt,
                         NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(sdb);
  return n;
}

/* Sends every group change of from's sync.db to to's, as a session would. */
static int sync_group(const char *from, const char *to) {
  mc_sync_db *a = NULL, *b = NULL;
  int ok = mc_sync_db_open(from, &a) == MC_OK && mc_sync_db_open(to, &b) == MC_OK;
  mc_rows_target tg;
  mc_rows_applier *ap = NULL;
  mc_rows_reader *rd = NULL;
  if (ok) {
    mc_rows_target_group(b, &tg);
    ok = mc_rows_applier_new(&tg, &ap) == MC_OK &&
         mc_rows_reader_open(mc_sync_db_handle(a), NULL, 0, 0, &rd) == MC_OK;
  }
  mc_row r;
  int got = 1, logged;
  while (ok && got && (ok = mc_rows_reader_next(rd, &r, &got) == MC_OK) && got)
    ok = mc_rows_apply(ap, &r, 1, &logged) == MC_OK;
  mc_rows_reader_close(rd);
  mc_rows_applier_free(ap);
  mc_sync_db_close(a);
  mc_sync_db_close(b);
  return ok;
}

static void remove_files(const char *dir, const char *const *names) {
  char path[1100];
  for (; *names; names++) {
    snprintf(path, sizeof path, "%s/%s", dir, *names);
    remove(path);
  }
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  char lib_dir[1024], config[1024], other[1024];
  snprintf(lib_dir, sizeof lib_dir, "%s/test_scrobbler_lib.d", scratch);
  snprintf(config, sizeof config, "%s/test_scrobbler_config.d", scratch);
  snprintf(other, sizeof other, "%s/test_scrobbler_other.d", scratch);
  static const char *const lib_files[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                                          MC_LIBRARY_DB_NAME "-shm", NULL};
  static const char *const sync_files[] = {MC_SYNC_DB_NAME, MC_SYNC_DB_NAME "-wal",
                                           MC_SYNC_DB_NAME "-shm", NULL};
  remove_files(lib_dir, lib_files);
  remove_files(config, sync_files);
  remove_files(other, sync_files);
  mc_mutex_init(&lock);
  mc_lastfm_post = fake_post;

  test_rule();
  test_sign();
  test_http_cancel();

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();
  CHECK(sqlite3_exec(mc_library_db(lib),
                     "INSERT INTO artists (uid, name) VALUES ('ar1', 'Low');"
                     "INSERT INTO albums (uid, title, artist_uid) VALUES"
                     " ('al1', 'Things We Lost in the Fire', 'ar1');"
                     "INSERT INTO tracks (uid, title, album_uid, artist_uid, track_no, duration_ms)"
                     " VALUES ('" T_OK "', 'Sunflower', 'al1', 'ar1', 1, 280000),"
                     " ('" T_SHORT "', 'Interlude', 'al1', 'ar1', 2, 20000),"
                     " ('" T_NO_ARTIST "', 'Untitled', NULL, NULL, NULL, 200000),"
                     " ('" T_SINGLE "', 'Just Like Christmas', NULL, 'ar1', 7, 180000),"
                     " ('" T_BAD "', 'Bad', 'al1', 'ar1', 3, 200000);",
                     NULL, NULL, NULL) == SQLITE_OK);

  mc_scrobbler *sc = NULL;
  CHECK(mc_scrobbler_new(lib, config, &sc) == MC_OK);
  if (!sc) return TEST_RESULT();
  int64_t now = (int64_t)mc_wall_ms();

  /* Signed out: plays aren't kept, and a build without a Last.fm key can't sign in. */
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now - 60000) == MC_OK);
  CHECK(mc_scrobbler_scrobble(sc, "nope", now) == MC_ERR_INVALID_ARG);
  mc_sleep_ms(200);
  CHECK(wait_status(sc, 0, MC_OK, 1000));
  CHECK(queue_rows(config) == 0);
  char *url = NULL;
  CHECK(mc_scrobbler_auth_finish(sc) == MC_ERR_INVALID_ARG); /* no begin */
  if (!mc_lastfm_build_key_present()) CHECK(mc_scrobbler_auth_begin(sc, &url) == MC_ERR_AUTH);
  {
    mc_scrobbler_status st;
    CHECK(mc_scrobbler_status_get(sc, &st) == MC_OK);
    CHECK(st.has_key == mc_lastfm_build_key_present());
    CHECK(st.service == MC_SCROBBLER_SERVICE_LASTFM);
    CHECK_STR(st.service_name, "Last.fm");
    CHECK_STR(st.api_url, MC_SCROBBLER_LASTFM_API_URL);
    CHECK_STR(st.auth_url, MC_SCROBBLER_LASTFM_AUTH_URL);
    mc_scrobbler_status_clear(&st);
  }
  if (mc_lastfm_build_key_present()) {
    /* The build's key, decoded for the call: in the form and the page to approve it on.
     * (The fake service answers; nothing reaches Last.fm.) */
    char key[MC_LASTFM_KEY_MAX], want[MC_LASTFM_KEY_MAX + 16];
    CHECK(mc_lastfm_build_key(key, sizeof key) == 1);
    CHECK(mc_scrobbler_auth_begin(sc, &url) == MC_OK);
    snprintf(want, sizeof want, "api_key=%s&", key);
    CHECK(url && !strncmp(url, MC_LASTFM_AUTH_URL "?", strlen(MC_LASTFM_AUTH_URL) + 1));
    CHECK(url && strstr(url, want) != NULL);
    CHECK(strstr(last_form, want) != NULL && strstr(last_form, "api_sig=") != NULL);
    mc_lastfm_wipe(key, sizeof key);
    mc_free(url);
    url = NULL;
  }

  /* Another service, with its own key: sign in, in the browser. */
  CHECK(mc_scrobbler_set_service(sc, "https://scrobble.invalid/2.0/",
                                 "https://scrobble.invalid/auth/", "key", "secret") == MC_OK);
  CHECK(mc_scrobbler_auth_begin(sc, &url) == MC_OK);
  CHECK_STR(url, "https://scrobble.invalid/auth/?api_key=key&token=tok123");
  mc_free(url);
  CHECK(strstr(last_form, "api_sig=") && strstr(last_form, "&format=json"));
  CHECK(mc_scrobbler_auth_finish(sc) == MC_ERR_AUTH); /* not approved yet */
  set(&approved, 1);
  CHECK(mc_scrobbler_auth_finish(sc) == MC_OK);
  set(&approved, 0);
  char *name = username(sc);
  CHECK_STR(name, "tania");
  mc_free(name);
  {
    mc_scrobbler_status st;
    CHECK(mc_scrobbler_status_get(sc, &st) == MC_OK);
    CHECK(st.service == MC_SCROBBLER_SERVICE_OTHER && st.has_key);
    CHECK_STR(st.service_name, "scrobble.invalid");
    CHECK_STR(st.auth_url, "https://scrobble.invalid/auth/");
    mc_scrobbler_status_clear(&st);
  }
  /* Only web addresses: anything else is turned down, and keeps the session. */
  CHECK(mc_scrobbler_set_service(sc, "ftp://scrobble.invalid/2.0/", "https://scrobble.invalid/auth/",
                                 "key", "secret") == MC_ERR_INVALID_ARG);
  CHECK(mc_scrobbler_set_service(sc, "https://scrobble.invalid/2.0/", "https://", "key", "secret") ==
        MC_ERR_INVALID_ARG);
  name = username(sc);
  CHECK_STR(name, "tania");
  mc_free(name);

  /* Now playing, and plays: too short and artistless ones are skipped; a track with no album
   * goes without one, nor its track number. */
  CHECK(mc_scrobbler_now_playing(sc, T_OK) == MC_OK);
  CHECK(wait_count(&now_playing_calls, 1, 2000));
  CHECK(strstr(last_form, "artist=Low&") && strstr(last_form, "track=Sunflower&"));
  CHECK(strstr(last_form, "sk=sk1&"));
  CHECK(mc_scrobbler_scrobble(sc, T_SHORT, now) == MC_OK);
  CHECK(mc_scrobbler_scrobble(sc, T_NO_ARTIST, now) == MC_OK);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now - 60000) == MC_OK);
  CHECK(wait_count(&scrobbled, 1, 2000));
  CHECK(strstr(last_form, "artist%5B0%5D=Low&") && strstr(last_form, "track%5B0%5D=Sunflower&"));
  CHECK(strstr(last_form, "album%5B0%5D=Things%20We%20Lost%20in%20the%20Fire&"));
  CHECK(strstr(last_form, "trackNumber%5B0%5D=1&") && strstr(last_form, "duration%5B0%5D=280&"));
  char ts[64];
  snprintf(ts, sizeof ts, "timestamp%%5B0%%5D=%lld&", (long long)((now - 60000) / 1000));
  CHECK(strstr(last_form, ts) != NULL);
  CHECK(mc_scrobbler_scrobble(sc, T_SINGLE, now) == MC_OK);
  CHECK(wait_count(&scrobbled, 2, 2000));
  CHECK(strstr(last_form, "track%5B0%5D=Just%20Like%20Christmas&") && !strstr(last_form, "album%5B"));
  CHECK(!strstr(last_form, "trackNumber%5B"));
  CHECK(wait_status(sc, 0, MC_OK, 2000));
  CHECK(get(&scrobbled) == 2);

  /* The service down: the play waits in the queue, across a restart. */
  set(&answer, ANSWER_RATE_LIMIT);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now) == MC_OK);
  CHECK(wait_status(sc, 1, MC_ERR_NET, 2000));
  mc_scrobbler_free(sc);
  CHECK(queue_rows(config) == 1);
  sc = NULL;
  CHECK(mc_scrobbler_new(lib, config, &sc) == MC_OK);
  if (!sc) return TEST_RESULT();
  CHECK(wait_status(sc, 1, MC_ERR_NET, 2000));
  /* Back up. Setting the same service keeps the session, and tries again straight away. */
  set(&answer, ANSWER_OK);
  CHECK(mc_scrobbler_set_service(sc, "https://scrobble.invalid/2.0/",
                                 "https://scrobble.invalid/auth/", "key", "secret") == MC_OK);
  CHECK(wait_status(sc, 0, MC_OK, 2000));
  CHECK(get(&scrobbled) == 3);
  name = username(sc);
  CHECK_STR(name, "tania");
  mc_free(name);

  /* Plays are sent fifty at a time. */
  set(&answer, ANSWER_RATE_LIMIT);
  for (int i = 0; i < 60; i++) CHECK(mc_scrobbler_scrobble(sc, T_OK, now + i * 1000) == MC_OK);
  CHECK(wait_status(sc, 60, MC_ERR_NET, 3000));
  set(&answer, ANSWER_OK);
  int calls = get(&scrobble_calls);
  CHECK(mc_scrobbler_set_service(sc, "https://scrobble.invalid/2.0/",
                                 "https://scrobble.invalid/auth/", "key", "secret") == MC_OK);
  CHECK(wait_status(sc, 0, MC_OK, 3000));
  CHECK(get(&scrobble_calls) == calls + 2);
  CHECK(get(&scrobbled) == 63);

  /* A batch the service won't take goes again a play at a time, and the bad play is dropped. */
  set(&answer, ANSWER_RATE_LIMIT);
  CHECK(mc_scrobbler_scrobble(sc, T_BAD, now) == MC_OK);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now) == MC_OK);
  CHECK(wait_status(sc, 2, MC_ERR_NET, 3000));
  set(&answer, ANSWER_OK);
  CHECK(mc_scrobbler_set_service(sc, "https://scrobble.invalid/2.0/",
                                 "https://scrobble.invalid/auth/", "key", "secret") == MC_OK);
  CHECK(wait_status(sc, 0, MC_OK, 3000));
  CHECK(get(&scrobbled) == 64);

  /* A refused session stops sending until the next sign-in, which sends what waited. */
  set(&answer, ANSWER_BAD_SESSION);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now) == MC_OK);
  CHECK(wait_status(sc, 1, MC_ERR_AUTH, 3000));
  mc_mutex_lock(&lock);
  session = "sk2";
  mc_mutex_unlock(&lock);
  CHECK(sign_in(sc));
  CHECK(wait_status(sc, 0, MC_OK, 3000));
  CHECK(get(&scrobbled) == 65);
  set(&answer, ANSWER_OK);

  /* The account is the group's: it reaches another device through sync. */
  CHECK(sync_group(config, other));
  mc_scrobbler *sc2 = NULL;
  CHECK(mc_scrobbler_new(lib, other, &sc2) == MC_OK);
  if (sc2) {
    uint64_t end = mc_mono_ms() + 2000;
    while (!(name = username(sc2)) && mc_mono_ms() < end) mc_sleep_ms(10);
    CHECK_STR(name, "tania");
    mc_free(name);
    mc_scrobbler_status st;
    CHECK(mc_scrobbler_status_get(sc2, &st) == MC_OK);
    CHECK_STR(st.api_url, "https://scrobble.invalid/2.0/");
    mc_scrobbler_status_clear(&st);
    mc_scrobbler_free(sc2);
  }

  /* Signing out: plays aren't kept again. Changing the service signs out too. */
  CHECK(mc_scrobbler_sign_out(sc) == MC_OK);
  name = username(sc);
  CHECK(name == NULL);
  int sent = get(&scrobbled);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now) == MC_OK);
  mc_sleep_ms(200);
  CHECK(wait_status(sc, 0, MC_OK, 1000));
  CHECK(get(&scrobbled) == sent);
  CHECK(sign_in(sc));
  CHECK(mc_scrobbler_set_service(sc, MC_SCROBBLER_LIBREFM_API_URL, MC_SCROBBLER_LIBREFM_AUTH_URL,
                                 MC_SCROBBLER_LIBREFM_API_KEY, MC_SCROBBLER_LIBREFM_API_SECRET) == MC_OK);
  {
    /* Shown at once, signed out. */
    mc_scrobbler_status st;
    CHECK(mc_scrobbler_status_get(sc, &st) == MC_OK);
    CHECK(st.username == NULL && st.has_key);
    CHECK(st.service == MC_SCROBBLER_SERVICE_LIBREFM);
    CHECK_STR(st.service_name, "Libre.fm");
    mc_scrobbler_status_clear(&st);
  }
  uint64_t end = mc_mono_ms() + 2000;
  while ((name = username(sc)) && mc_mono_ms() < end) {
    mc_free(name);
    mc_sleep_ms(10);
  }
  CHECK(name == NULL);
  CHECK(mc_scrobbler_auth_begin(sc, &url) == MC_OK);
  CHECK_STR(url, MC_SCROBBLER_LIBREFM_AUTH_URL "?api_key=" MC_SCROBBLER_LIBREFM_API_KEY "&token=tok123");
  mc_free(url);
  url = NULL;

  /* Freeing the scrobbler cuts a call that's hanging short, and its play stays queued. */
  CHECK(sign_in(sc));
  set(&hang, 1);
  CHECK(mc_scrobbler_scrobble(sc, T_OK, now) == MC_OK);
  CHECK(wait_count(&hung, 1, 2000));
  uint64_t t0 = mc_mono_ms();
  mc_scrobbler_free(sc);
  CHECK(mc_mono_ms() - t0 < 2000);
  CHECK(queue_rows(config) == 1);
  set(&hang, 0);
  sc = NULL;
  sent = get(&scrobbled);
  CHECK(mc_scrobbler_new(lib, config, &sc) == MC_OK);
  if (!sc) return TEST_RESULT();
  /* The play is sent (not just a fresh status, which reads the same), so nothing of the
   * worker's hangs below. */
  CHECK(wait_count(&scrobbled, sent + 1, 2000));
  CHECK(wait_status(sc, 0, MC_OK, 2000));

  /* So does cancelling a sign-in, from another thread; and a token whose sign-in was
   * cancelled isn't turned into a session, even once approved. */
  set(&hang, 1);
  auth_job job = {sc, MC_OK};
  mc_thread th;
  CHECK(mc_thread_start(&th, auth_begin_main, &job) == MC_OK);
  CHECK(wait_count(&hung, 2, 2000));
  t0 = mc_mono_ms();
  mc_scrobbler_auth_cancel(sc);
  mc_thread_join(&th);
  CHECK(mc_mono_ms() - t0 < 2000);
  CHECK(job.st == MC_ERR_NET);
  set(&hang, 0);
  CHECK(mc_scrobbler_auth_begin(sc, &url) == MC_OK);
  mc_free(url);
  mc_scrobbler_auth_cancel(sc);
  set(&approved, 1);
  CHECK(mc_scrobbler_auth_finish(sc) == MC_ERR_NET);
  set(&approved, 0);
  CHECK(sign_in(sc)); /* a new one starts afresh */

  /* Back to Last.fm, which signs out, and its key is the build's again. */
  CHECK(mc_scrobbler_set_service(sc, NULL, NULL, NULL, NULL) == MC_OK);
  {
    mc_scrobbler_status st;
    CHECK(mc_scrobbler_status_get(sc, &st) == MC_OK);
    CHECK(st.username == NULL && st.has_key == mc_lastfm_build_key_present());
    CHECK(st.service == MC_SCROBBLER_SERVICE_LASTFM);
    CHECK_STR(st.service_name, "Last.fm");
    CHECK_STR(st.auth_url, MC_SCROBBLER_LASTFM_AUTH_URL);
    mc_scrobbler_status_clear(&st);
  }

  mc_scrobbler_free(sc);
  mc_library_close(lib);
  mc_mutex_destroy(&lock);
  return TEST_RESULT();
}
