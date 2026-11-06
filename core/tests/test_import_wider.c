/* The importer's extended search and its scoring (docs/tagging-rules.md), against a fake
 * MusicBrainz (mb_set_fake) where releases are credited to the artist while the files'
 * album artist tag names the label:
 *
 *   - "LABEL - Cyclick [ABC-0026]": the normal search (album with "LABEL") finds nothing, so
 *     the extended search runs by itself, finds the release by its catalogue number, scores
 *     it with the track artist, and the album is imported;
 *   - "SPEEDCORE DANDY", album artist "SHARPNELSOUND (m1dy)": the normal search finds the
 *     release by "m1dy", which is scored with the artist in the tag's parentheses, so the
 *     album is imported without the extended search;
 *   - "DJ X - Day [XYZ-0200]", an ordinary album: the normal search finds only a release with
 *     a track more, scored as before and left in review; mc_import_search_wider() finds the
 *     right one by its catalogue number without accepting it, and stops there;
 *   - "DJ X - Dawn (Promo CDr)": the edition note is left out of the search terms, so the
 *     normal search finds "Dawn", and the album is imported.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

#define CYCLICK_ID "11111111-2222-3333-4444-555555555555"
#define DANDY_ID "61111111-2222-3333-4444-555555555555"
#define DAY4_ID "91111111-2222-3333-4444-555555555555" /* Day with a fourth track */
#define DAY_ID "a1111111-2222-3333-4444-555555555555"
#define DAWN_ID "b1111111-2222-3333-4444-555555555555"

static const char *const k_titles[] = {"One", "Two", "Three", "Four"};

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
static int write_track(const char *path, const char *artist, const char *album_artist, const char *album,
                       int track) {
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
  tags.album_artist = (char *)album_artist;
  tags.album = (char *)album;
  tags.title = (char *)k_titles[track - 1];
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* Three tracks by artist, named "01. Artist — One.wav" and so on. */
static int write_album(const char *dir, const char *artist, const char *album_artist, const char *album) {
  if (mc_mkdirs(dir) != MC_OK) return 0;
  for (int i = 1; i <= 3; i++) {
    char path[1300];
    snprintf(path, sizeof path, "%s/0%d. %s \xE2\x80\x94 %s.wav", dir, i, artist, k_titles[i - 1]);
    if (!write_track(path, artist, album_artist, album, i)) return 0;
  }
  return 1;
}

/* --- Fake MusicBrainz --- */

typedef struct fake_release {
  const char *id, *title, *artist, *catalog;
  int tracks;
} fake_release;

static const fake_release k_releases[] = {
    {CYCLICK_ID, "Cyclick", "DJ X", "ABC-0026", 3},
    {DANDY_ID, "SPEEDCORE DANDY", "m1dy", "SRPC-L005", 3},
    {DAY4_ID, "Day", "DJ X", "XYZ-0201", 4},
    {DAY_ID, "Day", "DJ X", "XYZ-0200", 3},
    {DAWN_ID, "Dawn", "DJ X", "XYZ-0300", 3},
};
#define RELEASES (int)(sizeof k_releases / sizeof *k_releases)

static int g_searches_by; /* extended-search queries */
static int g_lookups;

/* The normal search: "SPEEDCORE DANDY" is found by title (whatever the artist); "Cyclick"
 * only with the artist MusicBrainz credits, which the files' tags don't have; "Day" only
 * as the release with a fourth track; "Dawn" only as itself, without an edition note. */
static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)track_count;
  *n = 0;
  if (max < 1) return MC_OK;
  int by_dj_x = artist && strcmp(artist, "DJ X") == 0;
  if (strcmp(album, "SPEEDCORE DANDY") == 0) memcpy(ids[(*n)++], DANDY_ID, MC_UID_STRLEN);
  if (strcmp(album, "Cyclick") == 0 && by_dj_x) memcpy(ids[(*n)++], CYCLICK_ID, MC_UID_STRLEN);
  if (strcmp(album, "Day") == 0 && by_dj_x) memcpy(ids[(*n)++], DAY4_ID, MC_UID_STRLEN);
  if (strcmp(album, "Dawn") == 0 && by_dj_x) memcpy(ids[(*n)++], DAWN_ID, MC_UID_STRLEN);
  return MC_OK;
}

