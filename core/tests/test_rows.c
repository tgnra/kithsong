#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/rows.h"
#include "test.h"
#include "util/platform.h"

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

/* The text in a one-value query, or "" (NULL reads as "<null>"). */
static const char *query_text(sqlite3 *db, const char *sql) {
  static char buf[256];
  buf[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return buf;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(buf, sizeof buf, "%s", v ? v : "<null>");
  }
  sqlite3_finalize(stmt);
  return buf;
}

/* --- Rows built by hand --- */

static mc_row text_row(uint64_t hlc, const char *origin, const char *entity, const char *uid,
                       const char *field, const char *text) {
  mc_row r;
  memset(&r, 0, sizeof r);
  r.hlc = hlc;
  snprintf(r.origin, sizeof r.origin, "%s", origin);
  snprintf(r.entity, sizeof r.entity, "%s", entity);
  snprintf(r.row_uid, sizeof r.row_uid, "%s", uid);
  snprintf(r.field, sizeof r.field, "%s", field);
  if (text) {
    r.tag = MC_VAL_TEXT;
    r.data = (const unsigned char *)text;
    r.len = (uint32_t)strlen(text);
  }
  return r;
}

static mc_row int_row(uint64_t hlc, const char *origin, const char *entity, const char *uid,
                      const char *field, int64_t v) {
  mc_row r = text_row(hlc, origin, entity, uid, field, NULL);
  r.tag = MC_VAL_INT;
  r.i = v;
  return r;
}

static int rows_equal(const mc_row *a, const mc_row *b) {
  if (a->hlc != b->hlc || strcmp(a->origin, b->origin) || strcmp(a->entity, b->entity) ||
      strcmp(a->row_uid, b->row_uid) || strcmp(a->field, b->field) || a->tag != b->tag)
    return 0;
  switch (a->tag) {
  case MC_VAL_INT: return a->i == b->i;
  case MC_VAL_REAL: return a->r == b->r;
  case MC_VAL_TEXT:
  case MC_VAL_BLOB: return a->len == b->len && (!a->len || !memcmp(a->data, b->data, a->len));
  default: return 1;
  }
}

