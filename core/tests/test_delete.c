/* Deleting tracks, albums and artists to the trash, and restoring them when their files come
 * back (library/delete.c). The trash is a folder under the scratch dir, never the Recycle
 * Bin: files "come back" when the test moves them back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/delete_internal.h"
#include "library/library_internal.h"
#include "musiccore/delete.h"
#include "musiccore/library.h"
#include "musiccore/playlist.h"
#include "tagger/import_db.h"
#include "test.h"
#include "util/platform.h"

static char g_scratch[1024], g_trash[1100];

/* What the fake trash holds: where each file was, and where it is now. */
static struct {
  char from[1400], to[1300];
} g_bin[32];
static int g_trashed;
static const char *g_refuse; /* a file name the trash refuses, as an open file would be */

static mc_status fake_trash(const char *path) {
  if (g_trashed == 32) return MC_ERR_IO;
  if (g_refuse && strstr(path, g_refuse)) return MC_ERR_IO;
  snprintf(g_bin[g_trashed].from, sizeof g_bin[0].from, "%s", path);
  snprintf(g_bin[g_trashed].to, sizeof g_bin[0].to, "%s%ctrashed-%d", g_trash, MC_PATH_SEP,
           g_trashed);
  mc_status st = mc_rename(path, g_bin[g_trashed].to, 0);
  if (st == MC_OK) g_trashed++;
  return st;
}

/* Takes the trashed file whose old path ends in name back out of the trash, as the
 * Recycle Bin's Restore does: to its old path, folders and all. name uses '/'. */
