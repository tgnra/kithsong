/* The importer's matching order, one-file items and disc folders, against a fake MusicBrainz
 * (mb_set_fake):
 *
 *   - one-file items are matched after the albums, though the mix sorts first;
 *   - a one-file DJ mix whose album tag is its title has no recording on MusicBrainz, so the
 *     extended search runs for it, finds the mix's release by the catalogue number in its album
 *     tag, and it's imported as that release;
 *   - "Abunai Disc1" and "Abunai Disc2", side by side, are joined: MusicBrainz has a two-disc
 *     "Abunai" whose discs fit them. The joined item waits for the user, though its match is
 *     sure, and the other item is skipped as moved. Accepting it imports both discs;
 *   - "Other Disc1" and "Other Disc2" stay apart: MusicBrainz's "Other" has one disc.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/i18n.h"
#include "musiccore/musiccore.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

#define MIX_ID "11111111-2222-3333-4444-555555555555"
#define ABUNAI_ID "21111111-2222-3333-4444-555555555555"
#define OTHER_ID "31111111-2222-3333-4444-555555555555"
#define MIX "BREAK IT OLDSCHOOL [SRPC-L007]"

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
static int write_track(const char *path, const char *artist, const char *album, const char *title, int track) {
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

  mc_tags tags = {0};
  tags.artist = (char *)artist;
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* A folder `name` in src with the files of one disc: titles[0..n), album tag `album`. */
static int write_disc(const char *src, const char *name, const char *album, const char *const *titles, int n) {
  char dir[1300], path[1400];
  snprintf(dir, sizeof dir, "%s/%s", src, name);
  if (mc_mkdirs(dir) != MC_OK) return 0;
  for (int i = 0; i < n; i++) {
    snprintf(path, sizeof path, "%s/0%d %s.wav", dir, i + 1, titles[i]);
    if (!write_track(path, "Band", album, titles[i], i + 1)) return 0;
  }
  return 1;
}

static const char *const k_abunai1[] = {"Kick", "Snare", "Bass"};
static const char *const k_abunai2[] = {"Hat", "Clap"};
static const char *const k_other1[] = {"Uno", "Dos"};
static const char *const k_other2[] = {"Tres", "Cuatro"};

/* --- Fake MusicBrainz --- */

static int g_other2_searched;  /* the last album's own search has run */
static int g_mix_after_albums; /* ... by the time the mix was looked up */
static int g_mix_lookups;

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist, (void)track_count;
  *n = 0;
  if (strcmp(album, "Other Disc2") == 0) g_other2_searched = 1;
  if (max < 1) return MC_OK;
  if (strcmp(album, "Abunai") == 0) memcpy(ids[(*n)++], ABUNAI_ID, MC_UID_STRLEN);
  if (strcmp(album, "Other") == 0) memcpy(ids[(*n)++], OTHER_ID, MC_UID_STRLEN);
  return MC_OK;
}

static mc_status fake_search_by(const mb_release_query *q, int track_count, char (*ids)[MC_UID_STRLEN],
                                int max, int *n) {
  (void)track_count;
  *n = 0;
  for (int i = 0; i < q->catno_count && max > 0; i++)
    if (strcmp(q->catnos[i], "SRPC-L007") == 0) {
      memcpy(ids[0], MIX_ID, MC_UID_STRLEN);
      *n = 1;
    }
  return MC_OK;
}

static mc_status fake_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                                        int *n) {
  (void)artist, (void)out, (void)max;
  *n = 0;
  if (strcmp(title, MIX) == 0) g_mix_after_albums = g_other2_searched;
  return MC_OK;
}

