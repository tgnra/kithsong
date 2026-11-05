/* Hand edits (musiccore/edit.h; "Hand edits" in docs/tagging-rules.md). Each test makes a library
 * of real files through mc_import_db_write(), as an import leaves it. MusicBrainz and the
 * Cover Art Archive are fakes, so nothing here touches the network.
 *
 *   - rows: an edit is logged, and reaches a second library as any change does;
 *   - credits: a credit set by hand splits as an import's, artists made and left unused;
 *   - files: tags rewritten with the row's size, mtime and sha256 following, no new file_rev;
 *     transcodes never rewritten; a file whose hash is wrong left as it is;
 *   - filing: moved as organize.c says, collisions, tracks trading places, the cover, folders;
 *   - failures: a failed rename at every step, a failed tag write, a file in use, a crash at
 *     every step: rows and files as they were, nothing left behind;
 *   - releases: searching, the form's tags, matching an album to another release;
 *   - covers: what an image must be, what a chosen one replaces, failures and crashes as above,
 *     and that an import or a new release leaves it alone.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/edit_internal.h"
#include "library/library_internal.h"
#include "musiccore/edit.h"
#include "musiccore/musiccore.h"
#include "sync/rows.h"
#include "tagger/coverart.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "util/uid.h"
#include "test.h"
#include "test_image.h"

/* test.h's, for arrays too, which it warns can't be NULL. */
static int same_str(const char *a, const char *b) {
  return a && b && strcmp(a, b) == 0;
}
#undef CHECK_STR
#define CHECK_STR(a, b) CHECK(same_str((a), (b)))

#define ANN "a1111111-1111-4111-8111-111111111111"
#define BOB "a2222222-2222-4222-8222-222222222222"
#define NOBODY "a3333333-3333-4333-8333-333333333333"
#define REL_A "11111111-2222-3333-4444-555555555555"
#define REL_B "22222222-2222-3333-4444-555555555555"
#define REL_NEW "33333333-2222-3333-4444-555555555555"
#define RG_NEW "44444444-2222-3333-4444-555555555555"

/* --- Files --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  char (*more)[512] = realloc(n->items, (size_t)(n->count + 1) * sizeof *more);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd') remove_tree(path);
    else mc_remove(path);
  }
  free(n.items);
  mc_rmdir(dir);
}

/* Files under dir whose name starts with ".kithsong-", at any depth. */
static int count_strays(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  int c = 0;
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd') c += count_strays(path);
    else c += !strncmp(n.items[i] + 1, ".kithsong-", 10);
  }
  free(n.items);
  return c;
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* A tenth of a second of silent 8 kHz mono WAV with these tags. */
static int write_wav(const char *path, const mc_tags *tags) {
  enum { DATA_LEN = 1600 };
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + DATA_LEN);
  put_u32(hdr + 16, 16);
  hdr[20] = 1;
  hdr[22] = 1;
  put_u32(hdr + 24, 8000);
  put_u32(hdr + 28, 16000);
  hdr[32] = 2;
  hdr[34] = 16;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, DATA_LEN);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char silence[DATA_LEN];
  int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, sizeof silence, 1, f) == 1;
  if (fclose(f) != 0 || !ok) return 0;
  return mc_tags_write(path, tags) == MC_OK;
}

/* A library of its own under the test folder. */
typedef struct lib_dir {
  char path[1024];
  mc_library *lib;
} lib_dir;

static void lib_open(lib_dir *l, const char *base, const char *name) {
  snprintf(l->path, sizeof l->path, "%s/edit-%s", base, name);
  remove_tree(l->path);
  CHECK(mc_mkdirs(l->path) == MC_OK);
  CHECK(mc_library_open(l->path, &l->lib) == MC_OK);
}

static void lib_close(lib_dir *l) {
  mc_library_close(l->lib);
  remove_tree(l->path);
}

static char *abs_of(lib_dir *l, const char *rel) {
  static char paths[4][2048];
  static int next;
  char *out = paths[next++ % 4];
  snprintf(out, sizeof paths[0], "%s/%s", l->path, rel);
  return out;
}

static void write_bytes(const char *path, const char *text) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  fwrite(text, strlen(text), 1, f);
  fclose(f);
}

static char *hash_of(const char *path) {
  static char sha[2][MC_SHA256_STRLEN];
  static int next;
  char *out = sha[next++ % 2];
  if (mc_sha256_file(path, out) != MC_OK) out[0] = '\0';
  return out;
}

/* What a test imports: an album of n tracks titled by titles, the first as tagged here. */
typedef struct album_spec {
  const char *title, *artist, *mb_artistid, *mb_albumid;
  int year;
  int n;
  const char *const *titles;
} album_spec;

/* Imports spec into l: real files at the places organize.c gives them. Returns via uid the
 * album's. */
static void add_album(lib_dir *l, const album_spec *s, char album_uid[MC_UID_STRLEN]) {
  mc_db_track rows[8];
  char rels[8][512];
  memset(rows, 0, sizeof rows);
  char dir[256];
  if (s->year) snprintf(dir, sizeof dir, "%s/%s (%d)", s->artist, s->title, s->year);
  else snprintf(dir, sizeof dir, "%s/%s", s->artist, s->title);
  CHECK(mc_mkdirs(abs_of(l, dir)) == MC_OK);
  for (int i = 0; i < s->n; i++) {
    snprintf(rels[i], sizeof rels[i], "%s/%02d %s.wav", dir, i + 1, s->titles[i]);
    mc_tags tags = {0};
    tags.title = (char *)s->titles[i];
    tags.artist = (char *)s->artist;
    tags.album = (char *)s->title;
    tags.album_artist = (char *)s->artist;
    tags.year = s->year;
    tags.track = i + 1;
    tags.track_total = s->n;
    tags.genre = "Noise";
    char *abs = abs_of(l, rels[i]);
    CHECK(write_wav(abs, &tags));
    mc_db_track *t = &rows[i];
    t->path = rels[i];
    t->format = "wav";
    CHECK(mc_file_info(abs, &t->size_bytes, &t->mtime_ms) == MC_OK);
    t->title = s->titles[i];
    t->artist = (mc_db_artist){s->artist, NULL, s->mb_artistid};
    t->track_no = i + 1;
    t->disc_no = 1;
    t->year = s->year;
    t->genre = "Noise";
    t->duration_ms = 100;
    t->mb_albumid = s->mb_albumid;
  }
  mc_db_album album = {0};
  album.title = s->title;
  album.artist = (mc_db_artist){s->artist, NULL, s->mb_artistid};
  album.year = s->year;
  album.disc_total = 1;
  album.mb_albumid = s->mb_albumid;
  CHECK(mc_import_db_write(l->lib, &album, rows, s->n) == MC_OK);

  sqlite3_stmt *stmt;
  album_uid[0] = '\0';
  if (sqlite3_prepare_v2(mc_library_db(l->lib),
                         "SELECT album_uid FROM tracks t JOIN files f ON f.track_uid = t.uid WHERE f.path = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, rels[0], -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      snprintf(album_uid, MC_UID_STRLEN, "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }
  CHECK(album_uid[0]);
}

/* --- Queries --- */

static sqlite3_stmt *query(lib_dir *l, const char *sql, const char *a, const char *b) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(mc_library_db(l->lib), sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "sql error: %s\n", sqlite3_errmsg(mc_library_db(l->lib)));
    return NULL;
  }
  if (a) sqlite3_bind_text(stmt, 1, a, -1, SQLITE_STATIC);
  if (b) sqlite3_bind_text(stmt, 2, b, -1, SQLITE_STATIC);
  return stmt;
}

static int q_int(lib_dir *l, const char *sql, const char *a, const char *b) {
  sqlite3_stmt *stmt = query(l, sql, a, b);
  int n = stmt && sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return n;
}

