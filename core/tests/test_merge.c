/* Duplicate merging: the same album imported on two devices becomes one, under the smaller
 * uid, on both, and on a third device that hears of the two in another order. Rows travel
 * between libraries the way a session moves them (reader, then applier), without the
 * network. An older peer's singles albums (kind = 'singles') don't merge, nor break
 * anything. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/rows.h"
#include "tagger/import_db.h"
#include "test.h"
#include "util/platform.h"
#include "util/uid.h"

#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal",
                               MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void query_text(sqlite3 *db, const char *sql, char *out, size_t cap) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "<null>");
  }
  sqlite3_finalize(stmt);
}

/* Every row of the synced tables, in uid order, as text: equal dumps are equal libraries. */
static char *dump(mc_library *lib) {
  static const char *const tables[] = {"artists", "albums", "tracks", "track_plays",
                                       "playlists", "playlist_entries", "album_groups",
                                       "album_group_entries", "recents"};
  size_t cap = 1 << 16, len = 0;
  char *out = malloc(cap);
  if (!out) return NULL;
  out[0] = '\0';
  for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++) {
    char sql[128];
    snprintf(sql, sizeof sql, "SELECT * FROM %s ORDER BY uid", tables[t]);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) continue;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      for (int c = 0; c < sqlite3_column_count(stmt); c++) {
        const char *v = (const char *)sqlite3_column_text(stmt, c);
        size_t n = strlen(v ? v : "-") + 2;
        if (len + n + 64 > cap) {
          char *more = realloc(out, cap *= 2);
          if (!more) break;
          out = more;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s%c", v ? v : "-", c ? '|' : ':');
      }
      len += (size_t)snprintf(out + len, cap - len, "\n");
    }
    sqlite3_finalize(stmt);
  }
  return out;
}

/* --- Devices --- */

static void exec_ok(mc_library *lib, const char *sql) {
  int rc = sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "%s: %s\n", sql, mc_library_errmsg(lib));
  CHECK(rc == SQLITE_OK);
}

/* What one import of the album made on one device. */
typedef struct import {
  char artist[MC_UID_STRLEN], album[MC_UID_STRLEN], single_album[MC_UID_STRLEN];
  char tracks[3][MC_UID_STRLEN], single[MC_UID_STRLEN];
  int ntracks;
} import;

/* Imports "Things" (release REL, artist ART) as the importer would: ntracks tracks, each
 * with release track id RT<n> and a file, and the artist's single (release track RT9) as the
 * one track of its own release, SREL. */
static void import_album(mc_library *lib, const char *label, int ntracks, import *im) {
  char sql[1024];
  im->ntracks = ntracks;
  mc_uid_new(im->artist);
  mc_uid_new(im->album);
  mc_uid_new(im->single_album);
  mc_uid_new(im->single);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name, mb_artistid) VALUES ('%s', 'Low', 'ART');"
           "INSERT INTO albums (uid, title, artist_uid, year, mb_albumid)"
           " VALUES ('%s', 'Things', '%s', 1994, 'REL');"
           "INSERT INTO albums (uid, title, artist_uid, year, mb_albumid)"
           " VALUES ('%s', 'Single', '%s', 1996, 'SREL');"
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, mb_releasetrackid)"
           " VALUES ('%s', '%s', '%s', 'Single', 'RT9');",
           im->artist, im->album, im->artist, im->single_album, im->artist, im->single, im->single_album,
           im->artist);
  exec_ok(lib, sql);
  for (int i = 0; i < ntracks; i++) {
    char f[MC_UID_STRLEN];
    mc_uid_new(im->tracks[i]);
    mc_uid_new(f);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, mb_releasetrackid)"
             " VALUES ('%s', '%s', '%s', 'Song %d from %s', %d, 'RT%d');"
             "INSERT INTO files (uid, track_uid, path, format)"
             " VALUES ('%s', '%s', 'Low/Things/%02d.flac', 'flac');",
             im->tracks[i], im->album, im->artist, i + 1, label, i + 1, i + 1, f, im->tracks[i],
             i + 1);
    exec_ok(lib, sql);
  }
  CHECK(mc_library_commit(lib) == MC_OK);
}

