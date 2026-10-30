/* The scrobbler: calls from any thread post requests, and a worker thread with its own
 * connections to library.db and sync.db carries them out. Plays are looked up in the library
 * and queued in sync.db (scrobble_queue) straight away, then sent in batches whenever the
 * account is signed in and the service can be reached, backing off while it can't. Account
 * changes (auth, sign out, service) are written from the caller's thread on a connection of
 * its own, then the worker is woken to pick them up; ones arriving by sync are picked up on
 * its next poll. */
#include "musiccore/scrobbler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "config.h"
#include "scrobble/lastfm.h"
#include "scrobble/scrobble_internal.h"
#include "sync/sync_db.h"
#include "util/platform.h"

enum {
  MIN_TRACK_MS = 30 * 1000, /* Last.fm doesn't take shorter tracks */
  BACKOFF_MIN_MS = 30 * 1000,
  BACKOFF_MAX_MS = 30 * 60 * 1000,
  POLL_MS = 60 * 1000, /* for an account arriving by sync */
};
/* Last.fm ignores plays older than two weeks, so the queue drops them. */
#define MAX_AGE_S (14 * 24 * 3600)

typedef struct request {
  char track_uid[MC_UID_STRLEN];
  int64_t started_ms; /* -1 for now playing */
} request;

struct mc_scrobbler {
  char *lib_dir;
  char *config_dir;
  mc_thread thread;
  mc_mutex lock;
  mc_cond wake;

  /* Under lock. */
  int quit;
  request *reqs;
  int nreqs, cap;
  int account_changed; /* reread the account and try again now */
  char *token;         /* from mc_scrobbler_auth_begin() */
  unsigned auth_gen;   /* bumped by mc_scrobbler_auth_cancel(), stopping the calls before it */
  unsigned token_gen;  /* auth_gen when the token's mc_scrobbler_auth_begin() started */
  mc_scrobbler_status status;
};

/* --- The account --- */

typedef struct account {
  char *api_url, *auth_url;
  char *api_key, *api_secret;   /* NULL: the build's, which stay encoded until a call */
  char *session_key, *username; /* NULL when signed out */
} account;

static void free_wiped(char *s) {
  if (s) mc_lastfm_wipe(s, strlen(s));
  free(s);
}

static void account_clear(account *a) {
  free(a->api_url);
  free(a->auth_url);
  free(a->api_key);
  free_wiped(a->api_secret);
  free_wiped(a->session_key);
  free(a->username);
  memset(a, 0, sizeof *a);
}

/* Whether the account's service has an API key and secret, its own or the build's. */
static int account_has_key(const account *a) {
  int build = mc_lastfm_build_key_present();
  return (a->api_key ? 1 : build) && (a->api_secret ? 1 : build);
}

static char *column_or(sqlite3_stmt *stmt, int col, const char *fallback) {
  const char *v = stmt ? (const char *)sqlite3_column_text(stmt, col) : NULL;
  if (!v || !*v) v = fallback;
  return v ? mc_strdup(v) : NULL;
}

/* Reads the account, with Last.fm's addresses filling in what isn't set. The API key and
 * secret are left NULL when unset, meaning the build's. */
