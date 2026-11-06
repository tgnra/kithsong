/* Sets up two paired scratch instances for trying the orphan sweep in the real app:
 *
 *   manual_orphan_seed <scratch root>
 *
 * makes <root>/a and <root>/b, each with config/ (settings.json naming music/) and music/,
 * pairs them over loopback and runs a session so B holds A's tracks and both profiles are
 * confirmed. Then run kithsong.exe --config-dir <root>/a/config and likewise for b, with
 * KITHSONG_TEST_TRASH_DIR set in a debug build; orphans planted in between go after their next
 * session. Never give it anything but a scratch folder: it empties it first. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000

typedef struct dev {
  char config[1024], libdir[1024], uid[MC_UID_STRLEN];
  mc_library *lib;
  mc_sync *sync;
  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes, results, done;
  mc_pair_reason reason;
  mc_status status;
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name, (void)attempts;
  mc_mutex_lock(&d->m);
  snprintf(d->code, sizeof d->code, "%s", code);
  d->codes++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_pair_result(void *user, const mc_pair_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->reason = r->reason;
  d->results++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_sync_done(void *user, const mc_sync_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->status = r->status;
  d->done++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL,
                                        on_sync_done, NULL, NULL, NULL, NULL, NULL};

/* Until *n is past seen. */
static int wait_for(dev *d, const int *n, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (*n <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int ok = *n > seen;
  mc_mutex_unlock(&d->m);
  return ok;
}

static int start(dev *d, const char *root, const char *label) {
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_mkdirs(d->config) == MC_OK);
  char path[1100], escaped[2100] = "";
  for (const char *p = d->libdir; *p; p++) {
    size_t n = strlen(escaped);
    if (*p == '\\') strcpy(escaped + n, "\\\\");
    else escaped[n] = *p, escaped[n + 1] = '\0';
  }
  snprintf(path, sizeof path, "%s/settings.json", d->config);
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  fprintf(f, "{\"library_dir\": \"%s\"}\n", escaped);
  fclose(f);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) == MC_OK);
  if (!d->sync) return 0;
  snprintf(d->uid, sizeof d->uid, "%s", mc_sync_device_uid(d->sync));
  mc_sync_set_test_options(d->sync, 0, 0);
  mc_sync_set_test_manual(d->sync, 1);
  CHECK(mc_sync_start(d->sync) == MC_OK);
  char hints[64];
  snprintf(hints, sizeof hints, "[\"127.0.0.1:%u\"]", (unsigned)mc_sync_port(d->sync));
  mc_sync_set_test_hints(d->sync, hints);
  return mc_sync_port(d->sync) != 0;
}

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Track number no of an album, as a short silent WAV with a cover beside it. */
static void add_track(dev *d, const char *album, const char *folder, int no) {
  enum { DATA_LEN = 1600 };
  char rel[256], abs[1400], sql[1024];
  snprintf(rel, sizeof rel, "%s/%02d Song.wav", folder, no);
  snprintf(abs, sizeof abs, "%s/%s", d->libdir, folder);
  CHECK(mc_mkdirs(abs) == MC_OK);
  snprintf(abs, sizeof abs, "%s/%s/cover.jpg", d->libdir, folder);
  FILE *f = mc_fopen(abs, "wb");
  if (f) {
    fputs("\xFF\xD8\xFF\xD9", f);
    fclose(f);
  }
  snprintf(abs, sizeof abs, "%s/%s", d->libdir, rel);
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
  static unsigned char audio[DATA_LEN];
  audio[0] = (unsigned char)no; /* each its own */
  audio[1] = (unsigned char)album[0];
  f = mc_fopen(abs, "wb");
  CHECK(f != NULL);
  if (!f) return;
  fwrite(hdr, sizeof hdr, 1, f);
  fwrite(audio, sizeof audio, 1, f);
  fclose(f);

  char artist[MC_UID_STRLEN], al[MC_UID_STRLEN], track[MC_UID_STRLEN], file[MC_UID_STRLEN];
  mc_uid_new(artist);
  mc_uid_new(al);
  mc_uid_new(track);
  mc_uid_new(file);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(abs, &size, &mtime) == MC_OK);
  sqlite3 *db = mc_library_db(d->lib);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Seed %s %d')", artist, album, no);
  CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s %d', '%s', 2000)",
           al, album, no, artist);
  CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  snprintf(sql, sizeof sql, "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
                            " VALUES ('%s', '%s', '%s', 'Song', %d, 100)",
           track, al, artist, no);
  CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(db, "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
                               " VALUES (?1, ?2, ?3, 'wav', 128, ?4, ?5)", -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, file, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, track, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, size);
  sqlite3_bind_int64(stmt, 5, mtime);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  const char *root = argv[1];
  CHECK(mc_init() == MC_OK);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  static dev a, b;
  if (!start(&a, root, "a") || !start(&b, root, "b")) return 1;
  for (int i = 1; i <= 6; i++) add_track(&a, "Caf\xC3\xA9", "Seed/Caf\xC3\xA9 (2000)", i);

  int codes = a.codes, results = b.results, done = b.done;
  CHECK(mc_sync_pair_request_address(b.sync, "127.0.0.1", mc_sync_port(a.sync)) == MC_OK);
  CHECK(wait_for(&a, &a.codes, codes));
  CHECK(mc_sync_pair_enter(b.sync, a.code) == MC_OK);
  CHECK(wait_for(&b, &b.results, results) && b.reason == MC_PAIR_PAIRED);
  CHECK(wait_for(&b, &b.done, done));
  done = b.done;
  CHECK(mc_sync_confirm_now(a.sync, b.uid) == MC_OK);
  CHECK(wait_for(&b, &b.done, done) && b.status == MC_OK);
  printf("a %s\nb %s\n", a.uid, b.uid);
  mc_sync_free(a.sync);
  mc_sync_free(b.sync);
  mc_library_close(a.lib);
  mc_library_close(b.lib);
  mc_shutdown();
  return TEST_RESULT();
}
