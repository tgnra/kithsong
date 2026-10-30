/* Duplicates on import (tagger/dupes.c): quality, finding the release in the library, and
 * what a swap does to the files and rows. The import thread's own steps (copy, tag, rows,
 * finish) are mirrored by apply() below, since the thread needs MusicBrainz. Also
 * mc_trash()'s freedesktop.org trashes (the home one and a top directory's), on POSIX
 * only: on Windows it's the real Recycle Bin. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* setenv */
#define _DEFAULT_SOURCE         /* realpath */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "library/library_internal.h"
#include "musiccore/library.h"
#include "musiccore/playlist.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "test.h"
#include "util/platform.h"

static char g_scratch[1024], g_trash[1100];
static int g_trashed;

/* Stands in for the Recycle Bin, so the test never touches the real one. */
static mc_status fake_trash(const char *path) {
  char dest[1300];
  snprintf(dest, sizeof dest, "%s%ctrashed-%d", g_trash, MC_PATH_SEP, ++g_trashed);
  return mc_rename(path, dest, 0);
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void query_text(mc_library *lib, const char *sql, char *out, size_t n) {
  sqlite3_stmt *stmt;
  out[0] = '\0';
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    snprintf(out, n, "%s", (const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
}

static void write_file(const char *path, const char *content) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  fputs(content, f);
  fclose(f);
}

static int64_t file_size(const char *path) {
  int64_t size = 0, mtime = 0;
  mc_file_info(path, &size, &mtime);
  return size;
}

/* A short silent WAV, so the library's copy has a real bit depth and sample rate. */
static void write_wav(const char *path, int rate, int bits) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  int channels = 2, frames = rate / 10;
  unsigned data = (unsigned)(frames * channels * bits / 8);
  unsigned char h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                         16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         'd', 'a', 't', 'a', 0, 0, 0, 0};
  unsigned riff = 36 + data, byte_rate = (unsigned)(rate * channels * bits / 8);
  unsigned align = (unsigned)(channels * bits / 8);
  for (int i = 0; i < 4; i++) {
    h[4 + i] = (unsigned char)(riff >> (8 * i));
    h[24 + i] = (unsigned char)((unsigned)rate >> (8 * i));
    h[28 + i] = (unsigned char)(byte_rate >> (8 * i));
    h[40 + i] = (unsigned char)(data >> (8 * i));
  }
  h[22] = (unsigned char)channels;
  h[32] = (unsigned char)align;
  h[34] = (unsigned char)bits;
  fwrite(h, 1, sizeof h, f);
  for (unsigned i = 0; i < data; i++) fputc(0, f);
  fclose(f);
}

typedef struct tree {
  const char *dir;
} tree;

static void remove_tree(const char *dir);

static void remove_entry(void *user, const char *name, int is_dir) {
  char path[2048];
  snprintf(path, sizeof path, "%s%c%s", ((tree *)user)->dir, MC_PATH_SEP, name);
  if (is_dir) remove_tree(path);
  else mc_remove(path);
}

static void remove_tree(const char *dir) {
  tree t = {dir};
  mc_list_dir(dir, remove_entry, &t);
  mc_rmdir(dir);
}

/* --- Fixtures --- */

#define MBID_ALBUM "d0000000-0000-4000-8000-0000000000a1"
#define MBID_REISSUE "d0000000-0000-4000-8000-0000000000a2" /* another release of the group */
#define MBID_GROUP "d0000000-0000-4000-8000-0000000000b1"
#define MBID_ARTIST "d0000000-0000-4000-8000-0000000000c1"

static const char *const k_titles[] = {"Sunflower", "Whore", "Dinosaur Act"};
static const char *const k_rtids[] = {"d0000000-0000-4000-8000-000000000101",
                                      "d0000000-0000-4000-8000-000000000102",
                                      "d0000000-0000-4000-8000-000000000103"};
static const char *const k_reissue_rtids[] = {"d0000000-0000-4000-8000-000000000111",
                                              "d0000000-0000-4000-8000-000000000112",
                                              "d0000000-0000-4000-8000-000000000113"};

static mc_db_album album_desc(const char *mbid) {
  mc_db_album a = {0};
  a.title = "Things We Lost in the Fire";
  a.artist = (mc_db_artist){"Low", NULL, MBID_ARTIST};
  a.year = 2001;
  a.mb_albumid = mbid;
  a.mb_releasegroupid = mbid ? MBID_GROUP : NULL;
  return a;
}

/* A library at <scratch>/<name> holding the album as `format` files (kbps for lossy; for
 * "wav", real WAVs at rate/bits). */