/* The first column of the first row, or "" (a few can be in use at once). */
static char *q_str(lib_dir *l, const char *sql, const char *a, const char *b) {
  static char bufs[6][512];
  static int next;
  char *out = bufs[next++ % 6];
  out[0] = '\0';
  sqlite3_stmt *stmt = query(l, sql, a, b);
  if (stmt && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    snprintf(out, sizeof bufs[0], "%s", (const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return out;
}

static char *track_at(lib_dir *l, const char *album, int no) {
  char n[16];
  snprintf(n, sizeof n, "%d", no);
  return q_str(l, "SELECT uid FROM tracks WHERE album_uid = ?1 AND track_no = CAST(?2 AS INTEGER)", album, n);
}

static char *path_of(lib_dir *l, const char *track) {
  return q_str(l, "SELECT path FROM files WHERE track_uid = ?1", track, NULL);
}

static int logged(lib_dir *l, const char *entity, const char *row, const char *field) {
  sqlite3_stmt *stmt = query(l, "SELECT count(*) FROM changelog WHERE entity = ?1 AND row_uid = ?2 AND field = ?3",
                             entity, row);
  if (!stmt) return -1;
  sqlite3_bind_text(stmt, 3, field, -1, SQLITE_STATIC);
  int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return n;
}

/* Whether every files row matches its file, as sync's fast and deep checks ask. */
static int files_check_out(lib_dir *l) {
  sqlite3_stmt *stmt = query(l, "SELECT path, size_bytes, mtime_ms, sha256 FROM files", NULL, NULL);
  int ok = stmt != NULL;
  while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 0);
    const char *sha = (const char *)sqlite3_column_text(stmt, 3);
    int64_t size = 0, mtime = 0;
    char *abs = abs_of(l, rel);
    if (mc_file_info(abs, &size, &mtime) != MC_OK || size != sqlite3_column_int64(stmt, 1) ||
        mtime != sqlite3_column_int64(stmt, 2) || (sha && strcmp(sha, hash_of(abs)))) {
      fprintf(stderr, "%s doesn't match its row\n", rel);
      ok = 0;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

static void record_hashes(lib_dir *l) {
  sqlite3_stmt *stmt = query(l, "SELECT uid, path FROM files", NULL, NULL);
  while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    sqlite3_stmt *set = query(l, "UPDATE files SET sha256 = ?2 WHERE uid = ?1",
                              (const char *)sqlite3_column_text(stmt, 0),
                              hash_of(abs_of(l, (const char *)sqlite3_column_text(stmt, 1))));
    if (set) sqlite3_step(set);
    sqlite3_finalize(set);
  }
  sqlite3_finalize(stmt);
}

static const char *const k_titles[] = {"One", "Two", "Three"};

/* --- Rows --- */

/* Every changelog row of from, applied to to as a session would. */
static void sync_rows(lib_dir *from, lib_dir *to) {
  mc_rows_reader *rd;
  CHECK(mc_rows_reader_open(mc_library_db(from->lib), NULL, 0, 0, &rd) == MC_OK);
  mc_row *rows = NULL;
  int n = 0, got;
  mc_row r;
  while (mc_rows_reader_next(rd, &r, &got) == MC_OK && got) {
    mc_row *more = realloc(rows, (size_t)(n + 1) * sizeof *more);
    if (!more) break;
    rows = more;
    if (r.len) {
      unsigned char *copy = malloc(r.len);
      if (copy) memcpy(copy, r.data, r.len);
      r.data = copy;
    }
    rows[n++] = r;
  }
  mc_rows_reader_close(rd);
  mc_rows_target target;
  mc_rows_target_library(to->lib, &target);
  mc_rows_applier *a;
  CHECK(mc_rows_applier_new(&target, &a) == MC_OK);
  int count = 0;
  CHECK(mc_rows_apply(a, rows, n, &count) == MC_OK);
  mc_rows_applier_free(a);
  for (int i = 0; i < n; i++)
    if (rows[i].len) free((void *)rows[i].data);
  free(rows);
}

static void test_rows(const char *base) {
  lib_dir l, peer;
  lib_open(&l, base, "rows");
  lib_open(&peer, base, "rows-peer");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 3, k_titles};
  add_album(&l, &s, album);

  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  CHECK_STR(a.title, "Album");
  CHECK_STR(a.artist, "Ann");
  CHECK(a.year == 2000);
  CHECK_STR(a.mb_albumid, "");
  free(a.title);
  a.title = mc_strdup("Better Album");
  a.year = 2001;
  a.original_year = 1999;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);

  CHECK(logged(&l, "albums", album, "title") == 2);
  CHECK(logged(&l, "albums", album, "year") == 2);
  CHECK(logged(&l, "albums", album, "artist_uid") == 1); /* what wasn't changed logs nothing */
  CHECK_STR(q_str(&l, "SELECT title FROM albums WHERE uid = ?1", album, NULL), "Better Album");
  /* Tracks follow the album's year: the file says what they say. */
  CHECK(q_int(&l, "SELECT count(*) FROM tracks WHERE album_uid = ?1 AND year = 2001", album, NULL) == 3);

  /* A track's tags. */
  char track[MC_UID_STRLEN];
  snprintf(track, sizeof track, "%s", track_at(&l, album, 2));
  mc_track_tags t;
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  CHECK_STR(t.title, "Two");
  CHECK_STR(t.artist, ""); /* the album's */
  CHECK(t.track_no == 2 && t.disc_no == 1 && t.year == 2001);
  CHECK_STR(t.genre, "Noise");
  free(t.genre);
  t.genre = mc_strdup("Speedcore");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK(logged(&l, "tracks", track, "genre") == 2);
  CHECK(logged(&l, "tracks", track, "title") == 1);

  /* What's gone is said so, and nothing is written. */
  CHECK(mc_album_tags_get(l.lib, "00000000-0000-0000-0000-000000000000", &a) == MC_ERR_NOT_FOUND);
  mc_album_tags none = {0};
  CHECK(mc_album_tags_set(l.lib, "00000000-0000-0000-0000-000000000000", &none) == MC_ERR_NOT_FOUND);
  mc_track_tags none_t = {0};
  CHECK(mc_track_tags_set(l.lib, "00000000-0000-0000-0000-000000000000", &none_t) == MC_ERR_NOT_FOUND);

  /* It reaches another device like any change, files aside. */
  sync_rows(&l, &peer);
  CHECK_STR(q_str(&peer, "SELECT title FROM albums WHERE uid = ?1", album, NULL), "Better Album");
  CHECK(q_int(&peer, "SELECT year FROM tracks WHERE uid = ?1", track, NULL) == 2001);
  CHECK_STR(q_str(&peer, "SELECT genre FROM tracks WHERE uid = ?1", track, NULL), "Speedcore");
  CHECK(q_int(&peer, "SELECT count(*) FROM files", NULL, NULL) == 0);

  lib_close(&l);
  lib_close(&peer);
}

/* --- Credits --- */

static void test_credits(const char *base) {
  lib_dir l;
  lib_open(&l, base, "credits");
  char album[MC_UID_STRLEN], other[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 3, k_titles};
  add_album(&l, &s, album);
  static const char *const bobs[] = {"Solo"};
  album_spec s2 = {"Bobs", "Bob", BOB, NULL, 2001, 1, bobs};
  add_album(&l, &s2, other);
  char ann[MC_UID_STRLEN], bob[MC_UID_STRLEN];
  snprintf(ann, sizeof ann, "%s", q_str(&l, "SELECT uid FROM artists WHERE name = 'Ann'", NULL, NULL));
  snprintf(bob, sizeof bob, "%s", q_str(&l, "SELECT uid FROM artists WHERE name = 'Bob'", NULL, NULL));

  /* Both known: "&" splits, into the rows an import makes. */
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.artist);
  a.artist = mc_strdup("Ann & Bob");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM albums WHERE uid = ?1", album, NULL), "Ann & Bob");
  CHECK_STR(q_str(&l, "SELECT artist_uid FROM albums WHERE uid = ?1", album, NULL), ann);
  CHECK(q_int(&l, "SELECT count(*) FROM album_artists WHERE album_uid = ?1", album, NULL) == 2);
  CHECK_STR(q_str(&l, "SELECT artist_uid FROM album_artists WHERE album_uid = ?1 AND position = 1", album, NULL), bob);
  CHECK_STR(q_str(&l, "SELECT joinphrase FROM album_artists WHERE album_uid = ?1 AND position = 0", album, NULL), " & ");
  /* Tracks credited as the album was follow it, and have the same artists. */
  char track[MC_UID_STRLEN];
  snprintf(track, sizeof track, "%s", track_at(&l, album, 1));
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM tracks WHERE uid = ?1", track, NULL), "Ann & Bob");
  CHECK(q_int(&l, "SELECT count(*) FROM track_artists WHERE track_uid = ?1", track, NULL) == 2);
  CHECK(logged(&l, "albums", album, "artist_credit") >= 1);
  /* And the files say so, one artist per value. */
  mc_tags tags;
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, track)), &tags) == MC_OK);
  CHECK_STR(tags.artist, "Ann & Bob");
  CHECK_STR(tags.album_artist, "Ann & Bob");
  CHECK_STR(tags.album_artists, "Ann\nBob");
  mc_tags_clear(&tags);

  /* An unchanged credit isn't split again: set whole, it's left as it is. */
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  int rows = q_int(&l, "SELECT count(*) FROM changelog", NULL, NULL);
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  CHECK(q_int(&l, "SELECT count(*) FROM changelog", NULL, NULL) == rows);
  mc_album_tags_clear(&a);

  /* One of a track's own: "feat." always splits, and makes the artist that's new. */
  mc_track_tags t;
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  CHECK_STR(t.artist, "");
  free(t.artist);
  t.artist = mc_strdup("Ann feat. Cat");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM tracks WHERE uid = ?1", track, NULL), "Ann feat. Cat");
  char cat[MC_UID_STRLEN];
  snprintf(cat, sizeof cat, "%s", q_str(&l, "SELECT uid FROM artists WHERE name = 'Cat'", NULL, NULL));
  CHECK(cat[0]);
  CHECK(logged(&l, "artists", cat, "name") == 1);
  CHECK_STR(q_str(&l, "SELECT artist_uid FROM track_artists WHERE track_uid = ?1 AND position = 1", track, NULL), cat);

  /* "," with a part nobody knows stays one artist. */
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  free(t.artist);
  t.artist = mc_strdup("Tyler, the Creator");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK_STR(q_str(&l, "SELECT name FROM artists a JOIN tracks t ON t.artist_uid = a.uid WHERE t.uid = ?1", track, NULL),
            "Tyler, the Creator");
  CHECK(q_int(&l, "SELECT count(*) FROM track_artists WHERE track_uid = ?1", track, NULL) == 1);
  /* That left Cat with nothing: gone, and the delete syncs. */
  CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE uid = ?1", cat, NULL) == 0);
  CHECK(logged(&l, "artists", cat, "_deleted") == 1);

  /* "" is back to the album's: no artist of its own. The one it had goes too. */
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  CHECK_STR(t.artist, "Tyler, the Creator");
  free(t.artist);
  t.artist = mc_strdup("");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK(q_int(&l, "SELECT artist_uid IS NULL FROM tracks WHERE uid = ?1", track, NULL) == 1);
  CHECK(q_int(&l, "SELECT count(*) FROM track_artists WHERE track_uid = ?1", track, NULL) == 0);
  CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE name = 'Tyler, the Creator'", NULL, NULL) == 0);
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, track)), &tags) == MC_OK);
  CHECK_STR(tags.artist, "Ann & Bob");
  mc_tags_clear(&tags);

  lib_close(&l);
}

