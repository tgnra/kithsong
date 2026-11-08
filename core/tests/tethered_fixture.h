/* What test_plugin_tethered.c and test_plugin_plays.c share: a library of WAV (and pretend
 * MP3) originals, the test plugin's folder device (plugin_folder.c) loaded through the real
 * loader, and an mc_tethered to sync it with, waited on. */
#ifndef TETHERED_FIXTURE_H
#define TETHERED_FIXTURE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "kithcore/kithcore.h"
#include "kithcore/tethered.h"
#include "library/library_internal.h"
#include "test.h"
#include "util/platform.h"

#define TRACK_MS 1500
#define WAIT_MS 60000

typedef struct fx {
  char base[1024], libdir[1024], cfg[1024], dev[1024];
  mc_settings *s;
  mc_library *lib;
  mc_tethered *t;
  mc_mutex lock;
  int done;
  mc_tethered_result r;
  int progress;
  int devices_events;
} fx;

static inline void fx_put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static inline void fx_put32(FILE *f, uint32_t v) {
  fx_put16(f, v & 0xffff);
  fx_put16(f, v >> 16);
}

/* A 16-bit 44.1 kHz stereo WAV of a tone. */
static inline int fx_write_wav(const char *path, unsigned ms, double pitch) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t rate = 44100, frames = rate * ms / 1000, bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  fx_put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  fx_put32(f, 16);
  fx_put16(f, 1);
  fx_put16(f, 2);
  fx_put32(f, rate);
  fx_put32(f, rate * 4);
  fx_put16(f, 4);
  fx_put16(f, 16);
  fwrite("data", 1, 4, f);
  fx_put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) {
    unsigned v = (unsigned)(uint16_t)(int16_t)lrint(0.4 * 32767 * sin(2 * 3.14159265358979 * pitch * i / rate));
    fx_put16(f, v);
    fx_put16(f, v);
  }
  return fclose(f) == 0;
}

static inline int fx_write_text(const char *path, const char *text) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

static inline void fx_sha(const char *path, char out[65]) {
  out[0] = '\0';
  FILE *f = mc_fopen(path, "rb");
  if (!f) return;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  unsigned char buf[8192], digest[32];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);
  fclose(f);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static inline int fx_exec(fx *x, const char *sql) {
  sqlite3 *db = mc_library_db(x->lib);
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
}