static void add_track(mb_release *rel, int disc, int number, const char *title, char tag) {
  mb_track *t = &rel->tracks[rel->track_count++];
  char tid[] = "40000000-2222-3333-4444-555555555555", rid[] = "50000000-2222-3333-4444-555555555555";
  tid[0] = rid[1] = tag;
  tid[6] = rid[6] = (char)('0' + disc);
  tid[7] = rid[7] = (char)('0' + number);
  t->id = mc_strdup(tid);
  t->recording_id = mc_strdup(rid);
  t->title = mc_strdup(title);
  t->artist = mc_strdup(rel->artist);
  t->disc = disc;
  t->number = number;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  int mix = strcmp(id, MIX_ID) == 0, abunai = strcmp(id, ABUNAI_ID) == 0, other = strcmp(id, OTHER_ID) == 0;
  if (!mix && !abunai && !other) return MC_ERR_NOT_FOUND;
  g_mix_lookups += mix;
  out->id = mc_strdup(id);
  out->title = mc_strdup(mix ? "BREAK IT OLDSCHOOL" : abunai ? "Abunai" : "Other");
  out->artist = mc_strdup(mix ? "DJ SHARPNEL" : "Band");
  out->primary_artist = mc_strdup(out->artist);
  out->releasegroup_id = mc_strdup(mix ? "61111111-2222-3333-4444-555555555555"
                                       : abunai ? "71111111-2222-3333-4444-555555555555"
                                                : "81111111-2222-3333-4444-555555555555");
  out->release_type = mc_strdup("Album");
  out->catalog = mc_strdup(mix ? "SRPC-L007" : NULL);
  out->year = out->original_year = 2005;
  out->disc_total = abunai ? 2 : 1;
  out->tracks = calloc(5, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  if (mix) add_track(out, 1, 1, "BREAK IT OLDSCHOOL", '1');
  for (int i = 0; abunai && i < 3; i++) add_track(out, 1, i + 1, k_abunai1[i], '2');
  for (int i = 0; abunai && i < 2; i++) add_track(out, 2, i + 1, k_abunai2[i], '2');
  for (int i = 0; other && i < 2; i++) add_track(out, 1, i + 1, k_other1[i], '3');
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, fake_search_recordings, NULL, fake_search_by};

/* --- Session --- */

static int busy(mc_import_state s) {
  return s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING;
}

/* Waits until the session has `count` items and none is busy. */
static int wait_settled(mc_import *imp, int count) {
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      settled += !busy(it->state);
      mc_import_item_free(it);
    }
    if (n == count && settled == n) return 1;
    mc_sleep_ms(20);
  }
  return 0;
}

/* The item whose album is album (as grouped, or as joined), or -1. */
static int find_item(mc_import *imp, const char *album) {
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it;
    if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
    int same = it->album && strcmp(it->album, album) == 0;
    mc_import_item_free(it);
    if (same) return i;
  }
  return -1;
}