static void test_artist(const char *base) {
  lib_dir l;
  lib_open(&l, base, "artist");
  char album[MC_UID_STRLEN], other[MC_UID_STRLEN], third[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  static const char *const bobs[] = {"Solo"};
  album_spec s2 = {"Bobs", "Bob", BOB, NULL, 2001, 1, bobs};
  add_album(&l, &s2, other);
  static const char *const duets[] = {"Duet"};
  album_spec s3 = {"Duets", "Bob", BOB, NULL, 2002, 1, duets};
  add_album(&l, &s3, third);
  char ann[MC_UID_STRLEN], bob[MC_UID_STRLEN];
  snprintf(ann, sizeof ann, "%s", q_str(&l, "SELECT uid FROM artists WHERE name = 'Ann'", NULL, NULL));
  snprintf(bob, sizeof bob, "%s", q_str(&l, "SELECT uid FROM artists WHERE name = 'Bob'", NULL, NULL));

  /* A credit of two, and one spelled as it was on the release. */
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, third, &a) == MC_OK);
  free(a.artist);
  a.artist = mc_strdup("Ann & Bob");
  CHECK(mc_album_tags_set(l.lib, third, &a) == MC_OK);
  mc_album_tags_clear(&a);

  /* A sort name is in no file, and touches none. */
  char sum[MC_SHA256_STRLEN];
  char was[256];
  snprintf(was, sizeof was, "%s", path_of(&l, track_at(&l, album, 1)));
  snprintf(sum, sizeof sum, "%s", hash_of(abs_of(&l, was)));
  mc_artist_tags t;
  CHECK(mc_artist_tags_get(l.lib, ann, &t) == MC_OK);
  CHECK_STR(t.name, "Ann");
  CHECK_STR(t.sort_name, "");
  free(t.sort_name);
  t.sort_name = mc_strdup("Ann, The");
  CHECK(mc_artist_tags_set(l.lib, ann, &t) == MC_OK);
  CHECK_STR(q_str(&l, "SELECT sort_name FROM artists WHERE uid = ?1", ann, NULL), "Ann, The");
  CHECK_STR(hash_of(abs_of(&l, was)), sum);

  /* Renamed: everything that's spelled with the name, folder and tags included. */
  free(t.name);
  t.name = mc_strdup("Anne");
  CHECK(mc_artist_tags_set(l.lib, ann, &t) == MC_OK);
  mc_artist_tags_clear(&t);
  CHECK(logged(&l, "artists", ann, "name") == 2);
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM albums WHERE uid = ?1", third, NULL), "Anne & Bob");
  char *now = path_of(&l, track_at(&l, album, 1));
  CHECK_STR(now, "Anne/Album (2000)/01 One.wav");
  CHECK(mc_file_exists(abs_of(&l, now)));
  CHECK(!mc_file_exists(abs_of(&l, "Ann")));
  mc_tags tags;
  CHECK(mc_tags_read(abs_of(&l, now), &tags) == MC_OK);
  CHECK_STR(tags.artist, "Anne");
  CHECK_STR(tags.album_artist, "Anne");
  mc_tags_clear(&tags);
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, q_str(&l, "SELECT uid FROM tracks WHERE album_uid = ?1", third, NULL))), &tags) == MC_OK);
  CHECK_STR(tags.album_artist, "Anne & Bob");
  CHECK_STR(tags.album_artists, "Anne\nBob");
  mc_tags_clear(&tags);
  CHECK(files_check_out(&l));

  /* To a name that's taken: two artists of one name, as two with different MusicBrainz ids
   * can be. Sync merges by id, not by name. */
  CHECK(mc_artist_tags_get(l.lib, ann, &t) == MC_OK);
  free(t.name);
  t.name = mc_strdup("Bob");
  CHECK(mc_artist_tags_set(l.lib, ann, &t) == MC_OK);
  mc_artist_tags_clear(&t);
  CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE name = 'Bob'", NULL, NULL) == 2);
  CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE uid = ?1", bob, NULL) == 1);
  CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE uid = ?1", ann, NULL) == 1);

  /* An artist needs a name. */
  mc_artist_tags none = {0};
  CHECK(mc_artist_tags_set(l.lib, ann, &none) == MC_ERR_INVALID_ARG);
  CHECK(mc_artist_tags_get(l.lib, "00000000-0000-0000-0000-000000000000", &t) == MC_ERR_NOT_FOUND);

  lib_close(&l);
}

/* --- File tags --- */

static void test_file_tags(const char *base) {
  lib_dir l;
  lib_open(&l, base, "tags");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  record_hashes(&l);
  char track[MC_UID_STRLEN], rel[256], source[80];
  snprintf(track, sizeof track, "%s", track_at(&l, album, 1));
  snprintf(rel, sizeof rel, "%s", path_of(&l, track));
  CHECK(mc_library_begin_write(l.lib) == MC_OK);
  sqlite3_stmt *set = query(&l, "UPDATE files SET source_sha256 = 'imported from' WHERE track_uid = ?1", track, NULL);
  if (set) sqlite3_step(set);
  sqlite3_finalize(set);
  /* A transcode of it, as sync keeps one. */
  char transcode[MC_UID_STRLEN], other_rel[256] = "Ann/Album (2000)/01 One.opus";
  CHECK(mc_uid_new(transcode) == MC_OK);
  write_bytes(abs_of(&l, other_rel), "not really opus");
  int64_t ts = 0, tm = 0;
  CHECK(mc_file_info(abs_of(&l, other_rel), &ts, &tm) == MC_OK);
  char sql[512];
  snprintf(sql, sizeof sql, "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms, source_file_uid,"
                            " source_peer_uid) VALUES (?1, ?2, '%s', 'opus', %lld, %lld, 'x', 'y')", other_rel,
           (long long)ts, (long long)tm);
  set = query(&l, sql, transcode, track);
  if (set) CHECK(sqlite3_step(set) == SQLITE_DONE);
  sqlite3_finalize(set);
  CHECK(mc_library_commit(l.lib) == MC_OK);
  snprintf(source, sizeof source, "%s", q_str(&l, "SELECT coalesce(file_rev, '') FROM tracks WHERE uid = ?1", track, NULL));
  char file_uid[MC_UID_STRLEN], sha[MC_SHA256_STRLEN];
  snprintf(file_uid, sizeof file_uid, "%s", q_str(&l, "SELECT uid FROM files WHERE track_uid = ?1 AND format = 'wav'", track, NULL));
  snprintf(sha, sizeof sha, "%s", q_str(&l, "SELECT sha256 FROM files WHERE uid = ?1", file_uid, NULL));

  /* What doesn't touch the path: tags only, the file stays where it is. */
  mc_track_tags t;
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  free(t.genre);
  t.genre = mc_strdup("Speedcore");
  t.year = 1999;
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);

  mc_tags tags;
  CHECK_STR(q_str(&l, "SELECT path FROM files WHERE uid = ?1", file_uid, NULL), rel);
  CHECK(mc_tags_read(abs_of(&l, rel), &tags) == MC_OK);
  CHECK_STR(tags.genre, "Speedcore");
  CHECK(tags.year == 1999);
  CHECK_STR(tags.title, "One"); /* the rest as it was */
  CHECK(tags.track == 1 && tags.track_total == 2);
  mc_tags_clear(&tags);

  /* The row keeps up, so neither check takes it for corrupt; the file is the same file. */
  CHECK(files_check_out(&l));
  CHECK(strcmp(q_str(&l, "SELECT sha256 FROM files WHERE uid = ?1", file_uid, NULL), sha) != 0);
  CHECK_STR(q_str(&l, "SELECT uid FROM files WHERE path = ?1", rel, NULL), file_uid);
  CHECK_STR(q_str(&l, "SELECT source_sha256 FROM files WHERE uid = ?1", file_uid, NULL), "imported from");
  CHECK_STR(q_str(&l, "SELECT coalesce(file_rev, '') FROM tracks WHERE uid = ?1", track, NULL), source);
  CHECK(logged(&l, "tracks", track, "file_rev") == 0);
  /* The other track's file had nothing to do with it. */
  CHECK(count_strays(l.path) == 0);

  /* A transcode isn't rewritten. */
  CHECK(q_int(&l, "SELECT size_bytes FROM files WHERE uid = ?1", transcode, NULL) == ts);
  CHECK(mc_file_exists(abs_of(&l, other_rel)));

  /* A number taken away goes from the file too. */
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  t.year = 0;
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  /* ...falling back to the album's. */
  CHECK(mc_tags_read(abs_of(&l, rel), &tags) == MC_OK);
  CHECK(tags.year == 2000);
  mc_tags_clear(&tags);
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  a.original_year = 1990;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  CHECK(mc_tags_read(abs_of(&l, rel), &tags) == MC_OK);
  CHECK(tags.original_year == 1990);
  mc_tags_clear(&tags);
  a.original_year = 0;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK(mc_tags_read(abs_of(&l, rel), &tags) == MC_OK);
  CHECK(tags.original_year == 0);
  mc_tags_clear(&tags);

  /* A tag the rows don't know about stays, since only what changed is written. */
  mc_tags extra = {0};
  extra.mb_trackid = "b1111111-1111-4111-8111-111111111111";
  CHECK(mc_tags_write(abs_of(&l, rel), &extra) == MC_OK);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(abs_of(&l, rel), &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql, "UPDATE files SET size_bytes = %lld, mtime_ms = %lld, sha256 = NULL WHERE uid = ?1",
           (long long)size, (long long)mtime);
  set = query(&l, sql, file_uid, NULL);
  if (set) sqlite3_step(set);
  sqlite3_finalize(set);
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  free(t.genre);
  t.genre = mc_strdup("Gabber");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK(mc_tags_read(abs_of(&l, rel), &tags) == MC_OK);
  CHECK_STR(tags.genre, "Gabber");
  CHECK_STR(tags.mb_trackid, "b1111111-1111-4111-8111-111111111111");
  mc_tags_clear(&tags);
  /* It had no hash, and isn't given one: that's for a deep check, as for any new file. */
  CHECK(q_int(&l, "SELECT sha256 IS NULL FROM files WHERE uid = ?1", file_uid, NULL) == 1);

  lib_close(&l);
}

/* Bytes that don't match their hash aren't given a new one, nor is a file changed outside the
 * library touched: the rows change, the file stays, and the checks still say what they said. */
