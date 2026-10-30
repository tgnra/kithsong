#include "tagger/dupes.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "musiccore/tagger.h"
#include "util/platform.h"

#define SHA_STRLEN MC_SHA256_STRLEN

/* --- Quality --- */

static int same_ci(const char *a, const char *b) {
  if (!a || !b) return 0;
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return *a == *b;
}

int mc_quality_is_lossless(const mc_audio_quality *q) {
  static const char *const lossy[] = {"mp3", "opus", NULL};
  if (!q->format) return 0;
  for (const char *const *f = lossy; *f; f++)
    if (same_ci(q->format, *f)) return 0;
  return 1;
}

/* How many MP3 kbps one kbps of the codec is worth, roughly. */
static double codec_weight(const char *format) {
  return same_ci(format, "opus") ? 1.6 : 1.0;
}

static int cmp_int(int a, int b) {
  return (a > b) - (a < b);
}

int mc_quality_compare(const mc_audio_quality *a, const mc_audio_quality *b) {
  int la = mc_quality_is_lossless(a), lb = mc_quality_is_lossless(b);
  if (la != lb) return la - lb;
  if (la) {
    int bits = cmp_int(a->bits_per_sample ? a->bits_per_sample : 16,
                       b->bits_per_sample ? b->bits_per_sample : 16);
    if (bits) return bits;
    return a->sample_rate && b->sample_rate ? cmp_int(a->sample_rate, b->sample_rate) : 0;
  }
  if (a->bitrate_kbps <= 0 || b->bitrate_kbps <= 0) return 0; /* can't tell */
  double ea = a->bitrate_kbps * codec_weight(a->format);
  double eb = b->bitrate_kbps * codec_weight(b->format);
  if (ea > eb * 1.1) return 1;
  if (eb > ea * 1.1) return -1;
  return 0;
}

/* --- Small helpers --- */

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
  return stmt;
}

static void bind_text(sqlite3_stmt *stmt, int i, const char *s) {
  if (s && *s) sqlite3_bind_text(stmt, i, s, -1, SQLITE_STATIC);
  else sqlite3_bind_null(stmt, i);
}

static void copy_uid(char out[MC_UID_STRLEN], const unsigned char *v) {
  out[0] = '\0';
  if (v && strlen((const char *)v) < MC_UID_STRLEN) strcpy(out, (const char *)v);
}