static int unbin(const char *name_slashes) {
  char name[256];
  snprintf(name, sizeof name, "%s", name_slashes);
  for (char *p = name; *p; p++)
    if (*p == '/') *p = MC_PATH_SEP;
  for (int i = 0; i < g_trashed; i++) {
    size_t n = strlen(g_bin[i].from), m = strlen(name);
    if (n < m || strcmp(g_bin[i].from + n - m, name) || !mc_file_exists(g_bin[i].to)) continue;
    char dir[1400];
    snprintf(dir, sizeof dir, "%s", g_bin[i].from);
    char *sep = strrchr(dir, MC_PATH_SEP);
    if (sep) *sep = '\0';
    mc_mkdirs(dir);
    return mc_rename(g_bin[i].to, g_bin[i].from, 0) == MC_OK;
  }
  return 0;
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int64_t query_int64(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
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

/* Rows in config_dir/deleted.db, or -1 if it can't be read. */
static int records(const char *config_dir) {
  char path[1400];
  snprintf(path, sizeof path, "%s%cdeleted.db", config_dir, MC_PATH_SEP);
  sqlite3 *db;
  int n = -1;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM deleted", -1, &stmt, NULL) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }
  sqlite3_close(db);
  return n;
}

static void write_file(const char *path, const char *content) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  fputs(content, f);
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

/* An album of count tracks by artist into lib, as files under "<artist>/<title>/"; the
 * tracks' artist is track_artist where given. Fills uids with the tracks' uids. */
static void add_album(mc_library *lib, const char *artist, const char *title, int count,
                      const char *const *track_artists, char uids[][MC_UID_STRLEN]) {
  char dir[1400], rel[8][200], abs[1600], names[8][64];
  snprintf(dir, sizeof dir, "%s%c%s%c%s", mc_library_dir(lib), MC_PATH_SEP, artist, MC_PATH_SEP,
           title);
  mc_mkdirs(dir);
  mc_db_album a = {0};
  a.title = title;
  a.artist = (mc_db_artist){artist, NULL, NULL};
  a.year = 2001;
  mc_db_track t[8];
  for (int i = 0; i < count; i++) {
    snprintf(names[i], sizeof names[i], "%s %d", title, i + 1);
    snprintf(rel[i], sizeof rel[i], "%s/%s/%02d.flac", artist, title, i + 1);
    snprintf(abs, sizeof abs, "%s%c%02d.flac", dir, MC_PATH_SEP, i + 1);
    write_file(abs, names[i]);
    memset(&t[i], 0, sizeof t[i]);
    t[i].path = rel[i];
    t[i].format = "flac";
    t[i].size_bytes = (int64_t)strlen(names[i]);
    t[i].title = names[i];
    t[i].track_no = i + 1;
    t[i].disc_no = 1;
    t[i].duration_ms = 200000;
    if (track_artists && track_artists[i]) t[i].artist = (mc_db_artist){track_artists[i], NULL, NULL};
  }
  CHECK(mc_import_db_write(lib, &a, t, count) == MC_OK);
  for (int i = 0; i < count; i++) {
    char sql[300];
    snprintf(sql, sizeof sql, "SELECT track_uid FROM files WHERE path = '%s'", rel[i]);
    query_text(lib, sql, uids[i], MC_UID_STRLEN);
  }
}

static mc_library *open_library(const char *name, char config[1200]) {
  char dir[1200];
  snprintf(dir, sizeof dir, "%s%c%s", g_scratch, MC_PATH_SEP, name);
  snprintf(config, 1200, "%s%c%s-config", g_scratch, MC_PATH_SEP, name);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  return lib;
}

static int count_of(mc_library *lib, mc_entity e) {
  int64_t n = -1;
  mc_library_count(lib, e, &n);
  return (int)n;
}

/* --- Tests --- */

/* An album goes, with its cover and folders, its plays, playlist places and album group
 * place; its files coming back bring all that back, one file at a time. */
static void test_album(void) {
  char config[1200];
  mc_library *lib = open_library("album", config);
  if (!lib) return;
  char low[3][MC_UID_STRLEN], mimi[1][MC_UID_STRLEN];
  add_album(lib, "Low", "Things", 3, NULL, low);
  add_album(lib, "Mimi", "Other", 1, NULL, mimi);
  char cover[1600];
  snprintf(cover, sizeof cover, "%s%cLow%cThings%ccover.jpg", mc_library_dir(lib), MC_PATH_SEP,
           MC_PATH_SEP, MC_PATH_SEP);
  write_file(cover, "a cover");

  char album[MC_UID_STRLEN], playlist[MC_UID_STRLEN], group[MC_UID_STRLEN], sql[512];
  snprintf(sql, sizeof sql, "SELECT album_uid FROM tracks WHERE uid = '%s'", low[0]);
  query_text(lib, sql, album, sizeof album);
  CHECK(mc_playlist_create(lib, "Mix", playlist) == MC_OK);
  CHECK(mc_playlist_append(lib, playlist, low[0], NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, playlist, mimi[0], NULL) == MC_OK);
  CHECK(mc_playlist_append(lib, playlist, low[0], NULL) == MC_OK); /* twice */
  CHECK(mc_album_group_create(lib, "Slowcore", group) == MC_OK);
  CHECK(mc_album_group_add(lib, group, album) == MC_OK);
  CHECK(mc_library_record_play(lib, low[0]) == MC_OK);
  CHECK(mc_library_record_play(lib, low[0]) == MC_OK);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "UPDATE tracks SET added_ms = 1234567 WHERE uid = '%s'", low[0]);
  CHECK(sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);

  const char *uids[] = {album};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_ALBUMS, uids, 1, &plan) == MC_OK);
  CHECK(mc_delete_plan_tracks(plan) == 3);
  CHECK(mc_delete_plan_files(plan) == 4); /* and the cover */
  CHECK(mc_delete_plan_has(plan, low[1]) && !mc_delete_plan_has(plan, mimi[0]));
  int restored = -1;
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0); /* no store yet */

  mc_delete_result res;
  CHECK(mc_delete_run(lib, config, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 3 && res.files_trashed == 4 && res.tracks_kept == 0);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 1 && count_of(lib, MC_ENTITY_ALBUMS) == 1);
  CHECK(count_of(lib, MC_ENTITY_ARTISTS) == 1); /* Low had nothing else */
  CHECK(count_of(lib, MC_ENTITY_FILES) == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM playlist_entries") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM album_group_entries") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM track_plays") == 0);
  char low_dir[1400];
  snprintf(low_dir, sizeof low_dir, "%s%cLow", mc_library_dir(lib), MC_PATH_SEP);
  CHECK(!mc_file_exists(cover) && !mc_file_exists(low_dir)); /* folders tidied away */
  CHECK(g_trashed == 4);
  CHECK(records(config) == 3); /* the audio files; the cover needs no rows */
  /* Logged, so sync deletes them everywhere. */
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM changelog WHERE field = '_deleted' AND row_uid IN ('%s', '%s')",
           low[0], album);
  CHECK(query_int(lib, sql) == 2);

  /* Nothing's back yet. */
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0);
  CHECK(records(config) == 3);

  /* Two come back. */
  CHECK(unbin("01.flac") && unbin("02.flac"));
  int tracks_back = -1;
  CHECK(mc_delete_restore(lib, config, &restored, &tracks_back) == MC_OK && restored == 2 &&
        tracks_back == 2);
  CHECK(records(config) == 1);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 3 && count_of(lib, MC_ENTITY_ARTISTS) == 2);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM albums WHERE uid = '%s'", album);
  CHECK(query_int(lib, sql) == 1); /* the same album */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid IN ('%s', '%s')", low[0], low[1]);
  CHECK(query_int(lib, sql) == 2);
  snprintf(sql, sizeof sql, "SELECT sum(play_count) FROM track_plays WHERE track_uid = '%s'", low[0]);
  CHECK(query_int(lib, sql) == 2);
  snprintf(sql, sizeof sql, "SELECT added_ms FROM tracks WHERE uid = '%s'", low[0]);
  CHECK(query_int64(lib, sql) == 1234567);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM playlist_entries WHERE track_uid = '%s'", low[0]);
  CHECK(query_int(lib, sql) == 2);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM album_group_entries WHERE album_uid = '%s'", album);
  CHECK(query_int(lib, sql) == 1);
  mc_track_list *tracks = NULL;
  CHECK(mc_library_list_playlist_tracks(lib, playlist, &tracks) == MC_OK);
  CHECK(tracks && tracks->count == 3 && !strcmp(tracks->items[0].uid, low[0]) &&
        !strcmp(tracks->items[1].uid, mimi[0]) && tracks->items[0].path);
  mc_track_list_free(tracks);
  /* Restoring is logged too, so it comes back on other devices. */
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM changelog WHERE row_uid = '%s' AND field = 'title'", low[0]);
  CHECK(query_int(lib, sql) == 2);

  /* A different file where the third was isn't it. */
  char third[1600];
  snprintf(third, sizeof third, "%s%cLow%cThings%c03.flac", mc_library_dir(lib), MC_PATH_SEP,
           MC_PATH_SEP, MC_PATH_SEP);
  write_file(third, "something else entirely");
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0);
  CHECK(records(config) == 1);
  mc_remove(third);
  /* Its playlist went meanwhile: the track comes back without it. */
  CHECK(mc_playlist_delete(lib, playlist) == MC_OK);
  CHECK(unbin("03.flac"));
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 1);
  CHECK(records(config) == 0 && count_of(lib, MC_ENTITY_TRACKS) == 4);
  CHECK(query_int(lib, "SELECT count(*) FROM playlist_entries") == 0);
  mc_library_close(lib);
}

