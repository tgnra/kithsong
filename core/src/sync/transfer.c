/* The file phase: fetching what the profile wants, serving the peer, reconciling the library
 * folder, verification and sync_status. See transfer.h and "Files", "Lossy copies" and
 * "Verification" in docs/sync-protocol.md.
 *
 * Receiving. The plan (plan.c) says which tracks this device wants and in what format; a
 * wanted track is missing when no files row holds it in that format. The receiver asks the
 * peer what it has (HAVE_REQ), then fetches each missing track it can: an original, a
 * cached transcode, or a transcode the peer makes for it if the peer may transcode and the
 * work falls to it (see assigned_to_peer). Each file is written to <dest>.part while it is
 * hashed, and renamed into place only if the hash matches FILE_HDR's. A mismatch is retried
 * once; a second one counts the track as corrupt for this session.
 *
 * Serving. Originals are sent as they are; Opus copies come from the transcode cache
 * (<config>/transcode-cache, see transcode.h), from a transcode this device received at
 * that bitrate, or are encoded on demand when this device may transcode. A lossy original
 * no bigger than the Opus would be is sent instead of a transcode.
 *
 * Files rows. A received original gets source_peer_uid (who sent it) but no
 * source_file_uid, so it is still an original and may be served as one; a transcode gets
 * both. Originals imported here have neither: they are never deleted by verification, and
 * reconcile keeps them until an archive peer reports holding a verified copy.
 *
 * Replaced files. When an import swaps a track's original for a better file, the track gets
 * a new tracks.file_rev, and every files row records the file_rev it was made from
 * (files.file_rev). A copy received from a peer, or a transcode, whose file_rev differs from
 * its track's is stale (STALE_SQL): it satisfies nothing, so the track is fetched again,
 * and reconcile then drops the stale copy like one in the wrong format. Only files whose
 * file_rev matches are offered to peers. An original imported here is never stale: it's
 * this device's own, whatever another device swapped in. */
#include "sync/transfer.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>
#include <yyjson.h>

#include "library/library_internal.h"
#include "musiccore/delete.h"
#include "musiccore/transcode.h"
#include "sync/archive.h"
#include "sync/libmerge.h"
#include "sync/plan.h"
#include "tagger/coverart.h"
#include "tagger/organize.h"

/* Tracks per HAVE_REQ and PREPARE_REQ, so each message and its answer fit in a frame. */
#define HAVE_CHUNK 400
#define HAVE_MAX 600
#define PREPARE_CHUNK 1000
/* How long a receiver waits for FILE_HDR: the sender may be encoding the file. */
#define FETCH_WAIT_MS (10 * 60 * 1000)
#define MAX_FILE_BYTES ((int64_t)4 << 30)
/* The largest cover COVER_HDR may announce; a cover is held in memory whole. */
#define MAX_COVER_BYTES ((int64_t)32 << 20)
/* A device needs this much free space to transcode for others. */
#define MIN_TRANSCODE_FREE ((int64_t)256 << 20)
/* Unknown files younger than this are left alone: an import may be about to record them. */
#define UNKNOWN_GRACE_MS ((int64_t)60 * 60 * 1000)
#define SHA_STRLEN 65
/* For a files row f of track t: whether f was made from an older file of the track. */
#define STALE_SQL \
  "((f.source_file_uid IS NOT NULL OR f.source_peer_uid IS NOT NULL)" \
  " AND f.file_rev IS NOT t.file_rev)"

typedef struct mc_transfer {
  int receiving; /* holds sync->files_busy */
  int fetched;   /* this side ran its turn with a plan it can reconcile against */
  mc_plan *plan;
  /* Tracks whose originals imported here an archive peer holds verified copies of. */
  char (*archived)[MC_UID_STRLEN];
  int narchived, archived_cap;
} mc_transfer;

/* --- Small helpers --- */

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

static int is_sha(const char *s) {
  if (!s || strlen(s) != SHA_STRLEN - 1) return 0;
  for (; *s; s++)
    if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
  return 1;
}

static int valid_bitrate(int kbps) {
  for (int i = 0; i < MC_OPUS_BITRATE_COUNT; i++)
    if (mc_opus_bitrates[i] == kbps) return 1;
  return 0;
}

static char *join(const char *a, char sep, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *out = malloc(la + lb + 2);
  if (!out) return NULL;
  memcpy(out, a, la);
  size_t n = la;
  if (sep) out[n++] = sep;
  memcpy(out + n, b, lb + 1);
  return out;
}

mc_status (*mc_transfer_trash)(const char *path) = mc_trash;

static char *cache_dir(mc_sync *sync) {
  return join(sync->config_dir, MC_PATH_SEP, MC_TRANSCODE_CACHE_DIR);
}

/* <cache>/<file_uid>/opus-<kbps>.opus */
static char *cache_path(mc_sync *sync, const char *file_uid, int kbps) {
  char *dir = cache_dir(sync);
  char name[MC_UID_STRLEN + 32];
  snprintf(name, sizeof name, "%s%copus-%d.opus", file_uid, MC_PATH_SEP, kbps);
  char *path = dir ? join(dir, MC_PATH_SEP, name) : NULL;
  free(dir);
  return path;
}

/* The lowercase extension of path, or "" if it has none (at most 15 bytes). */
static void extension(const char *path, char out[16]) {
  out[0] = '\0';
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/'), *sep = strrchr(path, MC_PATH_SEP);
  if (sep > slash) slash = sep;
  if (!dot || (slash && dot < slash) || strlen(dot + 1) >= 16) return;
  size_t i = 0;
  for (const char *p = dot + 1; *p; p++) out[i++] = (char)tolower((unsigned char)*p);
  out[i] = '\0';
}

/* The audio extensions import takes; a received file must have one of them. */
static int audio_ext(const char *ext) {
  static const char *const exts[] = {"flac", "mp3", "opus", "wav", "aif", "aiff", NULL};
  for (const char *const *e = exts; *e; e++)
    if (!strcmp(ext, *e)) return 1;
  return 0;
}

static void hex(const unsigned char digest[32], char out[SHA_STRLEN]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i] = digits[digest[i] >> 4];
    out[2 * i + 1] = digits[digest[i] & 15];
  }
  out[SHA_STRLEN - 1] = '\0';
}

static mc_status hash_file(const char *path, char out[SHA_STRLEN]) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return MC_ERR_IO;
  unsigned char *buf = malloc(1 << 16);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mc_status st = buf && mbedtls_sha256_starts(&ctx, 0) == 0 ? MC_OK : MC_ERR_NOMEM;
  size_t n;
  while (st == MC_OK && (n = fread(buf, 1, 1 << 16, f)) > 0)
    if (mbedtls_sha256_update(&ctx, buf, n) != 0) st = MC_ERR_IO;
  if (st == MC_OK && ferror(f)) st = MC_ERR_IO;
  unsigned char digest[32];
  if (st == MC_OK && mbedtls_sha256_finish(&ctx, digest) != 0) st = MC_ERR_IO;
  if (st == MC_OK) hex(digest, out);
  mbedtls_sha256_free(&ctx);
  free(buf);
  fclose(f);
  return st;
}

static uint64_t fnv(uint64_t h, const char *s) {
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211u;
  return h;
}

/* --- Messages --- */

static mc_status send_doc(mc_channel *ch, uint8_t type, yyjson_mut_doc *doc) {
  size_t len = 0;
  char *json = doc ? yyjson_mut_write(doc, 0, &len) : NULL;
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = len <= MC_CHANNEL_MAX_PAYLOAD ? mc_channel_send(ch, type, json, len)
                                               : MC_ERR_INVALID_ARG;
  free(json);
  return st;
}

static yyjson_mut_doc *new_doc(yyjson_mut_val **root) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return NULL;
  *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, *root);
  return doc;
}

static yyjson_doc *parse(const unsigned char *payload, size_t len) {
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
    yyjson_doc_free(doc);
    return NULL;
  }
  return doc;
}

static const char *obj_str(yyjson_doc *doc, const char *key) {
  return yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), key));
}

static int64_t obj_int(yyjson_doc *doc, const char *key) {
  yyjson_val *v = yyjson_obj_get(yyjson_doc_get_root(doc), key);
  return yyjson_is_int(v) ? yyjson_get_sint(v) : -1;
}

static mc_status send_file_err(mc_channel *ch, const char *track, const char *reason) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_obj_add_strcpy(doc, root, "track", track ? track : "");
  yyjson_mut_obj_add_str(doc, root, "reason", reason);
  return send_doc(ch, MC_MSG_FILE_ERR, doc);
}

static void progress(mc_session *s) {
  mc_sync_progress p = {s->peer_uid,      MC_SYNC_PHASE_FILES, s->rows_sent,     s->rows_received,
                        s->files_done,    s->files_total,      s->bytes_received, s->bytes_sent};
  mc_sync_emit_progress(s->sync, &p);
}

/* --- Exclusion --- */

/* One device fetches, reconciles or verifies at a time, so two sessions never write the
 * same file or delete what the other just fetched. */
static int files_try(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int got = !sync->files_busy;
  if (got) sync->files_busy = 1;
  mc_mutex_unlock(&sync->lock);
  return got;
}

static void files_wait(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  while (sync->files_busy) mc_cond_wait_ms(&sync->cond, &sync->lock, 200);
  sync->files_busy = 1;
  mc_mutex_unlock(&sync->lock);
}

static void files_release(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  sync->files_busy = 0;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

/* --- Devices --- */

/* Whether uid may transcode for others (capabilities and kind: phones on battery and
 * watches never do), and its cpu_score. */
static int transcodes_for_others(mc_sync_db *sdb, const char *uid, int64_t *score) {
  sqlite3_stmt *stmt;
  int ok = 0;
  *score = 0;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT d.kind, c.cpu_score, c.free_bytes, c.on_power, c.can_transcode"
                         " FROM devices d JOIN capabilities c ON c.device_uid = d.uid"
                         " WHERE d.uid = ?1 AND d.removed = 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *kind = (const char *)sqlite3_column_text(stmt, 0);
    int known_free = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
    ok = (!kind || strcmp(kind, "watch") != 0) && sqlite3_column_int(stmt, 3) &&
         sqlite3_column_int(stmt, 4) &&
         (!known_free || sqlite3_column_int64(stmt, 2) >= MIN_TRANSCODE_FREE);
    *score = sqlite3_column_int64(stmt, 1);
    if (*score < 1) *score = 1;
  }
  sqlite3_finalize(stmt);
  return ok;
}

/* This device, as the sender. */
static int self_transcodes(mc_session *s) {
  if (s->sync->kind == MC_DEVICE_WATCH) return 0;
  int64_t score;
  return transcodes_for_others(s->conn->sdb, mc_sync_db_device_uid(s->conn->sdb), &score);
}

/* Whether uid's profile has it hold every original: an archive, or all, original, no
 * budget. */