/* The first row's uid, then finalizes. */
static int first_uid(sqlite3_stmt *stmt, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  if (stmt && sqlite3_step(stmt) == SQLITE_ROW) copy_uid(out, sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return out[0] != '\0';
}

static int same_id(const char *a, const char *b) {
  return a && b && *a && strcmp(a, b) == 0;
}

static char *dirname_of(const char *rel) {
  const char *slash = strrchr(rel, '/');
  if (!slash) return NULL;
  char *d = malloc((size_t)(slash - rel) + 1);
  if (d) {
    memcpy(d, rel, (size_t)(slash - rel));
    d[slash - rel] = '\0';
  }
  return d;
}

static mc_status sha256_file(const char *path, char out[SHA_STRLEN]) {
  static const char hex[] = "0123456789abcdef";
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
  mbedtls_sha256_free(&ctx);
  free(buf);
  fclose(f);
  if (st != MC_OK) return st;
  for (int i = 0; i < 32; i++) {
    out[2 * i] = hex[digest[i] >> 4];
    out[2 * i + 1] = hex[digest[i] & 15];
  }
  out[64] = '\0';
  return MC_OK;
}

/* --- The library's side --- */

/* A track of the album in the library, with its original on this device if there is one. */
typedef struct lib_track {
  char uid[MC_UID_STRLEN];
  char *title;
  int disc, number;
  char *mb_trackid, *mb_releasetrackid;
  int has_file; /* the original's file is there */
  int remote;   /* no file here, but another device's original has a known quality */
  char file_uid[MC_UID_STRLEN];
  char *path; /* relative */
  char format[16];
  mc_audio_quality quality;
  int paired; /* index into the incoming files, or -1 */
} lib_track;

static void lib_tracks_free(lib_track *t, int n) {
  for (int i = 0; i < n; i++) {
    free(t[i].title);
    free(t[i].mb_trackid);
    free(t[i].mb_releasetrackid);
    free(t[i].path);
  }
  free(t);
}

static char *col_dup(sqlite3_stmt *stmt, int col) {
  return mc_strdup((const char *)sqlite3_column_text(stmt, col));
}

/* The album's tracks, each with its original (imported here first); or, with album_uid
 * NULL, artist_uid's tracks with no album. The quality is read from the file, or from the
 * files row if the file can't be read. */
static mc_status read_album(mc_library *lib, const char *album_uid, const char *artist_uid,
                            const mc_dupe_remote *remote, lib_track **out, int *count) {
  *out = NULL;
  *count = 0;
#define READ_TRACKS \
  "SELECT t.uid, t.title, t.disc_no, t.track_no, t.mb_trackid, t.mb_releasetrackid," \
  " f.uid, f.path, f.format, f.bitrate_kbps" \
  " FROM tracks t LEFT JOIN files f ON f.uid =" \
  "  (SELECT g.uid FROM files g WHERE g.track_uid = t.uid AND g.source_file_uid IS NULL" \
  "   ORDER BY g.source_peer_uid IS NOT NULL, g.uid LIMIT 1)"
  sqlite3_stmt *stmt = prepare(
    mc_library_db(lib),
    album_uid ? READ_TRACKS " WHERE t.album_uid = ?1 ORDER BY t.disc_no, t.track_no, t.uid"
              : READ_TRACKS " WHERE t.artist_uid = ?1 AND t.album_uid IS NULL ORDER BY t.uid");
#undef READ_TRACKS
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, album_uid ? album_uid : artist_uid, -1, SQLITE_STATIC);
  lib_track *t = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (n == cap) {
      int grown = cap ? cap * 2 : 32;
      lib_track *more = realloc(t, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      t = more;
      cap = grown;
    }
    lib_track *r = &t[n++];
    memset(r, 0, sizeof *r);
    r->paired = -1;
    copy_uid(r->uid, sqlite3_column_text(stmt, 0));
    r->title = col_dup(stmt, 1);
    r->disc = sqlite3_column_int(stmt, 2);
    r->number = sqlite3_column_int(stmt, 3);
    r->mb_trackid = col_dup(stmt, 4);
    r->mb_releasetrackid = col_dup(stmt, 5);
    copy_uid(r->file_uid, sqlite3_column_text(stmt, 6));
    r->path = col_dup(stmt, 7);
    const char *format = (const char *)sqlite3_column_text(stmt, 8);
    snprintf(r->format, sizeof r->format, "%s", format ? format : "");
    r->quality.format = r->format;
    r->quality.bitrate_kbps = sqlite3_column_int(stmt, 9);

    char *abs = r->file_uid[0] && r->path ? mc_library_abspath(lib, r->path) : NULL;
    if (abs && mc_file_exists(abs)) {
      r->has_file = 1;
      mc_tags tags;
      if (mc_tags_read(abs, &tags) == MC_OK) {
        if (tags.bitrate_kbps) r->quality.bitrate_kbps = tags.bitrate_kbps;
        r->quality.sample_rate = tags.sample_rate;
        r->quality.bits_per_sample = tags.bits_per_sample;
        mc_tags_clear(&tags);
      }
    } else if (remote && remote->quality) {
      mc_audio_quality q = {0};
      if (remote->quality(remote->user, r->uid, &q)) {
        r->remote = 1;
        snprintf(r->format, sizeof r->format, "%s", q.format ? q.format : "");
        r->quality = q;
        r->quality.format = r->format;
      }
    }
    free(abs);
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    lib_tracks_free(t, n);
    return st;
  }
  *out = t;
  *count = n;
  return MC_OK;
}

/* Pairs incoming files with the album's tracks, strongest evidence first. pair[i] is the
 * library track of incoming file i, or -1. Returns how many were paired. */