static void test_encoding(void) {
  char o[MC_UID_STRLEN], u[MC_UID_STRLEN];
  mc_uid_new(o);
  mc_uid_new(u);
  static const unsigned char blob[] = {0, 1, 2, 0xff};
  mc_row rows[8];
  rows[0] = text_row(1, o, "tracks", u, "genre", NULL); /* NULL */
  rows[1] = int_row(UINT64_C(0x0123456789abcdef), o, "tracks", u, "track_no", -42);
  rows[2] = int_row(3, o, "tracks", u, "duration_ms", INT64_MAX);
  rows[3] = text_row(4, o, "tracks", u, "field", NULL);
  rows[3].tag = MC_VAL_REAL;
  rows[3].r = -1.5e300;
  rows[4] = text_row(5, o, "tracks", u, "title", "Sunflower \xc3\xa9");
  rows[5] = text_row(6, o, "tracks", u, "title", "");
  rows[6] = text_row(7, o, "devices", u, "public_key", NULL);
  rows[6].tag = MC_VAL_BLOB;
  rows[6].data = blob;
  rows[6].len = sizeof blob;
  rows[7] = rows[6];
  rows[7].len = 0;

  unsigned char buf[4096];
  mc_rows_writer w;
  mc_rows_writer_init(&w, buf, sizeof buf, MC_ROWS_LIBRARY);
  for (int i = 0; i < 8; i++) CHECK(mc_rows_writer_add(&w, &rows[i]) == 1);
  size_t len = mc_rows_writer_finish(&w);
  CHECK(buf[0] == MC_ROWS_LIBRARY && buf[1] == 0 && buf[2] == 8);

  int db = -1, n = 0;
  mc_row *got = NULL;
  CHECK(mc_rows_decode(buf, len, &db, &got, &n) == MC_OK);
  CHECK(db == MC_ROWS_LIBRARY && n == 8);
  for (int i = 0; got && i < n && i < 8; i++) CHECK(rows_equal(&rows[i], &got[i]));
  free(got);

  /* The layout, per the spec: <hlc: u64 BE> <origin: 36> <entity: u8 len + bytes>
   * <row_uid: 36> <field: u8 len + bytes> <tag> <value>. Row 0 has hlc 1 and a NULL. */
  static const unsigned char hlc1[8] = {0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(memcmp(buf + 3, hlc1, 8) == 0);
  CHECK(memcmp(buf + 11, o, 36) == 0);
  CHECK(buf[47] == 6 && memcmp(buf + 48, "tracks", 6) == 0);
  CHECK(memcmp(buf + 54, u, 36) == 0);
  CHECK(buf[90] == 5 && memcmp(buf + 91, "genre", 5) == 0);
  CHECK(buf[96] == MC_VAL_NULL);
  /* Row 1: an i64, big-endian. */
  CHECK(buf[97] == 0x01 && buf[104] == 0xef);
  /* Truncated or padded payloads are refused. */
  CHECK(mc_rows_decode(buf, len - 1, &db, &got, &n) == MC_ERR_FORMAT);
  unsigned char padded[4097];
  memcpy(padded, buf, len);
  padded[len] = 0;
  CHECK(mc_rows_decode(padded, len + 1, &db, &got, &n) == MC_ERR_FORMAT);

  /* A uid of the wrong length can't be written; a full payload says so. */
  mc_row bad = rows[4];
  bad.row_uid[5] = '\0';
  CHECK(mc_rows_writer_add(&w, &bad) == -1);
  mc_rows_writer_init(&w, buf, 200, MC_ROWS_GROUP);
  int added = 0;
  while (mc_rows_writer_add(&w, &rows[1]) == 1) added++;
  CHECK(added == 1); /* 3 bytes of header and 105 per row: a second doesn't fit in 200 */
}

/* --- Applying --- */

typedef struct fixture {
  mc_library *lib;
  sqlite3 *db;
  mc_rows_applier *a;
  /* Origins in a known order: lo < hi. */
  char lo[MC_UID_STRLEN], hi[MC_UID_STRLEN];
} fixture;

static int apply(fixture *f, const mc_row *rows, int n) {
  int logged = -1;
  mc_status st = mc_rows_apply(f->a, rows, n, &logged);
  CHECK(st == MC_OK);
  return st == MC_OK ? logged : -1;
}

static const char *track_field(fixture *f, const char *uid, const char *col) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT %s FROM tracks WHERE uid = '%s'", col, uid);
  return query_text(f->db, sql);
}

static int track_exists(fixture *f, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid = '%s'", uid);
  return query_int(f->db, sql) == 1;
}

static int logged_rows(fixture *f, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE row_uid = '%s'", uid);
  return query_int(f->db, sql);
}