static void test_left_alone(const char *base) {
  lib_dir l;
  lib_open(&l, base, "alone");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  record_hashes(&l);
  char first[MC_UID_STRLEN], rel[256];
  snprintf(first, sizeof first, "%s", track_at(&l, album, 1));
  snprintf(rel, sizeof rel, "%s", path_of(&l, first));
  /* Rot: the same size and mtime, another hash. */
  sqlite3_stmt *set = query(&l, "UPDATE files SET sha256 = ?2 WHERE track_uid = ?1", first,
                            "0000000000000000000000000000000000000000000000000000000000000000");
  if (set) sqlite3_step(set);
  sqlite3_finalize(set);
  char sum[MC_SHA256_STRLEN];
  snprintf(sum, sizeof sum, "%s", hash_of(abs_of(&l, rel)));

  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.title);
  a.title = mc_strdup("Renamed");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);

  CHECK_STR(q_str(&l, "SELECT title FROM albums WHERE uid = ?1", album, NULL), "Renamed");
  CHECK_STR(path_of(&l, first), rel);
  CHECK_STR(hash_of(abs_of(&l, rel)), sum);
  CHECK(q_int(&l, "SELECT sha256 LIKE '0000%' FROM files WHERE track_uid = ?1", first, NULL) == 1);
  /* The album's other file went on without it. */
  CHECK_STR(path_of(&l, track_at(&l, album, 2)), "Ann/Renamed (2000)/02 Two.wav");
  CHECK(count_strays(l.path) == 0);

  /* A file that's gone: only the row changes. */
  CHECK(mc_remove(abs_of(&l, path_of(&l, track_at(&l, album, 2)))) == MC_OK);
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  a.year = 2005;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK(q_int(&l, "SELECT year FROM albums WHERE uid = ?1", album, NULL) == 2005);

  lib_close(&l);
}

/* --- Filing --- */