/* Extended search: only catalogue numbers find anything. */
static mc_status fake_search_by(const mb_release_query *q, int track_count, char (*ids)[MC_UID_STRLEN],
                                int max, int *n) {
  (void)track_count;
  *n = 0;
  g_searches_by++;
  for (int i = 0; i < q->catno_count; i++)
    for (int r = 0; r < RELEASES && *n < max; r++)
      if (strcmp(q->catnos[i], k_releases[r].catalog) == 0) memcpy(ids[(*n)++], k_releases[r].id, MC_UID_STRLEN);
  return MC_OK;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  int r = 0;
  while (r < RELEASES && strcmp(id, k_releases[r].id) != 0) r++;
  if (r == RELEASES) return MC_ERR_NOT_FOUND;
  const fake_release *f = &k_releases[r];
  g_lookups++;
  char rgid[] = "21111111-2222-3333-4444-555555555555";
  rgid[0] = (char)('2' + r);
  out->id = mc_strdup(id);
  out->title = mc_strdup(f->title);
  out->artist = mc_strdup(f->artist);
  out->primary_artist = mc_strdup(f->artist);
  out->releasegroup_id = mc_strdup(rgid);
  out->release_type = mc_strdup("Album");
  out->label = mc_strdup("LABEL");
  out->catalog = mc_strdup(f->catalog);
  out->year = out->original_year = 2010;
  out->disc_total = 1;
  out->tracks = calloc((size_t)f->tracks, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  out->track_count = f->tracks;
  for (int i = 0; i < f->tracks; i++) {
    char tid[] = "31111110-2222-3333-4444-555555555555", rid[] = "41111110-2222-3333-4444-555555555555";
    tid[7] = rid[7] = (char)('1' + i);
    tid[0] = rid[0] = (char)('a' + r);
    mb_track *t = &out->tracks[i];
    t->id = mc_strdup(tid);
    t->recording_id = mc_strdup(rid);
    t->title = mc_strdup(k_titles[i]);
    t->artist = mc_strdup(f->artist);
    t->disc = 1;
    t->number = i + 1;
  }
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, fake_search_by};

/* --- Session --- */

/* The item whose album tag is album, or -1. */
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

/* Waits until item idx is in state and (if wider) has had the extended search. */
static mc_import_item *wait_item(mc_import *imp, int idx, mc_import_state state, int wider) {
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline) {
    mc_import_item *it;
    if (mc_import_item_get(imp, idx, &it) == MC_OK) {
      if (it->state == state && (!wider || it->searched_wider)) return it;
      mc_import_item_free(it);
    }
    mc_sleep_ms(20);
  }
  return NULL;
}