static void test_lww(fixture *f) {
  char t[MC_UID_STRLEN];
  mc_uid_new(t);
  const char *lo = f->lo, *hi = f->hi;

  /* A new track, then a newer title from another device. */
  mc_row r1[] = {text_row(100, lo, "tracks", t, "title", "First"),
                 int_row(101, lo, "tracks", t, "track_no", 3)};
  CHECK(apply(f, r1, 2) == 2);
  CHECK_STR(track_field(f, t, "title"), "First");
  mc_row r2[] = {text_row(200, hi, "tracks", t, "title", "Second")};
  CHECK(apply(f, r2, 1) == 1);
  CHECK_STR(track_field(f, t, "title"), "Second");

  /* An older change is logged, but doesn't reach the table. */
  mc_row r3[] = {text_row(150, lo, "tracks", t, "title", "Stale")};
  CHECK(apply(f, r3, 1) == 1);
  CHECK_STR(track_field(f, t, "title"), "Second");
  CHECK(logged_rows(f, t) == 4);
  /* The same row again is a duplicate: not logged twice. */
  CHECK(apply(f, r3, 1) == 0);
  CHECK(logged_rows(f, t) == 4);

  /* Equal hlcs: the higher origin wins, whichever arrives first. */
  mc_row r4[] = {text_row(300, hi, "tracks", t, "genre", "High"),
                 text_row(300, lo, "tracks", t, "genre", "Low")};
  CHECK(apply(f, r4, 2) == 2);
  CHECK_STR(track_field(f, t, "genre"), "High");
  char t2[MC_UID_STRLEN];
  mc_uid_new(t2);
  mc_row r5[] = {text_row(300, lo, "tracks", t2, "title", "Low"),
                 text_row(300, hi, "tracks", t2, "title", "High")};
  CHECK(apply(f, r5, 2) == 2);
  CHECK_STR(track_field(f, t2, "title"), "High");

  /* An older delete loses to the newer edits. */
  mc_row r6[] = {int_row(50, hi, "tracks", t, "_deleted", 1)};
  CHECK(apply(f, r6, 1) == 1);
  CHECK(track_exists(f, t));

  /* A delete newer than a concurrent edit wins, whatever the order... */
  mc_row r7[] = {int_row(400, lo, "tracks", t, "_deleted", 1),
                 text_row(390, hi, "tracks", t, "genre", "Concurrent")};
  CHECK(apply(f, r7, 2) == 2);
  CHECK(!track_exists(f, t));
  /* ...and a newer edit brings the row back with its other fields. */
  mc_row r8[] = {int_row(410, hi, "tracks", t, "disc_no", 2)};
  CHECK(apply(f, r8, 1) == 1);
  CHECK(track_exists(f, t));
  CHECK_STR(track_field(f, t, "title"), "Second");
  CHECK_STR(track_field(f, t, "track_no"), "3");
  CHECK_STR(track_field(f, t, "disc_no"), "2");
  CHECK_STR(track_field(f, t, "genre"), "Concurrent");

  /* Unknown entities and fields are kept for others, but never applied. */
  mc_row r9[] = {text_row(500, lo, "tracks", t, "mood", "Calm"),
                 text_row(501, lo, "moods", t, "name", "Calm")};
  CHECK(apply(f, r9, 2) == 2);
  CHECK(query_int(f->db, "SELECT count(*) FROM changelog WHERE field = 'mood'") == 1);

  /* Applied rows aren't re-logged as local edits, and the clock has seen them. */
  CHECK(query_int(f->db, "SELECT count(*) FROM changelog WHERE origin_uid = mc_node_uid()") == 0);
  mc_row far[] = {int_row(UINT64_C(1) << 62, lo, "tracks", t, "year", 1999)};
  CHECK(apply(f, far, 1) == 1);
  CHECK(f->lib->hlc.last >= UINT64_C(1) << 62);
}