static mc_library *make_library(const char *name, const char *mbid, const char *format, int kbps,
                                int rate, int bits) {
  char dir[1200], abs[1400];
  snprintf(dir, sizeof dir, "%s%c%s", g_scratch, MC_PATH_SEP, name);
  mc_library *lib = NULL;
  if (mc_library_open(dir, &lib) != MC_OK) return NULL;
  snprintf(abs, sizeof abs, "%s%cLow%cThings (2001)", dir, MC_PATH_SEP, MC_PATH_SEP);
  mc_mkdirs(abs);

  mc_db_album a = album_desc(mbid);
  mc_db_track t[3];
  char rel[3][160];
  for (int i = 0; i < 3; i++) {
    snprintf(rel[i], sizeof rel[i], "Low/Things (2001)/0%d %s.%s", i + 1, k_titles[i], format);
    snprintf(abs, sizeof abs, "%s%c%s", dir, MC_PATH_SEP, rel[i]);
    for (char *p = abs; *p; p++)
      if (*p == '/') *p = MC_PATH_SEP;
    if (!strcmp(format, "wav")) {
      write_wav(abs, rate, bits);
    } else {
      char content[128];
      snprintf(content, sizeof content, "library copy of %s at %d kbps", k_titles[i], kbps);
      write_file(abs, content);
    }
    memset(&t[i], 0, sizeof t[i]);
    t[i].path = rel[i];
    t[i].format = format;
    t[i].bitrate_kbps = kbps;
    t[i].size_bytes = file_size(abs);
    t[i].title = k_titles[i];
    t[i].track_no = i + 1;
    t[i].disc_no = 1;
    t[i].duration_ms = 200000;
    t[i].mb_releasetrackid = mbid ? k_rtids[i] : NULL;
    t[i].mb_albumid = mbid;
  }
  if (mc_import_db_write(lib, &a, t, 3) != MC_OK) {
    mc_library_close(lib);
    return NULL;
  }
  return lib;
}

/* Incoming files: n tracks of the album as `format` files outside the library. */
typedef struct incoming {
  char path[3][1300];
  mc_db_track row[3];
  mc_dupe_in in[3];
  int n;
} incoming;

static void make_incoming(incoming *x, const char *tag, int n, const char *mbid, const char *format,
                          int kbps, int rate, int bits) {
  memset(x, 0, sizeof *x);
  x->n = n;
  char dir[1200];
  snprintf(dir, sizeof dir, "%s%cin-%s", g_scratch, MC_PATH_SEP, tag);
  mc_mkdirs(dir);
  for (int i = 0; i < n; i++) {
    snprintf(x->path[i], sizeof x->path[i], "%s%c%d.%s", dir, MC_PATH_SEP, i + 1, format);
    char content[160];
    snprintf(content, sizeof content, "incoming %s copy of %s", tag, k_titles[i]);
    write_file(x->path[i], content);
    mc_db_track *r = &x->row[i];
    r->format = format;
    r->bitrate_kbps = kbps;
    r->title = k_titles[i];
    r->track_no = i + 1;
    r->disc_no = 1;
    r->duration_ms = 201000;
    r->mb_releasetrackid = mbid ? k_rtids[i] : NULL;
    r->mb_albumid = mbid;
    x->in[i].row = r;
    x->in[i].src_path = x->path[i];
    x->in[i].size = file_size(x->path[i]);
    x->in[i].quality = (mc_audio_quality){format, kbps, rate, bits};
  }
}

/* What write_item does with a plan: copy the files in (a swapped file waits next to the old
 * one when its name is taken), save the rows, then finish the swaps. (Files keep the old
 * names here even for a whole release, whose new names are the same.) */
static mc_status apply(mc_library *lib, mc_db_album *album, incoming *x, mc_dupe_plan *p) {
  mc_db_track rows[3];
  char *dest_rel[3] = {0}, *dest_abs[3] = {0};
  int idx[3], n = 0;
  if (p->album_uid[0]) album->existing_uid = p->album_uid;
  album->update_existing = p->whole;
  for (int i = 0; i < x->n; i++) {
    mc_dupe_decision *d = &p->d[i];
    if (d->action == MC_DUPE_SKIP) continue;
    char rel[512];
    if (d->action == MC_DUPE_SWAP) {
      const char *dot = strrchr(d->old_path, '.');
      snprintf(rel, sizeof rel, "%.*s.%s", (int)(dot - d->old_path), d->old_path, x->row[i].format);
    } else {
      snprintf(rel, sizeof rel, "%s/0%d %s.%s", p->album_dir ? p->album_dir : "Low/New",
               i + 1, k_titles[i], x->row[i].format);
    }
    char *abs = mc_library_abspath(lib, rel);
    if (mc_file_exists(abs)) {
      snprintf(rel + strlen(rel) - strlen(x->row[i].format) - 1, 32, " (2).%s", x->row[i].format);
      free(abs);
      abs = mc_library_abspath(lib, rel);
    }
    char *dir = mc_strdup(abs);
    char *sep = strrchr(dir, MC_PATH_SEP);
    if (sep) *sep = '\0';
    mc_mkdirs(dir);
    free(dir);
    if (mc_copy_file(x->path[i], abs, 0) != MC_OK) return MC_ERR_IO;
    dest_rel[n] = mc_strdup(rel);
    dest_abs[n] = abs;
    rows[n] = x->row[i];
    rows[n].path = dest_rel[n];
    rows[n].size_bytes = file_size(abs);
    if (d->action == MC_DUPE_SWAP || d->action == MC_DUPE_ATTACH) rows[n].track_uid = d->track_uid;
    if (d->action == MC_DUPE_SWAP) rows[n].replace_file_uid = d->file_uid;
    rows[n].update_track = p->whole;
    rows[n].source_sha256 = d->source_sha256[0] ? d->source_sha256 : NULL;
    idx[n++] = i;
  }
  mc_status st = n ? mc_import_db_write(lib, album, rows, n) : MC_OK;
  for (int k = 0; k < n; k++) {
    mc_dupe_decision *d = &p->d[idx[k]];
    if (st == MC_OK && d->action == MC_DUPE_SWAP) {
      const char *dot = strrchr(d->old_path, '.');
      char final_rel[512];
      snprintf(final_rel, sizeof final_rel, "%.*s.%s", (int)(dot - d->old_path), d->old_path,
               x->row[idx[k]].format);
      mc_dupes_finish_swap(lib, d->old_path, final_rel, &dest_rel[k], &dest_abs[k]);
    }
    free(dest_rel[k]);
    free(dest_abs[k]);
  }
  return st;
}