static void test_filing(const char *base) {
  lib_dir l;
  lib_open(&l, base, "filing");
  char album[MC_UID_STRLEN], other[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  album_spec s2 = {"Taken", "Ann", ANN, NULL, 2000, 1, k_titles};
  add_album(&l, &s2, other);
  write_bytes(abs_of(&l, "Ann/Album (2000)/cover.jpg"), "a cover");
  write_bytes(abs_of(&l, "Ann/Taken (2000)/stray.wav"), "someone's own");

  /* Into a folder that has a file of the same name: " (2)", and the other file stays. */
  char moved[MC_UID_STRLEN], kept[MC_UID_STRLEN];
  snprintf(moved, sizeof moved, "%s", track_at(&l, album, 1));
  snprintf(kept, sizeof kept, "%s", track_at(&l, other, 1));
  char sum[MC_SHA256_STRLEN];
  snprintf(sum, sizeof sum, "%s", hash_of(abs_of(&l, path_of(&l, kept))));
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.title);
  a.title = mc_strdup("Taken");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK_STR(path_of(&l, moved), "Ann/Taken (2000)/01 One (2).wav");
  CHECK_STR(path_of(&l, track_at(&l, album, 2)), "Ann/Taken (2000)/02 Two.wav");
  CHECK_STR(path_of(&l, kept), "Ann/Taken (2000)/01 One.wav");
  CHECK_STR(hash_of(abs_of(&l, path_of(&l, kept))), sum);
  CHECK(files_check_out(&l));
  /* The cover went with it, and the folder left empty is gone. */
  CHECK(mc_file_exists(abs_of(&l, "Ann/Taken (2000)/cover.jpg")));
  CHECK(!mc_file_exists(abs_of(&l, "Ann/Album (2000)")));
  CHECK(mc_file_exists(abs_of(&l, "Ann")));
  CHECK(count_strays(l.path) == 0);

  /* Out to an artist of its own: the artist folder goes when nothing's left in it. */
  CHECK(mc_album_tags_get(l.lib, other, &a) == MC_OK);
  free(a.artist);
  a.artist = mc_strdup("Zed");
  a.year = 0;
  CHECK(mc_album_tags_set(l.lib, other, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK_STR(path_of(&l, kept), "Zed/Taken/01 One.wav");

  /* Moved tags follow too. */
  mc_tags tags;
  CHECK(mc_tags_read(abs_of(&l, "Zed/Taken/01 One.wav"), &tags) == MC_OK);
  CHECK_STR(tags.album_artist, "Zed");
  CHECK_STR(tags.artist, "Zed");
  CHECK(tags.year == 0); /* its album no longer says */
  mc_tags_clear(&tags);

  /* A title with what Windows can't have in a name. */
  mc_track_tags t;
  CHECK(mc_track_tags_get(l.lib, moved, &t) == MC_OK);
  free(t.title);
  t.title = mc_strdup("What? Why/How");
  CHECK(mc_track_tags_set(l.lib, moved, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK_STR(path_of(&l, moved), "Ann/Taken (2000)/01 What_ Why_How.wav");

  /* A second disc: every file of the album says so, not just that one's. */
  char second[MC_UID_STRLEN];
  snprintf(second, sizeof second, "%s", track_at(&l, album, 2));
  CHECK(mc_track_tags_get(l.lib, second, &t) == MC_OK);
  t.disc_no = 2;
  t.track_no = 1;
  CHECK(mc_track_tags_set(l.lib, second, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK(q_int(&l, "SELECT disc_total FROM albums WHERE uid = ?1", album, NULL) == 2);
  CHECK_STR(path_of(&l, second), "Ann/Taken (2000)/2-01 Two.wav");
  CHECK_STR(path_of(&l, moved), "Ann/Taken (2000)/1-01 What_ Why_How.wav");
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, moved)), &tags) == MC_OK);
  CHECK(tags.disc_total == 2);
  mc_tags_clear(&tags);
  CHECK(files_check_out(&l));

  /* And back: nothing's left on the second disc, so the album hasn't one. */
  CHECK(mc_track_tags_get(l.lib, second, &t) == MC_OK);
  t.disc_no = 1;
  t.track_no = 2;
  CHECK(mc_track_tags_set(l.lib, second, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK(q_int(&l, "SELECT disc_total FROM albums WHERE uid = ?1", album, NULL) == 1);
  CHECK_STR(path_of(&l, second), "Ann/Taken (2000)/02 Two.wav");
  CHECK_STR(path_of(&l, moved), "Ann/Taken (2000)/01 What_ Why_How.wav");
  CHECK(files_check_out(&l));
  lib_close(&l);

  /* Two tracks trading places: each one's new name is the other's old one. */
  lib_open(&l, base, "trade");
  static const char *const same_titles[] = {"Same", "Same"};
  album_spec s3 = {"Album", "Ann", ANN, NULL, 2000, 2, same_titles};
  add_album(&l, &s3, album);
  char t1[MC_UID_STRLEN], t2[MC_UID_STRLEN];
  snprintf(t1, sizeof t1, "%s", track_at(&l, album, 1));
  snprintf(t2, sizeof t2, "%s", track_at(&l, album, 2));
  mc_tags mark = {0};
  mark.genre = "first";
  CHECK(mc_tags_write(abs_of(&l, path_of(&l, t1)), &mark) == MC_OK);
  int64_t size, mtime;
  CHECK(mc_file_info(abs_of(&l, path_of(&l, t1)), &size, &mtime) == MC_OK);
  char sql[256];
  snprintf(sql, sizeof sql, "UPDATE files SET size_bytes = %lld, mtime_ms = %lld WHERE track_uid = ?1",
           (long long)size, (long long)mtime);
  sqlite3_stmt *set = query(&l, sql, t1, NULL);
  if (set) sqlite3_step(set);
  sqlite3_finalize(set);
  CHECK(mc_track_tags_get(l.lib, t1, &t) == MC_OK);
  t.track_no = 2;
  CHECK(mc_track_tags_set(l.lib, t1, &t) == MC_OK);
  mc_track_tags_clear(&t);
  /* For now both are number 2, so the other kept its place and this one took a " (2)". */
  CHECK_STR(path_of(&l, t1), "Ann/Album (2000)/02 Same (2).wav");
  CHECK(mc_track_tags_get(l.lib, t2, &t) == MC_OK);
  t.track_no = 1;
  CHECK(mc_track_tags_set(l.lib, t2, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK_STR(path_of(&l, t2), "Ann/Album (2000)/01 Same.wav");
  CHECK(files_check_out(&l));
  lib_close(&l);
}

/* --- Failures --- */

/* Everything that says what's where, to compare before and after. */
typedef struct state {
  char rows[4096];
  char hashes[3][MC_SHA256_STRLEN];
  int changelog;
} state;

static void state_read(lib_dir *l, const char *album, state *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = query(l, "SELECT a.title, a.year, f.path, f.size_bytes, f.mtime_ms, f.sha256, t.title, t.track_no"
                                " FROM albums a JOIN tracks t ON t.album_uid = a.uid JOIN files f ON f.track_uid = t.uid"
                                " WHERE a.uid = ?1 ORDER BY f.path", album, NULL);
  size_t n = 0;
  int i = 0;
  while (stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    for (int c = 0; c < 8; c++) {
      const char *v = (const char *)sqlite3_column_text(stmt, c);
      n += (size_t)snprintf(out->rows + n, sizeof out->rows - n, "%s|", v ? v : "");
    }
    if (i < 3) snprintf(out->hashes[i++], MC_SHA256_STRLEN, "%s", hash_of(abs_of(l, (const char *)sqlite3_column_text(stmt, 2))));
  }
  sqlite3_finalize(stmt);
  out->changelog = q_int(l, "SELECT count(*) FROM changelog", NULL, NULL);
}

static void check_unchanged(lib_dir *l, const char *album, const state *was) {
  state now;
  state_read(l, album, &now);
  CHECK_STR(now.rows, was->rows);
  for (int i = 0; i < 3; i++) CHECK_STR(now.hashes[i], was->hashes[i]);
  CHECK(now.changelog == was->changelog);
  CHECK(files_check_out(l));
  CHECK(count_strays(l->path) == 0);
  CHECK(q_int(l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 0);
  CHECK(!mc_file_exists(abs_of(l, "Ann/Moved (2000)")));
  CHECK(!mc_file_exists(abs_of(l, "Other")));
}

static mc_status rename_album(lib_dir *l, const char *album) {
  mc_album_tags a;
  CHECK(mc_album_tags_get(l->lib, album, &a) == MC_OK);
  free(a.title);
  a.title = mc_strdup("Moved");
  free(a.artist);
  a.artist = mc_strdup("Other");
  mc_status st = mc_album_tags_set(l->lib, album, &a);
  mc_album_tags_clear(&a);
  return st;
}

static void test_failed_move(const char *base) {
  /* Three files, so six renames: each of them fails in turn. */
  for (int n = 1; n <= 6; n++) {
    lib_dir l;
    lib_open(&l, base, "fail");
    char album[MC_UID_STRLEN];
    album_spec s = {"Album", "Ann", ANN, NULL, 2000, 3, k_titles};
    add_album(&l, &s, album);
    record_hashes(&l);
    state was;
    state_read(&l, album, &was);
    mc_edit_test_fail_rename = n;
    CHECK(rename_album(&l, album) == MC_ERR_IO);
    mc_edit_test_fail_rename = 0;
    check_unchanged(&l, album, &was);
    CHECK(q_int(&l, "SELECT count(*) FROM artists WHERE name = 'Other'", NULL, NULL) == 0);
    /* And it goes through once the file's free. */
    CHECK(rename_album(&l, album) == MC_OK);
    CHECK_STR(path_of(&l, track_at(&l, album, 3)), "Other/Moved (2000)/03 Three.wav");
    CHECK(files_check_out(&l));
    lib_close(&l);
  }
}

static void test_failed_write(const char *base) {
  lib_dir l;
  lib_open(&l, base, "badfile");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  record_hashes(&l);
  state was;
  state_read(&l, album, &was);
  /* The second file's: the first is already copied and tagged by then. */
  mc_edit_test_fail_tags = 2;
  CHECK(rename_album(&l, album) == MC_ERR_IO);
  mc_edit_test_fail_tags = 0;
  check_unchanged(&l, album, &was);
  lib_close(&l);
}

static void test_in_use(const char *base) {
#ifdef _WIN32
  lib_dir l;
  lib_open(&l, base, "inuse");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 3, k_titles};
  add_album(&l, &s, album);
  state was;
  state_read(&l, album, &was);

  /* As the player has it open: the second track of three, to tag or to move. */
  char rel[256];
  snprintf(rel, sizeof rel, "%s", path_of(&l, track_at(&l, album, 2)));
  FILE *playing = mc_fopen(abs_of(&l, rel), "rb");
  CHECK(playing != NULL);
  CHECK(mc_file_in_use(abs_of(&l, rel)));
  CHECK(!mc_file_in_use(abs_of(&l, path_of(&l, track_at(&l, album, 1)))));
  CHECK(rename_album(&l, album) == MC_ERR_IN_USE);
  check_unchanged(&l, album, &was);

  mc_track_tags t;
  char track[MC_UID_STRLEN];
  snprintf(track, sizeof track, "%s", track_at(&l, album, 2));
  CHECK(mc_track_tags_get(l.lib, track, &t) == MC_OK);
  free(t.genre);
  t.genre = mc_strdup("Speedcore");
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_ERR_IN_USE);
  CHECK_STR(q_str(&l, "SELECT genre FROM tracks WHERE uid = ?1", track, NULL), "Noise");

  /* An edit of the track that isn't open isn't held up by it. */
  char first[MC_UID_STRLEN];
  mc_track_tags u;
  snprintf(first, sizeof first, "%s", track_at(&l, album, 1));
  CHECK(mc_track_tags_get(l.lib, first, &u) == MC_OK);
  free(u.genre);
  u.genre = mc_strdup("Gabber");
  CHECK(mc_track_tags_set(l.lib, first, &u) == MC_OK);
  mc_track_tags_clear(&u);

  /* Stopped, the same edit goes through. */
  fclose(playing);
  CHECK(mc_track_tags_set(l.lib, track, &t) == MC_OK);
  mc_track_tags_clear(&t);
  CHECK_STR(q_str(&l, "SELECT genre FROM tracks WHERE uid = ?1", track, NULL), "Speedcore");
  lib_close(&l);
#else
  (void)base;
#endif
}

/* Stopped dead before each rename of the swap, as a crash would stop it: the next open puts
 * files back, rows being as they were. After the commit: test_crash_after(). */
static void test_crash(const char *base) {
  for (int n = 1; n <= 6; n++) {
    lib_dir l;
    lib_open(&l, base, "crash");
    char album[MC_UID_STRLEN];
    album_spec s = {"Album", "Ann", ANN, NULL, 2000, 3, k_titles};
    add_album(&l, &s, album);
    record_hashes(&l);
    state was;
    state_read(&l, album, &was);
    mc_edit_test_abandon_rename = n;
    rename_album(&l, album);
    mc_edit_test_abandon_rename = 0;
    CHECK(q_int(&l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 1);
    mc_library_close(l.lib);
    CHECK(mc_library_open(l.path, &l.lib) == MC_OK);
    check_unchanged(&l, album, &was);
    lib_close(&l);
  }
}

static void test_crash_after(const char *base) {
  lib_dir l;
  lib_open(&l, base, "crash2");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  /* As if it had got as far as the commit: new files in place, the old ones as backups. */
  char rel[256];
  snprintf(rel, sizeof rel, "%s", path_of(&l, track_at(&l, album, 1)));
  char bak[300] = "Ann/Album (2000)/.kithsong-00000000-01 One.wav.old";
  CHECK(mc_copy_file(abs_of(&l, rel), abs_of(&l, bak), 0) == MC_OK);
  char journal[1024];
  snprintf(journal, sizeof journal, "done\n%s\t%s\tAnn/Album (2000)/.kithsong-00000000-01 One.wav\t%s\n", rel, rel, bak);
  CHECK(mc_library_meta_set(l.lib, "edit_swap", journal) == MC_OK);
  mc_library_close(l.lib);
  CHECK(mc_library_open(l.path, &l.lib) == MC_OK);
  CHECK(!mc_file_exists(abs_of(&l, bak)));
  CHECK(mc_file_exists(abs_of(&l, rel)));
  CHECK(q_int(&l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 0);
  lib_close(&l);
}

/* --- Releases --- */

static int g_lookups, g_searches, g_cover_fetches;
static mc_status g_lookup_status = MC_OK;

/* Release NEW: two tracks of the three the album has, retitled and renumbered, on two discs,
 * credited to two artists. */
static const char k_new_json[] =
  "{\"id\":\"" REL_NEW "\",\"title\":\"The Real Album\",\"date\":\"2011-08-08\",\"country\":\"JP\","
  "\"artist-credit\":["
  "{\"name\":\"Ann\",\"joinphrase\":\" & \",\"artist\":{\"id\":\"" ANN "\",\"name\":\"Ann\",\"sort-name\":\"Ann\"}},"
  "{\"name\":\"Bob\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" BOB "\",\"name\":\"Bob\",\"sort-name\":\"Bob\"}}],"
  "\"release-group\":{\"id\":\"" RG_NEW "\",\"primary-type\":\"Album\",\"first-release-date\":\"2009\"},"
  "\"media\":[{\"position\":1,\"format\":\"CD\",\"tracks\":["
  "{\"id\":\"31111111-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"Two\","
  "\"artist-credit\":[{\"name\":\"Ann\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" ANN "\",\"name\":\"Ann\"}}],"
  "\"recording\":{\"id\":\"41111111-2222-3333-4444-555555555555\"}}]},"
  "{\"position\":2,\"format\":\"CD\",\"tracks\":["
  "{\"id\":\"32222222-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"One!\","
  "\"artist-credit\":[{\"name\":\"Ann\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" ANN "\",\"name\":\"Ann\"}},"
  "{\"name\":\"Bob\",\"artist\":{\"id\":\"" BOB "\",\"name\":\"Bob\"}}],"
  "\"recording\":{\"id\":\"42222222-2222-3333-4444-555555555555\"}}]}]}";

/* A release that fits badly. */
static const char k_poor_json[] =
  "{\"id\":\"" REL_B "\",\"title\":\"Something Else\",\"date\":\"1980\","
  "\"artist-credit\":[{\"name\":\"Nobody\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" NOBODY "\",\"name\":\"Nobody\"}}],"
  "\"media\":[{\"position\":1,\"tracks\":["
  "{\"id\":\"33333333-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"Elsewhere\","
  "\"artist-credit\":[{\"name\":\"Nobody\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" NOBODY "\",\"name\":\"Nobody\"}}],"
  "\"recording\":{\"id\":\"43333333-2222-3333-4444-555555555555\"}}]}]}";

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  g_lookups++;
  if (g_lookup_status != MC_OK) return g_lookup_status;
  if (!strcmp(id, REL_NEW)) return mb_release_parse(k_new_json, sizeof k_new_json - 1, out);
  if (!strcmp(id, REL_B)) return mb_release_parse(k_poor_json, sizeof k_poor_json - 1, out);
  return MC_ERR_NOT_FOUND;
}

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist, (void)album, (void)track_count;
  g_searches++;
  if (g_lookup_status != MC_OK) return g_lookup_status;
  /* Worst first: the score puts them right. */
  *n = max < 2 ? max : 2;
  strcpy(ids[0], REL_B);
  strcpy(ids[1], REL_NEW);
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, NULL};

static mc_status fake_cover(const char *mb_albumid, const char *mb_releasegroupid, const char *artist,
                            const char *album, mc_buffer *img, const char **ext) {
  (void)artist, (void)album;
  g_cover_fetches++;
  CHECK_STR(mb_albumid, REL_NEW);
  CHECK_STR(mb_releasegroupid, RG_NEW);
  img->data = mc_strdup("the new cover");
  img->len = strlen(img->data);
  *ext = "jpg";
  return MC_OK;
}

static void test_search(const char *base) {
  lib_dir l;
  lib_open(&l, base, "search");
  char album[MC_UID_STRLEN];
  static const char *const titles[] = {"One", "Two"};
  album_spec s = {"The Real Album", "Ann", ANN, NULL, 2011, 2, titles};
  add_album(&l, &s, album);
  int rows = q_int(&l, "SELECT count(*) FROM changelog", NULL, NULL);

  mc_release_list *list = NULL;
  CHECK(mc_release_search(l.lib, album, "Ann", "The Real Album", &list) == MC_OK);
  CHECK(list && list->count == 2);
  if (list && list->count == 2) {
    CHECK_STR(list->items[0].mb_albumid, REL_NEW);
    CHECK_STR(list->items[0].title, "The Real Album");
    CHECK_STR(list->items[0].artist, "Ann & Bob");
    CHECK(list->items[0].year == 2011);
    CHECK_STR(list->items[0].country, "JP");
    CHECK_STR(list->items[0].format, "2\xC3\x97" "CD");
    CHECK(list->items[0].track_count == 2);
    CHECK(list->items[0].score > list->items[1].score);
    CHECK(list->items[0].score > 0.5 && list->items[0].score <= 1.0);
    CHECK(list->items[1].score >= 0.0);
    CHECK(list->items[1].country == NULL);
  }
  mc_release_list_free(list);

  /* Nothing reached, nothing to show. */
  g_lookup_status = MC_ERR_NET;
  CHECK(mc_release_search(l.lib, album, "Ann", "The Real Album", &list) == MC_ERR_NET);
  mc_album_tags t;
  CHECK(mc_release_tags(l.lib, album, REL_NEW, &t) == MC_ERR_NET);
  g_lookup_status = MC_OK;
  CHECK(mc_release_search(l.lib, "00000000-0000-0000-0000-000000000000", "Ann", "x", &list) == MC_ERR_NOT_FOUND);
  CHECK(mc_release_search(l.lib, album, "Ann", "", &list) == MC_ERR_INVALID_ARG);

  /* The form's tags: nothing changes. */
  CHECK(mc_release_tags(l.lib, album, REL_NEW, &t) == MC_OK);
  CHECK_STR(t.title, "The Real Album");
  CHECK_STR(t.artist, "Ann & Bob");
  CHECK(t.year == 2011 && t.original_year == 2009);
  CHECK_STR(t.mb_albumid, REL_NEW);
  mc_album_tags_clear(&t);
  CHECK(mc_release_tags(l.lib, album, "99999999-2222-3333-4444-555555555555", &t) == MC_ERR_NOT_FOUND);
  CHECK(q_int(&l, "SELECT count(*) FROM changelog", NULL, NULL) == rows);
  lib_close(&l);
}

static void test_rematch(const char *base) {
  lib_dir l;
  lib_open(&l, base, "rematch");
  char album[MC_UID_STRLEN];
  static const char *const titles[] = {"One", "Two", "Unrelated Thing"};
  album_spec s = {"Old Title", "Ann", NULL, REL_A, 2000, 3, titles};
  add_album(&l, &s, album);
  char one[MC_UID_STRLEN], two[MC_UID_STRLEN], lost[MC_UID_STRLEN];
  snprintf(one, sizeof one, "%s", track_at(&l, album, 1));
  snprintf(two, sizeof two, "%s", track_at(&l, album, 2));
  snprintf(lost, sizeof lost, "%s", track_at(&l, album, 3));
  record_hashes(&l);

  /* As a form filled by mc_release_tags() would, or a caller just changing the id. */
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  CHECK_STR(a.mb_albumid, REL_A);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup(REL_NEW);
  g_cover_fetches = 0;
  mc_coverart_set_fake(fake_cover);
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);

  /* The album is the release's. */
  CHECK_STR(q_str(&l, "SELECT title FROM albums WHERE uid = ?1", album, NULL), "The Real Album");
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM albums WHERE uid = ?1", album, NULL), "Ann & Bob");
  CHECK(q_int(&l, "SELECT year FROM albums WHERE uid = ?1", album, NULL) == 2011);
  CHECK(q_int(&l, "SELECT original_year FROM albums WHERE uid = ?1", album, NULL) == 2009);
  CHECK(q_int(&l, "SELECT disc_total FROM albums WHERE uid = ?1", album, NULL) == 2);
  CHECK_STR(q_str(&l, "SELECT mb_albumid FROM albums WHERE uid = ?1", album, NULL), REL_NEW);
  CHECK_STR(q_str(&l, "SELECT mb_releasegroupid FROM albums WHERE uid = ?1", album, NULL), RG_NEW);
  CHECK(logged(&l, "albums", album, "mb_albumid") == 2);
  /* Ann has the release's id now, found by name as an import finds her. */
  CHECK_STR(q_str(&l, "SELECT mb_artistid FROM artists WHERE name = 'Ann'", NULL, NULL), ANN);

  /* Each track paired by what it is, not by where it was. */
  CHECK_STR(q_str(&l, "SELECT title FROM tracks WHERE uid = ?1", two, NULL), "Two");
  CHECK(q_int(&l, "SELECT disc_no FROM tracks WHERE uid = ?1", two, NULL) == 1);
  CHECK(q_int(&l, "SELECT track_no FROM tracks WHERE uid = ?1", two, NULL) == 1);
  CHECK(q_int(&l, "SELECT year FROM tracks WHERE uid = ?1", two, NULL) == 2011);
  CHECK_STR(q_str(&l, "SELECT mb_trackid FROM tracks WHERE uid = ?1", two, NULL), "41111111-2222-3333-4444-555555555555");
  CHECK_STR(q_str(&l, "SELECT mb_releasetrackid FROM tracks WHERE uid = ?1", two, NULL), "31111111-2222-3333-4444-555555555555");
  CHECK_STR(q_str(&l, "SELECT mb_albumid FROM tracks WHERE uid = ?1", two, NULL), REL_NEW);
  CHECK_STR(q_str(&l, "SELECT title FROM tracks WHERE uid = ?1", one, NULL), "One!");
  CHECK(q_int(&l, "SELECT disc_no FROM tracks WHERE uid = ?1", one, NULL) == 2);
  CHECK_STR(q_str(&l, "SELECT artist_credit FROM tracks WHERE uid = ?1", one, NULL), "Ann feat. Bob");
  CHECK(q_int(&l, "SELECT count(*) FROM track_artists WHERE track_uid = ?1", one, NULL) == 2);

  /* One that isn't on the release keeps its tags; only its album's have changed. */
  CHECK_STR(q_str(&l, "SELECT title FROM tracks WHERE uid = ?1", lost, NULL), "Unrelated Thing");
  CHECK(q_int(&l, "SELECT track_no FROM tracks WHERE uid = ?1", lost, NULL) == 3);
  CHECK_STR(q_str(&l, "SELECT coalesce(mb_albumid, '') FROM tracks WHERE uid = ?1", lost, NULL), REL_A);
  CHECK(q_int(&l, "SELECT year FROM tracks WHERE uid = ?1", lost, NULL) == 2000);

  /* The files follow, and the cover came. */
  CHECK_STR(path_of(&l, one), "Ann & Bob/The Real Album (2011)/2-01 One!.wav");
  CHECK_STR(path_of(&l, two), "Ann & Bob/The Real Album (2011)/1-01 Two.wav");
  CHECK_STR(path_of(&l, lost), "Ann & Bob/The Real Album (2011)/1-03 Unrelated Thing.wav");
  mc_tags tags;
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, one)), &tags) == MC_OK);
  CHECK_STR(tags.title, "One!");
  CHECK_STR(tags.album, "The Real Album");
  CHECK_STR(tags.album_artist, "Ann & Bob");
  CHECK_STR(tags.artist, "Ann feat. Bob");
  CHECK_STR(tags.mb_albumid, REL_NEW);
  CHECK_STR(tags.mb_releasegroupid, RG_NEW);
  CHECK(tags.disc == 2 && tags.disc_total == 2 && tags.track == 1);
  CHECK(tags.year == 2011 && tags.original_year == 2009);
  mc_tags_clear(&tags);
  CHECK(files_check_out(&l));
  CHECK(!mc_file_exists(abs_of(&l, "Ann/Old Title (2000)")));
  CHECK(g_cover_fetches == 1);
  CHECK(mc_file_exists(abs_of(&l, "Ann & Bob/The Real Album (2011)/cover.jpg")));
  CHECK(q_int(&l, "SELECT art_hash IS NOT NULL FROM albums WHERE uid = ?1", album, NULL) == 1);

  /* What the caller typed wins over the release; a cover already there stays. */
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup(REL_B);
  free(a.title);
  a.title = mc_strdup("My Title");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK_STR(q_str(&l, "SELECT title FROM albums WHERE uid = ?1", album, NULL), "My Title");
  CHECK_STR(q_str(&l, "SELECT coalesce(a.artist_credit, ar.name) FROM albums a JOIN artists ar ON ar.uid = a.artist_uid"
                      " WHERE a.uid = ?1", album, NULL), "Nobody");
  CHECK(g_cover_fetches == 1);

  /* Back to an earlier release: this album's own history is no conflict. But each track has
   * the other's title by now, and pairing them that way would have sync merge the two, each
   * having held the id the other would get. So they keep what they have. */
  mc_track_tags t;
  const char *const swapped[2][2] = {{one, "Two"}, {two, "One!"}};
  for (int i = 0; i < 2; i++) {
    CHECK(mc_track_tags_get(l.lib, swapped[i][0], &t) == MC_OK);
    free(t.title);
    t.title = mc_strdup(swapped[i][1]);
    CHECK(mc_track_tags_set(l.lib, swapped[i][0], &t) == MC_OK);
    mc_track_tags_clear(&t);
  }
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup(REL_NEW);
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK_STR(q_str(&l, "SELECT mb_albumid FROM albums WHERE uid = ?1", album, NULL), REL_NEW);
  CHECK_STR(q_str(&l, "SELECT mb_releasetrackid FROM tracks WHERE uid = ?1", one, NULL), "32222222-2222-3333-4444-555555555555");
  CHECK_STR(q_str(&l, "SELECT mb_releasetrackid FROM tracks WHERE uid = ?1", two, NULL), "31111111-2222-3333-4444-555555555555");
  CHECK_STR(q_str(&l, "SELECT title FROM tracks WHERE uid = ?1", one, NULL), "Two");

  /* Unlinked: the release's ids go, its tags stay. */
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup("");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_OK);
  mc_album_tags_clear(&a);
  CHECK(q_int(&l, "SELECT mb_albumid IS NULL FROM albums WHERE uid = ?1", album, NULL) == 1);
  CHECK(q_int(&l, "SELECT mb_releasegroupid IS NULL FROM albums WHERE uid = ?1", album, NULL) == 1);
  CHECK(q_int(&l, "SELECT mb_releasetrackid IS NULL FROM tracks WHERE uid = ?1", two, NULL) == 1);
  CHECK(q_int(&l, "SELECT mb_trackid IS NOT NULL FROM tracks WHERE uid = ?1", two, NULL) == 1);
  CHECK(mc_tags_read(abs_of(&l, path_of(&l, two)), &tags) == MC_OK);
  CHECK(!tags.mb_albumid);
  mc_tags_clear(&tags);
  mc_coverart_set_fake(NULL);
  lib_close(&l);
}

