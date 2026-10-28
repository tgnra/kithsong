/* The importer's extended search (docs/tagging-rules.md), against a fake MusicBrainz
 * (mb_set_fake) where releases are credited to the artist while the files' album artist tag
 * names the label:
 *
 *   - "LABEL - Cyclick [ABC-0026]": the normal search (album with "LABEL") finds nothing, so
 *     the extended search runs by itself, finds the release by its catalogue number, scores
 *     it with the track artist, and the album is imported;
 *   - "LABEL (DJ X) - Night [XYZ-0100]": the normal search finds the release, but the album
 *     artist tag costs it enough to stay in review; mc_import_search_wider() rescores it with
 *     "DJ X" without accepting it, within the query budget.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

#define CYCLICK_ID "11111111-2222-3333-4444-555555555555"
#define NIGHT_ID "61111111-2222-3333-4444-555555555555"

static const char *const k_titles[] = {"One", "Two", "Three"};

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
static int write_track(const char *path, const char *album_artist, const char *album, int track) {
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
  tags.artist = "DJ X";
  tags.album_artist = (char *)album_artist;
  tags.album = (char *)album;
  tags.title = (char *)k_titles[track - 1];
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

static int write_album(const char *dir, const char *album_artist, const char *album) {
  if (mc_mkdirs(dir) != MC_OK) return 0;
  for (int i = 1; i <= 3; i++) {
    char path[1300];
    snprintf(path, sizeof path, "%s/0%d. DJ X \xE2\x80\x94 %s.wav", dir, i, k_titles[i - 1]);
    if (!write_track(path, album_artist, album, i)) return 0;
  }
  return 1;
}

/* --- Fake MusicBrainz --- */

static int g_searches_by; /* extended-search queries */
static int g_lookups;

/* The normal search: "Night" is found by title (whatever the artist); "Cyclick" only with
 * the artist MusicBrainz credits, which the files' tags don't have. */
static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)track_count;
  *n = 0;
  if (max < 1) return MC_OK;
  if (strcmp(album, "Night") == 0) memcpy(ids[(*n)++], NIGHT_ID, MC_UID_STRLEN);
  if (strcmp(album, "Cyclick") == 0 && artist && strcmp(artist, "DJ X") == 0)
    memcpy(ids[(*n)++], CYCLICK_ID, MC_UID_STRLEN);
  return MC_OK;
}

/* Extended search: the catalogue number finds Cyclick; nothing else finds anything. */
static mc_status fake_search_by(const mb_release_query *q, int track_count, char (*ids)[MC_UID_STRLEN],
                                int max, int *n) {
  (void)track_count;
  *n = 0;
  g_searches_by++;
  for (int i = 0; i < q->catno_count && max > 0; i++)
    if (strcmp(q->catnos[i], "ABC-0026") == 0) {
      memcpy(ids[0], CYCLICK_ID, MC_UID_STRLEN);
      *n = 1;
    }
  return MC_OK;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  int cyclick = strcmp(id, CYCLICK_ID) == 0;
  if (!cyclick && strcmp(id, NIGHT_ID) != 0) return MC_ERR_NOT_FOUND;
  g_lookups++;
  out->id = mc_strdup(id);
  out->title = mc_strdup(cyclick ? "Cyclick" : "Night");
  out->artist = mc_strdup("DJ X");
  out->primary_artist = mc_strdup("DJ X");
  out->releasegroup_id = mc_strdup(cyclick ? "21111111-2222-3333-4444-555555555555"
                                           : "71111111-2222-3333-4444-555555555555");
  out->release_type = mc_strdup("Album");
  out->label = mc_strdup("LABEL");
  out->catalog = mc_strdup(cyclick ? "ABC-0026" : "XYZ-0100");
  out->year = out->original_year = 2010;
  out->disc_total = 1;
  out->tracks = calloc(3, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  out->track_count = 3;
  for (int i = 0; i < 3; i++) {
    char tid[] = "31111110-2222-3333-4444-555555555555", rid[] = "41111110-2222-3333-4444-555555555555";
    tid[7] = rid[7] = (char)('1' + i);
    tid[0] = rid[0] = cyclick ? '3' : '8';
    mb_track *t = &out->tracks[i];
    t->id = mc_strdup(tid);
    t->recording_id = mc_strdup(rid);
    t->title = mc_strdup(k_titles[i]);
    t->artist = mc_strdup("DJ X");
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
  CHECK(write_album(dir, "LABEL", "Cyclick"));
  snprintf(dir, sizeof dir, "%s/LABEL (DJ X) - Night [XYZ-0100]", src);
  CHECK(write_album(dir, "LABEL (DJ X)", "Night"));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  /* Above the album artist's cost, so Night's normal match waits for review. */
  mc_import_options opts = {.threshold = 0.95, .fetch_art = 0};
  const char *paths[] = {src};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  CHECK(imp && wait_items(imp, 2));
  int cyclick = imp ? find_item(imp, "Cyclick") : -1, night = imp ? find_item(imp, "Night") : -1;
  CHECK(cyclick >= 0 && night >= 0);

  /* Cyclick: nothing from the normal search, so the extended search ran by itself; the
   * catalogue number found the release and the track artist made it a sure match. */
  mc_import_item *it = cyclick >= 0 ? wait_item(imp, cyclick, MC_IMPORT_DONE, 1) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(it->match_count == 1 && it->chosen == 0);
    CHECK(it->match_count && it->matches[0].confidence > 0.99);
    CHECK(it->match_count && strcmp(it->matches[0].mb_albumid, CYCLICK_ID) == 0);
    mc_import_item_free(it);
  }
  /* It stopped after the catalogue number search, the first, since that was enough. */
  CHECK(g_searches_by == 1);

  /* Night: a decent match, so no extended search by itself. */
  it = night >= 0 ? wait_item(imp, night, MC_IMPORT_REVIEW, 0) : NULL;
  CHECK(it != NULL);
  double before = 0;
  if (it) {
    CHECK(!it->searched_wider);
    CHECK(it->match_count == 1);
    before = it->match_count ? it->matches[0].confidence : 0;
    CHECK(before >= 0.75 && before < 0.95);
    mc_import_item_free(it);
  }

  /* On request: the same release, rescored with the artist in the album artist tag's
   * parentheses, and still waiting for the user. */
  int searches = g_searches_by, lookups = g_lookups;
  CHECK(night >= 0 && mc_import_search_wider(imp, night) == MC_OK);
  it = night >= 0 ? wait_item(imp, night, MC_IMPORT_REVIEW, 1) : NULL;
  CHECK(it != NULL);
  if (it) {
    CHECK(it->match_count == 1);
    CHECK(it->match_count && it->matches[0].confidence > 0.99 && it->matches[0].confidence > before);
    CHECK(it->chosen == -1);
    mc_import_item_free(it);
  }
  /* Catalogue number, title with DJ X, title with the label, title alone: the budget. */
  CHECK(g_searches_by - searches == 4);
  CHECK(g_lookups == lookups); /* the only hit was the release it had already */
  CHECK(mc_import_search_wider(imp, 99) == MC_ERR_INVALID_ARG);

  mc_import_free(imp);
  mc_library_close(lib);
  mb_set_fake(NULL);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