static int lib_file_exists(mc_library *lib, const char *rel) {
  char *abs = mc_library_abspath(lib, rel);
  int yes = abs && mc_file_exists(abs);
  free(abs);
  return yes;
}

/* --- Tests --- */

static void test_quality(void) {
  mc_audio_quality flac16 = {"flac", 900, 44100, 16}, flac24 = {"flac", 2500, 96000, 24};
  mc_audio_quality flac24_48 = {"flac", 1500, 48000, 24}, wav16 = {"wav", 1411, 44100, 16};
  mc_audio_quality mp3_320 = {"mp3", 320, 44100, 0}, mp3_128 = {"mp3", 128, 44100, 0};
  mc_audio_quality mp3_245 = {"mp3", 245, 44100, 0}, mp3_256 = {"mp3", 256, 44100, 0};
  mc_audio_quality opus_160 = {"opus", 160, 48000, 0}, mp3_192 = {"mp3", 192, 44100, 0};
  mc_audio_quality unknown = {"mp3", 0, 0, 0};

  CHECK(mc_quality_compare(&flac16, &mp3_320) > 0); /* lossless beats lossy */
  CHECK(mc_quality_compare(&mp3_320, &flac16) < 0);
  CHECK(mc_quality_compare(&flac24, &flac16) > 0);  /* bit depth */
  CHECK(mc_quality_compare(&flac24, &flac24_48) > 0); /* then sample rate */
  CHECK(mc_quality_compare(&flac16, &wav16) == 0);  /* the same audio in another container */
  CHECK(mc_quality_compare(&mp3_320, &mp3_128) > 0);
  CHECK(mc_quality_compare(&mp3_256, &mp3_245) == 0); /* within 10%: VBR noise */
  CHECK(mc_quality_compare(&opus_160, &mp3_192) > 0); /* codec-aware */
  CHECK(mc_quality_compare(&unknown, &mp3_128) == 0); /* can't tell: not better */
}

/* A better release replaces the library's copy, keeping every uid and what hangs off them. */
static void test_swap(void) {
  mc_library *lib = make_library("swap", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  char album_uid[64], track1[64], file1[64], pl[MC_UID_STRLEN], entry[MC_UID_STRLEN];
  query_text(lib, "SELECT uid FROM albums", album_uid, sizeof album_uid);
  query_text(lib, "SELECT uid FROM tracks WHERE track_no = 1", track1, sizeof track1);
  query_text(lib, "SELECT uid FROM files WHERE path LIKE '%01 %'", file1, sizeof file1);
  CHECK(mc_playlist_create(lib, "Favourites", pl) == MC_OK);
  CHECK(mc_playlist_append(lib, pl, track1, entry) == MC_OK);
  CHECK(mc_library_record_play(lib, track1) == MC_OK);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM, album_uid) == MC_OK);
  int seq_before = query_int(lib, "SELECT max(seq) FROM changelog");

  incoming x;
  make_incoming(&x, "flac", 3, MBID_ALBUM, "flac", 900, 44100, 16);
  mc_db_album a = album_desc(MBID_ALBUM);
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(!strcmp(p.album_uid, album_uid));
  CHECK(p.swaps == 3 && p.skips == 0);
  CHECK(p.album_dir && !strcmp(p.album_dir, "Low/Things (2001)"));
  CHECK(p.whole); /* every track of the release, found by its id */
  for (int i = 0; i < 3; i++) {
    CHECK(p.d[i].action == MC_DUPE_SWAP);
    CHECK(strlen(p.d[i].source_sha256) == 64 && !p.d[i].has_tags);
  }
  int trashed = g_trashed;
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);

  /* Same album, same tracks, same playlist entry, plays and recents. */
  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 3);
  char sql[512], now[64];
  query_text(lib, "SELECT uid FROM albums", now, sizeof now);
  CHECK(!strcmp(now, album_uid));
  query_text(lib, "SELECT uid FROM tracks WHERE track_no = 1", now, sizeof now);
  CHECK(!strcmp(now, track1));
  snprintf(sql, sizeof sql, "SELECT track_uid FROM playlist_entries WHERE uid = '%s'", entry);
  query_text(lib, sql, now, sizeof now);
  CHECK(!strcmp(now, track1));
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", track1);
  CHECK(query_int(lib, sql) == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM recents WHERE uid = '%s'", album_uid);
  CHECK(query_int(lib, sql) == 1);

  /* The files are the new ones, under the old names with the new extension; the old ones
   * went to the trash. */
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE format = 'flac' AND bitrate_kbps = 900"
                       " AND sha256 IS NULL AND source_peer_uid IS NULL") == 3);
  CHECK(lib_file_exists(lib, "Low/Things (2001)/01 Sunflower.flac"));
  CHECK(!lib_file_exists(lib, "Low/Things (2001)/01 Sunflower.mp3"));
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE path = 'Low/Things (2001)/01 Sunflower.flac'") == 1);
  CHECK(g_trashed - trashed == 3);
  /* The files row has a new uid (nothing cached for the old file applies) and the track a
   * new file_rev, logged so other devices replace their copies; the file rows match it. */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE uid = '%s'", file1);
  CHECK(query_int(lib, sql) == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE file_rev IS NOT NULL") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM files f JOIN tracks t ON t.uid = f.track_uid"
                       " WHERE f.file_rev = t.file_rev") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'tracks'"
                       " AND field = 'file_rev'") == 3);
  /* Only that (and the duration): the swap rewrites no other group data, as the match
   * says what the library did. */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE seq > %d AND NOT"
                            " (entity = 'tracks' AND field IN ('file_rev', 'duration_ms'))",
           seq_before);
  CHECK(query_int(lib, sql) == 0);

  /* Importing the same files again: now equal, so skipped. */
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 3);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* Same-extension swap: the new file waits at "(2)" until the old one is trashed. */
static void test_swap_same_name(void) {
  mc_library *lib = make_library("samename", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  incoming x;
  make_incoming(&x, "mp3-320", 3, MBID_ALBUM, "mp3", 320, 44100, 0);
  mc_db_album a = album_desc(MBID_ALBUM);
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 3);
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE bitrate_kbps = 320 AND path NOT LIKE '%(2)%'") == 3);
  CHECK(lib_file_exists(lib, "Low/Things (2001)/02 Whore.mp3"));
  CHECK(!lib_file_exists(lib, "Low/Things (2001)/02 Whore (2).mp3"));
  /* The file at the old name is the new one. */
  char *abs = mc_library_abspath(lib, "Low/Things (2001)/02 Whore.mp3");
  CHECK(abs && file_size(abs) == x.in[1].size);
  free(abs);
  mc_library_close(lib);
}