static void test_rematch_refused(const char *base) {
  lib_dir l;
  lib_open(&l, base, "refused");
  char album[MC_UID_STRLEN], other[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  static const char *const others[] = {"Other"};
  album_spec s2 = {"Other", "Ann", ANN, REL_NEW, 2000, 1, others};
  add_album(&l, &s2, other);
  state was;
  state_read(&l, album, &was);

  /* Another album has that release: refused before anything is asked of MusicBrainz. */
  mc_album_tags a;
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup(REL_NEW);
  g_lookups = 0;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_ERR_EXISTS);
  CHECK(g_lookups == 0);
  check_unchanged(&l, album, &was);

  /* Had that release once: sync would still take the two for one. */
  mc_album_tags b;
  CHECK(mc_album_tags_get(l.lib, other, &b) == MC_OK);
  free(b.mb_albumid);
  b.mb_albumid = mc_strdup("");
  CHECK(mc_album_tags_set(l.lib, other, &b) == MC_OK);
  mc_album_tags_clear(&b);
  state_read(&l, album, &was);
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_ERR_EXISTS);
  check_unchanged(&l, album, &was);
  mc_album_tags_clear(&a);

  /* No such release, or not an id at all, or offline: nothing changes. */
  CHECK(mc_album_tags_get(l.lib, album, &a) == MC_OK);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup("99999999-2222-3333-4444-555555555555");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_ERR_NOT_FOUND);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup("not an id");
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_ERR_INVALID_ARG);
  free(a.mb_albumid);
  a.mb_albumid = mc_strdup(REL_B);
  g_lookup_status = MC_ERR_NET;
  CHECK(mc_album_tags_set(l.lib, album, &a) == MC_ERR_NET);
  g_lookup_status = MC_OK;
  mc_album_tags_clear(&a);
  check_unchanged(&l, album, &was);
  lib_close(&l);
}

