/* Removing files from an import (mc_import_remove) and putting them back (mc_import_put_back),
 * against a fake MusicBrainz (mb_set_fake) that knows one three-track release. An album folder
 * holds the release's three tracks, a bonus track the release doesn't have, and a file whose
 * album tag puts it in an item of its own:
 *
 *   - removing the bonus track leaves no extra file, so the match's confidence goes up, but
 *     the item stays in review;
 *   - removing a file that's on a release track leaves that track missing, and putting it
 *     back pairs it with the track again;
 *   - removing the other item's only file skips it, still related so it can be put back;
 *   - importing the album then imports only the files left, and the removed ones stay where
 *     they were, untouched;
 *   - keeping an unmatched item's existing tags imports only the files left too.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "kithcore/kithcore.h"
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

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, NULL};

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

/* The item's snapshot once its file and removed counts are as given, or NULL after timeout_ms. */
static mc_import_item *wait_counts(mc_import *imp, int idx, int files, int removed, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    mc_import_item *it;
    if (mc_import_item_get(imp, idx, &it) == MC_OK) {
      if (it->file_count == files && it->removed_count == removed) return it;
      mc_import_item_free(it);
    }
    mc_sleep_ms(20);
  }
  return NULL;
}

static int ends_with(const char *s, const char *end) {
  size_t n = s ? strlen(s) : 0, m = strlen(end);
  return n >= m && strcmp(s + n - m, end) == 0;
}