static mc_status account_read(sqlite3 *db, account *a) {
  memset(a, 0, sizeof *a);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT api_url, auth_url, api_key, api_secret, session_key, username"
                         " FROM scrobble_account WHERE uid = '" MC_SCROBBLE_ACCOUNT_UID "'",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  sqlite3_stmt *row = rc == SQLITE_ROW ? stmt : NULL;
  a->api_url = column_or(row, 0, MC_LASTFM_API_URL);
  a->auth_url = column_or(row, 1, MC_LASTFM_AUTH_URL);
  a->api_key = column_or(row, 2, NULL);
  a->api_secret = column_or(row, 3, NULL);
  a->session_key = column_or(row, 4, NULL);
  a->username = column_or(row, 5, NULL);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    account_clear(a);
    return MC_ERR_DB;
  }
  if (!a->api_url || !a->auth_url) {
    account_clear(a);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

static mc_lastfm_service service_of(const account *a) {
  return (mc_lastfm_service){a->api_url, a->api_key, a->api_secret, a->session_key, NULL};
}

/* Runs sql against the account row, creating it first if need be: ?1 is its uid, ?2 on
 * the texts (NULL or "" binds NULL). Logged, so it reaches the group. */
static mc_status write_account(mc_scrobbler *sc, const char *sql, const char *const *texts,
                               int count) {
  mc_sync_db *sdb = NULL;
  mc_status st = mc_sync_db_open(sc->config_dir, &sdb);
  if (st != MC_OK) return st;
  sqlite3 *db = mc_sync_db_handle(sdb);
  if ((st = mc_sync_db_begin_write(sdb)) == MC_OK) {
    if (sqlite3_exec(db,
                     "INSERT OR IGNORE INTO scrobble_account (uid)"
                     " VALUES ('" MC_SCROBBLE_ACCOUNT_UID "')",
                     NULL, NULL, NULL) != SQLITE_OK)
      st = MC_ERR_DB;
    sqlite3_stmt *stmt = NULL;
    if (st == MC_OK && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) st = MC_ERR_DB;
    if (st == MC_OK) {
      sqlite3_bind_text(stmt, 1, MC_SCROBBLE_ACCOUNT_UID, -1, SQLITE_STATIC);
      for (int i = 0; i < count; i++)
        if (texts[i] && *texts[i]) sqlite3_bind_text(stmt, i + 2, texts[i], -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    }
    sqlite3_finalize(stmt);
    if (st == MC_OK) st = mc_sync_db_commit(sdb);
    if (st != MC_OK) mc_sync_db_rollback(sdb);
  }
  mc_sync_db_close(sdb);

  mc_mutex_lock(&sc->lock);
  sc->account_changed = 1;
  mc_cond_broadcast(&sc->wake);
  mc_mutex_unlock(&sc->lock);
  return st;
}

/* --- Plays --- */

/* A track as scrobbled: a single goes with the album it's from, a track with no album
 * without one, nor a track number. Artists as credited ("A feat. B"), as the files say. */
static const char k_track_sql[] =
  "SELECT coalesce(t.artist_credit, ar.name, al.artist_credit, aar.name), t.title, al.title,"
  " coalesce(al.artist_credit, aar.name),"
  " CASE WHEN al.uid IS NULL THEN NULL ELSE t.track_no END,"
  " t.duration_ms, t.mb_trackid"
  " FROM tracks t"
  " LEFT JOIN artists ar ON ar.uid = t.artist_uid"
  " LEFT JOIN albums al ON al.uid = t.album_uid"
  " LEFT JOIN artists aar ON aar.uid = al.artist_uid"
  " WHERE t.uid = ?1";

/* Steps k_track_sql for uid: 1 with a row Last.fm will take (an artist, and not too short),
 * else 0. stmt is left for the caller to finalize. */
static int find_track(sqlite3 *lib, const char *uid, sqlite3_stmt **stmt) {
  if (sqlite3_prepare_v2(lib, k_track_sql, -1, stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(*stmt, 1, uid, -1, SQLITE_STATIC);
  if (sqlite3_step(*stmt) != SQLITE_ROW || sqlite3_column_type(*stmt, 0) == SQLITE_NULL) return 0;
  return sqlite3_column_type(*stmt, 5) == SQLITE_NULL || sqlite3_column_int64(*stmt, 5) >= MIN_TRACK_MS;
}

static void enqueue(sqlite3 *lib, sqlite3 *db, const request *r) {
  sqlite3_stmt *track = NULL, *ins = NULL;
  if (find_track(lib, r->track_uid, &track) &&
      sqlite3_prepare_v2(db,
                         "INSERT INTO scrobble_queue (played_at_s, artist, track, album,"
                         " album_artist, track_no, duration_s, mb_trackid)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7 / 1000, ?8)",
                         -1, &ins, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(ins, 1, r->started_ms / 1000);
    for (int i = 0; i < 7; i++) sqlite3_bind_value(ins, i + 2, sqlite3_column_value(track, i));
    if (sqlite3_step(ins) != SQLITE_DONE)
      fprintf(stderr, "musiccore: couldn't queue a scrobble: %s\n", sqlite3_errmsg(db));
  }
  sqlite3_finalize(ins);
  sqlite3_finalize(track);
}

static const char *text(sqlite3_stmt *stmt, int col) {
  return (const char *)sqlite3_column_text(stmt, col);
}

static mc_status now_playing(sqlite3 *lib, const mc_lastfm_service *svc, const char *uid) {
  sqlite3_stmt *track = NULL;
  mc_status st = MC_OK;
  if (find_track(lib, uid, &track)) {
    mc_scrobble s = {0, text(track, 0), text(track, 1), text(track, 2), text(track, 3),
                     text(track, 6), sqlite3_column_int(track, 4),
                     (int)(sqlite3_column_int64(track, 5) / 1000)};
    int api_error;
    st = mc_lastfm_now_playing(svc, &s, &api_error);
  }
  sqlite3_finalize(track);
  return st;
}

/* Sends up to *batch of the oldest queued plays and takes the ones that are done off the
 * queue. *progress is 0 once there's nothing more to send. A batch the service won't take
 * is sent again a play at a time, and a single play it won't take is dropped. */
static mc_status submit(sqlite3 *db, const mc_lastfm_service *svc, int *batch, int *progress) {
  *progress = 0;
  char sql[160];
  snprintf(sql, sizeof sql, "DELETE FROM scrobble_queue WHERE played_at_s < %lld",
           (long long)(mc_wall_ms() / 1000) - MAX_AGE_S);
  if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return MC_ERR_DB;

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT seq, played_at_s, artist, track, album, album_artist, mb_trackid,"
                         " track_no, duration_s FROM scrobble_queue ORDER BY seq LIMIT ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_int(stmt, 1, *batch);
  int64_t seqs[MC_LASTFM_BATCH];
  mc_scrobble plays[MC_LASTFM_BATCH];
  char *strs[MC_LASTFM_BATCH][5];
  int n = 0;
  mc_status st = MC_OK;
  while (n < *batch && sqlite3_step(stmt) == SQLITE_ROW) {
    seqs[n] = sqlite3_column_int64(stmt, 0);
    for (int i = 0; i < 5; i++) {
      const char *v = text(stmt, 2 + i);
      strs[n][i] = v ? mc_strdup(v) : NULL;
      if (v && !strs[n][i]) st = MC_ERR_NOMEM;
    }
    plays[n] = (mc_scrobble){sqlite3_column_int64(stmt, 1), strs[n][0], strs[n][1], strs[n][2],
                             strs[n][3], strs[n][4], sqlite3_column_int(stmt, 7),
                             sqlite3_column_int(stmt, 8)};
    n++;
  }
  sqlite3_finalize(stmt);

  int done[MC_LASTFM_BATCH] = {0}, api_error = 0;
  if (st == MC_OK && n) st = mc_lastfm_scrobble(svc, plays, n, done, &api_error);
  int again = 0;
  if (st == MC_ERR_FORMAT && n > 1) {
    *batch = 1;
    again = 1;
    st = MC_OK;
  } else if (st == MC_ERR_FORMAT) {
    fprintf(stderr, "musiccore: the service won't take a scrobble (error %d); dropped\n", api_error);
    done[0] = 1;
    st = MC_OK;
  } else if (st == MC_OK) {
    *batch = MC_LASTFM_BATCH;
  }

  int kept = 0;
  sqlite3_stmt *del = NULL;
  if (n && !again && sqlite3_prepare_v2(db, "DELETE FROM scrobble_queue WHERE seq = ?1", -1, &del, NULL) == SQLITE_OK) {
    for (int i = 0; i < n; i++) {
      if (!done[i]) {
        kept++;
        continue;
      }
      sqlite3_bind_int64(del, 1, seqs[i]);
      sqlite3_step(del);
      sqlite3_reset(del);
    }
  }
  sqlite3_finalize(del);
  for (int i = 0; i < n; i++)
    for (int k = 0; k < 5; k++) free(strs[i][k]);

  if (st != MC_OK) return st;
  if (kept) return MC_ERR_NET; /* over the daily limit: back off */
  *progress = n > 0;
  return MC_OK;
}

static int queue_count(sqlite3 *db) {
  sqlite3_stmt *stmt;
  int n = 0;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM scrobble_queue", -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

/* --- Worker --- */

static int quitting(mc_scrobbler *sc) {
  mc_mutex_lock(&sc->lock);
  int quit = sc->quit;
  mc_mutex_unlock(&sc->lock);
  return quit;
}

/* The worker's calls stop when the scrobbler is freed. */
static int worker_cancelled(void *user) {
  return quitting(user);
}

static void run(void *arg) {
  mc_scrobbler *sc = arg;
  mc_sync_db *sdb = NULL;
  sqlite3 *lib = NULL;
  mc_status open_st = mc_sync_db_open(sc->config_dir, &sdb);
  if (open_st == MC_OK) {
    /* Only ever read, so a plain connection will do rather than the library's own open:
     * mc_scrobbler_new() was handed an open library, so the schema is current. Not
     * SQLITE_OPEN_READONLY, which can't always open a WAL database. */
    size_t len = strlen(sc->lib_dir) + sizeof MC_LIBRARY_DB_NAME + 1;
    char *path = malloc(len);
    open_st = MC_ERR_NOMEM;
    if (path) {
      snprintf(path, len, "%s%c" MC_LIBRARY_DB_NAME, sc->lib_dir, MC_PATH_SEP);
      open_st = sqlite3_open_v2(path, &lib, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, NULL) ==
                    SQLITE_OK
                  ? MC_OK
                  : MC_ERR_DB;
      if (lib) sqlite3_busy_timeout(lib, 5000);
    }
    free(path);
  }
  if (open_st != MC_OK) fprintf(stderr, "musiccore: the scrobbler couldn't open its databases\n");
  sqlite3 *db = sdb ? mc_sync_db_handle(sdb) : NULL;

  uint64_t next_try = 0;
  uint32_t backoff = 0;
  int batch = MC_LASTFM_BATCH;
  char *refused = NULL; /* a session key the service refused: not tried again */
  mc_status last_error = open_st;

  mc_mutex_lock(&sc->lock);
  for (;;) {
    request *reqs = sc->reqs;
    int nreqs = sc->nreqs;
    sc->reqs = NULL;
    sc->nreqs = sc->cap = 0;
    int quit = sc->quit;
    if (sc->account_changed) {
      sc->account_changed = 0;
      next_try = 0;
      backoff = 0;
    }
    mc_mutex_unlock(&sc->lock);

    account acc = {0};
    int sending = 0, pending = 0, has_key = mc_lastfm_build_key_present();
    if (open_st == MC_OK && account_read(db, &acc) == MC_OK) {
      has_key = account_has_key(&acc);
      if (refused && (!acc.session_key || strcmp(refused, acc.session_key))) {
        free(refused);
        refused = NULL;
        last_error = MC_OK;
      }
      const char *playing = NULL; /* only the newest matters */
      for (int i = 0; i < nreqs; i++) {
        if (reqs[i].started_ms < 0) playing = reqs[i].track_uid;
        else if (acc.session_key) enqueue(lib, db, &reqs[i]);
      }
      mc_lastfm_service svc = service_of(&acc);
      mc_http_cancel cancel = {worker_cancelled, sc};
      svc.cancel = &cancel;
      sending = acc.session_key && !refused && !quit;
      mc_status st = MC_OK;
      int called = 0;
      if (sending && playing) {
        st = now_playing(lib, &svc, playing);
        called = 1;
      }
      if (sending && st != MC_ERR_AUTH && mc_mono_ms() >= next_try && !quitting(sc)) {
        int progress = 1;
        while (progress && (st = submit(db, &svc, &batch, &progress)) == MC_OK && !quitting(sc)) {
        }
        called = 1;
      }
      if (st == MC_ERR_AUTH) {
        refused = mc_strdup(acc.session_key);
        sending = 0;
      } else if (st != MC_OK) { /* offline, the service down, or our own database */
        backoff = backoff ? backoff * 2 : BACKOFF_MIN_MS;
        if (backoff > BACKOFF_MAX_MS) backoff = BACKOFF_MAX_MS;
        next_try = mc_mono_ms() + backoff;
      } else if (called) {
        backoff = 0;
      }
      if (called) last_error = st;
      pending = queue_count(db);
    }
    free(reqs);

    mc_mutex_lock(&sc->lock);
    free(sc->status.username);
    free(sc->status.api_url);
    sc->status.username = acc.username;
    sc->status.api_url = acc.api_url;
    acc.username = acc.api_url = NULL;
    sc->status.pending = pending;
    sc->status.has_key = has_key;
    sc->status.last_error = last_error;
    account_clear(&acc);
    if (quit) break;
    if (sc->nreqs || sc->account_changed || sc->quit) continue;
    uint64_t now = mc_mono_ms(), wait = POLL_MS;
    if (sending && pending && next_try > now && next_try - now < wait) wait = next_try - now;
    else if (sending && pending && next_try <= now) wait = 1;
    mc_cond_wait_ms(&sc->wake, &sc->lock, (uint32_t)wait);
  }
  mc_mutex_unlock(&sc->lock);
  free(refused);
  sqlite3_close_v2(lib);
  mc_sync_db_close(sdb);
}

/* --- For merging libraries (scrobble_internal.h) --- */

int mc_scrobble_signed_in(sqlite3 *db, char **username) {
  if (username) *username = NULL;
  account acc;
  if (!db || account_read(db, &acc) != MC_OK) return 0;
  int in = acc.session_key != NULL;
  if (in && username) *username = mc_strdup(acc.username ? acc.username : "");
  account_clear(&acc);
  return in;
}

mc_status mc_scrobble_flush_once(sqlite3 *db, int *sent, int *left) {
  if (sent) *sent = 0;
  if (left) *left = 0;
  if (!db) return MC_ERR_INVALID_ARG;
  account acc;
  mc_status st = account_read(db, &acc);
  if (st != MC_OK) return st;
  int before = queue_count(db);
  if (!acc.session_key) {
    st = MC_ERR_AUTH;
  } else {
    mc_lastfm_service svc = service_of(&acc);
    int batch = MC_LASTFM_BATCH, progress = 1;
    while (progress && (st = submit(db, &svc, &batch, &progress)) == MC_OK) {
    }
  }
  account_clear(&acc);
  int after = queue_count(db);
  if (sent) *sent = before > after ? before - after : 0;
  if (left) *left = after;
  return st;
}

/* --- API --- */

mc_status mc_scrobbler_new(mc_library *lib, const char *config_dir, mc_scrobbler **out) {
  if (!lib || !config_dir || !*config_dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_scrobbler *sc = calloc(1, sizeof *sc);
  if (!sc) return MC_ERR_NOMEM;
  sc->lib_dir = mc_strdup(mc_library_dir(lib));
  sc->config_dir = mc_strdup(config_dir);
  if (!sc->lib_dir || !sc->config_dir) {
    free(sc->lib_dir);
    free(sc->config_dir);
    free(sc);
    return MC_ERR_NOMEM;
  }
  sc->status.has_key = mc_lastfm_build_key_present(); /* until the worker reads the account */
  mc_mutex_init(&sc->lock);
  mc_cond_init(&sc->wake);
  mc_status st = mc_thread_start(&sc->thread, run, sc);
  if (st != MC_OK) {
    mc_cond_destroy(&sc->wake);
    mc_mutex_destroy(&sc->lock);
    free(sc->lib_dir);
    free(sc->config_dir);
    free(sc);
    return st;
  }
  *out = sc;
  return MC_OK;
}

void mc_scrobbler_free(mc_scrobbler *sc) {
  if (!sc) return;
  mc_mutex_lock(&sc->lock);
  sc->quit = 1;
  mc_cond_broadcast(&sc->wake);
  mc_mutex_unlock(&sc->lock);
  mc_thread_join(&sc->thread);
  free(sc->reqs);
  free(sc->token);
  mc_scrobbler_status_clear(&sc->status);
  mc_cond_destroy(&sc->wake);
  mc_mutex_destroy(&sc->lock);
  free(sc->lib_dir);
  free(sc->config_dir);
  free(sc);
}

static mc_status post(mc_scrobbler *sc, const char *track_uid, int64_t started_ms) {
  if (!sc || !track_uid || strlen(track_uid) != MC_UID_STRLEN - 1) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sc->lock);
  mc_status st = MC_OK;
  if (sc->nreqs == sc->cap) {
    int cap = sc->cap ? sc->cap * 2 : 8;
    request *grown = realloc(sc->reqs, (size_t)cap * sizeof *grown);
    if (grown) {
      sc->reqs = grown;
      sc->cap = cap;
    } else {
      st = MC_ERR_NOMEM;
    }
  }
  if (st == MC_OK) {
    request *r = &sc->reqs[sc->nreqs++];
    memcpy(r->track_uid, track_uid, MC_UID_STRLEN);
    r->started_ms = started_ms;
    mc_cond_broadcast(&sc->wake);
  }
  mc_mutex_unlock(&sc->lock);
  return st;
}

mc_status mc_scrobbler_now_playing(mc_scrobbler *sc, const char *track_uid) {
  return post(sc, track_uid, -1);
}

mc_status mc_scrobbler_scrobble(mc_scrobbler *sc, const char *track_uid, int64_t started_ms) {
  if (started_ms < 0) return MC_ERR_INVALID_ARG;
  return post(sc, track_uid, started_ms);
}

/* The account as it is now, from a connection of the caller's own. */
static mc_status read_now(mc_scrobbler *sc, account *a) {
  mc_sync_db *sdb = NULL;
  mc_status st = mc_sync_db_open(sc->config_dir, &sdb);
  if (st == MC_OK) st = account_read(mc_sync_db_handle(sdb), a);
  mc_sync_db_close(sdb);
  return st;
}

/* A sign-in's calls stop once mc_scrobbler_auth_cancel() moves auth_gen past gen, or the
 * scrobbler is being freed. */
typedef struct auth_call {
  mc_scrobbler *sc;
  unsigned gen;
} auth_call;

static int auth_cancelled(void *user) {
  auth_call *c = user;
  mc_mutex_lock(&c->sc->lock);
  int cancelled = c->sc->quit || c->sc->auth_gen != c->gen;
  mc_mutex_unlock(&c->sc->lock);
  return cancelled;
}

mc_status mc_scrobbler_auth_begin(mc_scrobbler *sc, char **out_url) {
  if (!sc || !out_url) return MC_ERR_INVALID_ARG;
  *out_url = NULL;
  auth_call call = {sc, 0};
  mc_mutex_lock(&sc->lock);
  call.gen = sc->auth_gen;
  mc_mutex_unlock(&sc->lock);
  mc_http_cancel cancel = {auth_cancelled, &call};
  account a;
  mc_status st = read_now(sc, &a);
  if (st != MC_OK) return st;
  mc_lastfm_service svc = service_of(&a);
  svc.session_key = NULL;
  svc.cancel = &cancel;
  char *token = NULL;
  int api_error;
  st = mc_lastfm_get_token(&svc, &token, &api_error);
  char build_key[MC_LASTFM_KEY_MAX];
  const char *api_key = a.api_key;
  if (!api_key) api_key = mc_lastfm_build_key(build_key, sizeof build_key) ? build_key : "";
  char *key = st == MC_OK ? mc_url_escape(api_key) : NULL;
  mc_lastfm_wipe(build_key, sizeof build_key);
  char *tok = st == MC_OK ? mc_url_escape(token) : NULL;
  if (st == MC_OK && key && tok) {
    size_t len = strlen(a.auth_url) + strlen(key) + strlen(tok) + 32;
    if ((*out_url = malloc(len)))
      snprintf(*out_url, len, "%s%capi_key=%s&token=%s", a.auth_url,
               strchr(a.auth_url, '?') ? '&' : '?', key, tok);
  }
  if (st == MC_OK && !*out_url) st = MC_ERR_NOMEM;
  free(key);
  free(tok);
  account_clear(&a);
  if (st == MC_OK) {
    mc_mutex_lock(&sc->lock);
    free(sc->token);
    sc->token = token;
    sc->token_gen = call.gen;
    token = NULL;
    mc_mutex_unlock(&sc->lock);
  }
  free(token);
  return st;
}

mc_status mc_scrobbler_auth_finish(mc_scrobbler *sc) {
  if (!sc) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sc->lock);
  char *token = sc->token ? mc_strdup(sc->token) : NULL;
  /* The sign-in the token began, so a cancel between two polls stops the next one too. */
  auth_call call = {sc, sc->token_gen};
  mc_mutex_unlock(&sc->lock);
  if (!token) return MC_ERR_INVALID_ARG; /* no mc_scrobbler_auth_begin() */
  mc_http_cancel cancel = {auth_cancelled, &call};
  if (auth_cancelled(&call)) {
    free(token);
    return MC_ERR_NET;
  }

  account a;
  mc_status st = read_now(sc, &a);
  char *session_key = NULL, *username = NULL;
  if (st == MC_OK) {
    mc_lastfm_service svc = service_of(&a);
    svc.session_key = NULL;
    svc.cancel = &cancel;
    int api_error;
    st = mc_lastfm_get_session(&svc, token, &session_key, &username, &api_error);
    account_clear(&a);
  }
  if (st == MC_OK) {
    const char *texts[] = {session_key, username};
    st = write_account(sc, "UPDATE scrobble_account SET session_key = ?2, username = ?3 WHERE uid = ?1",
                       texts, 2);
  }
  if (st == MC_OK) {
    mc_mutex_lock(&sc->lock);
    if (sc->token && !strcmp(sc->token, token)) {
      free(sc->token);
      sc->token = NULL;
    }
    free(sc->status.username);
    sc->status.username = username;
    username = NULL;
    sc->status.last_error = MC_OK;
    mc_mutex_unlock(&sc->lock);
  }
  free(token);
  free(session_key);
  free(username);
  return st;
}

void mc_scrobbler_auth_cancel(mc_scrobbler *sc) {
  if (!sc) return;
  mc_mutex_lock(&sc->lock);
  sc->auth_gen++;
  mc_mutex_unlock(&sc->lock);
}

mc_status mc_scrobbler_sign_out(mc_scrobbler *sc) {
  if (!sc) return MC_ERR_INVALID_ARG;
  mc_status st = write_account(
    sc, "UPDATE scrobble_account SET session_key = NULL, username = NULL WHERE uid = ?1", NULL, 0);
  if (st == MC_OK) {
    mc_mutex_lock(&sc->lock);
    free(sc->status.username);
    sc->status.username = NULL;
    mc_mutex_unlock(&sc->lock);
  }
  return st;
}

mc_status mc_scrobbler_set_service(mc_scrobbler *sc, const char *api_url, const char *auth_url,
                                   const char *api_key, const char *api_secret) {
  if (!sc) return MC_ERR_INVALID_ARG;
  const char *texts[] = {api_url, auth_url, api_key, api_secret};
  /* Only a change signs out: setting the service it's already on keeps the session. */
  return write_account(sc,
                       "UPDATE scrobble_account SET api_url = ?2, auth_url = ?3, api_key = ?4,"
                       " api_secret = ?5, session_key = NULL, username = NULL"
                       " WHERE uid = ?1 AND (api_url IS NOT ?2 OR auth_url IS NOT ?3"
                       " OR api_key IS NOT ?4 OR api_secret IS NOT ?5)",
                       texts, 4);
}

mc_status mc_scrobbler_status_get(mc_scrobbler *sc, mc_scrobbler_status *out) {
  if (!sc || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  mc_mutex_lock(&sc->lock);
  *out = sc->status;
  out->username = sc->status.username ? mc_strdup(sc->status.username) : NULL;
  out->api_url = sc->status.api_url ? mc_strdup(sc->status.api_url) : NULL;
  int nomem = (sc->status.username && !out->username) || (sc->status.api_url && !out->api_url);
  mc_mutex_unlock(&sc->lock);
  if (nomem) {
    mc_scrobbler_status_clear(out);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

void mc_scrobbler_status_clear(mc_scrobbler_status *status) {
  if (!status) return;
  free(status->username);
  free(status->api_url);
  memset(status, 0, sizeof *status);
}