static int is_archive(mc_sync_db *sdb, const char *uid) {
  return mc_archive_is(mc_sync_db_handle(sdb), uid) > 0; /* a void demotion included */
}

static int holds_originals(mc_sync_db *sdb, const char *uid) {
  if (is_archive(sdb, uid)) return 1;
  sqlite3_stmt *stmt;
  int yes = 0;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT 1 FROM profiles WHERE device_uid = ?1 AND"
                         " mode = 'all' AND format = 'original' AND budget_enabled = 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return yes;
}

/* A member that could transcode for the receiver, with its weight. */
typedef struct transcoder {
  char uid[MC_UID_STRLEN];
  int64_t score;
} transcoder;

/* The online members that may transcode and hold every original, the session's peer
 * first. 0 (and *out NULL) if the peer itself may not transcode. */
static int list_transcoders(mc_session *s, transcoder **out) {
  *out = NULL;
  mc_sync_db *sdb = s->conn->sdb;
  int64_t score;
  if (!transcodes_for_others(sdb, s->peer_uid, &score)) return 0;
  transcoder *list = malloc(sizeof *list);
  if (!list) return 0;
  memcpy(list[0].uid, s->peer_uid, MC_UID_STRLEN);
  list[0].score = score;
  int n = 1;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT uid FROM devices WHERE removed = 0 AND uid <> ?1 AND uid <> ?2",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, s->peer_uid, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *uid = (const char *)sqlite3_column_text(stmt, 0);
      if (!is_uid(uid) || !mc_sync_peer_online(s->sync, uid) ||
          !transcodes_for_others(sdb, uid, &score) || !holds_originals(sdb, uid))
        continue;
      transcoder *more = realloc(list, (size_t)(n + 1) * sizeof *more);
      if (!more) break;
      list = more;
      memcpy(list[n].uid, uid, MC_UID_STRLEN);
      list[n].score = score;
      n++;
    }
    sqlite3_finalize(stmt);
  }
  *out = list;
  return n;
}

/* Transcodes are split across the candidates in proportion to their cpu_score: each track
 * goes to one of them by a hash of its uid, the same on every attempt. */
static int assigned_to_peer(const transcoder *tc, int n, const char *track_uid) {
  if (n <= 1) return n == 1;
  uint64_t total = 0;
  for (int i = 0; i < n; i++) total += (uint64_t)tc[i].score;
  uint64_t at = fnv(14695981039346656037u, track_uid) % total;
  for (int i = 0; i < n; i++) {
    if (at < (uint64_t)tc[i].score) return i == 0;
    at -= (uint64_t)tc[i].score;
  }
  return 0;
}

/* --- Files here --- */

/* A file this device can send. */
typedef struct held {
  char file_uid[MC_UID_STRLEN]; /* the original's uid: its own, or a transcode's source */
  char *path;                   /* absolute */
  char format[16];
  int bitrate;
  int64_t size, mtime;
  char sha[SHA_STRLEN]; /* "" if not recorded */
  int duration_ms;
  char source_sha[SHA_STRLEN]; /* an original's files.source_sha256, or "" */
} held;

static void held_free(held *h) {
  free(h->path);
  h->path = NULL;
}

/* Fills h from a row of uid, path, format, bitrate, size, mtime, sha256, duration, and
 * optionally source_sha256. 1 if the file is there. */
static int held_from_row(mc_library *lib, sqlite3_stmt *stmt, held *h) {
  memset(h, 0, sizeof *h);
  const char *uid = (const char *)sqlite3_column_text(stmt, 0);
  const char *path = (const char *)sqlite3_column_text(stmt, 1);
  const char *format = (const char *)sqlite3_column_text(stmt, 2);
  const char *sha = (const char *)sqlite3_column_text(stmt, 6);
  if (!is_uid(uid) || !path) return 0;
  memcpy(h->file_uid, uid, MC_UID_STRLEN);
  snprintf(h->format, sizeof h->format, "%s", format ? format : "");
  h->bitrate = sqlite3_column_int(stmt, 3);
  h->size = sqlite3_column_int64(stmt, 4);
  h->mtime = sqlite3_column_int64(stmt, 5);
  if (is_sha(sha)) memcpy(h->sha, sha, SHA_STRLEN);
  h->duration_ms = sqlite3_column_int(stmt, 7);
  const char *source_sha = sqlite3_column_count(stmt) > 8
                             ? (const char *)sqlite3_column_text(stmt, 8) : NULL;
  if (is_sha(source_sha)) memcpy(h->source_sha, source_sha, SHA_STRLEN);
  h->path = mc_library_abspath(lib, path);
  if (h->path && mc_file_exists(h->path)) return 1;
  held_free(h);
  return 0;
}

/* This device's original of track (imported ones first), if the file is there and is the
 * track's current file (file_rev). */
static int find_original(mc_library *lib, const char *track, held *h) {
  sqlite3_stmt *stmt;
  int found = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.uid, f.path, f.format, f.bitrate_kbps, f.size_bytes, f.mtime_ms,"
                         " f.sha256, t.duration_ms, f.source_sha256"
                         " FROM files f JOIN tracks t ON t.uid = f.track_uid"
                         " WHERE f.track_uid = ?1 AND f.source_file_uid IS NULL"
                         " AND f.file_rev IS t.file_rev"
                         " ORDER BY f.source_peer_uid IS NOT NULL, f.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
  while (!found && sqlite3_step(stmt) == SQLITE_ROW) found = held_from_row(lib, stmt, h);
  sqlite3_finalize(stmt);
  return found;
}

/* A transcode of track at kbps this device received, if the file is there. */
static int find_transcode(mc_library *lib, const char *track, int kbps, held *h) {
  sqlite3_stmt *stmt;
  int found = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.source_file_uid, f.path, f.format, f.bitrate_kbps, f.size_bytes,"
                         " f.mtime_ms, f.sha256, t.duration_ms"
                         " FROM files f JOIN tracks t ON t.uid = f.track_uid"
                         " WHERE f.track_uid = ?1 AND f.source_file_uid IS NOT NULL"
                         " AND f.bitrate_kbps = ?2 AND NOT " STALE_SQL,
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, kbps);
  while (!found && sqlite3_step(stmt) == SQLITE_ROW) found = held_from_row(lib, stmt, h);
  sqlite3_finalize(stmt);
  return found;
}

/* Whether some file of track here is in the format t wants, and not stale. */
static int track_satisfied(mc_library *lib, const mc_plan_track *t) {
  sqlite3_stmt *stmt;
  int yes = 0;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.format, f.bitrate_kbps, f.size_bytes, f.source_file_uid,"
                         " f.source_peer_uid FROM files f JOIN tracks t ON t.uid = f.track_uid"
                         " WHERE f.track_uid = ?1 AND NOT " STALE_SQL,
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, t->track_uid, -1, SQLITE_STATIC);
  while (!yes && sqlite3_step(stmt) == SQLITE_ROW) {
    mc_file_kind kind = mc_plan_file_kind((const char *)sqlite3_column_text(stmt, 3),
                                          (const char *)sqlite3_column_text(stmt, 4));
    yes = mc_plan_file_satisfies(t, kind, (const char *)sqlite3_column_text(stmt, 0),
                                 sqlite3_column_int(stmt, 1), sqlite3_column_int64(stmt, 2));
  }
  sqlite3_finalize(stmt);
  return yes;
}

/* --- Paths --- */

static int path_known(mc_library *lib, const char *rel) {
  sqlite3_stmt *stmt;
  int yes = 0;
#ifdef _WIN32
  const char *sql = "SELECT 1 FROM files WHERE path = ?1 COLLATE NOCASE";
#else
  const char *sql = "SELECT 1 FROM files WHERE path = ?1";
#endif
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
  sqlite3_bind_text(stmt, 1, rel, -1, SQLITE_STATIC);
  yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return yes;
}

static int path_taken(mc_library *lib, const char *rel) {
  if (path_known(lib, rel)) return 1;
  char *abs = mc_library_abspath(lib, rel);
  int yes = !abs || mc_file_exists(abs);
  free(abs);
  return yes;
}

/* Where a received file of track goes, relative to the library folder: the place import
 * would file it (see organize.h), as "name (2).ext" and so on if that's taken. Paths are
 * never taken from the peer. */
static char *dest_path(mc_library *lib, const char *track, const char *ext) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.title, t.track_no, t.disc_no, al.uid, al.title, al.year,"
                         " al.disc_total, coalesce(al.artist_credit, aar.name),"
                         " coalesce(t.artist_credit, ar.name)"
                         " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"
                         " LEFT JOIN artists aar ON aar.uid = al.artist_uid"
                         " LEFT JOIN artists ar ON ar.uid = t.artist_uid WHERE t.uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
  char *rel = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *title = (const char *)sqlite3_column_text(stmt, 0);
    const char *album_artist = (const char *)sqlite3_column_text(stmt, 7);
    const char *artist = (const char *)sqlite3_column_text(stmt, 8);
    if (!album_artist || !*album_artist) album_artist = artist;
    char *dir, *name;
    if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) { /* no album */
      dir = mc_artist_dir(artist);
      name = mc_track_file(title, 0, 0, 0, ext);
    } else {
      dir = mc_album_dir(album_artist, (const char *)sqlite3_column_text(stmt, 4),
                         sqlite3_column_int(stmt, 5));
      name = mc_track_file(title, sqlite3_column_int(stmt, 2), sqlite3_column_int(stmt, 6),
                           sqlite3_column_int(stmt, 1), ext);
    }
    if (dir && name) rel = join(dir, '/', name);
    free(dir);
    free(name);
  }
  sqlite3_finalize(stmt);
  if (!rel || !path_taken(lib, rel)) return rel;

  const char *dot = strrchr(rel, '.');
  size_t base = dot ? (size_t)(dot - rel) : strlen(rel), len = strlen(rel) + 16;
  char *alt = malloc(len);
  for (int n = 2; alt && n < 1000; n++) {
    snprintf(alt, len, "%.*s (%d)%s", (int)base, rel, n, dot ? dot : "");
    if (!path_taken(lib, alt)) {
      free(rel);
      return alt;
    }
  }
  free(alt);
  free(rel);
  return NULL;
}

/* rel's absolute path if reconcile may delete it, else NULL. Only files inside the library
 * folder qualify: rel is relative, has no empty, "." or ".." parts (nor ones of only dots and
 * spaces, which Windows reads as those) and no colon (a drive or a stream), and no folder on
 * the way is a link or junction, which could lead outside. Rows and walks never make such a
 * path; this is so a damaged row can't. */