static int pair_tracks(const mc_dupe_in *in, int count, lib_track *t, int n, int no_album,
                       int *pair) {
  for (int i = 0; i < count; i++) pair[i] = -1;
  for (int j = 0; j < n; j++) t[j].paired = -1;
  int paired = 0;
  for (int pass = 0; pass < 4; pass++) {
    if (no_album && (pass == 0 || pass == 2)) continue; /* no release or track numbers */
    for (int i = 0; i < count; i++) {
      if (pair[i] >= 0) continue;
      const mc_db_track *r = in[i].row;
      for (int j = 0; j < n; j++) {
        if (t[j].paired >= 0) continue;
        int match = 0;
        switch (pass) {
          case 0: match = same_id(r->mb_releasetrackid, t[j].mb_releasetrackid); break;
          case 1: match = same_id(r->mb_trackid, t[j].mb_trackid); break;
          case 2:
            match = r->track_no && r->track_no == t[j].number &&
                    (r->disc_no ? r->disc_no : 1) == (t[j].disc ? t[j].disc : 1) &&
                    same_ci(r->title, t[j].title);
            break;
          default: match = same_ci(r->title, t[j].title); break;
        }
        if (match) {
          pair[i] = j;
          t[j].paired = i;
          paired++;
          break;
        }
      }
    }
  }
  return paired;
}

/* The artist's uid, by MusicBrainz id then by name, or "". */
static int find_artist(sqlite3 *db, const mc_db_artist *a, char uid[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt;
  if (a->mb_artistid && *a->mb_artistid) {
    if ((stmt = prepare(db, "SELECT uid FROM artists WHERE mb_artistid = ?1"))) {
      bind_text(stmt, 1, a->mb_artistid);
      if (first_uid(stmt, uid)) return 1;
    }
  }
  if (!a->name || !*a->name) return 0;
  if (!(stmt = prepare(db, "SELECT uid FROM artists WHERE name = ?1 COLLATE NOCASE"))) return 0;
  bind_text(stmt, 1, a->name);
  return first_uid(stmt, uid);
}

/* The library album holding the incoming release, or "": see tagger.h. On success the
 * album's tracks are in *tracks, pair[] says which incoming file is which, and *found_by_id
 * whether the album has the release's MusicBrainz release or release group id. */
static mc_status find_release(mc_library *lib, const mc_db_album *album, const mc_dupe_in *in,
                              int count, const mc_dupe_remote *remote,
                              char album_uid[MC_UID_STRLEN], lib_track **tracks, int *ntracks,
                              int *pair, int *found_by_id) {
  sqlite3 *db = mc_library_db(lib);
  album_uid[0] = '\0';
  *tracks = NULL;
  *ntracks = 0;
  *found_by_id = 0;

  /* Tracks with no album: the same recording (or, without ids, title) among the artist's
   * own tracks with no album. */
  if (album->no_album) {
    const mc_db_artist *who = in[0].row->artist.name && *in[0].row->artist.name ? &in[0].row->artist
                                                                              : &album->artist;
    char artist[MC_UID_STRLEN];
    if (find_artist(db, who, artist)) {
      lib_track *t;
      int n;
      mc_status st = read_album(lib, NULL, artist, remote, &t, &n);
      if (st != MC_OK) return st;
      if (pair_tracks(in, count, t, n, 1, pair) > 0) {
        *tracks = t;
        *ntracks = n;
        return MC_OK;
      }
      lib_tracks_free(t, n);
    }
    for (int i = 0; i < count; i++) pair[i] = -1;
    return MC_OK;
  }

  /* Candidate albums, best evidence first. A name match must share the track list. */
  char cands[8][MC_UID_STRLEN];
  int by_id[8], ncand = 0;
  sqlite3_stmt *stmt;
  static const char *const by[] = {
    "SELECT uid FROM albums WHERE mb_albumid = ?1",
    "SELECT uid FROM albums WHERE mb_releasegroupid = ?1",
    "SELECT al.uid FROM albums al JOIN artists ar ON ar.uid = al.artist_uid"
    " WHERE al.title = ?1 COLLATE NOCASE AND ar.name = ?2 COLLATE NOCASE"};
  const char *keys[] = {album->mb_albumid, album->mb_releasegroupid, album->title};
  for (int k = 0; k < 3 && ncand < 8; k++) {
    if (!keys[k] || !*keys[k] || (k == 2 && (!album->artist.name || !*album->artist.name)))
      continue;
    if (!(stmt = prepare(db, by[k]))) return MC_ERR_DB;
    bind_text(stmt, 1, keys[k]);
    if (k == 2) bind_text(stmt, 2, album->artist.name);
    while (ncand < 8 && sqlite3_step(stmt) == SQLITE_ROW) {
      copy_uid(cands[ncand], sqlite3_column_text(stmt, 0));
      int dup = 0;
      for (int c = 0; c < ncand; c++) dup |= !strcmp(cands[c], cands[ncand]);
      if (!dup && cands[ncand][0]) by_id[ncand++] = k < 2;
    }
    sqlite3_finalize(stmt);
  }

  for (int c = 0; c < ncand; c++) {
    lib_track *t;
    int n;
    mc_status st = read_album(lib, cands[c], NULL, remote, &t, &n);
    if (st != MC_OK) return st;
    int paired = pair_tracks(in, count, t, n, 0, pair);
    /* An album by name needs at least half of the incoming tracks in its track list. */
    int ok = by_id[c] || 2 * paired >= count;
    if (ok) {
      memcpy(album_uid, cands[c], MC_UID_STRLEN);
      *tracks = t;
      *ntracks = n;
      *found_by_id = by_id[c];
      return MC_OK;
    }
    lib_tracks_free(t, n);
  }
  for (int i = 0; i < count; i++) pair[i] = -1;
  return MC_OK;
}

/* Whether the library has the incoming file (whose SHA-256 is sha) already, byte for byte:
 * a file imported from those bytes (files.source_sha256), or holding them (the recorded
 * sha256, else, for a file of the same size, hashed now). */
static int same_bytes_in_library(mc_library *lib, const mc_dupe_in *in, const char *sha) {
  if (!sha[0]) return 0;
  sqlite3_stmt *stmt = prepare(mc_library_db(lib), "SELECT 1 FROM files"
                                                   " WHERE source_sha256 = ?1 OR sha256 = ?1");
  if (!stmt) return 0;
  sqlite3_bind_text(stmt, 1, sha, -1, SQLITE_STATIC);
  int same = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if (same || in->size <= 0) return same;

  stmt = prepare(mc_library_db(lib), "SELECT path FROM files WHERE size_bytes = ?1"
                                     " AND (sha256 IS NULL OR length(sha256) <> 64)");
  if (!stmt) return 0;
  sqlite3_bind_int64(stmt, 1, in->size);
  while (!same && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 0);
    if (!rel) continue;
    char *abs = mc_library_abspath(lib, rel);
    int64_t size = 0, mtime = 0;
    char theirs[SHA_STRLEN];
    if (abs && mc_file_info(abs, &size, &mtime) == MC_OK && size == in->size &&
        sha256_file(abs, theirs) == MC_OK)
      same = strcmp(theirs, sha) == 0;
    free(abs);
  }
  sqlite3_finalize(stmt);
  return same;
}