/* A file the trash refuses keeps its track; the rest go. */
static void test_kept(void) {
  char config[1200];
  mc_library *lib = open_library("kept", config);
  if (!lib) return;
  char t[2][MC_UID_STRLEN];
  add_album(lib, "Low", "Busy", 2, NULL, t);
  const char *uids[] = {t[0], t[1]};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids, 2, &plan) == MC_OK);
  CHECK(mc_delete_plan_tracks(plan) == 2 && mc_delete_plan_files(plan) == 2);
  g_refuse = "02.flac";
  mc_delete_result res;
  CHECK(mc_delete_run(lib, config, plan, &res) == MC_OK);
  g_refuse = NULL;
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 1 && res.files_trashed == 1 && res.tracks_kept == 1);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 1 && count_of(lib, MC_ENTITY_FILES) == 1);
  CHECK(count_of(lib, MC_ENTITY_ALBUMS) == 1 && count_of(lib, MC_ENTITY_ARTISTS) == 1);
  char sql[200];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid = '%s'", t[1]);
  CHECK(query_int(lib, sql) == 1);
  CHECK(records(config) == 1);

  /* A track with no file here only loses its rows. */
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "DELETE FROM files WHERE track_uid = '%s'", t[1]);
  CHECK(sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids + 1, 1, &plan) == MC_OK);
  CHECK(mc_delete_plan_tracks(plan) == 1 && mc_delete_plan_files(plan) == 0);
  CHECK(mc_delete_run(lib, config, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 1 && res.files_trashed == 0);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 0 && count_of(lib, MC_ENTITY_ALBUMS) == 0);
  CHECK(records(config) == 1);
  mc_library_close(lib);
}