static char *deletable_path(mc_library *lib, const char *rel) {
  int ok = rel && *rel && !strchr(rel, ':');
  for (const char *p = rel; ok && *p;) {
    size_t n = strcspn(p, "/\\");
    ok = n > 0 && strspn(p, ". ") < n;
    p += n;
    if (*p) ok = ok && *++p; /* no trailing separator */
  }
  char *abs = ok ? mc_library_abspath(lib, rel) : NULL;
  char *sep = abs ? abs + strlen(mc_library_dir(lib)) : NULL;
  while (ok && abs && (sep = strchr(sep + 1, MC_PATH_SEP))) {
    *sep = '\0';
    ok = !mc_is_link(abs);
    *sep = MC_PATH_SEP;
  }
  if (ok && abs) return abs;
  free(abs);
  if (rel)
    fprintf(stderr, "musiccore: sync: not deleting %s: it isn't inside the library folder\n", rel);
  return NULL;
}

static int is_cover(const char *name) {
  return !strcmp(name, "cover.jpg") || !strcmp(name, "cover.png");
}

typedef struct dir_scan {
  int others;
  int covers;
} dir_scan;

static void scan_entry(void *user, const char *name, int is_dir) {
  dir_scan *d = user;
  if (!is_dir && is_cover(name))
    d->covers++;
  else
    d->others++;
}

/* After a file at rel went: removes its album folder and then its artist folder if
 * nothing but a cover is left in them. */
static void prune_dirs(mc_library *lib, const char *rel) {
  char *abs = deletable_path(lib, rel);
  for (int level = 0; abs && level < 2; level++) {
    char *sep = strrchr(abs, MC_PATH_SEP);
    if (!sep || (size_t)(sep - abs) <= strlen(mc_library_dir(lib))) break;
    *sep = '\0';
    dir_scan d = {0, 0};
    if (mc_list_dir(abs, scan_entry, &d) != MC_OK || d.others) break;
    if (d.covers) {
      const char *covers[] = {"cover.jpg", "cover.png"};
      for (int i = 0; i < 2; i++) {
        char *c = join(abs, MC_PATH_SEP, covers[i]);
        if (c && mc_file_exists(c)) mc_remove(c);
        free(c);
      }
    }
    if (mc_rmdir(abs) != MC_OK) break;
  }
  free(abs);
}

char *mc_transfer_deletable_path(mc_library *lib, const char *rel) {
  return deletable_path(lib, rel);
}

void mc_transfer_prune_dirs(mc_library *lib, const char *rel) {
  prune_dirs(lib, rel);
}

/* Deletes a files row, then its file and any album folder that leaves empty. */
static mc_status drop_file(mc_library *lib, const char *file_uid, const char *rel) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "DELETE FROM files WHERE uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK) {
    st = MC_ERR_DB;
  } else {
    sqlite3_bind_text(stmt, 1, file_uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) {
    mc_library_rollback(lib);
    return st;
  }
  char *abs = deletable_path(lib, rel);
  if (abs && mc_file_exists(abs)) mc_remove(abs);
  if (abs) prune_dirs(lib, rel);
  free(abs);
  return MC_OK;
}

/* --- Serving --- */

/* FILE_HDR then the bytes of path. The hash is the recorded one when the file is unchanged
 * since it was recorded, else computed first. An original says what it was imported from
 * (source_sha256), if known. Should the file shrink while it's sent, the rest is zeros: the
 * receiver's hash check then fails, as it should. */
static mc_status send_file(mc_session *s, const char *track, const held *h, const char *format,
                           int kbps) {
  int64_t size = 0, mtime = 0;
  char sha[SHA_STRLEN];
  if (mc_file_info(h->path, &size, &mtime) != MC_OK || size > MAX_FILE_BYTES)
    return send_file_err(s->ch, track, "io");
  if (h->sha[0] && size == h->size && mtime == h->mtime)
    memcpy(sha, h->sha, SHA_STRLEN);
  else if (hash_file(h->path, sha) != MC_OK)
    return send_file_err(s->ch, track, "io");
  FILE *f = mc_fopen(h->path, "rb");
  if (!f) return send_file_err(s->ch, track, "io");
  char ext[16];
  extension(h->path, ext);

  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  mc_status st = MC_ERR_NOMEM;
  if (doc) {
    yyjson_mut_obj_add_strcpy(doc, root, "track", track);
    yyjson_mut_obj_add_strcpy(doc, root, "file", h->file_uid);
    yyjson_mut_obj_add_str(doc, root, "format", format);
    yyjson_mut_obj_add_int(doc, root, "kbps", kbps);
    yyjson_mut_obj_add_strcpy(doc, root, "ext", ext);
    yyjson_mut_obj_add_int(doc, root, "size", size);
    yyjson_mut_obj_add_strcpy(doc, root, "sha256", sha);
    if (!strcmp(format, "original") && h->source_sha[0])
      yyjson_mut_obj_add_strcpy(doc, root, "source_sha256", h->source_sha);
    st = send_doc(s->ch, MC_MSG_FILE_HDR, doc);
  }
  unsigned char *buf = st == MC_OK ? malloc(MC_CHANNEL_MAX_PAYLOAD) : NULL;
  if (st == MC_OK && !buf) st = MC_ERR_NOMEM;
  mc_mutex_lock(&s->sync->lock);
  uint32_t delay = s->sync->send_delay_ms; /* tests only */
  mc_mutex_unlock(&s->sync->lock);
  for (int64_t sent = 0; st == MC_OK && sent < size;) {
    if (delay) mc_sleep_ms(delay);
    size_t want = size - sent < MC_CHANNEL_MAX_PAYLOAD ? (size_t)(size - sent)
                                                        : MC_CHANNEL_MAX_PAYLOAD;
    size_t n = fread(buf, 1, want, f);
    if (n < want) memset(buf + n, 0, want - n);
    st = mc_channel_send(s->ch, MC_MSG_FILE_DATA, buf, want);
    sent += (int64_t)want;
    s->bytes_sent += (int64_t)want;
  }
  free(buf);
  fclose(f);
  if (st == MC_OK) s->files_sent++;
  return st;
}

/* HAVE_REQ: for each track, whether this device has its original (and whether that copy is
 * verified: its sha256 is recorded) and whether it can send the requested Opus at once. */
static mc_status serve_have(mc_session *s, const unsigned char *payload, size_t len) {
  yyjson_doc *req = parse(payload, len);
  if (!req) return MC_ERR_FORMAT;
  yyjson_val *tracks = yyjson_obj_get(yyjson_doc_get_root(req), "tracks");
  const char *format = obj_str(req, "format");
  int kbps = format && !strcmp(format, "opus") ? (int)obj_int(req, "kbps") : 0;
  if (!yyjson_is_arr(tracks) || yyjson_arr_size(tracks) > HAVE_MAX) {
    yyjson_doc_free(req);
    return MC_ERR_FORMAT;
  }
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  if (!doc) {
    yyjson_doc_free(req);
    return MC_ERR_NOMEM;
  }
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  yyjson_mut_obj_add_val(doc, root, "tracks", arr);
  size_t idx, max;
  yyjson_val *v;
  yyjson_arr_foreach(tracks, idx, max, v) {
    const char *track = yyjson_get_str(v);
    if (!is_uid(track)) continue;
    held h;
    int original = find_original(s->conn->lib, track, &h), cached = 0;
    if (kbps && valid_bitrate(kbps)) {
      if (original && mc_plan_keep_lossy(h.format, h.bitrate, h.size, h.duration_ms, kbps))
        cached = 1;
      if (!cached && original) {
        char *c = cache_path(s->sync, h.file_uid, kbps);
        cached = c && mc_file_exists(c);
        free(c);
      }
      held tr;
      if (!cached && (cached = find_transcode(s->conn->lib, track, kbps, &tr))) held_free(&tr);
    }
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, o, "uid", track);
    yyjson_mut_obj_add_bool(doc, o, "original", original);
    yyjson_mut_obj_add_bool(doc, o, "cached", cached);
    yyjson_mut_obj_add_bool(doc, o, "verified", original && h.sha[0]);
    yyjson_mut_arr_append(arr, o);
    if (original) held_free(&h);
  }
  yyjson_doc_free(req);
  return send_doc(s->ch, MC_MSG_HAVE_RESP, doc);
}

/* FILE_REQ. *transcoded is set if this encoded something. */
static mc_status serve_file(mc_session *s, const unsigned char *payload, size_t len,
                            int *transcoded) {
  yyjson_doc *req = parse(payload, len);
  if (!req) return MC_ERR_FORMAT;
  char track[MC_UID_STRLEN] = "";
  const char *t = obj_str(req, "track"), *format = obj_str(req, "format");
  int opus = format && !strcmp(format, "opus");
  int kbps = (int)obj_int(req, "kbps");
  if (is_uid(t)) memcpy(track, t, MC_UID_STRLEN);
  int ok = is_uid(t) && format && (opus ? valid_bitrate(kbps) : !strcmp(format, "original"));
  yyjson_doc_free(req);
  if (!ok) return send_file_err(s->ch, track, "format");

  mc_library *lib = s->conn->lib;
  held h, tr;
  int original = find_original(lib, track, &h);
  mc_status st;
  if (!opus) {
    st = original ? send_file(s, track, &h, "original", 0) : send_file_err(s->ch, track, "missing");
  } else if (original && mc_plan_keep_lossy(h.format, h.bitrate, h.size, h.duration_ms, kbps)) {
    st = send_file(s, track, &h, "original", 0);
  } else {
    char *cached = original ? cache_path(s->sync, h.file_uid, kbps) : NULL;
    if (cached && mc_file_exists(cached)) {
      held c = h;
      c.path = cached;
      c.sha[0] = '\0';
      st = send_file(s, track, &c, "opus", kbps);
    } else if (find_transcode(lib, track, kbps, &tr)) {
      st = send_file(s, track, &tr, "opus", kbps);
      held_free(&tr);
    } else if (original && self_transcodes(s)) {
      char *dir = cache_dir(s->sync), *out = NULL;
      mc_transcode_opts opts = {MC_FORMAT_OPUS, kbps};
      mc_mutex_lock(&s->sync->transcode_lock);
      mc_status enc = dir ? mc_transcode_cached(dir, h.file_uid, h.path, &opts, &out) : MC_ERR_NOMEM;
      mc_mutex_unlock(&s->sync->transcode_lock);
      free(dir);
      if (enc == MC_OK) {
        *transcoded = 1;
        held c = h;
        c.path = out;
        c.sha[0] = '\0';
        st = send_file(s, track, &c, "opus", kbps);
      } else {
        fprintf(stderr, "musiccore: sync: transcoding %s failed: %s\n", track, mc_status_str(enc));
        st = send_file_err(s->ch, track, "io");
      }
      free(out);
    } else {
      st = send_file_err(s->ch, track, original ? "no_transcode" : "missing");
    }
    free(cached);
  }
  if (original) held_free(&h);
  return st;
}

/* --- PREPARE_REQ, serving --- */

typedef struct prepare_job {
  mc_sync *sync;
  int kbps;
  int count;
  char (*tracks)[MC_UID_STRLEN];
} prepare_job;

/* Fills the transcode cache for a device that is offline, in the background so the session
 * goes on. Stops early when sync stops. */