/* What the library says of track_uid, as the importer tags a file (write_item in tagger.c;
 * for a track with no album, without album tags, release ids or numbers). Ids the library hasn't are ""
 * (removed from the file), so none of the new file's own can contradict the track; the
 * genre is left to the file unless the library has one. */
static mc_status library_tags(sqlite3 *db, const char *track_uid, mc_tags *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = prepare(
    db,
    "SELECT t.title, coalesce(ta.name, aa.name), coalesce(ta.mb_artistid, ''), t.genre,"
    " t.track_no, t.disc_no, t.year, coalesce(t.mb_trackid, ''),"
    " coalesce(t.mb_releasetrackid, ''), al.title, al.uid IS NULL, aa.name,"
    " coalesce(aa.mb_artistid, ''), al.year, al.original_year, al.disc_total,"
    " coalesce(al.mb_albumid, ''), coalesce(al.mb_releasegroupid, ''),"
    /* the disc's track count, when the library has the whole disc */
    " (SELECT CASE WHEN count(*) >= max(coalesce(u.track_no, 0)) THEN count(*) END"
    "  FROM tracks u WHERE u.album_uid = t.album_uid"
    "  AND coalesce(u.disc_no, 1) = coalesce(t.disc_no, 1))"
    " FROM tracks t LEFT JOIN artists ta ON ta.uid = t.artist_uid"
    " LEFT JOIN albums al ON al.uid = t.album_uid LEFT JOIN artists aa ON aa.uid = al.artist_uid"
    " WHERE t.uid = ?1");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, track_uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return MC_ERR_NOT_FOUND;
  }
  int no_album = sqlite3_column_int(stmt, 10);
  out->title = col_dup(stmt, 0);
  out->artist = col_dup(stmt, 1);
  out->mb_artistid = col_dup(stmt, 2);
  out->genre = col_dup(stmt, 3);
  out->mb_trackid = col_dup(stmt, 7);
  out->mb_releasetrackid = col_dup(stmt, 8);
  out->album = no_album ? mc_strdup("") : col_dup(stmt, 9);
  out->album_artist = no_album ? mc_strdup("") : col_dup(stmt, 11);
  out->mb_albumartistid = col_dup(stmt, 12);
  out->original_year = sqlite3_column_int(stmt, 14);
  out->year = sqlite3_column_int(stmt, 6) ? sqlite3_column_int(stmt, 6) : sqlite3_column_int(stmt, 13);
  out->mb_albumid = col_dup(stmt, 16);
  out->mb_releasegroupid = col_dup(stmt, 17);
  if (!no_album) {
    out->track = sqlite3_column_int(stmt, 4);
    out->track_total = out->track ? sqlite3_column_int(stmt, 18) : 0;
    out->disc = sqlite3_column_int(stmt, 5);
    out->disc_total = sqlite3_column_int(stmt, 15);
  }
  sqlite3_finalize(stmt);
  if (!out->title || !out->mb_artistid || !out->mb_trackid || !out->mb_releasetrackid ||
      !out->mb_albumartistid || !out->mb_albumid || !out->mb_releasegroupid) {
    mc_tags_clear(out);
    return MC_ERR_NOMEM;
  }
  return MC_OK;
}