/* Whether dir/name is still there, titled title and with no MusicBrainz tags. */
static int file_unchanged(const char *dir, const char *name, const char *title) {
  char path[1300];
  snprintf(path, sizeof path, "%s/%s", dir, name);
  mc_tags tags;
  if (mc_tags_read(path, &tags) != MC_OK) return 0;
  int ok = tags.title && strcmp(tags.title, title) == 0 && !tags.mb_albumid;
  mc_tags_clear(&tags);
  return ok;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  char root[1024], src1[1100], src2[1100], path[1300], lib1[1100], lib2[1100];
  snprintf(root, sizeof root, "%s/test_import_remove.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src1, sizeof src1, "%s/src1/Album", root);
  snprintf(src2, sizeof src2, "%s/src2/Other", root);
  snprintf(lib1, sizeof lib1, "%s/lib1", root);
  snprintf(lib2, sizeof lib2, "%s/lib2", root);
  CHECK(mc_mkdirs(src1) == MC_OK && mc_mkdirs(src2) == MC_OK && mc_mkdirs(lib1) == MC_OK &&
        mc_mkdirs(lib2) == MC_OK);
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};

  snprintf(path, sizeof path, "%s/01.wav", src1);
  CHECK(write_track(path, "Test Album", "One", 1));
  snprintf(path, sizeof path, "%s/02.wav", src1);
  CHECK(write_track(path, "Test Album", "Two", 2));
  snprintf(path, sizeof path, "%s/03.wav", src1);
  CHECK(write_track(path, "Test Album", "Three", 3));
  snprintf(path, sizeof path, "%s/04.wav", src1);
  CHECK(write_track(path, "Test Album", "Bonus", 4));
  snprintf(path, sizeof path, "%s/05.wav", src1);
  CHECK(write_track(path, "Wrong Album", "Stray", 5));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  const char *paths1[] = {src1};
  CHECK(mc_library_open(lib1, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths1, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    /* Item 0 is the album, whose bonus track keeps it in review; item 1 the stray file. */
    CHECK(wait_all(imp, 2, MC_IMPORT_REVIEW, 20000));
    mc_import_item *it;
    double full = 1, complete = 0, lacking = 1;
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    CHECK(it->file_count == 4 && it->match_count == 1 && it->removed_count == 0);
    if (it->match_count == 1) {
      CHECK(it->matches[0].extra_files == 1);
      CHECK(it->matches[0].tracks[1].file == 1);
      full = it->matches[0].confidence;
    }
    if (it->file_count == 4) CHECK(ends_with(it->files[3].path, "04.wav"));
    mc_import_item_free(it);

    /* Bad arguments. */
    CHECK(mc_import_remove(imp, 0, 4) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_remove(imp, 0, -1) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_remove(imp, 2, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_remove(NULL, 0, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_put_back(imp, 0, 0) == MC_ERR_INVALID_ARG); /* nothing removed yet */

    /* 1. The bonus track: no extra file any more, confidence up, still in review. */
    CHECK(mc_import_remove(imp, 0, 3) == MC_OK);
    CHECK((it = wait_counts(imp, 0, 3, 1, 20000)) != NULL);
    if (it) {
      CHECK(it->state == MC_IMPORT_REVIEW);
      CHECK(ends_with(it->removed[0].path, "04.wav"));
      CHECK_STR(it->removed[0].title, "Bonus");
      if (it->match_count == 1) {
        CHECK(it->matches[0].extra_files == 0);
        complete = it->matches[0].confidence;
        CHECK(complete > full);
        for (int j = 0; j < 3; j++) CHECK(it->matches[0].tracks[j].file == j);
      }
      mc_import_item_free(it);
    }

    /* 2. Track 2's file: the track goes missing. */
    CHECK(mc_import_remove(imp, 0, 1) == MC_OK);
    CHECK((it = wait_counts(imp, 0, 2, 2, 20000)) != NULL);
    if (it) {
      CHECK(it->state == MC_IMPORT_REVIEW);
      CHECK(ends_with(it->removed[1].path, "02.wav"));
      if (it->match_count == 1) {
        CHECK(it->matches[0].tracks[0].file == 0);
        CHECK(it->matches[0].tracks[1].file == -1);
        CHECK(it->matches[0].tracks[2].file == 1);
        CHECK(it->matches[0].extra_files == 0);
        lacking = it->matches[0].confidence;
        CHECK(lacking < complete);
      }
      mc_import_item_free(it);
    }

    /* Put back, it's track 2 again, in its place; then it's removed again. */
    CHECK(mc_import_put_back(imp, 0, 2) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_put_back(imp, 0, 1) == MC_OK);
    CHECK((it = wait_counts(imp, 0, 3, 1, 20000)) != NULL);
    if (it) {
      CHECK(ends_with(it->files[1].path, "02.wav"));
      if (it->match_count == 1) {
        for (int j = 0; j < 3; j++) CHECK(it->matches[0].tracks[j].file == j);
        CHECK(it->matches[0].confidence > lacking);
        CHECK(it->matches[0].confidence > complete - 1e-9 && it->matches[0].confidence < complete + 1e-9);
      }
      mc_import_item_free(it);
    }
    CHECK(mc_import_remove(imp, 0, 1) == MC_OK);
    if ((it = wait_counts(imp, 0, 2, 2, 20000))) mc_import_item_free(it);

    /* 3. The stray file: its item is skipped, but still related, and can come back. */
    int related[4];
    CHECK(mc_import_related(imp, 0, related, 4) == 1 && related[0] == 1);
    CHECK(mc_import_remove(imp, 1, 0) == MC_OK);
    CHECK((it = wait_counts(imp, 1, 0, 1, 20000)) != NULL);
    if (it) {
      CHECK(it->state == MC_IMPORT_SKIPPED && it->outcome == MC_IMPORT_OUTCOME_REMOVED);
      CHECK(ends_with(it->removed[0].path, "05.wav"));
      mc_import_item_free(it);
    }
    CHECK(mc_import_related(imp, 0, related, 4) == 1 && related[0] == 1);
    CHECK(mc_import_accept_as_is(imp, 1) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_remove(imp, 1, 0) == MC_ERR_INVALID_ARG);
    /* Nothing can take a removed file. */
    CHECK(mc_import_assign(imp, 0, 0, 1, 1, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_put_back(imp, 1, 0) == MC_OK);
    CHECK((it = wait_counts(imp, 1, 1, 0, 20000)) != NULL);
    if (it) {
      CHECK(it->state == MC_IMPORT_REVIEW && it->outcome == MC_IMPORT_OUTCOME_NONE);
      mc_import_item_free(it);
    }
    CHECK(mc_import_remove(imp, 1, 0) == MC_OK);
    CHECK(wait_item(imp, 1, 0, MC_IMPORT_SKIPPED, 20000));

    /* 4. Imported: only One and Three; the removed files stay as they were. */
    CHECK(mc_import_accept(imp, 0, 0) == MC_OK);
    CHECK(wait_item(imp, 0, 2, MC_IMPORT_DONE, 20000));
    CHECK(track_count(lib) == 2);
    CHECK(mc_import_remove(imp, 0, 0) == MC_ERR_INVALID_ARG); /* not once it's imported */
    CHECK(mc_import_put_back(imp, 0, 0) == MC_ERR_INVALID_ARG);
    CHECK(mc_import_item_get(imp, 0, &it) == MC_OK);
    CHECK(it->removed_count == 2);
    if (it->match_count == 1) CHECK(it->matches[0].tracks[1].file == -1);
    mc_import_item_free(it);
    mc_import_free(imp);
  }
  CHECK(track_count(lib) == 2);
  CHECK(file_unchanged(src1, "02.wav", "Two"));
  CHECK(file_unchanged(src1, "04.wav", "Bonus"));
  CHECK(file_unchanged(src1, "05.wav", "Stray"));
  mc_library_close(lib);

  /* 5. No match: keeping the existing tags imports the files left. */
  snprintf(path, sizeof path, "%s/01.wav", src2);
  CHECK(write_track(path, "Unknown Album", "Keep", 1));
  snprintf(path, sizeof path, "%s/02.wav", src2);
  CHECK(write_track(path, "Unknown Album", "Drop", 2));
  lib = NULL;
  imp = NULL;
  const char *paths2[] = {src2};
  CHECK(mc_library_open(lib2, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths2, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    CHECK(wait_all(imp, 1, MC_IMPORT_REVIEW, 20000));
    CHECK(mc_import_remove(imp, 0, 1) == MC_OK);
    mc_import_item *it = wait_counts(imp, 0, 1, 1, 20000);
    CHECK(it != NULL);
    mc_import_item_free(it);
    CHECK(mc_import_accept_as_is(imp, 0) == MC_OK);
    CHECK(wait_item(imp, 0, 1, MC_IMPORT_DONE, 20000));
    mc_import_free(imp);
  }
  CHECK(track_count(lib) == 1);
  CHECK(file_unchanged(src2, "02.wav", "Drop"));
  mc_library_close(lib);

  mb_set_fake(NULL);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