static inline int64_t fx_int(fx *x, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t n = -1;
  if (sqlite3_prepare_v2(mc_library_db(x->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static inline void fx_text(fx *x, const char *sql, char *out, size_t cap) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(x->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

/* --- Events --- */

static inline void fx_on_devices(void *user) {
  fx *x = user;
  mc_mutex_lock(&x->lock);
  x->devices_events++;
  mc_mutex_unlock(&x->lock);
}

static inline void fx_on_progress(void *user, const mc_tethered_progress *p) {
  fx *x = user;
  (void)p;
  mc_mutex_lock(&x->lock);
  x->progress++;
  mc_mutex_unlock(&x->lock);
}

static inline void fx_on_done(void *user, const mc_tethered_result *r) {
  fx *x = user;
  mc_mutex_lock(&x->lock);
  x->r = *r;
  x->r.device_uid = NULL;
  x->done = 1;
  mc_mutex_unlock(&x->lock);
}

/* --- Setting up --- */

/* A fresh library, config and device folder under scratch/name, the test plugins loaded
 * from scratch/plugins with the folder one on, and an mc_tethered. */
static inline void fx_open(fx *x, const char *scratch, const char *name) {
  memset(x, 0, sizeof *x);
  mc_mutex_init(&x->lock);
  char plugins[1024];
  snprintf(x->base, sizeof x->base, "%s/%s", scratch, name);
  snprintf(x->libdir, sizeof x->libdir, "%s/lib", x->base);
  snprintf(x->cfg, sizeof x->cfg, "%s/cfg", x->base);
  snprintf(x->dev, sizeof x->dev, "%s/dev", x->base);
  snprintf(plugins, sizeof plugins, "%s/plugins", scratch);
  mc_remove_tree(x->base);
  CHECK(mc_mkdirs(x->libdir) == MC_OK);
  CHECK(mc_mkdirs(x->cfg) == MC_OK);
  CHECK(mc_mkdirs(x->dev) == MC_OK);
  CHECK(mc_settings_load(x->cfg, &x->s) == MC_OK);
  const char *dirs[] = {plugins};
  CHECK(mc_plugins_load(x->s, dirs, 1) == MC_OK);
  CHECK(mc_plugins_set_enabled("kith_test_folder", 1) == MC_OK);
  CHECK(mc_library_open(x->libdir, &x->lib) == MC_OK);
  mc_tethered_events ev = {fx_on_devices, fx_on_progress, fx_on_done};
  CHECK(mc_tethered_new(x->lib, x->cfg, &ev, x, &x->t) == MC_OK);
}

static inline void fx_close(fx *x) {
  mc_tethered_free(x->t);
  mc_library_close(x->lib);
  mc_plugins_unload();
  mc_settings_free(x->s);
  mc_mutex_destroy(&x->lock);
  mc_remove_tree(x->base);
}

/* An album by artist (made if new); its uid into uid. */
static inline void fx_album(fx *x, const char *artist, const char *title, char uid[MC_UID_STRLEN]) {
  char ar[MC_UID_STRLEN] = "", sql[1024];
  snprintf(sql, sizeof sql, "SELECT uid FROM artists WHERE name = '%s'", artist);
  fx_text(x, sql, ar, sizeof ar);
  CHECK(mc_library_begin_write(x->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", ar, artist);
    CHECK(fx_exec(x, sql) == SQLITE_OK);
  }
  mc_uid_new(uid);
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s', '%s', 1994)",
           uid, title, ar);
  CHECK(fx_exec(x, sql) == SQLITE_OK);
  CHECK(mc_library_commit(x->lib) == MC_OK);
}

/* A track of album, credited as the album is. format "wav" makes a real WAV original,
 * "mp3" a pretend one of 192 kbps (it's only ever copied), NULL none at all. The track's
 * uid into uid, and its file's into file_uid unless NULL. */
static inline void fx_track(fx *x, const char *album, const char *title, int no, const char *format,
                     char uid[MC_UID_STRLEN], char *file_uid) {
  char sql[2048], rel[256], abs[2048], fuid[MC_UID_STRLEN] = "";
  mc_uid_new(uid);
  CHECK(mc_library_begin_write(x->lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " SELECT '%s', uid, artist_uid, '%s', %d, %d FROM albums WHERE uid = '%s'",
           uid, title, no, TRACK_MS, album);
  CHECK(fx_exec(x, sql) == SQLITE_OK);
  CHECK(mc_library_commit(x->lib) == MC_OK);
  if (format) {
    snprintf(rel, sizeof rel, "%s.%s", uid, format);
    snprintf(abs, sizeof abs, "%s/%s", x->libdir, rel);
    if (!strcmp(format, "wav")) {
      CHECK(fx_write_wav(abs, TRACK_MS, 220.0 + 110.0 * no));
    } else {
      FILE *f = mc_fopen(abs, "wb");
      CHECK(f != NULL);
      for (int i = 0; f && i < 36000; i++) fputc((i * 7 + no) & 0xff, f);
      if (f) fclose(f);
    }
    int64_t size = 0, mtime = 0;
    CHECK(mc_file_info(abs, &size, &mtime) == MC_OK);
    mc_uid_new(fuid);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
             " VALUES ('%s', '%s', '%s', '%s', %d, %lld, %lld)",
             fuid, uid, rel, format, strcmp(format, "wav") ? 192 : 1411, (long long)size,
             (long long)mtime);
    CHECK(mc_library_begin_write(x->lib) == MC_OK);
    CHECK(fx_exec(x, sql) == SQLITE_OK);
    CHECK(mc_library_commit(x->lib) == MC_OK);
  }
  if (file_uid) memcpy(file_uid, fuid, MC_UID_STRLEN);
}

/* The device's file name in it. */
static inline void fx_dev_path(fx *x, const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s", x->dev, name);
}

/* --- The device's manifest.tsv --- */

typedef struct mline {
  char key[64], track_uid[40], file_uid[40], setting[32], sha[65], format[16];
  int kbps;
  long long size;
  char title[128], artist[128], album[128];
  int duration_ms, play_count;
  long long last_played_ms;
} mline;

static inline char *fx_field(char **s) {
  char *start = *s;
  if (!start) return (char *)"";
  char *tab = strchr(start, '\t');
  if (tab) {
    *tab = '\0';
    *s = tab + 1;
  } else {
    char *nl = strpbrk(start, "\r\n");
    if (nl) *nl = '\0';
    *s = NULL;
  }
  return start;
}

/* Reads the manifest into lines (up to max); the count. */
static inline int fx_manifest(fx *x, mline *lines, int max) {
  char p[2048], buf[4096];
  fx_dev_path(x, "manifest.tsv", p, sizeof p);
  FILE *f = mc_fopen(p, "rb");
  int n = 0;
  while (f && n < max && fgets(buf, sizeof buf, f)) {
    if (buf[0] == '\n' || !buf[0]) continue;
    char *s = buf;
    mline *l = &lines[n++];
    memset(l, 0, sizeof *l);
    snprintf(l->key, sizeof l->key, "%s", fx_field(&s));
    snprintf(l->track_uid, sizeof l->track_uid, "%s", fx_field(&s));
    snprintf(l->file_uid, sizeof l->file_uid, "%s", fx_field(&s));
    snprintf(l->setting, sizeof l->setting, "%s", fx_field(&s));
    snprintf(l->sha, sizeof l->sha, "%s", fx_field(&s));
    snprintf(l->format, sizeof l->format, "%s", fx_field(&s));
    l->kbps = atoi(fx_field(&s));
    l->size = atoll(fx_field(&s));
    snprintf(l->title, sizeof l->title, "%s", fx_field(&s));
    snprintf(l->artist, sizeof l->artist, "%s", fx_field(&s));
    snprintf(l->album, sizeof l->album, "%s", fx_field(&s));
    l->duration_ms = atoi(fx_field(&s));
    l->play_count = atoi(fx_field(&s));
    l->last_played_ms = atoll(fx_field(&s));
  }
  if (f) fclose(f);
  return n;
}

/* The line for track_uid; NULL if none. */
static inline mline *fx_find(mline *lines, int n, const char *track_uid) {
  for (int i = 0; i < n; i++)
    if (!strcmp(lines[i].track_uid, track_uid)) return &lines[i];
  return NULL;
}

/* The file of a manifest line, in Music/. */
static inline void fx_music(fx *x, const mline *l, char *out, size_t cap) {
  snprintf(out, cap, "%s/Music/%s.%s", x->dev, l->key, l->format);
}

/* --- Syncing --- */

/* Syncs uid and waits for on_done; the result into r. 0 if it never ended. */
static inline int fx_sync(fx *x, const char *uid, mc_tethered_result *r) {
  mc_mutex_lock(&x->lock);
  x->done = 0;
  mc_mutex_unlock(&x->lock);
  mc_status st = mc_tethered_sync(x->t, uid);
  CHECK(st == MC_OK);
  if (st != MC_OK) return 0;
  for (int waited = 0; waited < WAIT_MS; waited += 20) {
    mc_mutex_lock(&x->lock);
    int done = x->done;
    if (done) *r = x->r;
    mc_mutex_unlock(&x->lock);
    if (done) {
      /* on_done comes a moment before syncing is cleared: wait for that too. */
      for (int i = 0; i < 500; i++) {
        mc_tethered_device_list *l = NULL;
        int busy = 0;
        if (mc_tethered_list(x->t, &l) == MC_OK)
          for (int k = 0; k < l->count; k++)
            if (!strcmp(l->items[k].uid, uid)) busy = l->items[k].syncing;
        mc_tethered_device_list_free(l);
        if (!busy) break;
        mc_sleep_ms(10);
      }
      return 1;
    }
    mc_sleep_ms(20);
  }
  fprintf(stderr, "sync never ended\n");
  return 0;
}

static inline void fx_print(const char *what, const mc_tethered_result *r) {
  fprintf(stderr,
          "%s: status %d over %d full %d written %d removed %d updated %d failed %d missing %d"
          " foreign %d plays %d/%d unscrobbled %d playlists %d\n",
          what, (int)r->status, r->over_budget, r->full, r->written, r->removed, r->updated,
          r->failed, r->missing, r->foreign, r->plays_imported, r->plays_unmatched,
          r->plays_not_scrobbled, r->playlists_imported);
}

#endif
