/* Importing offline, without touching the network: MUSICAPP_MUSICBRAINZ_PROBE points the
 * offline check at a closed local port. Checks that the check fails, that an offline
 * session says so and puts every item in review without looking it up, that skipping
 * tagging then imports them all with their existing tags, and that a session started with
 * skip_tagging imports everything without checking at all. A picture embedded in a file is
 * written out as the album's cover.jpg. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* setenv */
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/i18n.h"
#include "musiccore/musiccore.h"
#include "util/platform.h"
#include "test.h"

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

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Not a whole JPEG, but what the cover code checks: the magic bytes. */
static const unsigned char k_jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 'c', 'o', 'v', 'e', 'r', 0xFF, 0xD9};

/* An ID3v2.3 tag holding k_jpeg as the front cover, as a WAV's "id3 " chunk carries it. */
static size_t id3_with_picture(unsigned char *out) {
  static const char mime[] = "image/jpeg";
  size_t body = 1 + sizeof mime + 1 + 1 + sizeof k_jpeg; /* encoding, MIME, type, description */
  size_t n = 0;
  memcpy(out, "ID3\x03\x00\x00", 6);
  size_t tag = 10 + body;
  for (int i = 0; i < 4; i++) out[6 + i] = (unsigned char)((tag >> (7 * (3 - i))) & 0x7F);
  n = 10;
  memcpy(out + n, "APIC", 4);
  for (int i = 0; i < 4; i++) out[n + 4 + i] = (unsigned char)(body >> (8 * (3 - i)));
  out[n + 8] = out[n + 9] = 0;
  n += 10;
  out[n++] = 0; /* ISO-8859-1 */
  memcpy(out + n, mime, sizeof mime);
  n += sizeof mime;
  out[n++] = 3; /* front cover */
  out[n++] = 0; /* no description */
  memcpy(out + n, k_jpeg, sizeof k_jpeg);
  return n + sizeof k_jpeg;
}

/* A tenth of a second of silent 8 kHz mono WAV, tagged, with k_jpeg embedded if picture. */
static int write_track(const char *path, const char *artist, const char *album, const char *title,
                       int track, int picture) {
  enum { DATA_LEN = 1600 };
  unsigned char id3[64];
  size_t id3_len = picture ? id3_with_picture(id3) : 0;
  size_t chunk_len = id3_len ? 8 + id3_len + (id3_len & 1) : 0;
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, (uint32_t)(36 + DATA_LEN + chunk_len));
  put_u32(hdr + 16, 16);
  hdr[20] = 1; /* PCM */
  hdr[22] = 1; /* mono */
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
  if (ok && chunk_len) {
    unsigned char chunk_hdr[8] = "id3 ";
    put_u32(chunk_hdr + 4, (uint32_t)id3_len);
    ok = fwrite(chunk_hdr, sizeof chunk_hdr, 1, f) == 1 && fwrite(id3, id3_len, 1, f) == 1 &&
         (!(id3_len & 1) || fputc(0, f) != EOF);
  }
  if (fclose(f) != 0 || !ok) return 0;

  mc_tags tags = {0};
  tags.artist = (char *)artist;
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

static void set_probe(const char *value) {
#ifdef _WIN32
  _putenv_s("MUSICAPP_MUSICBRAINZ_PROBE", value);
#else
  setenv("MUSICAPP_MUSICBRAINZ_PROBE", value, 1);
#endif
}

/* --- Sessions --- */

typedef struct events {
  mc_mutex lock;
  int offline_events;
  int offline; /* the last OFFLINE event's arg */
} events;

static void on_event(void *user, mc_import_event ev, int arg) {
  events *e = user;
  if (ev != MC_IMPORT_EV_OFFLINE) return;
  mc_mutex_lock(&e->lock);
  e->offline_events++;
  e->offline = arg;
  mc_mutex_unlock(&e->lock);
}

/* Waits until the session has items and every one is in state, or timeout_ms passes. */
static int wait_all(mc_import *imp, mc_import_state state, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), match = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      match += it->state == state;
      mc_import_item_free(it);
    }
    if (n > 0 && match == n) return n;
    mc_sleep_ms(20);
  }
  return 0;
}

static int64_t track_count(mc_library *lib) {
  int64_t n = -1;
  mc_library_count(lib, MC_ENTITY_TRACKS, &n);
  return n;
}

static int file_equals(const char *path, const unsigned char *want, size_t len) {
  unsigned char got[64];
  FILE *f = mc_fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(got, 1, sizeof got, f);
  fclose(f);
  return n == len && memcmp(got, want, len) == 0;
}