/* Worse or equal releases are skipped; nothing is written. */
static void test_worse_and_equal(void) {
  mc_library *lib = make_library("worse", MBID_ALBUM, "wav", 0, 44100, 16);
  CHECK(lib != NULL);
  if (!lib) return;
  mc_db_album a = album_desc(MBID_ALBUM);
  incoming x;
  mc_dupe_plan p;

  make_incoming(&x, "mp3", 3, MBID_ALBUM, "mp3", 320, 44100, 0);
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 3);
  for (int i = 0; i < 3; i++) CHECK(p.d[i].action == MC_DUPE_SKIP);
  mc_dupe_plan_free(&p);

  /* The library's WAVs are read from disk: 16-bit 44.1 kHz, the same as this FLAC. */
  make_incoming(&x, "flac16", 3, MBID_ALBUM, "flac", 900, 44100, 16);
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 3);
  mc_dupe_plan_free(&p);

  /* A 24-bit one is better. */
  make_incoming(&x, "flac24", 3, MBID_ALBUM, "flac", 2500, 96000, 24);
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 3);
  mc_dupe_plan_free(&p);

  /* Mixed: better on two tracks but worse on one is not better as a whole. */
  make_incoming(&x, "mixed", 3, MBID_ALBUM, "flac", 2500, 96000, 24);
  x.in[2].quality = (mc_audio_quality){"mp3", 320, 44100, 0};
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 3);
  mc_dupe_plan_free(&p);

  /* Better on one track, equal on two: not most of them. */
  make_incoming(&x, "onebetter", 3, MBID_ALBUM, "flac", 900, 44100, 16);
  x.in[0].quality = (mc_audio_quality){"flac", 2500, 96000, 24};
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 3);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* A partial release never replaces a full one, however good; its tracks are skipped. */
static void test_partial(void) {
  mc_library *lib = make_library("partial", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  mc_db_album a = album_desc(MBID_ALBUM);
  incoming x;
  make_incoming(&x, "partial", 2, MBID_ALBUM, "flac", 900, 44100, 16);
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 2, &p) == MC_OK);
  CHECK(p.swaps == 0 && p.skips == 2);
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE format = 'mp3'") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 3);
  mc_library_close(lib);
}

/* Without MusicBrainz ids: album artist, title and the track list find the album. Tracks
 * the library lacks are added to it, the rest skipped. */