/* --- Covers --- */

static void write_image(const char *path, const test_image *t) {
  FILE *f = mc_fopen(path, "wb");
  CHECK(f != NULL);
  if (f) {
    fwrite(t->data, 1, t->len, f);
    fclose(f);
  }
}

static char *sha_of_image(lib_dir *l, const test_image *t) {
  char *path = abs_of(l, "image.tmp");
  write_image(path, t);
  char *sha = hash_of(path);
  mc_remove(path);
  return sha;
}

static void test_art_check(const char *base) {
  test_image jpg = test_image_make(0, 1, 32), png = test_image_make(1, 2, 32);
  CHECK(mc_album_art_check(jpg.data, jpg.len) == MC_OK);
  CHECK(mc_album_art_check(png.data, png.len) == MC_OK);
  CHECK(mc_album_art_check(NULL, 0) == MC_ERR_INVALID_ARG);

  /* Not an image, another kind of image, the right first bytes with nothing behind them, half
   * a PNG. */
  CHECK(mc_album_art_check("hello, this is plain text", 25) == MC_ERR_FORMAT);
  CHECK(mc_album_art_check("GIF89a\x01\0\x01\0\0\0\0;", 14) == MC_ERR_FORMAT);
  CHECK(mc_album_art_check("\xFF\xD8\xFF\xE0 and then rubbish", 22) == MC_ERR_FORMAT);
  CHECK(mc_album_art_check(png.data, png.len / 2) == MC_ERR_FORMAT);

  /* Too many bytes, however good; too many pixels, however few bytes. */
  size_t big = (size_t)MC_ALBUM_ART_MAX_BYTES + 1;
  unsigned char *over = calloc(1, big);
  CHECK(over != NULL);
  if (over) {
    memcpy(over, png.data, png.len);
    CHECK(mc_album_art_check(over, big) == MC_ERR_FORMAT);
    free(over);
  }
  unsigned char wide[64];
  memcpy(wide, png.data, sizeof wide);
  wide[18] = (MC_ALBUM_ART_MAX_SIDE + 1) >> 8; /* IHDR's width, big-endian: the CRC stays wrong, */
  wide[19] = (MC_ALBUM_ART_MAX_SIDE + 1) & 255; /* which only the decoder would look at */
  CHECK(mc_album_art_check(wide, sizeof wide) == MC_ERR_FORMAT);

  lib_dir l;
  lib_open(&l, base, "artcheck");
  write_image(abs_of(&l, "good.png"), &png);
  write_bytes(abs_of(&l, "bad.png"), "not a picture");
  CHECK(mc_album_art_check_file(abs_of(&l, "good.png")) == MC_OK);
  CHECK(mc_album_art_check_file(abs_of(&l, "bad.png")) == MC_ERR_FORMAT);
  CHECK(mc_album_art_check_file(abs_of(&l, "none.png")) != MC_OK);
  lib_close(&l);
  free(jpg.data);
  free(png.data);
}