static int album_has_art(mc_library *lib, const char *title) {
  mc_album_list *list = NULL;
  int has = 0;
  if (mc_library_list_albums(lib, &list) != MC_OK) return 0;
  for (int i = 0; i < list->count; i++)
    if (list->items[i].title && !strcmp(list->items[i].title, title)) has = list->items[i].has_art;
  mc_album_list_free(list);
  return has;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  char root[1024], src[1100], album[1200], path[1300], lib1[1100], lib2[1100];
  snprintf(root, sizeof root, "%s/test_import_offline.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(album, sizeof album, "%s/Album", src);
  snprintf(lib1, sizeof lib1, "%s/lib1", root);
  snprintf(lib2, sizeof lib2, "%s/lib2", root);
  CHECK(mc_mkdirs(album) == MC_OK && mc_mkdirs(lib1) == MC_OK && mc_mkdirs(lib2) == MC_OK);

  /* An album of two tracks, the second with a picture embedded, and a loose single. */
  snprintf(path, sizeof path, "%s/01.wav", album);
  CHECK(write_track(path, "Nobody", "Offline Album", "First", 1, 0));
  snprintf(path, sizeof path, "%s/02.wav", album);
  CHECK(write_track(path, "Nobody", "Offline Album", "Second", 2, 1));
  snprintf(path, sizeof path, "%s/loose.wav", src);
  CHECK(write_track(path, "Nobody", NULL, "Loose", 0, 0));

  /* Nothing listens on port 1. */
  set_probe("127.0.0.1:1");
  CHECK(mc_musicbrainz_reachable(1000) == MC_ERR_NET);

  const char *paths[] = {src};
  events ev = {0};
  mc_mutex_init(&ev.lock);

  /* Offline: every item waits in review without a lookup, then skipping tagging imports them. */
  mc_library *lib = NULL;
  CHECK(mc_library_open(lib1, &lib) == MC_OK);
  mc_import *imp = NULL;
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};
  uint64_t start = mc_mono_ms();
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, on_event, &ev, &imp) == MC_OK);
  if (imp) {
    CHECK(wait_all(imp, MC_IMPORT_REVIEW, 20000) == 2);
    /* Lookups would each wait on the rate limit and a connect timeout; the check alone is quick. */
    CHECK(mc_mono_ms() - start < 10000);
    CHECK(mc_import_offline(imp) == 1);
    mc_mutex_lock(&ev.lock);
    CHECK(ev.offline_events == 1 && ev.offline == 1);
    mc_mutex_unlock(&ev.lock);
    for (int i = 0; i < 2; i++) {
      mc_import_item *it;
      CHECK(mc_import_item_get(imp, i, &it) == MC_OK);
      CHECK_STR(it->error, mc_str(MC_STR_IMPORT_ERROR_UNREACHABLE));
      CHECK(it->match_count == 0);
      mc_import_item_free(it);
    }

    CHECK(mc_import_skip_tagging(imp) == MC_OK);
    CHECK(wait_all(imp, MC_IMPORT_DONE, 20000) == 2);
    for (int i = 0; i < 2; i++) {
      mc_import_item *it;
      CHECK(mc_import_item_get(imp, i, &it) == MC_OK);
      CHECK(it->chosen == -1); /* as-is */
      mc_import_item_free(it);
    }
    mc_import_free(imp);
  }
  CHECK(track_count(lib) == 3);
  /* The embedded picture was written out as the album's cover (no download: fetch_art is off),
   * and the album knows it has art. */
  snprintf(path, sizeof path, "%s/Nobody/Offline Album/cover.jpg", lib1);
  CHECK(file_equals(path, k_jpeg, sizeof k_jpeg));
  CHECK(album_has_art(lib, "Offline Album"));
  mc_library_close(lib);

  /* skip_tagging from the start: imported as-is, no check, no OFFLINE event. */
  ev.offline_events = 0;
  lib = NULL;
  imp = NULL;
  CHECK(mc_library_open(lib2, &lib) == MC_OK);
  opts.skip_tagging = 1;
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, on_event, &ev, &imp) == MC_OK);
  if (imp) {
    CHECK(wait_all(imp, MC_IMPORT_DONE, 20000) == 2);
    CHECK(mc_import_offline(imp) == 0);
    mc_import_free(imp);
  }
  CHECK(ev.offline_events == 0);
  CHECK(track_count(lib) == 3);
  mc_library_close(lib);

  /* Bad arguments. */
  CHECK(mc_import_skip_tagging(NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_import_offline(NULL) == 0);

  mc_mutex_destroy(&ev.lock);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
