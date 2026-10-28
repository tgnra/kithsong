/* Assigning a file to a release track the tagger left missing (mc_import_assign), against a
 * fake MusicBrainz (mb_set_fake) that knows one three-track release:
 *
 *   - a file grouped into an item of its own (its album tag is wrong), with both items
 *     already imported: the file moves into the album, is re-tagged and renamed as the
 *     track, its library row moves too, the album it was filed under is deleted, and the
 *     item it left is skipped as moved;
 *   - a file of the album's own item that matched no track, while the item is in review:
 *     assigning it imports the album with the file as that track.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

#define REL_ID "11111111-2222-3333-4444-555555555555"

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

/* A tenth of a second of silent 8 kHz mono WAV, tagged. */
static int write_track(const char *path, const char *album, const char *title, int track) {
  enum { DATA_LEN = 1600 };
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + DATA_LEN);
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
  if (fclose(f) != 0 || !ok) return 0;

  mc_tags tags = {0};
  tags.artist = "Band";
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* --- Fake MusicBrainz --- */

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist, (void)track_count;
  *n = 0;
  if (max > 0 && strcmp(album, "Test Album") == 0) {
    memcpy(ids[0], REL_ID, MC_UID_STRLEN);
    *n = 1;
  }
  return MC_OK;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  if (strcmp(id, REL_ID) != 0) return MC_ERR_NOT_FOUND;
  static const char *const titles[] = {"One", "Two", "Three"};
  out->id = mc_strdup(REL_ID);
  out->title = mc_strdup("Test Album");
  out->artist = mc_strdup("Band");
  out->primary_artist = mc_strdup("Band");
  out->releasegroup_id = mc_strdup("21111111-2222-3333-4444-555555555555");
  out->release_type = mc_strdup("Album");
  out->year = out->original_year = 2001;
  out->disc_total = 1;
  out->tracks = calloc(3, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  out->track_count = 3;
  for (int i = 0; i < 3; i++) {
    char tid[] = "31111110-2222-3333-4444-555555555555", rid[] = "41111110-2222-3333-4444-555555555555";
    tid[7] = rid[7] = (char)('1' + i);
    mb_track *t = &out->tracks[i];
    t->id = mc_strdup(tid);
    t->recording_id = mc_strdup(rid);
    t->title = mc_strdup(titles[i]);
    t->artist = mc_strdup("Band");
    t->disc = 1;
    t->number = i + 1;
    t->length_ms = 0; /* unknown, so only titles and numbers count */
  }
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL};

/* --- Sessions --- */

static mc_import_state item_state(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return MC_IMPORT_FAILED;
  mc_import_state s = it->state;
  mc_import_item_free(it);
  return s;
}

/* Waits until the session has `count` items, each in state, or timeout_ms passes. */
static int wait_all(mc_import *imp, int count, mc_import_state state, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), match = 0;
    for (int i = 0; i < n; i++) match += item_state(imp, i) == state;
    if (n == count && match == n) return 1;
    mc_sleep_ms(20);
  }
  return 0;
}

/* Waits until item idx has `files` files and is in state. */
static int wait_item(mc_import *imp, int idx, int files, mc_import_state state, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    mc_import_item *it;
    if (mc_import_item_get(imp, idx, &it) == MC_OK) {
      int ok = it->file_count == files && it->state == state;
      mc_import_item_free(it);
      if (ok) return 1;
    }
    mc_sleep_ms(20);
  }
  return 0;
}

static int64_t track_count(mc_library *lib) {
  int64_t n = -1;
  mc_library_count(lib, MC_ENTITY_TRACKS, &n);
  return n;
}

/* The uid of the album titled title, or "" if there's none. */
static void album_uid(mc_library *lib, const char *title, char uid[MC_UID_STRLEN]) {
  uid[0] = '\0';
  mc_album_list *list = NULL;
  if (mc_library_list_albums(lib, &list) != MC_OK) return;
  for (int i = 0; i < list->count; i++)
    if (list->items[i].title && strcmp(list->items[i].title, title) == 0) memcpy(uid, list->items[i].uid, MC_UID_STRLEN);
  mc_album_list_free(list);
}