static void prepare_worker(void *arg) {
  prepare_job *j = arg;
  mc_sync *sync = j->sync;
  mc_sync_conn c;
  char *dir = cache_dir(sync);
  if (dir && mc_sync_conn_open(sync, &c) == MC_OK) {
    mc_transcode_opts opts = {MC_FORMAT_OPUS, j->kbps};
    int made = 0;
    for (int i = 0; i < j->count; i++) {
      mc_mutex_lock(&sync->lock);
      int stop = sync->stopping;
      mc_mutex_unlock(&sync->lock);
      if (stop) break;
      held h;
      if (!find_original(c.lib, j->tracks[i], &h)) continue;
      if (!mc_plan_keep_lossy(h.format, h.bitrate, h.size, h.duration_ms, j->kbps)) {
        char *out = NULL;
        mc_mutex_lock(&sync->transcode_lock);
        if (mc_transcode_cached(dir, h.file_uid, h.path, &opts, &out) == MC_OK) made++;
        mc_mutex_unlock(&sync->transcode_lock);
        free(out);
      }
      held_free(&h);
    }
    if (made) mc_transcode_cache_trim(dir, MC_TRANSCODE_CACHE_MAX);
    mc_sync_conn_close(&c);
  }
  free(dir);
  free(j->tracks);
  free(j);
}

static mc_status serve_prepare(mc_session *s, const unsigned char *payload, size_t len) {
  yyjson_doc *req = parse(payload, len);
  if (!req) return MC_ERR_FORMAT;
  yyjson_val *tracks = yyjson_obj_get(yyjson_doc_get_root(req), "tracks");
  int kbps = (int)obj_int(req, "kbps");
  if (!yyjson_is_arr(tracks) || !valid_bitrate(kbps)) {
    yyjson_doc_free(req);
    return MC_ERR_FORMAT;
  }
  /* No reply either way; one that may not transcode just ignores it. */
  prepare_job *j = self_transcodes(s) ? calloc(1, sizeof *j) : NULL;
  size_t n = yyjson_arr_size(tracks);
  if (j && n) j->tracks = malloc(n * sizeof *j->tracks);
  if (j && j->tracks) {
    j->sync = s->sync;
    j->kbps = kbps;
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(tracks, idx, max, v) {
      const char *track = yyjson_get_str(v);
      if (is_uid(track)) memcpy(j->tracks[j->count++], track, MC_UID_STRLEN);
    }
    if (mc_sync_spawn(s->sync, prepare_worker, j) == MC_OK) j = NULL;
  }
  if (j) free(j->tracks);
  free(j);
  yyjson_doc_free(req);
  return MC_OK;
}

/* --- Covers, serving --- */

/* "jpg" or "png" by the image's magic bytes, else NULL. */
static const char *image_ext(const unsigned char *img, size_t len) {
  static const unsigned char png[] = {0x89, 'P', 'N', 'G'}, jpg[] = {0xFF, 0xD8, 0xFF};
  if (len >= 4 && !memcmp(img, png, 4)) return "png";
  if (len >= 3 && !memcmp(img, jpg, 3)) return "jpg";
  return NULL;
}

/* A whole file of up to MAX_COVER_BYTES. */
static int read_small(const char *path, unsigned char **out, size_t *len) {
  int64_t size = 0, mtime = 0;
  *out = NULL;
  if (mc_file_info(path, &size, &mtime) != MC_OK || size <= 0 || size > MAX_COVER_BYTES) return 0;
  FILE *f = mc_fopen(path, "rb");
  unsigned char *buf = f ? malloc((size_t)size) : NULL;
  int ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
  if (f) fclose(f);
  if (!ok) {
    free(buf);
    return 0;
  }
  *out = buf;
  *len = (size_t)size;
  return 1;
}

/* An album's cover as this device has it: the image file in a folder holding the album's
 * files (whatever mc_coverart_find() takes: cover, folder or front, any case). Originals'
 * folders are looked at first. Pictures embedded in the files are never used. */
static int album_cover(mc_library *lib, const char *album, unsigned char **img, size_t *len,
                       const char **ext) {
  sqlite3_stmt *stmt;
  *img = NULL;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.path FROM files f JOIN tracks t ON t.uid = f.track_uid"
                         " WHERE t.album_uid = ?1 ORDER BY f.source_file_uid IS NOT NULL, f.path"
                         " LIMIT 50",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, album, -1, SQLITE_STATIC);
  char **paths = NULL, *last_dir = NULL;
  int n = 0, found = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    char *abs = mc_library_abspath(lib, (const char *)sqlite3_column_text(stmt, 0));
    char **more = abs ? realloc(paths, (size_t)(n + 1) * sizeof *more) : NULL;
    if (!more) {
      free(abs);
      break;
    }
    paths = more;
    paths[n++] = abs;
  }
  sqlite3_finalize(stmt);
  /* An image file beside them, looking in each folder once. */
  for (int i = 0; i < n && !found; i++) {
    char *dir = mc_strdup(paths[i]), *sep = dir ? strrchr(dir, MC_PATH_SEP) : NULL;
    if (sep) *sep = '\0';
    if (!sep || (last_dir && !strcmp(last_dir, dir))) {
      free(dir);
      continue;
    }
    free(last_dir);
    last_dir = dir;
    char *cover = mc_coverart_find(dir);
    if (cover && read_small(cover, img, len)) {
      if ((*ext = image_ext(*img, *len)) != NULL) found = 1;
      else free(*img);
    }
    free(cover);
  }
  free(last_dir);
  for (int i = 0; i < n; i++) free(paths[i]);
  free(paths);
  if (!found) *img = NULL;
  return found;
}

/* COVER_REQ {"album"}: COVER_HDR {"album", "ext", "size", "sha256"} and the bytes as
 * FILE_DATA, or FILE_ERR {"album", "reason"} when this device has no cover for it. */
static mc_status serve_cover(mc_session *s, const unsigned char *payload, size_t len) {
  yyjson_doc *req = parse(payload, len);
  if (!req) return MC_ERR_FORMAT;
  char album[MC_UID_STRLEN] = "";
  const char *a = obj_str(req, "album");
  if (is_uid(a)) memcpy(album, a, MC_UID_STRLEN);
  yyjson_doc_free(req);

  unsigned char *img = NULL;
  size_t size = 0;
  const char *ext = NULL;
  int have = album[0] && album_cover(s->conn->lib, album, &img, &size, &ext);
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  if (!doc) {
    free(img);
    return MC_ERR_NOMEM;
  }
  yyjson_mut_obj_add_strcpy(doc, root, "album", album);
  if (!have) {
    yyjson_mut_obj_add_str(doc, root, "reason", album[0] ? "missing" : "format");
    return send_doc(s->ch, MC_MSG_FILE_ERR, doc);
  }
  unsigned char digest[32];
  char sha[SHA_STRLEN];
  mbedtls_sha256(img, size, digest, 0);
  hex(digest, sha);
  yyjson_mut_obj_add_str(doc, root, "ext", ext);
  yyjson_mut_obj_add_int(doc, root, "size", (int64_t)size);
  yyjson_mut_obj_add_strcpy(doc, root, "sha256", sha);
  mc_status st = send_doc(s->ch, MC_MSG_COVER_HDR, doc);
  for (size_t sent = 0; st == MC_OK && sent < size;) {
    size_t want = size - sent < MC_CHANNEL_MAX_PAYLOAD ? size - sent : MC_CHANNEL_MAX_PAYLOAD;
    st = mc_channel_send(s->ch, MC_MSG_FILE_DATA, img + sent, want);
    sent += want;
    s->bytes_sent += (int64_t)want;
  }
  free(img);
  return st;
}

/* The other side's turn: serves its requests until FILES_DONE. */
static mc_status serve_turn(mc_session *s) {
  int transcoded = 0;
  mc_status st = MC_OK;
  for (;;) {
    uint8_t type;
    const unsigned char *payload;
    size_t len;
    st = mc_channel_recv(s->ch, &type, &payload, &len);
    if (st != MC_OK) break;
    if (type == MC_MSG_FILES_DONE) break;
    if (type == MC_MSG_HAVE_REQ)
      st = serve_have(s, payload, len);
    else if (type == MC_MSG_FILE_REQ)
      st = serve_file(s, payload, len, &transcoded);
    else if (type == MC_MSG_PREPARE_REQ)
      st = serve_prepare(s, payload, len);
    else if (type == MC_MSG_COVER_REQ)
      st = serve_cover(s, payload, len);
    else
      st = MC_ERR_FORMAT; /* an ERROR, or out of turn */
    if (st != MC_OK) break;
    progress(s);
  }
  if (transcoded) {
    char *dir = cache_dir(s->sync);
    if (dir) mc_transcode_cache_trim(dir, MC_TRANSCODE_CACHE_MAX);
    free(dir);
  }
  return st;
}

/* --- Receiving --- */

enum { GOT_OK, GOT_REFUSED, GOT_MISMATCH, GOT_FAILED };

static mc_status insert_file(mc_library *lib, const char *track, const char *rel,
                             const char *format, int kbps, int64_t size, int64_t mtime,
                             const char *source_file, const char *peer, const char *sha,
                             const char *source_sha) {
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st != MC_OK) return st;
  if ((st = mc_library_begin_write(lib)) != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps,"
                         " size_bytes, mtime_ms, source_file_uid, source_peer_uid, sha256, file_rev,"
                         " source_sha256)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10,"
                         "  (SELECT file_rev FROM tracks WHERE uid = ?2), ?11)",
                         -1, &stmt, NULL) != SQLITE_OK) {
    st = MC_ERR_DB;
  } else {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, track, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, format, -1, SQLITE_STATIC);
    if (kbps) sqlite3_bind_int(stmt, 5, kbps);
    sqlite3_bind_int64(stmt, 6, size);
    sqlite3_bind_int64(stmt, 7, mtime);
    if (source_file) sqlite3_bind_text(stmt, 8, source_file, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, peer, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, sha, -1, SQLITE_STATIC);
    if (source_sha) sqlite3_bind_text(stmt, 11, source_sha, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

static mc_status fetch_cover(mc_session *s, const char *album, const char *dir);

/* The album folder of a received file gets the peer's cover as soon as the file is in, when
 * the album has art (albums.art_hash) and the folder no cover yet, so the album shows with
 * its art while the rest of it is still coming. Pictures embedded in the file are never
 * used: the cover file is the album's art. fetch_covers() catches any folder this missed.
 * abs is the received file. The return value is only for errors that end the session. */
static mc_status ensure_cover(mc_session *s, const char *track, const char *abs) {
  if (!s->peer_covers) return MC_OK; /* an older peer would end the session */
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(s->conn->lib),
                         "SELECT al.uid FROM tracks t JOIN albums al ON al.uid = t.album_uid"
                         " WHERE t.uid = ?1 AND al.art_hash IS NOT NULL",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_OK;
  sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
  char album[MC_UID_STRLEN] = "";
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *a = (const char *)sqlite3_column_text(stmt, 0);
    if (is_uid(a)) memcpy(album, a, MC_UID_STRLEN);
  }
  sqlite3_finalize(stmt);
  char *dir = album[0] ? mc_strdup(abs) : NULL, *sep = dir ? strrchr(dir, MC_PATH_SEP) : NULL;
  if (!sep) {
    free(dir);
    return MC_OK;
  }
  *sep = '\0';
  char *cover = mc_coverart_find(dir);
  mc_status st = cover ? MC_OK : fetch_cover(s, album, dir);
  free(cover);
  free(dir);
  return st;
}