/* --- Finishing a swap --- */

mc_status (*mc_dupes_trash)(const char *path) = mc_trash;

void mc_dupes_finish_swap(mc_library *lib, const char *old_rel, const char *final_rel,
                          char **dest_rel, char **dest_abs) {
  char *old_abs = old_rel ? mc_library_abspath(lib, old_rel) : NULL;
  if (old_abs && mc_file_exists(old_abs) && mc_dupes_trash(old_abs) != MC_OK)
    fprintf(stderr, "musiccore: import: couldn't move %s to the trash; left in place\n", old_abs);
  free(old_abs);
  if (!final_rel || !*dest_rel || !*dest_abs || strcmp(*dest_rel, final_rel) == 0) return;
  char *final_abs = mc_library_abspath(lib, final_rel);
  char *rel = mc_strdup(final_rel);
  if (final_abs && rel && !mc_file_exists(final_abs) &&
      mc_rename(*dest_abs, final_abs, 0) == MC_OK) {
    if (mc_import_db_move_file(lib, *dest_rel, final_rel) == MC_OK) {
      free(*dest_abs);
      free(*dest_rel);
      *dest_abs = final_abs;
      *dest_rel = rel;
      return;
    }
    mc_rename(final_abs, *dest_abs, 0); /* keep the file where its row says */
  }
  free(final_abs);
  free(rel);
}

/* --- The plan --- */

void mc_dupe_plan_free(mc_dupe_plan *plan) {
  if (!plan) return;
  for (int i = 0; i < plan->count; i++) {
    free(plan->d[i].old_path);
    if (plan->d[i].has_tags) mc_tags_clear(&plan->d[i].tags);
  }
  free(plan->d);
  free(plan->album_dir);
  memset(plan, 0, sizeof *plan);
}

mc_status mc_dupes_plan(mc_library *lib, const mc_db_album *album, const mc_dupe_in *in,
                        int count, mc_dupe_plan *out) {
  return mc_dupes_plan_remote(lib, album, in, count, NULL, out);
}