/* An artist: their own albums whole, and tracks credited to them on others' albums. */
static void test_artist(void) {
  char config[1200];
  mc_library *lib = open_library("artist", config);
  if (!lib) return;
  char own[2][MC_UID_STRLEN], comp[3][MC_UID_STRLEN];
  const char *credits[] = {"Ada", "Low", "Bea"};
  add_album(lib, "Low", "Own", 2, NULL, own);
  add_album(lib, "Various", "Comp", 3, credits, comp);
  char low[MC_UID_STRLEN], sql[200];
  snprintf(sql, sizeof sql, "SELECT uid FROM artists WHERE name = 'Low'");
  query_text(lib, sql, low, sizeof low);
  const char *uids[] = {low};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_ARTISTS, uids, 1, &plan) == MC_OK);
  CHECK(mc_delete_plan_tracks(plan) == 3 && mc_delete_plan_files(plan) == 3);
  CHECK(mc_delete_plan_has(plan, comp[1]) && !mc_delete_plan_has(plan, comp[0]));
  mc_delete_result res;
  CHECK(mc_delete_run(lib, config, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 3);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 2 && count_of(lib, MC_ENTITY_ALBUMS) == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM artists WHERE name = 'Low'") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM artists WHERE name = 'Various'") == 1);

  /* The credited track comes back credited to the artist, who comes back too. */
  CHECK(unbin("Comp/02.flac"));
  int restored = 0;
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 1);
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM tracks t JOIN artists a ON a.uid = t.artist_uid"
           " WHERE t.uid = '%s' AND a.uid = '%s'",
           comp[1], low);
  CHECK(query_int(lib, sql) == 1);
  mc_library_close(lib);
}

/* Old records expire; a file imported again since drops its record. */
static void test_expiry(void) {
  char config[1200];
  mc_library *lib = open_library("expiry", config);
  if (!lib) return;
  char t[2][MC_UID_STRLEN];
  add_album(lib, "Low", "Old", 2, NULL, t);
  const char *uids[] = {t[0], t[1]};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids, 2, &plan) == MC_OK);
  CHECK(mc_delete_run(lib, config, plan, NULL) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(records(config) == 2);

  char path[1400];
  snprintf(path, sizeof path, "%s%cdeleted.db", config, MC_PATH_SEP);
  sqlite3 *db;
  CHECK(sqlite3_open(path, &db) == SQLITE_OK);
  CHECK(sqlite3_exec(db, "UPDATE deleted SET deleted_ms = 1000 WHERE rel LIKE '%01.flac'", NULL,
                     NULL, NULL) == SQLITE_OK);
  sqlite3_close(db);
  int restored = -1;
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0);
  CHECK(records(config) == 1);

  /* The second, imported again before it's restored: it's in the library already. */
  char again[2][MC_UID_STRLEN];
  CHECK(unbin("Old/02.flac"));
  add_album(lib, "Low", "Old", 2, NULL, again);
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0);
  CHECK(records(config) == 0);
  mc_library_close(lib);
}

/* Where there's no trash: the files and the left-over cover are removed, not trashed, and
 * nothing is recorded; a file that can't be removed keeps its track. */
static int g_removed;

static mc_status counting_remove(const char *path) {
  if (g_refuse && strstr(path, g_refuse)) return MC_ERR_IO;
  mc_status st = mc_remove(path);
  if (st == MC_OK) g_removed++;
  return st;
}

/* A track with two files here (an original and a copy): each file counts as it comes back,
 * the track only with the first. */
static void test_two_files(void) {
  char config[1200];
  mc_library *lib = open_library("two_files", config);
  if (!lib) return;
  char t[1][MC_UID_STRLEN], sql[1024], copy[1600];
  add_album(lib, "Low", "Twice", 1, NULL, t);
  snprintf(copy, sizeof copy, "%s%cLow%cTwice%c01.opus", mc_library_dir(lib), MC_PATH_SEP, MC_PATH_SEP,
           MC_PATH_SEP);
  write_file(copy, "a copy");
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, size_bytes)"
           " VALUES ('f0000000-0000-4000-8000-000000000001', '%s', 'Low/Twice/01.opus', 'opus', 6)",
           t[0]);
  CHECK(sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);

  const char *uids[] = {t[0]};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, uids, 1, &plan) == MC_OK);
  mc_delete_result res;
  CHECK(mc_delete_run(lib, config, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 1 && res.files_trashed == 2);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 0);

  int restored = -1, tracks = -1;
  CHECK(unbin("Twice/01.opus"));
  CHECK(mc_delete_restore(lib, config, &restored, &tracks) == MC_OK && restored == 1 && tracks == 1);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 1 && count_of(lib, MC_ENTITY_FILES) == 1);
  CHECK(unbin("Twice/01.flac"));
  CHECK(mc_delete_restore(lib, config, &restored, &tracks) == MC_OK && restored == 1 && tracks == 0);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 1 && count_of(lib, MC_ENTITY_FILES) == 2);
  mc_library_close(lib);
}