static mc_import_item *get(mc_import *imp, int idx) {
  mc_import_item *it = NULL;
  if (idx < 0 || mc_import_item_get(imp, idx, &it) != MC_OK) return NULL;
  return it;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  char root[1024], src[1100], lib_dir[1100], dir[1300], path[1400];
  snprintf(root, sizeof root, "%s/test_import_order.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(lib_dir) == MC_OK);

  /* "A Mix" sorts first, so it is item 0. */
  snprintf(dir, sizeof dir, "%s/A Mix", src);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/BREAK IT OLDSCHOOL.wav", dir);
  CHECK(write_track(path, "DJ SHARPNEL", MIX, MIX, 0));
  CHECK(write_disc(src, "Abunai Disc1", "Abunai Disc1", k_abunai1, 3));
  CHECK(write_disc(src, "Abunai Disc2", "Abunai Disc2", k_abunai2, 2));
  CHECK(write_disc(src, "Other Disc1", "Other Disc1", k_other1, 2));
  CHECK(write_disc(src, "Other Disc2", "Other Disc2", k_other2, 2));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};
  const char *paths[] = {src};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  CHECK(imp && wait_settled(imp, 5));

  /* The mix: matched last, by the extended search, and imported as its release. */
  CHECK(g_mix_after_albums);
  mc_import_item *it = get(imp, find_item(imp, MIX));
  CHECK(it != NULL);
  if (it) {
    CHECK(it->kind == MC_IMPORT_SINGLE);
    CHECK(it->state == MC_IMPORT_DONE && it->searched_wider);
    CHECK(it->match_count == 1 && it->chosen == 0);
    if (it->match_count) CHECK(strcmp(it->matches[0].mb_albumid, MIX_ID) == 0 && !it->matches[0].is_single);
    mc_import_item_free(it);
  }
  CHECK(g_mix_lookups == 1);

  /* Abunai: one item of both discs, waiting for the user with a sure match. */
  int abunai = find_item(imp, "Abunai"), disc2 = find_item(imp, "Abunai Disc2");
  CHECK(abunai >= 0 && disc2 >= 0);
  it = get(imp, abunai);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_REVIEW);
    CHECK(it->file_count == 5);
    char *why = mc_format_alloc(mc_str_plural(MC_STR_IMPORT_JOINED_DISCS, 2), MC_D(2));
    CHECK_STR(it->error, why);
    mc_free(why);
    CHECK(it->match_count == 1);
    if (it->match_count) {
      const mc_import_match *m = &it->matches[0];
      CHECK(strcmp(m->mb_albumid, ABUNAI_ID) == 0 && m->confidence > 0.99 && m->extra_files == 0);
      for (int t = 0; t < m->track_count; t++) CHECK(m->tracks[t].file >= 0);
    }
    if (it->file_count == 5) CHECK(it->files[3].disc == 2 && it->files[3].track == 1);
    mc_import_item_free(it);
  }
  it = get(imp, disc2);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_SKIPPED && it->outcome == MC_IMPORT_OUTCOME_MOVED && it->file_count == 0);
    mc_import_item_free(it);
  }

  /* Other: MusicBrainz has no two-disc "Other", so both discs are items of their own. */
  int other1 = find_item(imp, "Other Disc1"), other2 = find_item(imp, "Other Disc2");
  CHECK(other1 >= 0 && other2 >= 0);
  for (int k = 0; k < 2; k++) {
    it = get(imp, k ? other2 : other1);
    CHECK(it != NULL);
    if (it) {
      CHECK(it->state == MC_IMPORT_REVIEW && it->file_count == 2);
      mc_import_item_free(it);
    }
  }

  /* Accepted, Abunai goes in as one album of two discs. */
  CHECK(abunai >= 0 && mc_import_accept(imp, abunai, 0) == MC_OK);
  uint64_t deadline = mc_mono_ms() + 20000;
  int done = 0;
  while (abunai >= 0 && !done && mc_mono_ms() < deadline) {
    if ((it = get(imp, abunai))) done = it->state == MC_IMPORT_DONE;
    mc_import_item_free(it);
    if (!done) mc_sleep_ms(20);
  }
  CHECK(done);
  mc_import_free(imp);

  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  const char *uid = NULL;
  for (int i = 0; albums && i < albums->count; i++)
    if (albums->items[i].title && strcmp(albums->items[i].title, "Abunai") == 0) uid = albums->items[i].uid;
  CHECK(uid != NULL);
  mc_track_list *tracks = NULL;
  CHECK(uid && mc_library_list_album_tracks(lib, uid, &tracks) == MC_OK);
  if (tracks) {
    CHECK(tracks->count == 5);
    if (tracks->count == 5) {
      CHECK(tracks->items[3].disc_no == 2 && tracks->items[3].track_no == 1);
      CHECK_STR(tracks->items[3].title, "Hat");
      CHECK(tracks->items[3].path && strstr(tracks->items[3].path, "2-01 Hat.wav"));
    }
    mc_track_list_free(tracks);
  }
  mc_album_list_free(albums);

  mc_library_close(lib);
  mb_set_fake(NULL);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