/* FILE_REQ for t, then FILE_HDR and FILE_DATA into <dest>.part, renamed into place and
 * recorded if the hash matches. *got says how it went; the return value is only for errors
 * that end the session. */
static mc_status fetch(mc_session *s, const mc_plan_track *t, int opus, int *got) {
  *got = GOT_FAILED;
  mc_library *lib = s->conn->lib;
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_obj_add_strcpy(doc, root, "track", t->track_uid);
  yyjson_mut_obj_add_str(doc, root, "format", opus ? "opus" : "original");
  yyjson_mut_obj_add_int(doc, root, "kbps", opus ? t->format.bitrate_kbps : 0);
  mc_status st = send_doc(s->ch, MC_MSG_FILE_REQ, doc);
  if (st != MC_OK) return st;

  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_channel_set_timeout(s->ch, FETCH_WAIT_MS); /* it may be encoding */
  st = mc_channel_recv(s->ch, &type, &payload, &len);
  mc_channel_set_timeout(s->ch, mc_channel_default_timeout());
  if (st != MC_OK) return st;
  if (type == MC_MSG_FILE_ERR) {
    *got = GOT_REFUSED;
    return MC_OK;
  }
  if (type != MC_MSG_FILE_HDR) return MC_ERR_FORMAT;

  /* The header, copied out before the next receive. */
  yyjson_doc *hdr = parse(payload, len);
  if (!hdr) return MC_ERR_FORMAT;
  const char *h_track = obj_str(hdr, "track"), *h_file = obj_str(hdr, "file"),
             *h_format = obj_str(hdr, "format"), *h_ext = obj_str(hdr, "ext"),
             *h_sha = obj_str(hdr, "sha256"), *h_source_sha = obj_str(hdr, "source_sha256");
  int64_t size = obj_int(hdr, "size");
  int kbps = (int)obj_int(hdr, "kbps");
  if (!h_track || strcmp(h_track, t->track_uid) != 0 || size < 0 || size > MAX_FILE_BYTES) {
    yyjson_doc_free(hdr);
    return MC_ERR_FORMAT; /* can't even tell how much follows */
  }
  char file_uid[MC_UID_STRLEN] = "", ext[16] = "", sha[SHA_STRLEN] = "",
       source_sha[SHA_STRLEN] = "";
  int sent_opus = h_format && !strcmp(h_format, "opus");
  int sent_original = h_format && !strcmp(h_format, "original");
  if (is_uid(h_file)) memcpy(file_uid, h_file, MC_UID_STRLEN);
  if (h_ext && strlen(h_ext) < sizeof ext) {
    for (size_t i = 0; h_ext[i]; i++) ext[i] = (char)tolower((unsigned char)h_ext[i]);
    ext[strlen(h_ext)] = '\0';
  }
  if (is_sha(h_sha)) memcpy(sha, h_sha, SHA_STRLEN);
  if (sent_original && is_sha(h_source_sha)) memcpy(source_sha, h_source_sha, SHA_STRLEN);
  yyjson_doc_free(hdr);
  /* An original stands in for Opus when it is lossy and small enough; nothing else does. */
  int accept = file_uid[0] && sha[0] && audio_ext(ext) &&
               (sent_original || (opus && sent_opus && kbps == t->format.bitrate_kbps));

  char *rel = accept ? dest_path(lib, t->track_uid, ext) : NULL;
  char *abs = rel ? mc_library_abspath(lib, rel) : NULL;
  char *part = abs ? join(abs, 0, ".part") : NULL;
  FILE *f = NULL;
  if (part) {
    char *dir = mc_strdup(abs), *sep = dir ? strrchr(dir, MC_PATH_SEP) : NULL;
    if (sep) *sep = '\0';
    if (dir && mc_mkdirs(dir) == MC_OK) f = mc_fopen(part, "wb");
    free(dir);
  }
  int write_ok = f != NULL;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  for (int64_t have = 0; have < size;) {
    if ((st = mc_channel_recv(s->ch, &type, &payload, &len)) != MC_OK) break;
    if (type != MC_MSG_FILE_DATA || (int64_t)len > size - have) {
      st = MC_ERR_FORMAT;
      break;
    }
    mbedtls_sha256_update(&ctx, payload, len);
    if (f && write_ok && fwrite(payload, 1, len, f) != len) write_ok = 0;
    have += (int64_t)len;
    s->bytes_received += (int64_t)len;
  }
  unsigned char digest[32];
  char got_sha[SHA_STRLEN];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  hex(digest, got_sha);
  if (f && fclose(f) != 0) write_ok = 0;

  if (st == MC_OK && accept && write_ok && strcmp(got_sha, sha) != 0) {
    fprintf(stderr, "musiccore: sync: %s from %s failed its checksum\n", t->track_uid,
            s->peer_uid);
    *got = GOT_MISMATCH;
  } else if (st == MC_OK && !accept) {
    *got = GOT_REFUSED;
  } else if (st == MC_OK && write_ok && mc_rename(part, abs, 0) == MC_OK) {
    int64_t fsize = 0, mtime = 0;
    mc_file_info(abs, &fsize, &mtime);
    /* A received original keeps no source_file_uid: it is still an original. */
    if (insert_file(lib, t->track_uid, rel, ext, sent_opus ? kbps : 0, fsize, mtime,
                    sent_opus ? file_uid : NULL, s->peer_uid, sha,
                    source_sha[0] ? source_sha : NULL) == MC_OK) {
      *got = GOT_OK;
      s->files_received++;
      st = ensure_cover(s, t->track_uid, abs);
    } else {
      mc_remove(abs); /* the track went meanwhile, or the database failed */
    }
  }
  if (part && mc_file_exists(part)) mc_remove(part);
  if (rel && *got != GOT_OK) prune_dirs(lib, rel);
  free(part);
  free(abs);
  free(rel);
  return st;
}

/* What the peer said about one track. */
typedef struct want {
  const mc_plan_track *t; /* NULL: an original imported here we asked about for reconcile */
  char uid[MC_UID_STRLEN];
  int original, cached, verified;
  int imported; /* this device holds an original of it imported here, in the wrong format */
} want;

static int want_cmp(const void *key, const void *elem) {
  return strcmp(key, ((const want *)elem)->uid);
}

static int want_sort(const void *a, const void *b) {
  return strcmp(((const want *)a)->uid, ((const want *)b)->uid);
}

static int uid_cmp(const void *a, const void *b) {
  return strcmp(a, b);
}

static mc_status ask_have(mc_session *s, want *w, int n, const mc_plan *plan) {
  for (int at = 0; at < n; at += HAVE_CHUNK) {
    int end = at + HAVE_CHUNK < n ? at + HAVE_CHUNK : n;
    yyjson_mut_val *root;
    yyjson_mut_doc *doc = new_doc(&root);
    if (!doc) return MC_ERR_NOMEM;
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "tracks", arr);
    for (int i = at; i < end; i++) yyjson_mut_arr_add_strcpy(doc, arr, w[i].uid);
    yyjson_mut_obj_add_str(doc, root, "format", plan->kbps ? "opus" : "original");
    yyjson_mut_obj_add_int(doc, root, "kbps", plan->kbps);
    mc_status st = send_doc(s->ch, MC_MSG_HAVE_REQ, doc);
    uint8_t type;
    const unsigned char *payload;
    size_t len;
    if (st == MC_OK) st = mc_channel_recv(s->ch, &type, &payload, &len);
    if (st != MC_OK) return st;
    if (type != MC_MSG_HAVE_RESP) return MC_ERR_FORMAT;
    yyjson_doc *resp = parse(payload, len);
    yyjson_val *tracks = resp ? yyjson_obj_get(yyjson_doc_get_root(resp), "tracks") : NULL;
    if (!yyjson_is_arr(tracks)) {
      yyjson_doc_free(resp);
      return MC_ERR_FORMAT;
    }
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(tracks, idx, max, v) {
      const char *uid = yyjson_get_str(yyjson_obj_get(v, "uid"));
      /* w is sorted by uid within the whole list, so search the chunk. */
      want *e = uid ? bsearch(uid, w + at, (size_t)(end - at), sizeof *w, want_cmp) : NULL;
      if (!e) continue;
      e->original = yyjson_get_bool(yyjson_obj_get(v, "original"));
      e->cached = yyjson_get_bool(yyjson_obj_get(v, "cached"));
      e->verified = yyjson_get_bool(yyjson_obj_get(v, "verified"));
    }
    yyjson_doc_free(resp);
  }
  return MC_OK;
}

static void add_archived(mc_transfer *x, const char *uid) {
  if (x->narchived == x->archived_cap) {
    int grown = x->archived_cap ? x->archived_cap * 2 : 64;
    void *more = realloc(x->archived, (size_t)grown * sizeof *x->archived);
    if (!more) return;
    x->archived = more;
    x->archived_cap = grown;
  }
  memcpy(x->archived[x->narchived++], uid, MC_UID_STRLEN);
}