/* Sends from's rows that to lacks, as a session would, in batches of batch rows. */
static void send(mc_library *from, mc_library *to, int batch) {
  mc_origin_hlc *vv;
  int nvv;
  CHECK(mc_changelog_version_vector(mc_library_db(to), &vv, &nvv) == MC_OK);
  mc_rows_reader *rd = NULL;
  CHECK(mc_rows_reader_open(mc_library_db(from), vv, nvv, 0, &rd) == MC_OK);
  free(vv);
  if (!rd) return;
  /* Rows point into the reader until its next call, so encode each batch as ROWS. */
  mc_rows_target tg;
  mc_rows_target_library(to, &tg);
  mc_rows_applier *a = NULL;
  CHECK(mc_rows_applier_new(&tg, &a) == MC_OK);
  static unsigned char buf[1 << 16];
  mc_rows_writer w;
  mc_row r;
  int got = 1;
  while (got && a) {
    mc_rows_writer_init(&w, buf, sizeof buf, MC_ROWS_LIBRARY);
    while ((int)w.count < batch && mc_rows_reader_next(rd, &r, &got) == MC_OK && got)
      CHECK(mc_rows_writer_add(&w, &r) == 1);
    size_t len = mc_rows_writer_finish(&w);
    int db, n;
    mc_row *rows = NULL;
    CHECK(mc_rows_decode(buf, len, &db, &rows, &n) == MC_OK);
    if (n) CHECK(mc_rows_apply(a, rows, n, NULL) == MC_OK);
    free(rows);
  }
  mc_rows_applier_free(a);
  mc_rows_reader_close(rd);
}

static int same_digest(mc_library *x, mc_library *y) {
  char dx[MC_DIGEST_STRLEN], dy[MC_DIGEST_STRLEN];
  CHECK(mc_rows_vv_digest(mc_library_db(x), dx) == MC_OK);
  CHECK(mc_rows_vv_digest(mc_library_db(y), dy) == MC_OK);
  return strcmp(dx, dy) == 0;
}

static const char *smaller(const char *x, const char *y) {
  return strcmp(x, y) < 0 ? x : y;
}