static void test_permanent(void) {
  char config[1200];
  mc_library *lib = open_library("permanent", config);
  if (!lib) return;
  char t[3][MC_UID_STRLEN], album[MC_UID_STRLEN], sql[512];
  add_album(lib, "Low", "Gone", 3, NULL, t);
  char cover[1600], low_dir[1400];
  snprintf(low_dir, sizeof low_dir, "%s%cLow", mc_library_dir(lib), MC_PATH_SEP);
  snprintf(cover, sizeof cover, "%s%cGone%ccover.jpg", low_dir, MC_PATH_SEP, MC_PATH_SEP);
  write_file(cover, "a cover");
  snprintf(sql, sizeof sql, "SELECT album_uid FROM tracks WHERE uid = '%s'", t[0]);
  query_text(lib, sql, album, sizeof album);

  /* One track first, refused: it stays. */
  const char *one[] = {t[2]};
  mc_delete_plan *plan = NULL;
  CHECK(mc_delete_plan_new(lib, MC_DELETE_TRACKS, one, 1, &plan) == MC_OK);
  g_refuse = "03.flac";
  mc_delete_result res;
  int trashed = g_trashed;
  CHECK(mc_delete_run_permanently(lib, plan, &res) == MC_OK);
  g_refuse = NULL;
  CHECK(res.tracks_deleted == 0 && res.files_trashed == 0 && res.tracks_kept == 1);
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 3);
  mc_delete_plan_free(plan);

  /* Then the album. */
  const char *uids[] = {album};
  CHECK(mc_delete_plan_new(lib, MC_DELETE_ALBUMS, uids, 1, &plan) == MC_OK);
  CHECK(mc_delete_plan_tracks(plan) == 3 && mc_delete_plan_files(plan) == 4);
  CHECK(mc_delete_run_permanently(lib, plan, &res) == MC_OK);
  mc_delete_plan_free(plan);
  CHECK(res.tracks_deleted == 3 && res.files_trashed == 4 && res.tracks_kept == 0);
  CHECK(g_removed == 4 && g_trashed == trashed); /* none went to the trash */
  CHECK(count_of(lib, MC_ENTITY_TRACKS) == 0 && count_of(lib, MC_ENTITY_ALBUMS) == 0);
  CHECK(count_of(lib, MC_ENTITY_ARTISTS) == 0 && count_of(lib, MC_ENTITY_FILES) == 0);
  CHECK(!mc_file_exists(cover) && !mc_file_exists(low_dir));
  CHECK(records(config) == -1); /* no deleted.db at all */
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM changelog WHERE field = '_deleted' AND row_uid IN ('%s', '%s')",
           t[0], album);
  CHECK(query_int(lib, sql) == 2); /* logged, so sync deletes them everywhere */
  int restored = -1;
  CHECK(mc_delete_restore(lib, config, &restored, NULL) == MC_OK && restored == 0);
  CHECK(mc_delete_run_permanently(lib, NULL, &res) == MC_ERR_INVALID_ARG);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  snprintf(g_scratch, sizeof g_scratch, "%s%ctest_delete.d", argc > 1 ? argv[1] : ".", MC_PATH_SEP);
  remove_tree(g_scratch); /* the last run's */
  snprintf(g_trash, sizeof g_trash, "%s%ctrash", g_scratch, MC_PATH_SEP);
  CHECK(mc_mkdirs(g_trash) == MC_OK);
  mc_delete_trash = fake_trash;
  mc_delete_remove = counting_remove;

  test_album();
  test_kept();
  test_artist();
  test_expiry();
  test_two_files();
  test_permanent();
  return TEST_RESULT();
}