static void test_by_name_and_gaps(void) {
  mc_library *lib = make_library("byname", NULL, "flac", 900, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  /* Track 3 has no file on this device (say, it synced from another). */
  CHECK(sqlite3_exec(mc_library_db(lib), "DELETE FROM files WHERE path LIKE '%03 %'", NULL, NULL,
                     NULL) == SQLITE_OK);
  char album_uid[64], track3[64];
  query_text(lib, "SELECT uid FROM albums", album_uid, sizeof album_uid);
  query_text(lib, "SELECT uid FROM tracks WHERE track_no = 3", track3, sizeof track3);

  mc_db_album a = album_desc(NULL);
  a.title = "THINGS WE LOST IN THE FIRE"; /* case doesn't matter */
  a.year = 0;
  incoming x;
  make_incoming(&x, "byname", 3, NULL, "mp3", 320, 44100, 0);
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(!strcmp(p.album_uid, album_uid));
  CHECK(p.d[0].action == MC_DUPE_SKIP && p.d[1].action == MC_DUPE_SKIP);
  CHECK(p.d[2].action == MC_DUPE_ATTACH && !strcmp(p.d[2].track_uid, track3));
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 3);
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE track_uid = '%s' AND format = 'mp3'", track3);
  CHECK(query_int(lib, sql) == 1);
  CHECK(lib_file_exists(lib, "Low/Things (2001)/03 Dinosaur Act.mp3"));

  /* Another album of that name by the same artist, with another track list, is not it. */
  mc_db_track other = {0};
  incoming y;
  make_incoming(&y, "other", 1, NULL, "mp3", 320, 44100, 0);
  other = y.row[0];
  other.title = "Something Else Entirely";
  y.in[0].row = &other;
  CHECK(mc_dupes_plan(lib, &a, y.in, 1, &p) == MC_OK);
  CHECK(p.album_uid[0] == '\0' && p.d[0].action == MC_DUPE_IMPORT);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* A file whose bytes are already in the library is skipped wherever it is. */
static void test_exact_file(void) {
  mc_library *lib = make_library("exact", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  /* The same bytes as the library's track 2, coming in as a loose track without ids. */
  char src[1300], *lib_copy = mc_library_abspath(lib, "Low/Things (2001)/02 Whore.mp3");
  snprintf(src, sizeof src, "%s%cloose-whore.mp3", g_scratch, MC_PATH_SEP);
  mc_remove(src);
  CHECK(lib_copy && mc_copy_file(lib_copy, src, 0) == MC_OK);
  free(lib_copy);
  mc_db_album loose = {0};
  loose.no_album = 1;
  loose.artist = (mc_db_artist){"Someone Else", NULL, NULL};
  mc_db_track row = {0};
  row.title = "Not the Same Title";
  row.format = "mp3";
  mc_dupe_in in = {&row, src, file_size(src), {"mp3", 128, 44100, 0}};
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_SKIP && p.skips == 1);
  mc_dupe_plan_free(&p);

  /* One byte different: imported. */
  write_file(src, "library copy of Whore at 128 kbpz");
  in.size = file_size(src);
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_IMPORT && p.skips == 0);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* A swap that isn't the whole release: the new files are tagged as the library's tracks,
 * whatever their own match said. Here a reissue in the same release group, for the two
 * tracks this device holds (the third has no file anywhere). */
static void test_swap_retag(void) {
  mc_library *lib = make_library("retag", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  CHECK(sqlite3_exec(mc_library_db(lib), "DELETE FROM files WHERE path LIKE '%03 %'", NULL, NULL,
                     NULL) == SQLITE_OK);
  char album_uid[64];
  query_text(lib, "SELECT uid FROM albums", album_uid, sizeof album_uid);
  incoming x;
  make_incoming(&x, "reissue", 2, MBID_REISSUE, "flac", 900, 44100, 16);
  for (int i = 0; i < 2; i++) x.row[i].mb_releasetrackid = k_reissue_rtids[i];
  mc_db_album a = album_desc(MBID_REISSUE);
  a.title = "Things We Lost in the Fire (Deluxe)";
  a.year = 2021;
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 2, &p) == MC_OK);
  CHECK(!strcmp(p.album_uid, album_uid)); /* by release group */
  CHECK(p.swaps == 2 && !p.whole);         /* track 3 isn't paired */
  for (int i = 0; i < 2; i++) {
    const mc_dupe_decision *d = &p.d[i];
    CHECK(d->action == MC_DUPE_SWAP && d->has_tags);
    CHECK_STR(d->tags.title, k_titles[i]);
    CHECK_STR(d->tags.album, "Things We Lost in the Fire");
    CHECK_STR(d->tags.album_artist, "Low");
    CHECK_STR(d->tags.artist, "Low");
    CHECK_STR(d->tags.mb_albumid, MBID_ALBUM); /* the library's release, not the reissue */
    CHECK_STR(d->tags.mb_releasegroupid, MBID_GROUP);
    CHECK_STR(d->tags.mb_releasetrackid, k_rtids[i]);
    CHECK_STR(d->tags.mb_trackid, ""); /* none in the library: the file's own are removed */
    CHECK_STR(d->tags.mb_albumartistid, MBID_ARTIST);
    CHECK(d->tags.year == 2001 && d->tags.track == i + 1 && d->tags.disc == 1);
    CHECK(d->tags.track_total == 3);
  }
  int seq_before = query_int(lib, "SELECT max(seq) FROM changelog");
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);
  /* The library's records are as they were. */
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE mb_albumid = '" MBID_ALBUM "'"
                       " AND title = 'Things We Lost in the Fire' AND year = 2001") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE mb_releasetrackid LIKE '%10_'") == 3);
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE seq > %d AND NOT"
                            " (entity = 'tracks' AND field IN ('file_rev', 'duration_ms'))",
           seq_before);
  CHECK(query_int(lib, sql) == 0);
  mc_library_close(lib);
}

/* The whole release swapped for another release of its group: the match wins, and the
 * library's album and tracks take its metadata, keeping their uids. */