mc_status mc_dupes_plan_remote(mc_library *lib, const mc_db_album *album, const mc_dupe_in *in,
                               int count, const mc_dupe_remote *remote, mc_dupe_plan *out) {
  if (!out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!lib || !album || count <= 0 || !in) return MC_ERR_INVALID_ARG;
  out->d = calloc((size_t)count, sizeof *out->d);
  int *pair = malloc((size_t)count * sizeof *pair);
  if (!out->d || !pair) {
    free(pair);
    mc_dupe_plan_free(out);
    return MC_ERR_NOMEM;
  }
  out->count = count;

  lib_track *t = NULL;
  int n = 0, by_id = 0;
  mc_status st = find_release(lib, album, in, count, remote, out->album_uid, &t, &n, pair, &by_id);
  if (st != MC_OK) {
    free(pair);
    mc_dupe_plan_free(out);
    return st;
  }

  /* Judge the release as a whole, over the tracks this device has a file of. Tracks with no
   * album aren't a release: only the paired ones count. */
  int held = 0, covered = 0, better = 0, worse = 0;
  for (int j = 0; j < n; j++) {
    if ((!t[j].has_file && !t[j].remote) || (album->no_album && t[j].paired < 0)) continue;
    held++;
    if (t[j].paired < 0) continue;
    covered++;
    int c = mc_quality_compare(&in[t[j].paired].quality, &t[j].quality);
    better += c > 0;
    worse += c < 0;
  }
  int swap = held > 0 && covered == held && worse == 0 && 2 * better > covered;
  /* The whole release, if every track of the album is paired and none is held elsewhere. */
  out->whole = swap && by_id;
  for (int j = 0; j < n; j++)
    if (t[j].paired < 0 || t[j].remote) out->whole = 0;

  for (int i = 0; i < count; i++) {
    mc_dupe_decision *d = &out->d[i];
    d->action = MC_DUPE_IMPORT;
    if (pair[i] < 0) continue;
    lib_track *lt = &t[pair[i]];
    memcpy(d->track_uid, lt->uid, MC_UID_STRLEN);
    if (lt->remote) {
      d->remote = 1;
      d->action = swap ? MC_DUPE_ATTACH : MC_DUPE_SKIP;
    } else if (!lt->has_file) {
      d->action = MC_DUPE_ATTACH;
    } else if (swap) {
      d->action = MC_DUPE_SWAP;
      memcpy(d->file_uid, lt->file_uid, MC_UID_STRLEN);
      if (!(d->old_path = mc_strdup(lt->path))) st = MC_ERR_NOMEM;
    } else {
      d->action = MC_DUPE_SKIP;
    }
  }

  /* The source hash of every file that goes in, and new files that are byte for byte in
   * the library already, anywhere. */
  for (int i = 0; i < count; i++) {
    mc_dupe_decision *d = &out->d[i];
    if (d->action != MC_DUPE_SKIP && sha256_file(in[i].src_path, d->source_sha256) != MC_OK)
      d->source_sha256[0] = '\0';
    if ((d->action == MC_DUPE_IMPORT || d->action == MC_DUPE_ATTACH) &&
        same_bytes_in_library(lib, &in[i], d->source_sha256)) {
      d->action = MC_DUPE_SKIP;
      d->source_sha256[0] = '\0';
    }
    out->swaps += d->action == MC_DUPE_SWAP;
    out->skips += d->action == MC_DUPE_SKIP;
  }

  /* A file becoming a copy of a library track is tagged as that track. */
  for (int i = 0; i < count && st == MC_OK && !out->whole; i++) {
    mc_dupe_decision *d = &out->d[i];
    if (d->action != MC_DUPE_SWAP && (d->action != MC_DUPE_ATTACH || d->remote)) continue;
    mc_status ts = library_tags(mc_library_db(lib), d->track_uid, &d->tags);
    if (ts == MC_OK) d->has_tags = 1;
    else if (ts == MC_ERR_NOMEM) st = ts;
  }

  /* New tracks of a release that's here go in the folder its files are in. Tracks with no
   * album have no such folder. */
  for (int j = 0; j < n && !album->no_album && !out->album_dir && st == MC_OK; j++)
    if (t[j].has_file && t[j].path) out->album_dir = dirname_of(t[j].path);

  lib_tracks_free(t, n);
  free(pair);
  if (st != MC_OK) mc_dupe_plan_free(out);
  return st;
}