static void test_art_set(const char *base) {
  lib_dir l, other;
  lib_open(&l, base, "art");
  lib_open(&other, base, "art2");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(&l, &s, album);
  record_hashes(&l);
  const char *dir = "Ann/Album (2000)";
  char music[MC_SHA256_STRLEN];
  snprintf(music, sizeof music, "%s", hash_of(abs_of(&l, path_of(&l, track_at(&l, album, 1)))));
  int file_rev = q_int(&l, "SELECT file_rev FROM tracks WHERE album_uid = ?1 LIMIT 1", album, NULL);

  char *shown = (char *)"unset";
  CHECK(mc_album_art_get(l.lib, album, &shown) == MC_OK && shown == NULL);

  /* A folder with none. Rows follow, logged, and the music is left alone. */
  test_image jpg = test_image_make(0, 1, 32), png = test_image_make(1, 2, 32), jpg2 = test_image_make(0, 3, 32);
  char want[MC_SHA256_STRLEN];
  snprintf(want, sizeof want, "%s", sha_of_image(&l, &jpg));
  CHECK(mc_album_art_set(l.lib, album, jpg.data, jpg.len) == MC_OK);
  CHECK_STR(hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")), want);
  CHECK(mc_album_art_get(l.lib, album, &shown) == MC_OK && shown);
  if (shown) CHECK_STR(hash_of(shown), want);
  mc_free(shown);
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);
  CHECK_STR(q_str(&l, "SELECT art_chosen FROM albums WHERE uid = ?1", album, NULL), want);
  CHECK(logged(&l, "albums", album, "art_hash"));
  CHECK(logged(&l, "albums", album, "art_chosen"));
  CHECK_STR(hash_of(abs_of(&l, path_of(&l, track_at(&l, album, 1)))), music);
  CHECK(q_int(&l, "SELECT file_rev FROM tracks WHERE album_uid = ?1 LIMIT 1", album, NULL) == file_rev);
  CHECK(files_check_out(&l));
  CHECK(count_strays(l.path) == 0);
  CHECK(q_int(&l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 0);

  /* A second library hears of it as of any change. */
  sync_rows(&l, &other);
  CHECK_STR(q_str(&other, "SELECT art_chosen FROM albums WHERE uid = ?1", album, NULL), want);
  CHECK_STR(q_str(&other, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);

  /* A PNG in place of a JPEG: the JPEG would otherwise go on being found first. */
  snprintf(want, sizeof want, "%s", sha_of_image(&l, &png));
  CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_OK);
  CHECK(!mc_file_exists(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
  char *found = mc_coverart_find(abs_of(&l, dir));
  CHECK(found && strstr(found, "cover.png"));
  CHECK_STR(hash_of(found), want);
  mc_free(found);
  CHECK_STR(q_str(&l, "SELECT art_chosen FROM albums WHERE uid = ?1", album, NULL), want);

  /* Older ones of other names go too, but for folder.jpg and front.jpg, the user's, ranked below;
   * and the same image again is fine. */
  write_bytes(abs_of(&l, "Ann/Album (2000)/cover.jpeg"), "old");
  write_bytes(abs_of(&l, "Ann/Album (2000)/folder.jpg"), "old");
  CHECK(mc_album_art_set(l.lib, album, jpg2.data, jpg2.len) == MC_OK);
  CHECK(mc_album_art_set(l.lib, album, jpg2.data, jpg2.len) == MC_OK);
  CHECK(mc_file_exists(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
  CHECK(!mc_file_exists(abs_of(&l, "Ann/Album (2000)/cover.jpeg")));
  CHECK(!mc_file_exists(abs_of(&l, "Ann/Album (2000)/cover.png")));
  CHECK(mc_file_exists(abs_of(&l, "Ann/Album (2000)/folder.jpg")));
  mc_remove(abs_of(&l, "Ann/Album (2000)/folder.jpg"));

  /* From a file, which stays where it is. A bad image changes nothing. */
  write_image(abs_of(&l, "picked.png"), &png);
  snprintf(want, sizeof want, "%s", sha_of_image(&l, &png));
  CHECK(mc_album_art_set_file(l.lib, album, abs_of(&l, "picked.png")) == MC_OK);
  CHECK(mc_file_exists(abs_of(&l, "picked.png")));
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);
  CHECK(mc_album_art_set(l.lib, album, "not a picture", 13) == MC_ERR_FORMAT);
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);

  /* An album in two folders gets it in both. */
  char *t2 = track_at(&l, album, 2);
  char from[256];
  snprintf(from, sizeof from, "%s", path_of(&l, t2));
  CHECK(mc_mkdirs(abs_of(&l, "Ann/Album (2000)/Extra")) == MC_OK);
  CHECK(mc_rename(abs_of(&l, from), abs_of(&l, "Ann/Album (2000)/Extra/02 Two.wav"), 0) == MC_OK);
  CHECK(mc_library_begin_write(l.lib) == MC_OK);
  sqlite3_stmt *stmt = query(&l, "UPDATE files SET path = 'Ann/Album (2000)/Extra/02 Two.wav' WHERE path = ?1", from, NULL);
  CHECK(stmt && sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  CHECK(mc_library_commit(l.lib) == MC_OK);
  snprintf(want, sizeof want, "%s", sha_of_image(&l, &jpg));
  CHECK(mc_album_art_set(l.lib, album, jpg.data, jpg.len) == MC_OK);
  CHECK_STR(hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")), want);
  CHECK_STR(hash_of(abs_of(&l, "Ann/Album (2000)/Extra/cover.jpg")), want);

  /* Nowhere to put one. */
  CHECK(mc_album_art_set(l.lib, "nope", jpg.data, jpg.len) == MC_ERR_NOT_FOUND);
  char album2[MC_UID_STRLEN];
  album_spec s2 = {"Far", "Ann", ANN, NULL, 2001, 1, k_titles};
  add_album(&l, &s2, album2);
  mc_remove(abs_of(&l, "Ann/Far (2001)/01 One.wav"));
  CHECK(mc_album_art_set(l.lib, album2, jpg.data, jpg.len) == MC_ERR_NOT_FOUND);
  CHECK(mc_album_art_get(l.lib, album2, &shown) == MC_ERR_NOT_FOUND);
  CHECK(q_int(&l, "SELECT art_chosen IS NULL FROM albums WHERE uid = ?1", album2, NULL) == 1);

  free(jpg.data);
  free(png.data);
  free(jpg2.data);
  lib_close(&l);
  lib_close(&other);
}

/* An album with a JPEG cover in one folder and none in another, about to take a PNG: one rename
 * to a backup, and two into place. */
static void art_failing_album(lib_dir *l, const char *name, const char *base, char album[MC_UID_STRLEN],
                              const test_image *old) {
  lib_open(l, base, name);
  album_spec s = {"Album", "Ann", ANN, NULL, 2000, 2, k_titles};
  add_album(l, &s, album);
  write_image(abs_of(l, "Ann/Album (2000)/cover.jpg"), old);
  char from[256];
  snprintf(from, sizeof from, "%s", path_of(l, track_at(l, album, 2)));
  CHECK(mc_mkdirs(abs_of(l, "Ann/Album (2000)/Extra")) == MC_OK);
  CHECK(mc_rename(abs_of(l, from), abs_of(l, "Ann/Album (2000)/Extra/02 Two.wav"), 0) == MC_OK);
  CHECK(mc_library_begin_write(l->lib) == MC_OK);
  sqlite3_stmt *stmt = query(l, "UPDATE files SET path = 'Ann/Album (2000)/Extra/02 Two.wav' WHERE path = ?1", from, NULL);
  CHECK(stmt && sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  CHECK(mc_library_commit(l->lib) == MC_OK);
}

static void art_unchanged(lib_dir *l, const char *album, const char *old_sha) {
  CHECK_STR(hash_of(abs_of(l, "Ann/Album (2000)/cover.jpg")), old_sha);
  CHECK(!mc_file_exists(abs_of(l, "Ann/Album (2000)/cover.png")));
  CHECK(!mc_file_exists(abs_of(l, "Ann/Album (2000)/Extra/cover.png")));
  CHECK(q_int(l, "SELECT art_chosen IS NULL AND art_hash IS NULL FROM albums WHERE uid = ?1", album, NULL) == 1);
  CHECK(count_strays(l->path) == 0);
  CHECK(q_int(l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 0);
}

static void test_art_failures(const char *base) {
  test_image old = test_image_make(0, 1, 32), png = test_image_make(1, 2, 32);
  char album[MC_UID_STRLEN], old_sha[MC_SHA256_STRLEN];

  /* Each rename fails in turn; the second copy can't be written; a crash stops each rename. */
  for (int n = 1; n <= 3; n++) {
    lib_dir l;
    art_failing_album(&l, "artfail", base, album, &old);
    snprintf(old_sha, sizeof old_sha, "%s", hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
    mc_edit_test_fail_rename = n;
    CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_ERR_IO);
    mc_edit_test_fail_rename = 0;
    art_unchanged(&l, album, old_sha);
    CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_OK);
    CHECK(mc_file_exists(abs_of(&l, "Ann/Album (2000)/Extra/cover.png")));
    CHECK(!mc_file_exists(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
    lib_close(&l);
  }
  {
    lib_dir l;
    art_failing_album(&l, "artfail", base, album, &old);
    snprintf(old_sha, sizeof old_sha, "%s", hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
    mc_edit_test_fail_tags = 2;
    CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_ERR_IO);
    mc_edit_test_fail_tags = 0;
    art_unchanged(&l, album, old_sha);
    lib_close(&l);
  }
  for (int n = 1; n <= 3; n++) {
    lib_dir l;
    art_failing_album(&l, "artcrash", base, album, &old);
    snprintf(old_sha, sizeof old_sha, "%s", hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
    mc_edit_test_abandon_rename = n;
    mc_album_art_set(l.lib, album, png.data, png.len);
    mc_edit_test_abandon_rename = 0;
    CHECK(q_int(&l, "SELECT count(*) FROM meta WHERE key = 'edit_swap'", NULL, NULL) == 1);
    mc_library_close(l.lib);
    CHECK(mc_library_open(l.path, &l.lib) == MC_OK);
    art_unchanged(&l, album, old_sha);
    lib_close(&l);
  }
#ifdef _WIN32
  {
    lib_dir l;
    art_failing_album(&l, "artopen", base, album, &old);
    snprintf(old_sha, sizeof old_sha, "%s", hash_of(abs_of(&l, "Ann/Album (2000)/cover.jpg")));
    FILE *open = mc_fopen(abs_of(&l, "Ann/Album (2000)/cover.jpg"), "rb");
    CHECK(open != NULL);
    CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_ERR_IN_USE);
    if (open) fclose(open);
    art_unchanged(&l, album, old_sha);
    CHECK(mc_album_art_set(l.lib, album, png.data, png.len) == MC_OK);
    lib_close(&l);
  }
#endif
  free(old.data);
  free(png.data);
}

/* A cover the user chose outlasts what would otherwise replace art. */
static void test_art_kept(const char *base) {
  lib_dir l;
  lib_open(&l, base, "artkept");
  char album[MC_UID_STRLEN];
  album_spec s = {"Album", "Ann", ANN, REL_A, 2000, 2, k_titles};
  add_album(&l, &s, album);
  test_image jpg = test_image_make(0, 1, 32);
  CHECK(mc_album_art_set(l.lib, album, jpg.data, jpg.len) == MC_OK);
  char want[MC_SHA256_STRLEN];
  snprintf(want, sizeof want, "%s", q_str(&l, "SELECT art_chosen FROM albums WHERE uid = ?1", album, NULL));

  /* An import writing to it, as a whole release swapped for a better one does, brings other art. */
  mc_db_album a = {0};
  a.title = "Album";
  a.artist = (mc_db_artist){"Ann", NULL, ANN};
  a.year = 2000;
  a.disc_total = 1;
  a.mb_albumid = REL_A;
  a.art_hash = "0000000000000000000000000000000000000000000000000000000000000000";
  char uid[MC_UID_STRLEN], hash[65];
  CHECK(mc_import_db_chosen_art(l.lib, &a, uid, hash) == 1);
  CHECK_STR(uid, album);
  CHECK_STR(hash, want);
  CHECK(mc_import_db_write(l.lib, &a, NULL, 0) == MC_OK);
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);
  a.existing_uid = album;
  a.update_existing = 1;
  CHECK(mc_import_db_write(l.lib, &a, NULL, 0) == MC_OK);
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);

  /* Found by name too, as an album with no release is; and another album isn't. */
  a.existing_uid = NULL;
  a.update_existing = 0;
  a.mb_albumid = NULL;
  a.artist = (mc_db_artist){"ann", NULL, NULL};
  a.title = "ALBUM";
  CHECK(mc_import_db_chosen_art(l.lib, &a, uid, hash) == 1);
  a.title = "Another";
  CHECK(mc_import_db_chosen_art(l.lib, &a, uid, hash) == 0);

  /* Matching it to another release looks for no cover. */
  g_cover_fetches = 0;
  mb_set_fake(&k_fake);
  mc_coverart_set_fake(fake_cover);
  mc_album_tags t;
  CHECK(mc_album_tags_get(l.lib, album, &t) == MC_OK);
  free(t.mb_albumid);
  t.mb_albumid = mc_strdup(REL_NEW);
  CHECK(mc_album_tags_set(l.lib, album, &t) == MC_OK);
  mc_album_tags_clear(&t);
  mb_set_fake(NULL);
  mc_coverart_set_fake(NULL);
  CHECK(g_cover_fetches == 0);
  CHECK_STR(q_str(&l, "SELECT art_hash FROM albums WHERE uid = ?1", album, NULL), want);
  char *cover = NULL;
  CHECK(mc_album_art_get(l.lib, album, &cover) == MC_OK && cover);
  if (cover) CHECK_STR(hash_of(cover), want);
  mc_free(cover);

  free(jpg.data);
  lib_close(&l);
}

int main(int argc, char **argv) {
  const char *base = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  test_rows(base);
  test_credits(base);
  test_artist(base);
  test_file_tags(base);
  test_left_alone(base);
  test_filing(base);
  test_failed_move(base);
  test_failed_write(base);
  test_in_use(base);
  test_crash(base);
  test_crash_after(base);
  test_search(base);
  test_rematch(base);
  test_rematch_refused(base);
  test_art_check(base);
  test_art_set(base);
  test_art_failures(base);
  test_art_kept(base);
  mb_set_fake(NULL);
  mc_shutdown();
  return TEST_RESULT();
}