static void test_swap_whole_release(void) {
  mc_library *lib = make_library("whole", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  char album_uid[64], track1[64];
  query_text(lib, "SELECT uid FROM albums", album_uid, sizeof album_uid);
  query_text(lib, "SELECT uid FROM tracks WHERE track_no = 1", track1, sizeof track1);
  incoming x;
  make_incoming(&x, "whole", 3, MBID_REISSUE, "flac", 900, 44100, 16);
  for (int i = 0; i < 3; i++) x.row[i].mb_releasetrackid = k_reissue_rtids[i];
  mc_db_album a = album_desc(MBID_REISSUE);
  a.title = "Things We Lost in the Fire (Deluxe)";
  a.year = 2021;
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(!strcmp(p.album_uid, album_uid));
  CHECK(p.swaps == 3 && p.whole);
  for (int i = 0; i < 3; i++) CHECK(!p.d[i].has_tags);
  CHECK(apply(lib, &a, &x, &p) == MC_OK);
  mc_dupe_plan_free(&p);

  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 1);
  char now[64];
  query_text(lib, "SELECT uid FROM albums WHERE mb_albumid = '" MBID_REISSUE "'"
                  " AND title = 'Things We Lost in the Fire (Deluxe)' AND year = 2021",
             now, sizeof now);
  CHECK(!strcmp(now, album_uid));
  query_text(lib, "SELECT uid FROM tracks WHERE mb_releasetrackid = '" "d0000000-0000-4000-8000-000000000111" "'",
             now, sizeof now);
  CHECK(!strcmp(now, track1));
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE mb_albumid = '" MBID_REISSUE "'") == 3);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE format = 'flac'") == 3);
  /* Logged, so other devices see the new release. */
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'albums'"
                       " AND field = 'mb_albumid' AND value = '" MBID_REISSUE "'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'tracks'"
                       " AND field = 'mb_releasetrackid'") >= 3);

  /* Found by name only, it's never the whole release: the library's records stay. */
  mc_library_close(lib);
  lib = make_library("whole-byname", NULL, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  a = album_desc(NULL);
  make_incoming(&x, "whole-byname", 3, NULL, "flac", 900, 44100, 16);
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(p.swaps == 3 && !p.whole);
  CHECK(p.d[0].has_tags && !strcmp(p.d[0].tags.mb_albumid, ""));
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* A file the library was imported from is known by its source hash, though the library's
 * copy has other bytes now (the import tagged it). */
static void test_source_hash(void) {
  mc_library *lib = make_library("source", MBID_ALBUM, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  mc_db_album other = album_desc(NULL);
  other.title = "Something New";
  incoming x;
  make_incoming(&x, "source", 1, NULL, "mp3", 320, 44100, 0);
  x.row[0].title = "New Song";
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &other, x.in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_IMPORT && strlen(p.d[0].source_sha256) == 64);
  CHECK(apply(lib, &other, &x, &p) == MC_OK);
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE source_sha256 = '%s'",
           p.d[0].source_sha256);
  CHECK(query_int(lib, sql) == 1);
  mc_dupe_plan_free(&p);

  /* As if the import had tagged the copy. */
  char rel[256];
  query_text(lib, "SELECT path FROM files WHERE source_sha256 IS NOT NULL", rel, sizeof rel);
  char *abs = mc_library_abspath(lib, rel);
  CHECK(abs != NULL);
  if (abs) write_file(abs, "the copy, tagged since");
  free(abs);

  /* The same source again, as someone else's track with no album: already in the library. */
  mc_db_album loose = {0};
  loose.no_album = 1;
  loose.artist = (mc_db_artist){"Someone Else", NULL, NULL};
  CHECK(mc_dupes_plan(lib, &loose, x.in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_SKIP && p.skips == 1 && !p.d[0].source_sha256[0]);
  mc_dupe_plan_free(&p);

  /* A row with a recorded sha256 only, of another size, is found by it too. */
  CHECK(sqlite3_exec(mc_library_db(lib), "UPDATE files SET sha256 = source_sha256,"
                     " source_sha256 = NULL, size_bytes = 1", NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_dupes_plan(lib, &loose, x.in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_SKIP);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

/* A track with no album already among the artist's tracks with no album. Each is judged on
 * its own: the artist's other loose tracks don't count. */
static void test_no_album(void) {
  char dir[1200];
  snprintf(dir, sizeof dir, "%s%cnoalbum", g_scratch, MC_PATH_SEP);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  mc_db_album loose = {0};
  loose.no_album = 1;
  loose.artist = (mc_db_artist){"Low", NULL, MBID_ARTIST};
  mc_db_track t = {0};
  t.path = "Low/Canada.mp3";
  t.format = "mp3";
  t.bitrate_kbps = 128;
  t.title = "Canada";
  t.mb_trackid = "d0000000-0000-4000-8000-000000000201";
  mc_db_track other = t;
  other.path = "Low/Monkey.mp3";
  other.title = "Monkey";
  other.mb_trackid = "d0000000-0000-4000-8000-000000000203";
  char *abs = mc_library_abspath(lib, "Low");
  mc_mkdirs(abs);
  free(abs);
  abs = mc_library_abspath(lib, t.path);
  write_file(abs, "canada, 128");
  free(abs);
  abs = mc_library_abspath(lib, other.path);
  write_file(abs, "monkey, 128");
  free(abs);
  CHECK(mc_import_db_write(lib, &loose, &t, 1) == MC_OK);
  CHECK(mc_import_db_write(lib, &loose, &other, 1) == MC_OK);

  char src[1300];
  snprintf(src, sizeof src, "%s%ccanada.flac", g_scratch, MC_PATH_SEP);
  write_file(src, "canada, lossless");
  mc_db_track row = t;
  row.path = NULL;
  row.format = "flac";
  mc_dupe_in in = {&row, src, file_size(src), {"flac", 900, 44100, 16}};
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_SWAP && p.swaps == 1);
  CHECK(!p.whole && p.d[0].has_tags);
  CHECK_STR(p.d[0].tags.album, ""); /* none, and so removed */
  CHECK_STR(p.d[0].tags.title, "Canada");
  CHECK_STR(p.d[0].tags.mb_albumid, "");
  CHECK(p.d[0].tags.track == 0);
  CHECK(p.album_uid[0] == '\0' && !p.album_dir);
  mc_dupe_plan_free(&p);

  in.quality = (mc_audio_quality){"mp3", 96, 44100, 0};
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_SKIP);
  mc_dupe_plan_free(&p);

  /* Another recording by the artist is new, and so is one by someone else. */
  row.mb_trackid = "d0000000-0000-4000-8000-000000000202";
  row.title = "Over the Ocean";
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_IMPORT);
  mc_dupe_plan_free(&p);
  row.mb_trackid = t.mb_trackid;
  row.title = "Canada";
  row.artist = (mc_db_artist){"Someone Else", NULL, NULL};
  CHECK(mc_dupes_plan(lib, &loose, &in, 1, &p) == MC_OK);
  CHECK(p.d[0].action == MC_DUPE_IMPORT);
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

#ifndef _WIN32
/* mc_trash() into a trash under the scratch folder: the file moves to files/, with an info
 * file saying where it came from, and a second file of the same name gets another name. */
static void test_posix_trash(void) {
  char xdg[1200], a[1300], b[1300], p[1400];
  snprintf(xdg, sizeof xdg, "%s/xdg", g_scratch);
  setenv("XDG_DATA_HOME", xdg, 1);
  snprintf(a, sizeof a, "%s/old song.mp3", g_scratch);
  snprintf(b, sizeof b, "%s/sub/old song.mp3", g_scratch);
  snprintf(p, sizeof p, "%s/sub", g_scratch);
  mc_mkdirs(p);
  write_file(a, "a");
  write_file(b, "b");
  CHECK(mc_trash(a) == MC_OK && !mc_file_exists(a));
  CHECK(mc_trash(b) == MC_OK && !mc_file_exists(b));
  snprintf(p, sizeof p, "%s/Trash/files/old song.mp3", xdg);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/Trash/files/old song.2.mp3", xdg);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/Trash/info/old song.mp3.trashinfo", xdg);
  FILE *f = mc_fopen(p, "rb");
  char info[2048] = "";
  if (f) {
    info[fread(info, 1, sizeof info - 1, f)] = '\0';
    fclose(f);
  }
  CHECK(strncmp(info, "[Trash Info]\nPath=/", 19) == 0);
  CHECK(strstr(info, "/old%20song.mp3\n") != NULL && strstr(info, "DeletionDate=") != NULL);
  CHECK(mc_trash(a) != MC_OK); /* gone already */
}

static void read_text(const char *path, char *out, size_t size) {
  out[0] = '\0';
  FILE *f = mc_fopen(path, "rb");
  if (!f) return;
  out[fread(out, 1, size - 1, f)] = '\0';
  fclose(f);
}

/* A file on another file system than the home trash goes to its top directory's trash:
 * there's one file system here, so the scratch folder stands in for the top directory. */
static void test_topdir_trash(void) {
  char top[1200], a[1300], b[1300], p[1400], info[2048];
  unsigned uid = (unsigned)getuid();
  snprintf(top, sizeof top, "%s/top", g_scratch);
  snprintf(p, sizeof p, "%s/music/x y", top);
  CHECK(mc_mkdirs(p) == MC_OK);
  snprintf(a, sizeof a, "%s/music/x y/song.mp3", top);
  snprintf(b, sizeof b, "%s/music/x y/other.mp3", top);
  write_file(a, "a");
  write_file(b, "b");

  /* The mount point is above the scratch folder, and on the same file system. */
  char *found = mc_trash_topdir(a);
  CHECK(found != NULL);
  if (found) {
    struct stat fs, ts;
    CHECK(stat(found, &ts) == 0 && stat(a, &fs) == 0 && ts.st_dev == fs.st_dev);
    char *real = realpath(g_scratch, NULL);
    CHECK(real && (strcmp(found, "/") == 0 ||
                   (strncmp(real, found, strlen(found)) == 0 && real[strlen(found)] == '/')));
    free(real);
    free(found);
  }

  /* No .Trash: .Trash-$uid is made, private, with the path relative to the top. */
  CHECK(mc_trash_in_topdir(a, top) == MC_OK && !mc_file_exists(a));
  snprintf(p, sizeof p, "%s/.Trash-%u", top, uid);
  struct stat st;
  CHECK(lstat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
  snprintf(p, sizeof p, "%s/.Trash-%u/files/song.mp3", top, uid);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/.Trash-%u/info/song.mp3.trashinfo", top, uid);
  read_text(p, info, sizeof info);
  CHECK(strncmp(info, "[Trash Info]\nPath=music/x%20y/song.mp3\nDeletionDate=", 52) == 0);

  /* A .Trash that isn't sticky isn't used. */
  snprintf(p, sizeof p, "%s/.Trash", top);
  CHECK(mkdir(p, 0777) == 0 && chmod(p, 0777) == 0);
  write_file(a, "a");
  CHECK(mc_trash_in_topdir(a, top) == MC_OK);
  snprintf(p, sizeof p, "%s/.Trash-%u/files/song.2.mp3", top, uid);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/.Trash/%u", top, uid);
  CHECK(!mc_file_exists(p));

  /* A sticky one is: .Trash/$uid is made in it. */
  snprintf(p, sizeof p, "%s/.Trash", top);
  CHECK(chmod(p, 01777) == 0);
  CHECK(mc_trash_in_topdir(b, top) == MC_OK && !mc_file_exists(b));
  snprintf(p, sizeof p, "%s/.Trash/%u/files/other.mp3", top, uid);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/.Trash/%u/info/other.mp3.trashinfo", top, uid);
  read_text(p, info, sizeof info);
  CHECK(strstr(info, "\nPath=music/x%20y/other.mp3\n") != NULL);

  /* A .Trash that's a link to a sticky directory isn't. */
  char real_trash[1300];
  snprintf(real_trash, sizeof real_trash, "%s/elsewhere", g_scratch);
  CHECK(mkdir(real_trash, 01777) == 0 && chmod(real_trash, 01777) == 0);
  snprintf(p, sizeof p, "%s/.Trash", top);
  CHECK(mc_remove_tree(p) == MC_OK && symlink(real_trash, p) == 0);
  write_file(b, "b");
  CHECK(mc_trash_in_topdir(b, top) == MC_OK);
  snprintf(p, sizeof p, "%s/.Trash-%u/files/other.mp3", top, uid);
  CHECK(mc_file_exists(p));
  snprintf(p, sizeof p, "%s/%u", real_trash, uid);
  CHECK(!mc_file_exists(p));

  /* A file not under the top directory isn't trashed there. */
  snprintf(p, sizeof p, "%s/music", top);
  write_file(b, "b");
  CHECK(mc_trash_in_topdir(b, p) == MC_OK); /* under it: fine */
  snprintf(a, sizeof a, "%s/outside.mp3", g_scratch);
  write_file(a, "a");
  CHECK(mc_trash_in_topdir(a, p) == MC_ERR_INVALID_ARG && mc_file_exists(a));
}
#endif

/* A release credited to several is found by its credit as shown, and a file swapped in is
 * tagged with the library's credit: ARTIST as credited, ARTISTS one per artist, the ids
 * only where every artist has one. */
static void test_credited(void) {
  mc_library *lib = make_library("credited", NULL, "mp3", 128, 0, 0);
  CHECK(lib != NULL);
  if (!lib) return;
  char album_uid[64], track1[64];
  query_text(lib, "SELECT uid FROM albums", album_uid, sizeof album_uid);
  query_text(lib, "SELECT uid FROM tracks WHERE track_no = 1", track1, sizeof track1);
  const mc_db_credit_part duo[] = {{{"Low", NULL, MBID_ARTIST}, " & "}, {{"Mimi", NULL, NULL}, NULL}};
  const mc_db_credit_part feat[] = {{{"Low", NULL, MBID_ARTIST}, " feat. "}, {{"Mimi", NULL, NULL}, NULL}};
  mc_db_artist whole = {"Low & Mimi", NULL, NULL};
  CHECK(mc_import_db_set_credit(lib, 1, album_uid, &whole, duo, 2) == MC_OK);
  whole.name = "Low feat. Mimi";
  CHECK(mc_import_db_set_credit(lib, 0, track1, &whole, feat, 2) == MC_OK);

  incoming x;
  make_incoming(&x, "credited", 3, NULL, "flac", 900, 44100, 16);
  mc_db_album a = album_desc(NULL);
  a.artist = (mc_db_artist){"Low & Mimi", NULL, NULL};
  mc_dupe_plan p;
  CHECK(mc_dupes_plan(lib, &a, x.in, 3, &p) == MC_OK);
  CHECK(!strcmp(p.album_uid, album_uid));
  CHECK(p.swaps == 3);
  mc_tags *t = &p.d[0].tags;
  CHECK(p.d[0].action == MC_DUPE_SWAP && p.d[0].has_tags);
  CHECK_STR(t->artist, "Low feat. Mimi");
  CHECK_STR(t->artists, "Low\nMimi");
  CHECK_STR(t->mb_artistid, MBID_ARTIST);
  CHECK(t->mb_artistids == NULL); /* Mimi has none */
  CHECK_STR(t->album_artist, "Low & Mimi");
  CHECK_STR(t->album_artists, "Low\nMimi");
  /* The others were imported as Low's, and stay so: a track keeps its own credit. */
  CHECK_STR(p.d[1].tags.artist, "Low");
  CHECK_STR(p.d[1].tags.artists, "Low");
  mc_dupe_plan_free(&p);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  snprintf(g_scratch, sizeof g_scratch, "%s%ctest_dupes.d", argc > 1 ? argv[1] : ".",
           MC_PATH_SEP);
  remove_tree(g_scratch); /* the last run's */
  snprintf(g_trash, sizeof g_trash, "%s%ctrash", g_scratch, MC_PATH_SEP);
  CHECK(mc_mkdirs(g_trash) == MC_OK);
  mc_dupes_trash = fake_trash;

  test_quality();
  test_swap();
  test_swap_same_name();
  test_worse_and_equal();
  test_partial();
  test_by_name_and_gaps();
  test_exact_file();
  test_swap_retag();
  test_swap_whole_release();
  test_source_hash();
  test_no_album();
  test_credited();
#ifndef _WIN32
  test_posix_trash();
  test_topdir_trash();
#endif
  return TEST_RESULT();
}