/* This device's turn as the receiver, with its plan in x. */
static mc_status fetch_missing(mc_session *s, mc_transfer *x) {
  mc_library *lib = s->conn->lib;
  const mc_plan *plan = x->plan;
  want *w = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;

  /* Missing wanted tracks (plan order is uid order). */
  for (int i = 0; i < plan->count && st == MC_OK; i++) {
    if (track_satisfied(lib, &plan->tracks[i])) continue;
    if (n == cap) {
      int grown = cap ? cap * 2 : 64;
      want *more = realloc(w, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      w = more;
      cap = grown;
    }
    memset(&w[n], 0, sizeof w[n]);
    w[n].t = &plan->tracks[i];
    memcpy(w[n].uid, plan->tracks[i].track_uid, MC_UID_STRLEN);
    n++;
  }
  /* With an archive: the originals imported here that reconcile would drop if the archive
   * had them (no longer wanted, or wanted in another format). Merged in, so w stays sorted
   * and unique. */
  int peer_archive = !plan->archive && is_archive(s->conn->sdb, s->peer_uid);
  int nmissing = n;
  if (st == MC_OK && peer_archive) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(mc_library_db(lib),
                           "SELECT DISTINCT track_uid, format, bitrate_kbps, size_bytes FROM files"
                           " WHERE source_file_uid IS NULL AND source_peer_uid IS NULL",
                           -1, &stmt, NULL) == SQLITE_OK) {
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *uid = (const char *)sqlite3_column_text(stmt, 0);
        if (!is_uid(uid)) continue;
        const mc_plan_track *t = mc_plan_find(plan, uid);
        if (t && mc_plan_file_satisfies(t, MC_FILE_IMPORTED,
                                        (const char *)sqlite3_column_text(stmt, 1),
                                        sqlite3_column_int(stmt, 2), sqlite3_column_int64(stmt, 3)))
          continue;
        if (t && !track_satisfied(lib, t)) { /* asked about already, as missing */
          want *m = bsearch(uid, w, (size_t)nmissing, sizeof *w, want_cmp);
          if (m) m->imported = 1;
          continue;
        }
        int dup = 0;
        for (int i = 0; i < n && !dup; i++) dup = !w[i].t && !strcmp(w[i].uid, uid);
        if (dup) continue;
        if (n == cap) {
          int grown = cap ? cap * 2 : 64;
          want *more = realloc(w, (size_t)grown * sizeof *more);
          if (!more) break;
          w = more;
          cap = grown;
        }
        memset(&w[n], 0, sizeof w[n]);
        memcpy(w[n].uid, uid, MC_UID_STRLEN);
        n++;
      }
      sqlite3_finalize(stmt);
    }
    if (n) qsort(w, (size_t)n, sizeof *w, want_sort);
  }
  if (st == MC_OK && n) st = ask_have(s, w, n, plan);

  transcoder *tc = NULL;
  int ntc = st == MC_OK ? list_transcoders(s, &tc) : 0;
  /* What to fetch, and how. */
  int *how = n ? calloc((size_t)n, sizeof *how) : NULL; /* 0 skip, 1 original, 2 opus */
  if (n && !how) st = MC_ERR_NOMEM;
  s->files_total = 0;
  for (int i = 0; st == MC_OK && i < n; i++) {
    /* Reconcile only looks this up for originals imported here. */
    if (peer_archive && w[i].original && w[i].verified) add_archived(x, w[i].uid);
    if (!w[i].t) continue;
    if (w[i].t->format.format == MC_FORMAT_ORIGINAL)
      how[i] = w[i].original ? 1 : 0;
    /* An original imported here goes when this archive has it, so the archive encodes it:
     * the other transcoders may not hold it yet. */
    else if (w[i].cached ||
             (w[i].original && ntc && (w[i].imported || assigned_to_peer(tc, ntc, w[i].uid))))
      how[i] = 2;
    s->files_total += how[i] != 0;
  }
  if (x->narchived) qsort(x->archived, (size_t)x->narchived, sizeof *x->archived, uid_cmp);
  free(tc);
  progress(s);

  for (int i = 0; st == MC_OK && i < n; i++) {
    if (!how[i]) continue;
    int got = GOT_FAILED;
    st = fetch(s, w[i].t, how[i] == 2, &got);
    if (st == MC_OK && got == GOT_MISMATCH) {
      /* Once more; this session has only this peer to ask. */
      st = fetch(s, w[i].t, how[i] == 2, &got);
      if (st == MC_OK && got == GOT_MISMATCH) s->files_corrupt++;
    }
    s->files_done++;
    progress(s);
  }
  free(how);
  free(w);
  return st;
}

/* --- Covers, asking --- */

/* COVER_REQ for album, saving a good answer as dir's cover. The return value is only for
 * errors that end the session. */
static mc_status fetch_cover(mc_session *s, const char *album, const char *dir) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = new_doc(&root);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_obj_add_strcpy(doc, root, "album", album);
  mc_status st = send_doc(s->ch, MC_MSG_COVER_REQ, doc);
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  if (st == MC_OK) st = mc_channel_recv(s->ch, &type, &payload, &len);
  if (st != MC_OK) return st;
  if (type == MC_MSG_FILE_ERR) return MC_OK;
  if (type != MC_MSG_COVER_HDR) return MC_ERR_FORMAT;
  yyjson_doc *hdr = parse(payload, len);
  if (!hdr) return MC_ERR_FORMAT;
  const char *h_album = obj_str(hdr, "album"), *h_sha = obj_str(hdr, "sha256");
  int64_t size = obj_int(hdr, "size");
  char sha[SHA_STRLEN] = "";
  int ok = h_album && !strcmp(h_album, album) && size > 0 && size <= MAX_COVER_BYTES;
  if (ok && is_sha(h_sha)) memcpy(sha, h_sha, SHA_STRLEN);
  yyjson_doc_free(hdr);
  if (!ok) return MC_ERR_FORMAT; /* can't tell how much follows */

  unsigned char *img = malloc((size_t)size);
  if (!img) return MC_ERR_NOMEM;
  for (int64_t have = 0; have < size;) {
    if ((st = mc_channel_recv(s->ch, &type, &payload, &len)) != MC_OK) break;
    if (type != MC_MSG_FILE_DATA || (int64_t)len > size - have) {
      st = MC_ERR_FORMAT;
      break;
    }
    memcpy(img + have, payload, len);
    have += (int64_t)len;
    s->bytes_received += (int64_t)len;
  }
  if (st == MC_OK) {
    unsigned char digest[32];
    char got[SHA_STRLEN];
    mbedtls_sha256(img, (size_t)size, digest, 0);
    hex(digest, got);
    const char *ext = image_ext(img, (size_t)size); /* from the bytes, not the peer */
    mc_buffer buf = {(char *)img, (size_t)size};
    if (strcmp(got, sha) != 0)
      fprintf(stderr, "musiccore: sync: the cover of %s from %s failed its checksum\n", album,
              s->peer_uid);
    else if (ext && mc_coverart_save(dir, &buf, ext) == MC_OK)
      s->covers_received++;
  }
  free(img);
  return st;
}

/* Asks the peer for the cover of each folder here that holds files of an album with art
 * (albums.art_hash) but has no cover. A folder usually gets one as its first file arrives
 * (ensure_cover()); this catches up folders whose files were already here, or a device that
 * synced before covers were sent. */
static mc_status fetch_covers(mc_session *s) {
  if (!s->peer_covers) return MC_OK; /* an older peer would end the session */
  mc_library *lib = s->conn->lib;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT al.uid, f.path FROM albums al"
                         " JOIN tracks t ON t.album_uid = al.uid"
                         " JOIN files f ON f.track_uid = t.uid"
                         " WHERE al.art_hash IS NOT NULL"
                         " ORDER BY al.uid, f.path",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  typedef struct need {
    char album[MC_UID_STRLEN];
    char *dir;
  } need;
  need *ns = NULL;
  int n = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *album = (const char *)sqlite3_column_text(stmt, 0);
    const char *rel = (const char *)sqlite3_column_text(stmt, 1);
    char *dir = is_uid(album) && rel ? mc_library_abspath(lib, rel) : NULL;
    char *sep = dir ? strrchr(dir, MC_PATH_SEP) : NULL;
    if (sep) *sep = '\0';
    int dup = 0;
    for (int i = n - 1; sep && i >= 0 && !dup && !strcmp(ns[i].album, album); i--)
      dup = !strcmp(ns[i].dir, dir);
    need *more = sep && !dup ? realloc(ns, (size_t)(n + 1) * sizeof *more) : NULL;
    if (!more) {
      free(dir);
      continue;
    }
    ns = more;
    memcpy(ns[n].album, album, MC_UID_STRLEN);
    ns[n++].dir = dir;
  }
  sqlite3_finalize(stmt);
  mc_status st = MC_OK;
  for (int i = 0; i < n; i++) {
    char *cover = st == MC_OK ? mc_coverart_find(ns[i].dir) : NULL;
    if (st == MC_OK && !cover && mc_file_exists(ns[i].dir))
      st = fetch_cover(s, ns[i].album, ns[i].dir);
    free(cover);
    free(ns[i].dir);
  }
  free(ns);
  return st;
}

/* --- PREPARE_REQ, asking --- */

/* For each member that is offline and wants Opus copies, asks the peer to fill its cache if
 * the peer is the best online holder of the originals (by cpu_score), once per change in
 * what that member wants. */
static mc_status send_prepares(mc_session *s) {
  mc_sync_db *sdb = s->conn->sdb;
  int64_t peer_score;
  if (!transcodes_for_others(sdb, s->peer_uid, &peer_score) || !holds_originals(sdb, s->peer_uid))
    return MC_OK;
  transcoder *tc = NULL;
  int ntc = list_transcoders(s, &tc);
  for (int i = 1; i < ntc; i++)
    if (tc[i].score > peer_score || (tc[i].score == peer_score && strcmp(tc[i].uid, s->peer_uid) < 0)) {
      free(tc);
      return MC_OK; /* someone else should */
    }
  free(tc);

  typedef struct member {
    char uid[MC_UID_STRLEN];
    int64_t free_bytes;
  } member;
  member *ms = NULL;
  int n = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT d.uid, c.free_bytes FROM devices d"
                         " LEFT JOIN capabilities c ON c.device_uid = d.uid"
                         " WHERE d.removed = 0 AND d.uid <> ?1 AND d.uid <> ?2",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, s->peer_uid, -1, SQLITE_STATIC);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid) || mc_sync_peer_online(s->sync, uid)) continue;
    member *more = realloc(ms, (size_t)(n + 1) * sizeof *more);
    if (!more) break;
    ms = more;
    memcpy(ms[n].uid, uid, MC_UID_STRLEN);
    ms[n].free_bytes = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 1);
    n++;
  }
  sqlite3_finalize(stmt);

  mc_status st = MC_OK;
  for (int m = 0; st == MC_OK && m < n; m++) {
    mc_plan *plan = NULL;
    if (mc_plan_compute(sdb, s->conn->lib, ms[m].uid, ms[m].free_bytes, &plan) != MC_OK) continue;
    uint64_t sig = 14695981039346656037u;
    int count = 0;
    if (plan->status == MC_PLAN_OK && plan->kbps) {
      char k[16];
      snprintf(k, sizeof k, "%d", plan->kbps);
      sig = fnv(sig, k);
      for (int i = 0; i < plan->count; i++)
        if (plan->tracks[i].format.format == MC_FORMAT_OPUS) {
          sig = fnv(sig, plan->tracks[i].track_uid);
          count++;
        }
    }
    sig |= 1; /* never 0, which means nothing was asked for */
    if (count && mc_sync_peer_swap_prepared(s->sync, ms[m].uid, sig) != sig) {
      for (int at = 0; st == MC_OK && at < plan->count;) {
        yyjson_mut_val *root;
        yyjson_mut_doc *doc = new_doc(&root);
        if (!doc) {
          st = MC_ERR_NOMEM;
          break;
        }
        yyjson_mut_obj_add_strcpy(doc, root, "device", ms[m].uid);
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, root, "tracks", arr);
        for (int added = 0; at < plan->count && added < PREPARE_CHUNK; at++)
          if (plan->tracks[at].format.format == MC_FORMAT_OPUS) {
            yyjson_mut_arr_add_strcpy(doc, arr, plan->tracks[at].track_uid);
            added++;
          }
        yyjson_mut_obj_add_int(doc, root, "kbps", plan->kbps);
        st = send_doc(s->ch, MC_MSG_PREPARE_REQ, doc);
      }
    }
    mc_plan_free(plan);
  }
  free(ms);
  return st;
}