/* Checks the library has Test Album with all three tracks, track 3 being titled "Three" in
 * its row and in its file's tags. */
static void check_album(mc_library *lib) {
  char uid[MC_UID_STRLEN];
  album_uid(lib, "Test Album", uid);
  CHECK(uid[0]);
  mc_track_list *tracks = NULL;
  CHECK(uid[0] && mc_library_list_album_tracks(lib, uid, &tracks) == MC_OK);
  if (!tracks) return;
  CHECK(tracks->count == 3);
  if (tracks->count == 3) {
    const mc_track_info *t = &tracks->items[2];
    CHECK(t->track_no == 3);
    CHECK_STR(t->title, "Three");
    CHECK(t->path && mc_file_exists(t->path));
    CHECK(t->path && strstr(t->path, "03 Three.wav"));
    mc_tags tags;
    CHECK(t->path && mc_tags_read(t->path, &tags) == MC_OK);
    if (t->path) {
      CHECK_STR(tags.title, "Three");
      CHECK_STR(tags.album, "Test Album");
      CHECK_STR(tags.mb_albumid, REL_ID);
      CHECK_STR(tags.mb_releasetrackid, "31111113-2222-3333-4444-555555555555");
      CHECK(tags.track == 3 && tags.track_total == 3);
      mc_tags_clear(&tags);
    }
  }
  mc_track_list_free(tracks);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  char root[1024], src1[1100], src2[1100], path[1300], lib1[1100], lib2[1100];
  snprintf(root, sizeof root, "%s/test_import_assign.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src1, sizeof src1, "%s/src1/Album", root);
  snprintf(src2, sizeof src2, "%s/src2/Album", root);
  snprintf(lib1, sizeof lib1, "%s/lib1", root);
  snprintf(lib2, sizeof lib2, "%s/lib2", root);
  CHECK(mc_mkdirs(src1) == MC_OK && mc_mkdirs(src2) == MC_OK && mc_mkdirs(lib1) == MC_OK &&
        mc_mkdirs(lib2) == MC_OK);
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};

  /* 1. Track 3's album tag is wrong, so it's an item of its own; both get imported. */
  snprintf(path, sizeof path, "%s/01.wav", src1);
  CHECK(write_track(path, "Test Album", "One", 1));
  snprintf(path, sizeof path, "%s/02.wav", src1);
  CHECK(write_track(path, "Test Album", "Two", 2));
  snprintf(path, sizeof path, "%s/03.wav", src1);
  CHECK(write_track(path, "Wrong Album", "Three", 3));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  const char *paths1[] = {src1};
  CHECK(mc_library_open(lib1, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths1, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    /* Item 0 is the album with track 3 missing; item 1 the stray file, with no matches. */
    CHECK(wait_all(imp, 2, MC_IMPORT_REVIEW, 20000));
    mc_import_item *it;
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    CHECK(it->file_count == 2 && it->match_count == 1);
    if (it->match_count == 1) CHECK(it->matches[0].track_count == 3 && it->matches[0].tracks[2].file == -1);
    mc_import_item_free(it);
    int related[4];
    CHECK(mc_import_related(imp, 0, related, 4) == 1 && related[0] == 1);
    CHECK(mc_import_related(imp, 1, related, 4) == 1 && related[0] == 0);

    CHECK(mc_import_accept(imp, 0, 0) == MC_OK);
    CHECK(mc_import_accept_as_is(imp, 1) == MC_OK);
    CHECK(wait_all(imp, 2, MC_IMPORT_DONE, 20000));
    CHECK(track_count(lib) == 3);
    char wrong[MC_UID_STRLEN];
    album_uid(lib, "Wrong Album", wrong);
    CHECK(wrong[0]);
    char *stray = NULL;
    CHECK(mc_import_item_get(imp, 1, &it) == MC_OK);
    if (it->file_count == 1) stray = mc_strdup(it->files[0].path); /* in the library now */
    mc_import_item_free(it);
    CHECK(stray && mc_file_exists(stray));

    /* Bad arguments: no such track, match, item or file. */
    CHECK(mc_import_assign(imp, 0, 0, 3, 1, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_assign(imp, 0, 1, 2, 1, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_assign(imp, 0, 0, 2, 2, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_assign(imp, 0, 0, 2, 1, 1) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_assign(NULL, 0, 0, 2, 1, 0) == MC_ERR_INVALID_ARG);

    CHECK(mc_import_assign(imp, 0, 0, 2, 1, 0) == MC_OK);
    CHECK(wait_item(imp, 1, 0, MC_IMPORT_SKIPPED, 20000));
    /* Item 0 gets the file at once, then goes through WRITING back to DONE. */
    uint64_t deadline = mc_mono_ms() + 20000;
    while (mc_mono_ms() < deadline && !(stray && !mc_file_exists(stray) && item_state(imp, 0) == MC_IMPORT_DONE))
      mc_sleep_ms(20);
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    CHECK(it->state == MC_IMPORT_DONE && it->file_count == 3 && it->chosen == 0);
    if (it->match_count == 1) {
      CHECK(it->matches[0].tracks[2].file == 2);
      CHECK(it->matches[0].extra_files == 0);
    }
    mc_import_item_free(it);
    CHECK(mc_import_item_get(imp, 1, &it) == MC_OK);
    CHECK(it->outcome == MC_IMPORT_OUTCOME_MOVED && it->file_count == 0);
    mc_import_item_free(it);
    /* An item with no files can't be imported any more, nor is it related to anything. */
    CHECK(mc_import_accept_as_is(imp, 1) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_related(imp, 0, related, 4) == 0);

    mc_import_free(imp);
    CHECK(stray && !mc_file_exists(stray));
    free(stray);
  }
  CHECK(track_count(lib) == 3); /* moved, not copied */
  check_album(lib);
  char wrong[MC_UID_STRLEN];
  album_uid(lib, "Wrong Album", wrong);
  CHECK(!wrong[0]); /* left empty, so deleted */
  snprintf(path, sizeof path, "%s/Band/Wrong Album", lib1);
  CHECK(!mc_file_exists(path));
  mc_library_close(lib);

  /* 2. Track 3's file matches nothing (wrong title and number), and the album is still in
   * review: assigning it imports the album. */
  snprintf(path, sizeof path, "%s/01.wav", src2);
  CHECK(write_track(path, "Test Album", "One", 1));
  snprintf(path, sizeof path, "%s/02.wav", src2);
  CHECK(write_track(path, "Test Album", "Two", 2));
  snprintf(path, sizeof path, "%s/03.wav", src2);
  CHECK(write_track(path, "Test Album", "Qwxzzy", 9));

  lib = NULL;
  imp = NULL;
  const char *paths2[] = {src2};
  CHECK(mc_library_open(lib2, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths2, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    CHECK(wait_all(imp, 1, MC_IMPORT_REVIEW, 20000));
    mc_import_item *it;
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    CHECK(it->file_count == 3 && it->match_count == 1);
    if (it->match_count == 1) {
      CHECK(it->matches[0].tracks[2].file == -1);
      CHECK(it->matches[0].extra_files == 1);
    }
    mc_import_item_free(it);
    CHECK(mc_import_related(imp, 0, NULL, 0) == 0);

    CHECK(mc_import_assign(imp, 0, 0, 2, 0, 2) == MC_OK);
    CHECK(wait_all(imp, 1, MC_IMPORT_DONE, 20000));
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    if (it->match_count == 1) CHECK(it->matches[0].tracks[2].file == 2 && it->matches[0].extra_files == 0);
    mc_import_item_free(it);
    mc_import_free(imp);
  }
  CHECK(track_count(lib) == 3);
  check_album(lib);
  mc_library_close(lib);

  /* The originals are never changed. */
  mc_tags tags;
  snprintf(path, sizeof path, "%s/03.wav", src2);
  CHECK(mc_tags_read(path, &tags) == MC_OK);
  CHECK_STR(tags.title, "Qwxzzy");
  mc_tags_clear(&tags);

  mb_set_fake(NULL);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
