/* The background scanner (musiccore/analysis.h, scanner.h).
 *
 * What a track needs is worked out from the tables each time, so there's no queue or cursor to
 * keep: a restart goes on where it stopped, a new import or a result arriving by sync changes
 * what's next, and raising MC_ANALYSIS_VERSION makes every track need it again.
 *
 * A file is tried if it's the track's current file (files.file_rev) and no result as good
 * exists: one of the current file, from the same or a better source, of this version or newer.
 * An original is tried before a copy. So a device holding only a transcode does analyse it, for
 * want of anything better, and stops as soon as a result from an original reaches it.
 *
 * A file that won't decode goes in analysis_failed, by its uid, size and mtime: it isn't tried
 * again until it changes or the algorithm does. That table is this device's own, as whether a
 * file decodes here says nothing of another device's file. A file that's only missing is passed
 * over and not recorded: the drive it's on may be back tomorrow. */
#include "analysis/scanner.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "util/platform.h"

#define BUSY_WAIT_MS 5000 /* as long as mc_library_open() waits for a lock */
#define HOLD_POLL_MS 250
/* Work this long, then rest as long, so a scan of a big library doesn't keep a core flat out
 * and a laptop's fan going for hours. Low priority keeps it out of the way, but not cool. */
#define BURST_MS 200

static atomic_int g_holds;
static uint32_t g_start_ms = 30000, g_idle_ms = 5 * 60 * 1000;
static int g_rests = 1;

void mc_analysis_hold(void) {
  atomic_fetch_add(&g_holds, 1);
}

void mc_analysis_release(void) {
  atomic_fetch_sub(&g_holds, 1);
}

static int held(void) {
  return atomic_load(&g_holds) > 0;
}

void mc_analysis_scanner_set_timing(uint32_t start_ms, uint32_t idle_ms, int rests) {
  g_start_ms = start_ms;
  g_idle_ms = idle_ms;
  g_rests = rests;
}

/* --- One track --- */

/* One row per file that could be tried, the track's best first. ?1 is the cursor, ?2 the
 * version. */
#define NEXT_SQL                                                                     \
  "SELECT t.uid, t.file_rev, f.uid, f.path, f.size_bytes, f.mtime_ms,"               \
  " f.source_file_uid IS NOT NULL"                                                   \
  " FROM tracks t JOIN files f ON f.track_uid = t.uid"                               \
  " WHERE t.uid > ?1 AND f.file_rev IS t.file_rev"                                   \
  " AND NOT EXISTS (SELECT 1 FROM analysis_failed x WHERE x.file_uid = f.uid"        \
  "  AND x.size_bytes IS f.size_bytes AND x.mtime_ms IS f.mtime_ms"                  \
  "  AND x.analysis_version >= ?2)"                                                  \
  " AND NOT EXISTS (SELECT 1 FROM track_analysis a WHERE a.track_uid = t.uid"        \
  "  AND a.file_rev IS t.file_rev AND a.source <= (f.source_file_uid IS NOT NULL)"   \
  "  AND a.analysis_version >= ?2)"                                                  \
  " ORDER BY t.uid, f.source_file_uid IS NOT NULL, f.uid LIMIT 1"

typedef struct candidate {
  char track[MC_UID_STRLEN], file[MC_UID_STRLEN];
  char *file_rev, *path;
  int64_t size, mtime;
  int has_size, has_mtime;
  mc_analysis_source source;
} candidate;

static void candidate_clear(candidate *c) {
  free(c->file_rev);
  free(c->path);
}