/* This side's turn as the receiver. */
static mc_status receive_turn(mc_session *s) {
  mc_transfer *x = s->xfer;
  mc_status st = MC_OK;
  /* A profile its user hasn't confirmed fetches and deletes nothing (a session pressed for
   * has confirmed it by now). */
  if (x && !s->user &&
      mc_profile_confirm_state(mc_sync_db_handle(s->conn->sdb),
                               mc_sync_db_device_uid(s->conn->sdb)) != MC_SYNC_CONFIRMED) {
    s->files_paused = 1;
    x = NULL;
  }
  /* A library set aside for a merge (libmerge.h) has files in its folder that no files row
   * knows yet: nothing is fetched or deleted until they're merged in. */
  if (x && mc_libmerge_pending(s->conn->lib, NULL)) {
    s->files_paused = 1;
    x = NULL;
  }
  /* Another session already fetching skips this turn: its peer may have it all anyway. */
  if (x && files_try(s->sync)) {
    x->receiving = 1;
    int64_t free_bytes = -1;
    if (mc_free_space(mc_library_dir(s->conn->lib), &free_bytes) != MC_OK) free_bytes = -1;
    if (mc_plan_compute(s->conn->sdb, s->conn->lib, mc_sync_db_device_uid(s->conn->sdb),
                        free_bytes, &x->plan) == MC_OK &&
        x->plan->status == MC_PLAN_OK) {
      x->fetched = 1;
      st = fetch_missing(s, x);
      if (st == MC_OK) st = fetch_covers(s);
    }
    if (st == MC_OK) st = send_prepares(s);
  }
  if (st == MC_OK) st = mc_channel_send(s->ch, MC_MSG_FILES_DONE, "{}", 2);
  return st;
}

/* --- Reconcile and verification --- */

/* One files row. */
typedef struct file_row {
  char uid[MC_UID_STRLEN];
  char track[MC_UID_STRLEN];
  char *path;
  char format[16];
  int bitrate;
  int64_t size, mtime;
  int has_mtime;
  char sha[SHA_STRLEN];
  mc_file_kind kind;
  int stale; /* made from an older file of the track (see the top) */
} file_row;

static void rows_free(file_row *rows, int n) {
  for (int i = 0; i < n; i++) free(rows[i].path);
  free(rows);
}

/* Every files row, by track. */
static mc_status read_files(mc_library *lib, file_row **out, int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.uid, f.track_uid, f.path, f.format, f.bitrate_kbps, f.size_bytes,"
                         " f.mtime_ms, f.sha256, f.source_file_uid, f.source_peer_uid, " STALE_SQL
                         " FROM files f LEFT JOIN tracks t ON t.uid = f.track_uid"
                         " ORDER BY f.track_uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  file_row *rows = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    const char *track = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    if (!is_uid(uid) || !is_uid(track) || !path) continue;
    if (n == cap) {
      int grown = cap ? cap * 2 : 256;
      file_row *more = realloc(rows, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      rows = more;
      cap = grown;
    }
    file_row *r = &rows[n];
    memset(r, 0, sizeof *r);
    memcpy(r->uid, uid, MC_UID_STRLEN);
    memcpy(r->track, track, MC_UID_STRLEN);
    if (!(r->path = mc_strdup(path))) {
      st = MC_ERR_NOMEM;
      break;
    }
    n++;
    const char *format = (const char *)sqlite3_column_text(stmt, 3);
    snprintf(r->format, sizeof r->format, "%s", format ? format : "");
    r->bitrate = sqlite3_column_int(stmt, 4);
    r->size = sqlite3_column_int64(stmt, 5);
    r->has_mtime = sqlite3_column_type(stmt, 6) != SQLITE_NULL;
    r->mtime = sqlite3_column_int64(stmt, 6);
    const char *sha = (const char *)sqlite3_column_text(stmt, 7);
    if (is_sha(sha)) memcpy(r->sha, sha, SHA_STRLEN);
    r->kind = mc_plan_file_kind((const char *)sqlite3_column_text(stmt, 8),
                                (const char *)sqlite3_column_text(stmt, 9));
    r->stale = sqlite3_column_int(stmt, 10);
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    rows_free(rows, n);
    return st;
  }
  *out = rows;
  *count = n;
  return MC_OK;
}

/* Files rows deleted during the library exchange are noted here (a temp trigger, so only
 * this connection's writes, and rolled back with them). */
static const char k_watch_sql[] =
  "CREATE TEMP TABLE IF NOT EXISTS sync_dropped_files (path TEXT, track_uid TEXT, original INTEGER);"
  "CREATE TEMP TRIGGER IF NOT EXISTS sync_files_dropped AFTER DELETE ON main.files BEGIN"
  " INSERT INTO sync_dropped_files (path, track_uid, original)"
  " VALUES (old.path, old.track_uid, old.source_file_uid IS NULL AND old.source_peer_uid IS NULL);"
  " END;";
static const char k_unwatch_sql[] =
  "DROP TRIGGER IF EXISTS temp.sync_files_dropped;"
  "DROP TABLE IF EXISTS temp.sync_dropped_files;";

/* Removes the files of tracks the exchange deleted (another device deleted them, see
 * musiccore/delete.h). Originals imported here go to the trash, where they can still be got
 * back, unless there's none; files received from peers are simply removed. */
static void drop_deleted_tracks(mc_session *s) {
  mc_library *lib = s->conn->lib;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT d.path, max(d.original) FROM temp.sync_dropped_files d"
                         " WHERE NOT EXISTS (SELECT 1 FROM main.files f WHERE f.path = d.path)"
                         " AND NOT EXISTS (SELECT 1 FROM main.tracks t WHERE t.uid = d.track_uid)"
                         " GROUP BY d.path",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 0);
    int original = sqlite3_column_int(stmt, 1);
    char *abs = deletable_path(lib, rel);
    if (abs && mc_file_exists(abs) &&
        ((original && mc_transfer_trash(abs) == MC_OK) || mc_remove(abs) == MC_OK)) {
      s->files_deleted++;
      prune_dirs(lib, rel);
    }
    free(abs);
  }
  sqlite3_finalize(stmt);
}

static int archived(const mc_transfer *x, const char *track) {
  return x->narchived &&
         bsearch(track, x->archived, (size_t)x->narchived, sizeof *x->archived, uid_cmp) != NULL;
}

/* Files of tracks no longer wanted, in the wrong format, or stale go. A wrong-format or
 * stale copy stays while the track has no good copy yet, so it stays playable; an original
 * imported here stays until an archive holds a verified copy. */
static void reconcile_files(mc_session *s, mc_transfer *x) {
  mc_library *lib = s->conn->lib;
  file_row *rows;
  int n;
  if (read_files(lib, &rows, &n) != MC_OK) return;
  for (int i = 0; i < n;) {
    int end = i;
    while (end < n && !strcmp(rows[end].track, rows[i].track)) end++;
    const mc_plan_track *t = mc_plan_find(x->plan, rows[i].track);
    int satisfied = 0;
    for (int j = i; t && j < end; j++)
      satisfied |= !rows[j].stale && mc_plan_file_satisfies(t, rows[j].kind, rows[j].format,
                                                             rows[j].bitrate, rows[j].size);
    for (int j = i; j < end; j++) {
      file_row *r = &rows[j];
      if (t && (!satisfied ||
                (!r->stale && mc_plan_file_satisfies(t, r->kind, r->format, r->bitrate, r->size))))
        continue;
      if (r->kind == MC_FILE_IMPORTED && !archived(x, r->track)) continue;
      if (drop_file(lib, r->uid, r->path) == MC_OK) s->files_deleted++;
    }
    i = end;
  }
  rows_free(rows, n);
}

/* An archive keeps everything, except a stale copy once the track's current original has
 * arrived: that's the same track, replaced by a better file. */
static void drop_stale_on_archive(mc_session *s) {
  mc_library *lib = s->conn->lib;
  file_row *rows;
  int n;
  if (read_files(lib, &rows, &n) != MC_OK) return;
  for (int i = 0; i < n;) {
    int end = i, current = 0;
    while (end < n && !strcmp(rows[end].track, rows[i].track)) end++;
    for (int j = i; j < end; j++) current |= !rows[j].stale && rows[j].kind != MC_FILE_TRANSCODE;
    for (int j = i; current && j < end; j++)
      if (rows[j].stale && drop_file(lib, rows[j].uid, rows[j].path) == MC_OK) s->files_deleted++;
    i = end;
  }
  rows_free(rows, n);
}

typedef struct unknown_scan {
  mc_library *lib;
  int64_t before_ms;
  char **paths;
  int n, cap;
} unknown_scan;

static int on_library_file(void *user, const char *path) {
  unknown_scan *u = user;
  char *rel = mc_library_relpath(u->lib, path);
  if (!rel) return 0;
  char ext[16];
  extension(rel, ext);
  int64_t size, mtime;
  int candidate = (audio_ext(ext) || !strcmp(ext, "part")) && strncmp(rel, "library.db", 10) != 0 &&
                  mc_file_info(path, &size, &mtime) == MC_OK && mtime < u->before_ms &&
                  !path_known(u->lib, rel);
  if (candidate && u->n == u->cap) {
    int grown = u->cap ? u->cap * 2 : 16;
    char **more = realloc(u->paths, (size_t)grown * sizeof *more);
    if (more) {
      u->paths = more;
      u->cap = grown;
    }
  }
  if (candidate && u->n < u->cap) {
    u->paths[u->n++] = rel;
    return 0;
  }
  free(rel);
  return 0;
}

/* Audio files (and stale .part files) in the library folder that no files row knows:
 * removed, or on an archive only reported. An audio file may be the user's own, put there by
 * hand, so it goes to the trash where they can get it back; where there's no trash it can
 * use (e.g. a file system it can't make a .Trash-$uid on) it's deleted, as are
 * .part files, which are only ever half a copy. Android has no trash, so there every one is
 * deleted without trying mc_trash(): a trash under $HOME there would be one nobody can open,
 * keeping the files out of sight but taking up space. Files younger than UNKNOWN_GRACE_MS
 * are left for an import that may be about to record them. */