/* A new track's fields arrive out of order, across batches, and before its album. */
static void test_new_rows(fixture *f) {
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], t[MC_UID_STRLEN], pl[MC_UID_STRLEN],
    pe[MC_UID_STRLEN];
  mc_uid_new(ar);
  mc_uid_new(al);
  mc_uid_new(t);
  mc_uid_new(pl);
  mc_uid_new(pe);
  const char *o = f->lo;

  /* No title yet (NOT NULL): the track waits. */
  mc_row b1[] = {text_row(1000, o, "tracks", t, "album_uid", al),
                 int_row(1001, o, "tracks", t, "track_no", 7)};
  CHECK(apply(f, b1, 2) == 2);
  CHECK(!track_exists(f, t));

  /* Children first in the batch, parents after: dependency order and deferred foreign
   * keys make it work. */
  mc_row b2[] = {text_row(1002, o, "playlist_entries", pe, "track_uid", t),
                 text_row(1003, o, "playlist_entries", pe, "playlist_uid", pl),
                 text_row(1004, o, "playlist_entries", pe, "position", "a0"),
                 text_row(1005, o, "tracks", t, "title", "Moonshine"),
                 text_row(1006, o, "tracks", t, "artist_uid", ar),
                 text_row(1007, o, "albums", al, "title", "Fallen"),
                 text_row(1008, o, "albums", al, "artist_uid", ar),
                 text_row(1009, o, "playlists", pl, "name", "Night"),
                 text_row(1010, o, "artists", ar, "name", "Low")};
  CHECK(apply(f, b2, 9) == 9);
  CHECK(track_exists(f, t));
  CHECK(!strcmp(track_field(f, t, "album_uid"), al));
  CHECK_STR(track_field(f, t, "track_no"), "7");
  CHECK(!strcmp(track_field(f, t, "artist_uid"), ar));
  CHECK(query_int(f->db, "SELECT count(*) FROM playlist_entries") == 1);
  CHECK(query_int(f->db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);

  /* An album deleted while another device pointed a track at it: the reference is
   * resolved like ON DELETE SET NULL, and the batch still commits. */
  char t2[MC_UID_STRLEN], al2[MC_UID_STRLEN];
  mc_uid_new(t2);
  mc_uid_new(al2);
  mc_row b3[] = {text_row(1100, o, "albums", al2, "title", "Gone"),
                 text_row(1101, o, "tracks", t2, "title", "Orphan")};
  CHECK(apply(f, b3, 2) == 2);
  mc_row b4[] = {int_row(1200, o, "albums", al2, "_deleted", 1),
                 text_row(1201, f->hi, "tracks", t2, "album_uid", al2)};
  CHECK(apply(f, b4, 2) == 2);
  CHECK(track_exists(f, t2));
  CHECK_STR(track_field(f, t2, "album_uid"), "<null>");

  /* Deleting a track cascades its playlist entries, as a local delete would. */
  mc_row b5[] = {int_row(1300, o, "tracks", t, "_deleted", 1)};
  CHECK(apply(f, b5, 1) == 1);
  CHECK(!track_exists(f, t));
  CHECK(query_int(f->db, "SELECT count(*) FROM playlist_entries") == 0);
  CHECK(query_int(f->db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);
}

/* Album groups (schema v5): entries reference a group and an album, both NOT NULL with
 * ON DELETE CASCADE. Applied through the tracked tables in reverse, so the parent-first
 * order must come from the foreign keys, not the list. */
static void test_album_groups(fixture *f) {
  size_t n;
  const mc_changelog_table *tracked = mc_library_tracked(&n);
  mc_changelog_table reversed[16];
  CHECK(n <= 16);
  for (size_t i = 0; i < n && i < 16; i++) reversed[i] = tracked[n - 1 - i];
  mc_rows_target tg;
  mc_rows_target_library(f->lib, &tg);
  tg.tables = reversed;
  mc_rows_applier *saved = f->a;
  CHECK(mc_rows_applier_new(&tg, &f->a) == MC_OK);
  if (!f->a) {
    f->a = saved;
    return;
  }

  char g[MC_UID_STRLEN], al[MC_UID_STRLEN], e1[MC_UID_STRLEN], e2[MC_UID_STRLEN];
  mc_uid_new(g);
  mc_uid_new(al);
  mc_uid_new(e1);
  mc_uid_new(e2);
  const char *o = f->lo;
  /* The entry's fields first, then its album and group, all in one batch; a second entry
   * lacks its position (NOT NULL) and waits. */
  mc_row b1[] = {text_row(2000, o, "album_group_entries", e1, "group_uid", g),
                 text_row(2001, o, "album_group_entries", e1, "album_uid", al),
                 text_row(2002, o, "album_group_entries", e1, "position", "a0"),
                 text_row(2003, o, "album_group_entries", e2, "group_uid", g),
                 text_row(2004, o, "album_group_entries", e2, "album_uid", al),
                 text_row(2005, o, "albums", al, "title", "Grouped"),
                 text_row(2006, o, "album_groups", g, "name", "Winter")};
  CHECK(apply(f, b1, 7) == 7);
  CHECK(query_int(f->db, "SELECT count(*) FROM album_groups") == 1);
  CHECK(query_int(f->db, "SELECT count(*) FROM album_group_entries") == 1);
  mc_row b2[] = {text_row(2007, o, "album_group_entries", e2, "position", "a1")};
  CHECK(apply(f, b2, 1) == 1);
  CHECK(query_int(f->db, "SELECT count(*) FROM album_group_entries") == 2);
  CHECK(query_int(f->db, "SELECT count(*) FROM pragma_foreign_key_check") == 0);

  /* Deleting the album removes it from the group, as a local delete would. */
  mc_row b3[] = {int_row(2100, o, "albums", al, "_deleted", 1)};
  CHECK(apply(f, b3, 1) == 1);
  CHECK(query_int(f->db, "SELECT count(*) FROM album_group_entries") == 0);
  CHECK(query_int(f->db, "SELECT count(*) FROM album_groups") == 1);

  mc_rows_applier_free(f->a);
  f->a = saved;
}

/* The reader sends what a peer lacks, in hlc order; resend sends differing origins whole. */
static void test_reader(fixture *f) {
  mc_origin_hlc *mine;
  int n;
  CHECK(mc_changelog_version_vector(f->db, &mine, &n) == MC_OK && n == 2);
  int total = query_int(f->db, "SELECT count(*) FROM changelog");

  /* An empty vector: everything. */
  mc_rows_reader *rd;
  mc_row r;
  int got, count = 0;
  uint64_t last = 0;
  int ordered = 1;
  CHECK(mc_rows_reader_open(f->db, NULL, 0, 0, &rd) == MC_OK);
  while (mc_rows_reader_next(rd, &r, &got) == MC_OK && got) {
    if (r.hlc < last) ordered = 0;
    last = r.hlc;
    count++;
  }
  mc_rows_reader_close(rd);
  CHECK(count == total && ordered);

  /* The peer has lo up to 1005 and all of hi. */
  mc_origin_hlc peer[2];
  memcpy(peer, mine, sizeof peer);
  for (int i = 0; i < 2; i++)
    if (!strcmp(peer[i].origin_uid, f->lo)) peer[i].hlc = 1005;
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE origin_uid = '%s' AND hlc > 1005",
           f->lo);
  int want = query_int(f->db, sql);
  count = 0;
  int only_lo = 1;
  CHECK(mc_rows_reader_open(f->db, peer, 2, 0, &rd) == MC_OK);
  while (mc_rows_reader_next(rd, &r, &got) == MC_OK && got) {
    if (strcmp(r.origin, f->lo) || r.hlc <= 1005) only_lo = 0;
    count++;
  }
  mc_rows_reader_close(rd);
  CHECK(count == want && only_lo);

  /* Resend: lo differs, so all of lo; hi matches, so none of it. */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE origin_uid = '%s'", f->lo);
  want = query_int(f->db, sql);
  count = 0;
  CHECK(mc_rows_reader_open(f->db, peer, 2, 1, &rd) == MC_OK);
  while (mc_rows_reader_next(rd, &r, &got) == MC_OK && got) count++;
  mc_rows_reader_close(rd);
  CHECK(count == want);

  /* Digests depend only on the vector. */
  char d1[MC_DIGEST_STRLEN], d2[MC_DIGEST_STRLEN];
  CHECK(mc_rows_vv_digest(f->db, d1) == MC_OK);
  mc_rows_vv_digest_of(mine, n, d2);
  CHECK(!strcmp(d1, d2));
  mc_rows_vv_digest_of(peer, 2, d2);
  CHECK(strcmp(d1, d2) != 0);
  free(mine);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  test_encoding();

  char path[1024];
  snprintf(path, sizeof path, "%s/test_rows.d", scratch);
  remove_db(path);
  fixture f;
  memset(&f, 0, sizeof f);
  CHECK(mc_library_open(path, &f.lib) == MC_OK);
  if (!f.lib) return TEST_RESULT();
  f.db = mc_library_db(f.lib);
  mc_uid_new(f.lo);
  mc_uid_new(f.hi);
  if (strcmp(f.lo, f.hi) > 0) {
    char tmp[MC_UID_STRLEN];
    memcpy(tmp, f.lo, sizeof tmp);
    memcpy(f.lo, f.hi, sizeof tmp);
    memcpy(f.hi, tmp, sizeof tmp);
  }
  mc_rows_target tg;
  mc_rows_target_library(f.lib, &tg);
  CHECK(mc_rows_applier_new(&tg, &f.a) == MC_OK);
  if (f.a) {
    test_lww(&f);
    test_new_rows(&f);
    test_album_groups(&f);
    test_reader(&f);
  }
  mc_rows_applier_free(f.a);
  mc_library_close(f.lib);
  return TEST_RESULT();
}