/* MC_ERR_NOT_FOUND if nothing's left. */
static mc_status next_candidate(mc_library *lib, const char *cursor, candidate *c) {
  memset(c, 0, sizeof *c);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), NEXT_SQL, -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, cursor, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, MC_ANALYSIS_VERSION);
  int rc = sqlite3_step(stmt);
  mc_status st = rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  if (st == MC_OK) {
    snprintf(c->track, sizeof c->track, "%s", (const char *)sqlite3_column_text(stmt, 0));
    snprintf(c->file, sizeof c->file, "%s", (const char *)sqlite3_column_text(stmt, 2));
    const char *rev = (const char *)sqlite3_column_text(stmt, 1);
    if (rev && !(c->file_rev = mc_strdup(rev))) st = MC_ERR_NOMEM;
    c->path = mc_library_abspath(lib, (const char *)sqlite3_column_text(stmt, 3));
    if (!c->path) st = MC_ERR_NOMEM;
    c->has_size = sqlite3_column_type(stmt, 4) != SQLITE_NULL;
    c->size = sqlite3_column_int64(stmt, 4);
    c->has_mtime = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
    c->mtime = sqlite3_column_int64(stmt, 5);
    c->source = sqlite3_column_int(stmt, 6) ? MC_ANALYSIS_LOSSY_COPY : MC_ANALYSIS_ORIGINAL;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) candidate_clear(c);
  return st;
}

/* Not logged, so no write transaction of the library's: this device's own, and one row. */
static mc_status record_failed(mc_library *lib, const candidate *c) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "INSERT OR REPLACE INTO analysis_failed"
                         " (file_uid, size_bytes, mtime_ms, analysis_version)"
                         " SELECT ?1, ?2, ?3, ?4 WHERE EXISTS (SELECT 1 FROM files WHERE uid = ?1)",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, c->file, -1, SQLITE_STATIC);
  if (c->has_size) sqlite3_bind_int64(stmt, 2, c->size);
  if (c->has_mtime) sqlite3_bind_int64(stmt, 3, c->mtime);
  sqlite3_bind_int(stmt, 4, MC_ANALYSIS_VERSION);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_analysis_scan_one(mc_library *lib, char cursor[MC_UID_STRLEN],
                               mc_analysis_cancel_fn cancel, void *user, mc_scan_step *step) {
  if (!lib || !cursor || !step) return MC_ERR_INVALID_ARG;
  *step = MC_SCAN_NONE;
  candidate c;
  mc_status st = next_candidate(lib, cursor, &c);
  if (st == MC_ERR_NOT_FOUND) return MC_OK;
  if (st != MC_OK) return st;

  /* The file as it was before and after: a retag meanwhile may have moved the audio under the
   * decoder, and proves nothing about the file either way. */
  int64_t size, mtime, size_after, mtime_after;
  if (!mc_file_exists(c.path) || mc_file_info(c.path, &size, &mtime) != MC_OK) {
    *step = MC_SCAN_SKIPPED;
    memcpy(cursor, c.track, MC_UID_STRLEN);
    candidate_clear(&c);
    return MC_OK;
  }
  mc_analysis a;
  mc_status got = mc_analysis_file(c.path, cancel, user, &a);
  int same = mc_file_info(c.path, &size_after, &mtime_after) == MC_OK && size == size_after &&
             mtime == mtime_after;
  if (got == MC_ERR_IN_USE) {
    *step = MC_SCAN_CANCELLED;
  } else if (got == MC_ERR_NOMEM || !same) {
    *step = MC_SCAN_SKIPPED;
    memcpy(cursor, c.track, MC_UID_STRLEN);
  } else if (got != MC_OK) {
    /* The cursor stays: this track may have another file to try. */
    if ((st = record_failed(lib, &c)) == MC_OK) *step = MC_SCAN_FAILED;
  } else if (cancel && cancel(user)) {
    *step = MC_SCAN_CANCELLED;
  } else {
    a.source = c.source;
    if ((st = mc_analysis_store(lib, c.track, c.file_rev, c.source, &a)) == MC_OK)
      *step = MC_SCAN_DONE;
    memcpy(cursor, c.track, MC_UID_STRLEN);
  }
  candidate_clear(&c);
  return st;
}

/* --- The thread --- */

struct mc_analysis_scanner {
  char *dir;
  mc_mutex lock; /* guards stop */
  mc_cond wake;  /* broadcast on stop */
  mc_thread thread;
  int stop;
  uint64_t burst_from;
};