static void reconcile_unknown(mc_session *s, int archive) {
  mc_library *lib = s->conn->lib;
  unknown_scan u = {lib, (int64_t)mc_wall_ms() - UNKNOWN_GRACE_MS, NULL, 0, 0};
  mc_walk(mc_library_dir(lib), on_library_file, &u);
  for (int i = 0; i < u.n; i++) {
    s->files_unknown++;
    if (archive) {
      fprintf(stderr, "musiccore: sync: unknown file in the library folder: %s\n", u.paths[i]);
    } else {
      char ext[16];
      extension(u.paths[i], ext);
#ifdef __ANDROID__
      int trash = 0;
#else
      int trash = strcmp(ext, "part") != 0;
#endif
      char *abs = deletable_path(lib, u.paths[i]);
      if (abs && ((trash && mc_transfer_trash(abs) == MC_OK) || mc_remove(abs) == MC_OK)) {
        s->files_deleted++;
        prune_dirs(lib, u.paths[i]);
      }
      free(abs);
    }
    free(u.paths[i]);
  }
  free(u.paths);
}

/* Checks every files row. Failed copies from peers are deleted with their rows so they're
 * fetched again; failed originals imported here are only counted. A deep check records
 * the hash of files that have none. */
static void verify_files(mc_library *lib, int deep, mc_verify_result *res) {
  memset(res, 0, sizeof *res);
  file_row *rows;
  int n;
  if (read_files(lib, &rows, &n) != MC_OK) return;
  for (int i = 0; i < n; i++) {
    file_row *r = &rows[i];
    char *abs = mc_library_abspath(lib, r->path);
    int64_t size = 0, mtime = 0;
    int ok = abs && mc_file_info(abs, &size, &mtime) == MC_OK && size == r->size &&
             (!r->has_mtime || mtime == r->mtime);
    char sha[SHA_STRLEN];
    if (ok && deep) {
      ok = hash_file(abs, sha) == MC_OK && (!r->sha[0] || !strcmp(sha, r->sha));
      if (ok && !r->sha[0] && mc_library_begin_write(lib) == MC_OK) {
        sqlite3_stmt *stmt;
        int done = 0;
        if (sqlite3_prepare_v2(mc_library_db(lib), "UPDATE files SET sha256 = ?2 WHERE uid = ?1",
                               -1, &stmt, NULL) == SQLITE_OK) {
          sqlite3_bind_text(stmt, 1, r->uid, -1, SQLITE_STATIC);
          sqlite3_bind_text(stmt, 2, sha, -1, SQLITE_STATIC);
          done = sqlite3_step(stmt) == SQLITE_DONE;
          sqlite3_finalize(stmt);
        }
        if (done) mc_library_commit(lib);
        else mc_library_rollback(lib);
      }
    }
    free(abs);
    res->checked++;
    if (ok) {
      res->ok++;
      continue;
    }
    res->failed++;
    fprintf(stderr, "musiccore: sync: %s failed its %s check\n", r->path, deep ? "deep" : "fast");
    if (r->kind != MC_FILE_IMPORTED && drop_file(lib, r->uid, r->path) == MC_OK) res->deleted++;
  }
  rows_free(rows, n);
}

typedef struct mc_sync_status_row status_row;

/* This device's sync_status from plan and what it holds now. *latest_ms: the wall time of
 * the library's latest change. */
static void compute_status(mc_library *lib, const mc_plan *plan, int corrupt, status_row *v,
                           int64_t *latest_ms) {
  memset(v, 0, sizeof *v);
  v->verified_ms = (int64_t)mc_wall_ms();
  v->expected = plan->count;
  for (int i = 0; i < plan->count; i++) v->ok += track_satisfied(lib, &plan->tracks[i]);
  v->corrupt = corrupt < v->expected - v->ok ? corrupt : v->expected - v->ok;
  v->missing = v->expected - v->ok - v->corrupt;
  *latest_ms = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT sum(size_bytes), (SELECT max(hlc) FROM changelog) FROM files", -1,
                         &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      v->bytes = sqlite3_column_int64(stmt, 0);
      *latest_ms = (int64_t)((uint64_t)sqlite3_column_int64(stmt, 1) >> 16); /* its wall ms */
    }
    sqlite3_finalize(stmt);
  }
}

/* Whether v should be written: there's no row yet or a count changed, or with refresh
 * (mc_sync_verify()) also when the stored check is older than the library's latest change.
 * Writing only on change keeps an idle group from logging a row every session. */
static int status_due(mc_sync_db *sdb, const status_row *v, int refresh, int64_t latest_ms) {
  sqlite3_stmt *stmt;
  int due = 1;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT verified_ms, expected, ok, missing, corrupt, bytes FROM sync_status"
                         " WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 1;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    int changed = sqlite3_column_int64(stmt, 1) != v->expected ||
                  sqlite3_column_int64(stmt, 2) != v->ok ||
                  sqlite3_column_int64(stmt, 3) != v->missing ||
                  sqlite3_column_int64(stmt, 4) != v->corrupt ||
                  sqlite3_column_int64(stmt, 5) != v->bytes;
    due = changed || (refresh && sqlite3_column_int64(stmt, 0) < latest_ms);
  }
  sqlite3_finalize(stmt);
  return due;
}

/* Writes v as this device's sync_status. */
static mc_status store_status(mc_sync_db *sdb, const status_row *v) {
  sqlite3 *db = mc_sync_db_handle(sdb);
  sqlite3_stmt *stmt;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO sync_status (device_uid, verified_ms, expected, ok, missing,"
                         " corrupt, bytes) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)"
                         " ON CONFLICT (device_uid) DO UPDATE SET verified_ms = excluded.verified_ms,"
                         " expected = excluded.expected, ok = excluded.ok,"
                         " missing = excluded.missing, corrupt = excluded.corrupt,"
                         " bytes = excluded.bytes",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, v->verified_ms);
    sqlite3_bind_int64(stmt, 3, v->expected);
    sqlite3_bind_int64(stmt, 4, v->ok);
    sqlite3_bind_int64(stmt, 5, v->missing);
    sqlite3_bind_int64(stmt, 6, v->corrupt);
    sqlite3_bind_int64(stmt, 7, v->bytes);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

void mc_transfer_write_status(mc_sync *sync, mc_sync_conn *c) {
  mc_mutex_lock(&sync->lock);
  int pending = sync->status_pending;
  status_row v = sync->status;
  sync->status_pending = 0;
  mc_mutex_unlock(&sync->lock);
  if (pending && store_status(c->sdb, &v) != MC_OK) {
    mc_mutex_lock(&sync->lock);
    if (!sync->status_pending) {
      sync->status = v;
      sync->status_pending = 1;
    }
    mc_mutex_unlock(&sync->lock);
  }
}

/* --- The phase --- */

void mc_transfer_begin(mc_session *s) {
  if (s->xfer) return;
  s->xfer = calloc(1, sizeof *s->xfer);
  if (s->xfer && sqlite3_exec(mc_library_db(s->conn->lib), k_watch_sql, NULL, NULL, NULL) != SQLITE_OK)
    fprintf(stderr, "musiccore: sync: can't watch for deleted files: %s\n",
            sqlite3_errmsg(mc_library_db(s->conn->lib)));
}

mc_status mc_transfer_run(mc_session *s) {
  progress(s);
  mc_status st;
  if (s->receive_first) {
    st = receive_turn(s);
    if (st == MC_OK) st = serve_turn(s);
  } else {
    st = serve_turn(s);
    if (st == MC_OK) st = receive_turn(s);
  }
  return st;
}

void mc_transfer_end(mc_session *s, int ok) {
  mc_transfer *x = s->xfer;
  if (!x) return;
  mc_library *lib = s->conn->lib;
  drop_deleted_tracks(s);
  sqlite3_exec(mc_library_db(lib), k_unwatch_sql, NULL, NULL, NULL);
  if (ok && x->fetched) {
    if (!x->plan->archive) reconcile_files(s, x);
    else drop_stale_on_archive(s);
    /* Files the user took back out of the trash would look unknown: their rows first. */
    mc_delete_restore(lib, s->sync->config_dir, NULL, NULL);
    reconcile_unknown(s, x->plan->archive);
    mc_verify_result v;
    verify_files(lib, 0, &v);
    s->files_deleted += (int)v.deleted;
    status_row row;
    int64_t latest;
    compute_status(lib, x->plan, s->files_corrupt, &row, &latest);
    s->files_missing = (int)row.missing;
    if (status_due(s->conn->sdb, &row, 0, latest)) {
      /* Written before the next session's group exchange, which follows soon. */
      mc_mutex_lock(&s->sync->lock);
      s->sync->status = row;
      s->sync->status_pending = 1;
      mc_mutex_unlock(&s->sync->lock);
      s->follow_up = 1;
    }
  }
  if (x->receiving) files_release(s->sync);
  mc_plan_free(x->plan);
  free(x->archived);
  free(x);
  s->xfer = NULL;
}

void mc_transfer_fill_result(const mc_session *s, mc_sync_result *r) {
  r->files_received = s->files_received;
  r->files_missing = s->files_missing;
  r->files_corrupt = s->files_corrupt;
  r->files_deleted = s->files_deleted;
  r->files_unknown = s->files_unknown;
  r->files_sent = s->files_sent;
  r->bytes_received = s->bytes_received;
  r->bytes_sent = s->bytes_sent;
  r->files_paused = s->files_paused;
  r->covers_received = s->covers_received;
}

/* --- Public API --- */

mc_status mc_sync_verify(mc_sync *sync, mc_verify_mode mode, mc_verify_result *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!sync || (mode != MC_VERIFY_FAST && mode != MC_VERIFY_DEEP)) return MC_ERR_INVALID_ARG;
  mc_sync_lib_state state = MC_SYNC_LIBRARY_NO_GROUP;
  if (mc_sync_library_state(sync, &state) != MC_OK) state = MC_SYNC_LIBRARY_NO_GROUP;
  files_wait(sync);
  mc_mutex_lock(&sync->api_lock);
  mc_verify_result r;
  verify_files(sync->api.lib, mode == MC_VERIFY_DEEP, &r);
  if (state == MC_SYNC_LIBRARY_GROUP) {
    int64_t free_bytes = -1;
    if (mc_free_space(mc_library_dir(sync->api.lib), &free_bytes) != MC_OK) free_bytes = -1;
    mc_plan *plan = NULL;
    if (mc_plan_compute(sync->api.sdb, sync->api.lib, sync->device_uid, free_bytes, &plan) ==
            MC_OK &&
        plan->status == MC_PLAN_OK)
    {
      status_row v;
      int64_t latest;
      compute_status(sync->api.lib, plan, 0, &v, &latest);
      /* Written now, outside any session; it replaces one a session left pending. */
      mc_mutex_lock(&sync->lock);
      sync->status_pending = 0;
      mc_mutex_unlock(&sync->lock);
      if (status_due(sync->api.sdb, &v, 1, latest)) store_status(sync->api.sdb, &v);
    }
    mc_plan_free(plan);
  }
  mc_mutex_unlock(&sync->api_lock);
  files_release(sync);
  if (state == MC_SYNC_LIBRARY_GROUP && r.failed) mc_sync_notify_changed(sync);
  if (out) *out = r;
  return MC_OK;
}