/* Merged state, as every device must see it. */
static void check_merged(mc_library *lib, const import *a, const import *b) {
  sqlite3 *db = mc_library_db(lib);
  char sql[512], text[256];
  const char *album = smaller(a->album, b->album), *artist = smaller(a->artist, b->artist);
  const char *single_album = smaller(a->single_album, b->single_album);

  CHECK(query_int(db, "SELECT count(*) FROM artists") == 1);
  query_text(db, "SELECT uid FROM artists", text, sizeof text);
  CHECK_TEXT(text, artist);
  CHECK(query_int(db, "SELECT count(*) FROM albums") == 2);
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM albums WHERE uid IN ('%s', '%s') AND artist_uid = '%s'", album,
           single_album, artist);
  CHECK(query_int(db, sql) == 2);

  /* RT1 and RT2 are one track each; B's RT3 joins the album; the single is one. */
  CHECK(query_int(db, "SELECT count(*) FROM tracks") == 4);
  for (int i = 0; i < 2; i++) {
    snprintf(sql, sizeof sql, "SELECT uid FROM tracks WHERE mb_releasetrackid = 'RT%d'", i + 1);
    query_text(db, sql, text, sizeof text);
    CHECK_TEXT(text, smaller(a->tracks[i], b->tracks[i]));
  }
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM tracks WHERE album_uid = '%s' AND artist_uid = '%s'", album,
           artist);
  CHECK(query_int(db, sql) == 3);
  snprintf(sql, sizeof sql, "SELECT album_uid FROM tracks WHERE uid = '%s'",
           smaller(a->single, b->single));
  query_text(db, sql, text, sizeof text);
  CHECK_TEXT(text, single_album);

  /* References moved: the playlist entry, the album group entry, the plays, the recents. */
  snprintf(sql, sizeof sql, "SELECT track_uid FROM playlist_entries");
  query_text(db, sql, text, sizeof text);
  CHECK_TEXT(text, smaller(a->tracks[0], b->tracks[0]));
  query_text(db, "SELECT album_uid FROM album_group_entries", text, sizeof text);
  CHECK_TEXT(text, album);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_plays WHERE track_uid IN ('%s', '%s')",
           smaller(a->tracks[0], b->tracks[0]), smaller(a->tracks[1], b->tracks[1]));
  CHECK(query_int(db, sql) == 2);
  CHECK(query_int(db, "SELECT count(*) FROM recents") == 1);
  query_text(db, "SELECT uid FROM recents", text, sizeof text);
  CHECK_TEXT(text, album);
  CHECK(query_int(db, "SELECT count(*) FROM aliases") == 6);
  CHECK(query_int(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

/* The credit tables as text, owners and artists by name (uids differ between runs). */
static void dump_credits(mc_library *lib, char *out, size_t cap) {
  query_text(mc_library_db(lib),
             "SELECT group_concat(row, ';') FROM (SELECT o.title || ':' || c.position || ':' || ar.name"
             " || ':' || c.joinphrase AS row FROM track_artists c JOIN tracks o ON o.uid = c.track_uid"
             " JOIN artists ar ON ar.uid = c.artist_uid"
             " UNION ALL SELECT o.title || ':' || c.position || ':' || ar.name || ':' || c.joinphrase"
             " FROM album_artists c JOIN albums o ON o.uid = c.album_uid"
             " JOIN artists ar ON ar.uid = c.artist_uid ORDER BY 1)",
             out, cap);
}

/* What one device's import of a credited album made. */
typedef struct credited {
  char album[MC_UID_STRLEN], track[MC_UID_STRLEN], mimi[MC_UID_STRLEN];
} credited;

/* "Duets" by Low & Mimi (release DREL), with two tracks: D1, Low feat. Mimi, and D2, the
 * album's. Each device makes its own uids, so two such imports are duplicates. */
static void import_credited(mc_library *lib, const char *dir, credited *out) {
  const mc_db_credit_part duo[] = {{{"Low", NULL, "ART"}, " & "}, {{"Mimi", NULL, "MIMI"}, NULL}};
  const mc_db_credit_part feat[] = {{{"Low", NULL, "ART"}, " feat. "}, {{"Mimi", NULL, "MIMI"}, NULL}};
  mc_db_album al = {0};
  al.title = "Duets";
  al.artist = (mc_db_artist){"Low & Mimi", NULL, NULL};
  al.credit = duo;
  al.credit_count = 2;
  al.mb_albumid = "DREL";
  char p1[256], p2[256];
  snprintf(p1, sizeof p1, "%s/01.flac", dir);
  snprintf(p2, sizeof p2, "%s/02.flac", dir);
  mc_db_track t[2] = {{0}, {0}};
  t[0].path = p1;
  t[0].format = "flac";
  t[0].title = "D1";
  t[0].track_no = 1;
  t[0].artist = (mc_db_artist){"Low feat. Mimi", NULL, NULL};
  t[0].credit = feat;
  t[0].credit_count = 2;
  t[0].mb_releasetrackid = "DRT1";
  t[1].path = p2;
  t[1].format = "flac";
  t[1].title = "D2";
  t[1].track_no = 2;
  t[1].mb_releasetrackid = "DRT2";
  CHECK(mc_import_db_write(lib, &al, t, 2) == MC_OK);
  sqlite3 *db = mc_library_db(lib);
  query_text(db, "SELECT uid FROM albums WHERE mb_albumid = 'DREL'", out->album, sizeof out->album);
  query_text(db, "SELECT uid FROM tracks WHERE mb_releasetrackid = 'DRT1'", out->track, sizeof out->track);
  query_text(db, "SELECT uid FROM artists WHERE mb_artistid = 'MIMI'", out->mimi, sizeof out->mimi);
}

/* Credits through duplicate merging: the artists' rows follow the merged artist, a merged-away
 * album's or track's credit rows go and the survivor keeps its own, and a late row for a
 * loser's credit doesn't come back. Both devices end up the same. */
static void test_credits(const char *scratch) {
  char pa[1024], pb[1024];
  snprintf(pa, sizeof pa, "%s/test_merge_credits_a.d", scratch);
  snprintf(pb, sizeof pb, "%s/test_merge_credits_b.d", scratch);
  remove_db(pa);
  remove_db(pb);
  mc_library *a = NULL, *b = NULL;
  CHECK(mc_library_open(pa, &a) == MC_OK);
  CHECK(mc_library_open(pb, &b) == MC_OK);
  if (!a || !b) return;
  credited ca, cb;
  import_credited(a, "Low & Mimi/Duets", &ca);
  import_credited(b, "Low & Mimi/Duets", &cb);
  send(a, b, 5);
  send(b, a, 5);
  CHECK(same_digest(a, b));

  const char *album = smaller(ca.album, cb.album), *track = smaller(ca.track, cb.track);
  const char *mimi = smaller(ca.mimi, cb.mimi);
  const char *lose_track = track == ca.track ? cb.track : ca.track;
  for (int i = 0; i < 2; i++) {
    sqlite3 *db = mc_library_db(i ? b : a);
    char sql[512], text[512];
    CHECK(query_int(db, "SELECT count(*) FROM artists") == 2);
    CHECK(query_int(db, "SELECT count(*) FROM albums") == 1 && query_int(db, "SELECT count(*) FROM tracks") == 2);
    /* Each owner has its own rows only, under uids derived from it, naming the survivors. */
    CHECK(query_int(db, "SELECT count(*) FROM album_artists") == 2);
    CHECK(query_int(db, "SELECT count(*) FROM track_artists") == 4);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM album_artists WHERE album_uid = '%s'", album);
    CHECK(query_int(db, sql) == 2);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM track_artists WHERE track_uid = '%s'", track);
    CHECK(query_int(db, sql) == 2);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM track_artists WHERE track_uid = '%s'", lose_track);
    CHECK(query_int(db, sql) == 0);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM track_artists WHERE artist_uid = '%s' AND position = 1", mimi);
    CHECK(query_int(db, sql) == 2);
    CHECK(query_int(db, "SELECT count(*) FROM track_artists WHERE artist_uid NOT IN (SELECT uid FROM artists)") == 0);
    snprintf(sql, sizeof sql, "SELECT artist_credit FROM tracks WHERE uid = '%s'", track);
    query_text(db, sql, text, sizeof text);
    CHECK_TEXT(text, "Low feat. Mimi");
    CHECK(query_int(db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
    /* Mimi's page lists both tracks and the album. */
    mc_track_list *tracks = NULL;
    CHECK(mc_library_list_artist_tracks(i ? b : a, mimi, &tracks) == MC_OK && tracks && tracks->count == 2);
    mc_track_list_free(tracks);
  }
  char da[1024], db_[1024];
  dump_credits(a, da, sizeof da);
  dump_credits(b, db_, sizeof db_);
  CHECK_TEXT(da, db_);
  CHECK_TEXT(da, "D1:0:Low: feat. ;D1:1:Mimi:;D2:0:Low: & ;D2:1:Mimi:;Duets:0:Low: & ;Duets:1:Mimi:");

  /* Edits sync like any other: A drops Mimi from D1, B puts her back with another join. Row 1
   * is deleted and then live again under the same uid, its join the newest. */
  mc_db_artist low = {"Low", NULL, "ART"};
  CHECK(mc_import_db_set_credit(a, 0, track, &low, NULL, 0) == MC_OK);
  send(a, b, 5);
  dump_credits(b, db_, sizeof db_);
  CHECK_TEXT(db_, "D1:0:Low:;D2:0:Low: & ;D2:1:Mimi:;Duets:0:Low: & ;Duets:1:Mimi:");
  char q[256], text[64];
  snprintf(q, sizeof q, "SELECT coalesce(artist_credit, '<null>') FROM tracks WHERE uid = '%s'", track);
  query_text(mc_library_db(b), q, text, sizeof text);
  CHECK_TEXT(text, "<null>");
  const mc_db_credit_part with[] = {{{"Low", NULL, "ART"}, " with "}, {{"Mimi", NULL, "MIMI"}, NULL}};
  mc_db_artist whole = {"Low with Mimi", NULL, NULL};
  CHECK(mc_import_db_set_credit(b, 0, track, &whole, with, 2) == MC_OK);
  send(b, a, 5);
  dump_credits(a, da, sizeof da);
  CHECK_TEXT(da, "D1:0:Low: with ;D1:1:Mimi:;D2:0:Low: & ;D2:1:Mimi:;Duets:0:Low: & ;Duets:1:Mimi:");
  query_text(mc_library_db(a), q, text, sizeof text);
  CHECK_TEXT(text, "Low with Mimi");
  CHECK(same_digest(a, b));

  /* A late row for the losing track's credit, from a device that hasn't merged: it names an
   * owner that's gone, and goes. */
  char row_uid[MC_UID_STRLEN], origin[MC_UID_STRLEN];
  CHECK(mc_uid_derive(lose_track, 2, row_uid) == MC_OK);
  mc_uid_new(origin);
  static const char *const fields[] = {"track_uid", "artist_uid", "position", "joinphrase"};
  mc_row rows[4];
  memset(rows, 0, sizeof rows);
  const char *values[] = {lose_track, mimi, NULL, ""};
  for (int i = 0; i < 4; i++) {
    rows[i].hlc = a->hlc.last + 100 + (uint64_t)i;
    memcpy(rows[i].origin, origin, MC_UID_STRLEN);
    snprintf(rows[i].entity, sizeof rows[i].entity, "track_artists");
    memcpy(rows[i].row_uid, row_uid, MC_UID_STRLEN);
    snprintf(rows[i].field, sizeof rows[i].field, "%s", fields[i]);
    if (values[i]) {
      rows[i].tag = MC_VAL_TEXT;
      rows[i].data = (const unsigned char *)values[i];
      rows[i].len = (uint32_t)strlen(values[i]);
    } else {
      rows[i].tag = MC_VAL_INT;
      rows[i].i = 2;
    }
  }
  mc_rows_target tg;
  mc_rows_target_library(a, &tg);
  mc_rows_applier *ap = NULL;
  CHECK(mc_rows_applier_new(&tg, &ap) == MC_OK);
  if (ap) CHECK(mc_rows_apply(ap, rows, 4, NULL) == MC_OK);
  mc_rows_applier_free(ap);
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_artists WHERE uid = '%s'", row_uid);
  CHECK(query_int(mc_library_db(a), sql) == 0);
  CHECK(query_int(mc_library_db(a), "SELECT count(*) FROM pragma_foreign_key_check") == 0);

  /* A newer peer's rows for a table and a column this version doesn't have: logged, passed
   * on, never applied, and no error. */
  mc_row newer[2];
  memset(newer, 0, sizeof newer);
  for (int i = 0; i < 2; i++) {
    newer[i].hlc = a->hlc.last + 200 + (uint64_t)i;
    memcpy(newer[i].origin, origin, MC_UID_STRLEN);
    newer[i].tag = MC_VAL_TEXT;
    newer[i].data = (const unsigned char *)"x";
    newer[i].len = 1;
  }
  snprintf(newer[0].entity, sizeof newer[0].entity, "track_moods");
  mc_uid_new(newer[0].row_uid);
  snprintf(newer[0].field, sizeof newer[0].field, "mood");
  snprintf(newer[1].entity, sizeof newer[1].entity, "tracks");
  memcpy(newer[1].row_uid, track, MC_UID_STRLEN);
  snprintf(newer[1].field, sizeof newer[1].field, "mood");
  ap = NULL;
  CHECK(mc_rows_applier_new(&tg, &ap) == MC_OK);
  int logged = 0;
  if (ap) CHECK(mc_rows_apply(ap, newer, 2, &logged) == MC_OK && logged == 2);
  mc_rows_applier_free(ap);
  send(a, b, 5);
  CHECK(query_int(mc_library_db(b), "SELECT count(*) FROM changelog WHERE field = 'mood'") == 2);
  CHECK(same_digest(a, b));
  mc_library_close(a);
  mc_library_close(b);
  if (!test_failures) {
    remove_db(pa);
    remove_db(pb);
  }
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char pa[1024], pb[1024], pc[1024];
  snprintf(pa, sizeof pa, "%s/test_merge_a.d", scratch);
  snprintf(pb, sizeof pb, "%s/test_merge_b.d", scratch);
  snprintf(pc, sizeof pc, "%s/test_merge_c.d", scratch);
  remove_db(pa);
  remove_db(pb);
  remove_db(pc);
  mc_library *a = NULL, *b = NULL, *c = NULL;
  CHECK(mc_library_open(pa, &a) == MC_OK);
  CHECK(mc_library_open(pb, &b) == MC_OK);
  CHECK(mc_library_open(pc, &c) == MC_OK);
  if (!a || !b || !c) return TEST_RESULT();

  /* A and B import the same album, each on its own: B's copy has a bonus track. A puts
   * track 1 in a playlist and plays it; B plays track 2, puts the album in a group and
   * opens it. */
  import ia, ib;
  import_album(a, "A", 2, &ia);
  import_album(b, "B", 3, &ib);
  char sql[1024], pl[MC_UID_STRLEN], pe[MC_UID_STRLEN], g[MC_UID_STRLEN], ge[MC_UID_STRLEN];
  mc_uid_new(pl);
  mc_uid_new(pe);
  mc_uid_new(g);
  mc_uid_new(ge);
  CHECK(mc_library_begin_write(a) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO playlists (uid, name) VALUES ('%s', 'Mix');"
           "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
           " VALUES ('%s', '%s', '%s', 'a0');",
           pl, pe, pl, ia.tracks[0]);
  exec_ok(a, sql);
  CHECK(mc_library_commit(a) == MC_OK);
  CHECK(mc_library_record_play(a, ia.tracks[0]) == MC_OK);
  CHECK(mc_library_begin_write(b) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO album_groups (uid, name) VALUES ('%s', 'Winter');"
           "INSERT INTO album_group_entries (uid, group_uid, album_uid, position)"
           " VALUES ('%s', '%s', '%s', 'a0');",
           g, ge, g, ib.album);
  exec_ok(b, sql);
  CHECK(mc_library_commit(b) == MC_OK);
  CHECK(mc_library_record_play(b, ib.tracks[1]) == MC_OK);
  CHECK(mc_library_record_opened(b, MC_RECENT_ALBUM, ib.album) == MC_OK);
  CHECK(mc_library_record_opened(a, MC_RECENT_ALBUM, ia.album) == MC_OK);

  /* A session: A's rows to B, then B's to A, in small batches. */
  send(a, b, 7);
  send(b, a, 7);
  CHECK(same_digest(a, b));
  check_merged(a, &ia, &ib);
  check_merged(b, &ia, &ib);

  /* Each device's files stay, on the surviving tracks. */
  for (int i = 0; i < 2; i++) {
    mc_library *lib = i ? b : a;
    const import *im = i ? &ib : &ia;
    CHECK(query_int(mc_library_db(lib), "SELECT count(*) FROM files") == im->ntracks);
    CHECK(query_int(mc_library_db(lib), "SELECT count(*) FROM files f"
                                        " JOIN tracks t ON t.uid = f.track_uid") == im->ntracks);
  }

  /* C hears of them in the other order, B first, one row at a time: the same library. */
  send(b, c, 1);
  send(a, c, 1000);
  CHECK(same_digest(a, c));
  check_merged(c, &ia, &ib);
  char *da = dump(a), *db = dump(b), *dc = dump(c);
  CHECK(da && db && dc && !strcmp(da, db) && !strcmp(da, dc));
  free(da);
  free(db);
  free(dc);

  /* Late rows naming merged-away uids, from a device that hasn't merged: an edit to the
   * losing album's title, newer than anything; a playlist entry for the losing track; and a
   * new MusicBrainz id on the loser, which doesn't split the album. They go to the
   * survivors. */
  const char *lose_album = strcmp(ia.album, ib.album) < 0 ? ib.album : ia.album;
  const char *lose_track = strcmp(ia.tracks[1], ib.tracks[1]) < 0 ? ib.tracks[1] : ia.tracks[1];
  char late[MC_UID_STRLEN], e2[MC_UID_STRLEN];
  mc_uid_new(late);
  mc_uid_new(e2);
  uint64_t hlc = a->hlc.last + 1000;
  static const char *const fields[] = {"title", "mb_albumid"};
  static const char *const values[] = {"Things (Remastered)", "REL2"};
  mc_row rows[5];
  memset(rows, 0, sizeof rows);
  for (int i = 0; i < 5; i++) {
    rows[i].hlc = hlc + (uint64_t)i;
    memcpy(rows[i].origin, late, MC_UID_STRLEN);
    rows[i].tag = MC_VAL_TEXT;
  }
  for (int i = 0; i < 2; i++) {
    snprintf(rows[i].entity, sizeof rows[i].entity, "albums");
    memcpy(rows[i].row_uid, lose_album, MC_UID_STRLEN);
    snprintf(rows[i].field, sizeof rows[i].field, "%s", fields[i]);
    rows[i].data = (const unsigned char *)values[i];
    rows[i].len = (uint32_t)strlen(values[i]);
  }
  static const char *const efields[] = {"playlist_uid", "track_uid", "position"};
  const char *evalues[] = {pl, lose_track, "a1"};
  for (int i = 0; i < 3; i++) {
    mc_row *r = &rows[2 + i];
    snprintf(r->entity, sizeof r->entity, "playlist_entries");
    memcpy(r->row_uid, e2, MC_UID_STRLEN);
    snprintf(r->field, sizeof r->field, "%s", efields[i]);
    r->data = (const unsigned char *)evalues[i];
    r->len = (uint32_t)strlen(evalues[i]);
  }
  mc_library *libs[] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    mc_rows_target tg;
    mc_rows_target_library(libs[i], &tg);
    mc_rows_applier *ap = NULL;
    CHECK(mc_rows_applier_new(&tg, &ap) == MC_OK);
    if (ap) CHECK(mc_rows_apply(ap, rows, 5, NULL) == MC_OK);
    mc_rows_applier_free(ap);
    sqlite3 *ldb = mc_library_db(libs[i]);
    char text[256];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM albums WHERE uid = '%s'", lose_album);
    CHECK(query_int(ldb, sql) == 0);
    snprintf(sql, sizeof sql, "SELECT title || '/' || mb_albumid FROM albums WHERE uid = '%s'",
             smaller(ia.album, ib.album));
    query_text(ldb, sql, text, sizeof text);
    CHECK_TEXT(text, "Things (Remastered)/REL2");
    snprintf(sql, sizeof sql, "SELECT track_uid FROM playlist_entries WHERE uid = '%s'", e2);
    query_text(ldb, sql, text, sizeof text);
    CHECK_TEXT(text, smaller(ia.tracks[1], ib.tracks[1]));
    CHECK(query_int(ldb, "SELECT count(*) FROM tracks") == 4);
    CHECK(query_int(ldb, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
  }
  da = dump(a);
  dc = dump(c);
  CHECK(da && dc && !strcmp(da, dc));
  free(da);
  free(dc);

  /* Importing the album again where it's merged finds the survivor (the importer looks
   * albums up by mb_albumid), so nothing new appears to merge. */
  {
    char found[MC_UID_STRLEN];
    query_text(mc_library_db(a), "SELECT uid FROM albums WHERE mb_albumid = 'REL2'", found,
               sizeof found);
    CHECK_TEXT(found, smaller(ia.album, ib.album));
  }

  /* An older peer's singles albums: two "Low Singles" of one artist, with kind = 'singles'.
   * They're logged and kept for peers, but kind is a field this version doesn't know: the
   * albums stay two ordinary albums, and nothing merges. */
  {
    char s1[MC_UID_STRLEN], s2[MC_UID_STRLEN];
    mc_uid_new(s1);
    mc_uid_new(s2);
    const char *artist = smaller(ia.artist, ib.artist);
    static const char *const sfields[] = {"title", "artist_uid", "kind"};
    const char *svalues[] = {"Low Singles", artist, "singles"};
    mc_row old[6];
    memset(old, 0, sizeof old);
    for (int i = 0; i < 6; i++) {
      old[i].hlc = hlc + 10 + (uint64_t)i;
      memcpy(old[i].origin, late, MC_UID_STRLEN);
      old[i].tag = MC_VAL_TEXT;
      snprintf(old[i].entity, sizeof old[i].entity, "albums");
      memcpy(old[i].row_uid, i < 3 ? s1 : s2, MC_UID_STRLEN);
      snprintf(old[i].field, sizeof old[i].field, "%s", sfields[i % 3]);
      old[i].data = (const unsigned char *)svalues[i % 3];
      old[i].len = (uint32_t)strlen(svalues[i % 3]);
    }
    mc_rows_target tg;
    mc_rows_target_library(c, &tg);
    mc_rows_applier *ap = NULL;
    CHECK(mc_rows_applier_new(&tg, &ap) == MC_OK);
    if (ap) CHECK(mc_rows_apply(ap, old, 6, NULL) == MC_OK);
    mc_rows_applier_free(ap);
    sqlite3 *cdb = mc_library_db(c);
    CHECK(query_int(cdb, "SELECT count(*) FROM albums WHERE title = 'Low Singles' AND kind = 'album'") == 2);
    CHECK(query_int(cdb, "SELECT count(*) FROM changelog WHERE field = 'kind' AND value = 'singles'") == 2);
    CHECK(query_int(cdb, "SELECT count(*) FROM aliases") == 6);
    CHECK(query_int(cdb, "SELECT count(*) FROM merge_keys WHERE field = 'kind'") == 0);
  }

  mc_library_close(a);
  mc_library_close(b);
  mc_library_close(c);
  test_credits(scratch);
  mc_shutdown();
  if (!test_failures) {
    remove_db(pa);
    remove_db(pb);
    remove_db(pc);
  }
  return TEST_RESULT();
}