static int stopping(mc_analysis_scanner *s) {
  mc_mutex_lock(&s->lock);
  int stop = s->stop;
  mc_mutex_unlock(&s->lock);
  return stop;
}

/* Waits up to ms, or until told to stop; whether it was. */
static int wait_ms(mc_analysis_scanner *s, uint32_t ms) {
  uint64_t until = mc_mono_ms() + ms;
  mc_mutex_lock(&s->lock);
  for (;;) {
    uint64_t now = mc_mono_ms();
    if (s->stop || now >= until) break;
    mc_cond_wait_ms(&s->wake, &s->lock, (uint32_t)(until - now));
  }
  int stop = s->stop;
  mc_mutex_unlock(&s->lock);
  return stop;
}

/* Between pieces of a file and before a write: stands aside for a hold, and rests. */
static int pause_point(void *user) {
  mc_analysis_scanner *s = user;
  if (stopping(s)) return 1;
  uint64_t now = mc_mono_ms();
  if (g_rests && now - s->burst_from >= BURST_MS) {
    if (wait_ms(s, (uint32_t)(now - s->burst_from))) return 1;
    s->burst_from = mc_mono_ms();
  }
  while (held())
    if (wait_ms(s, HOLD_POLL_MS)) return 1;
  return 0;
}

/* Instead of a busy timeout, so a stop isn't held up by another connection's lock. */
static int busy(void *user, int tries) {
  if (stopping(user) || tries >= BUSY_WAIT_MS / 20) return 0;
  mc_sleep_ms(20);
  return 1;
}

static void scan(mc_analysis_scanner *s, mc_library *lib) {
  char cursor[MC_UID_STRLEN] = "";
  int worked = 0;
  while (!stopping(s)) {
    s->burst_from = mc_mono_ms();
    mc_scan_step step = MC_SCAN_NONE;
    mc_status st = mc_analysis_scan_one(lib, cursor, pause_point, s, &step);
    if (st != MC_OK) { /* the library's busy or gone for now: there's always the next pass */
      fprintf(stderr, "musiccore: analysis: %s\n", mc_status_str(st));
      step = MC_SCAN_NONE;
    }
    if (step == MC_SCAN_DONE || step == MC_SCAN_FAILED) worked = 1;
    if (step != MC_SCAN_NONE) continue;
    /* The end of a pass. One that did something goes round once more for what came meanwhile,
     * and what's passed over waits for the next idle look. */
    cursor[0] = '\0';
    if (!worked && wait_ms(s, g_idle_ms)) break;
    worked = 0;
  }
}

static void scanner_thread(void *arg) {
  mc_analysis_scanner *s = arg;
  mc_thread_background();
  if (wait_ms(s, g_start_ms)) return;
  mc_library *lib = NULL;
  if (mc_library_open_internal(s->dir, &lib) != MC_OK) return;
  sqlite3_busy_handler(mc_library_db(lib), busy, s);
  scan(s, lib);
  mc_library_close(lib);
}

mc_status mc_analysis_scanner_start(const char *dir, mc_analysis_scanner **out) {
  if (!dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_analysis_scanner *s = calloc(1, sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  if (!(s->dir = mc_strdup(dir))) {
    free(s);
    return MC_ERR_NOMEM;
  }
  mc_mutex_init(&s->lock);
  mc_cond_init(&s->wake);
  mc_status st = mc_thread_start(&s->thread, scanner_thread, s);
  if (st != MC_OK) {
    mc_cond_destroy(&s->wake);
    mc_mutex_destroy(&s->lock);
    free(s->dir);
    free(s);
    return st;
  }
  *out = s;
  return MC_OK;
}

void mc_analysis_scanner_stop(mc_analysis_scanner *s) {
  if (!s) return;
  mc_mutex_lock(&s->lock);
  s->stop = 1;
  mc_cond_broadcast(&s->wake);
  mc_mutex_unlock(&s->lock);
  mc_thread_join(&s->thread);
  mc_cond_destroy(&s->wake);
  mc_mutex_destroy(&s->lock);
  free(s->dir);
  free(s);
}