static int wait_items(mc_import *imp, int count) {
  uint64_t deadline = mc_mono_ms() + 20000;
  while (mc_mono_ms() < deadline && mc_import_item_count(imp) < count) mc_sleep_ms(20);
  return mc_import_item_count(imp) == count;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  char root[1024], src[1100], dir[1300], lib_dir[1100];
  snprintf(root, sizeof root, "%s/test_import_wider.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(lib_dir) == MC_OK);
  snprintf(dir, sizeof dir, "%s/LABEL - Cyclick [ABC-0026] (C78)", src);
  CHECK(write_album(dir, "DJ X", "LABEL", "Cyclick"));
  snprintf(dir, sizeof dir, "%s/SPEEDCORE DANDY", src);
  CHECK(write_album(dir, "m1dy", "SHARPNELSOUND (m1dy)", "SPEEDCORE DANDY"));
  snprintf(dir, sizeof dir, "%s/DJ X - Day [XYZ-0200]", src);
  CHECK(write_album(dir, "DJ X", "DJ X", "Day"));
  snprintf(dir, sizeof dir, "%s/DJ X - Dawn (Promo CDr)", src);
  CHECK(write_album(dir, "DJ X", "DJ X", "Dawn (Promo CDr)"));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  mc_import_options opts = {.fetch_art = 0}; /* the default threshold, 90% */
  const char *paths[] = {src};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  CHECK(imp && wait_items(imp, 4));
  int cyclick = imp ? find_item(imp, "Cyclick") : -1, dandy = imp ? find_item(imp, "SPEEDCORE DANDY") : -1;
  int day = imp ? find_item(imp, "Day") : -1, dawn = imp ? find_item(imp, "Dawn (Promo CDr)") : -1;
  CHECK(cyclick >= 0 && dandy >= 0 && day >= 0 && dawn >= 0);

  /* Dawn: searched without "(Promo CDr)", found, and close enough to import. */
  mc_import_item *it = dawn >= 0 ? wait_item(imp, dawn, MC_IMPORT_DONE, 0) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(it->match_count >= 1 && strcmp(it->matches[0].mb_albumid, DAWN_ID) == 0);
    CHECK(!it->searched_wider);
    mc_import_item_free(it);
  }

  /* Cyclick: nothing from the normal search, so the extended search ran by itself; the
   * catalogue number found the release and the track artist made it a sure match. */
  it = cyclick >= 0 ? wait_item(imp, cyclick, MC_IMPORT_DONE, 1) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(it->match_count == 1 && it->chosen == 0);
    CHECK(it->match_count && it->matches[0].confidence > 0.99);
    CHECK(it->match_count && strcmp(it->matches[0].mb_albumid, CYCLICK_ID) == 0);
    mc_import_item_free(it);
  }
  /* It stopped after the catalogue number search, the first, since that was enough. */
  CHECK(g_searches_by == 1);

  /* SPEEDCORE DANDY: scored with "m1dy" from the album artist tag's parentheses, a sure
   * match, imported with no extended search. */
  it = dandy >= 0 ? wait_item(imp, dandy, MC_IMPORT_DONE, 0) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(!it->searched_wider);
    CHECK(it->match_count == 1 && it->chosen == 0);
    CHECK(it->match_count && it->matches[0].confidence > 0.99);
    CHECK(it->match_count && strcmp(it->matches[0].mb_albumid, DANDY_ID) == 0);
    mc_import_item_free(it);
  }

  /* Day: its own album artist fits, so it scores as it always did, perfect but for the
   * missing track: a quarter of the release's tracks, weighing 10 of 32 units (the files
   * have no year, so the year's half doesn't count). Too good for the extended search by
   * itself, but partial, so it waits for the user. */
  it = day >= 0 ? wait_item(imp, day, MC_IMPORT_REVIEW, 0) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(!it->searched_wider);
    CHECK(it->match_count == 1);
    CHECK(it->match_count && strcmp(it->matches[0].mb_albumid, DAY4_ID) == 0);
    CHECK(it->match_count && fabs(it->matches[0].confidence - (1.0 - 10.0 / 4 / 32)) < 1e-6);
    mc_import_item_free(it);
  }
  CHECK(g_searches_by == 1);

  /* On request: the catalogue number finds the complete release, which comes first but
   * waits for the user like the rest; that was enough, so the search stops there. */
  int searches = g_searches_by, lookups = g_lookups;
  CHECK(day >= 0 && mc_import_search_wider(imp, day) == MC_OK);
  it = day >= 0 ? wait_item(imp, day, MC_IMPORT_REVIEW, 1) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(it->match_count == 2);
    CHECK(it->match_count && strcmp(it->matches[0].mb_albumid, DAY_ID) == 0);
    CHECK(it->match_count && it->matches[0].confidence > 0.99);
    CHECK(it->chosen == -1);
    mc_import_item_free(it);
  }
  CHECK(g_searches_by - searches == 1);
  CHECK(g_lookups - lookups == 1);
  CHECK(mc_import_search_wider(imp, 99) == MC_ERR_INVALID_ARG);

  mc_import_free(imp);
  mc_library_close(lib);
  mb_set_fake(NULL);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
