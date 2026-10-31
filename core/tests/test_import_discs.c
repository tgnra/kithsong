/* Albums matched to some discs of a release while other items may be the rest, against a
 * fake MusicBrainz (mb_set_fake):
 *
 *   - "Twin/Disc One" and "Twin/Disc Two" share the album tag "Twin", but their folders
 *     aren't disc folders, so they're items of their own. MusicBrainz's "Twin" has two discs.
 *     Disc One is matched first, to disc 1 of it: it isn't put to the user but waits, saying
 *     it's waiting for the album's other disc, while Disc Two is looked up. Disc Two fits
 *     disc 2, so it's joined into Disc One, which then waits for the user with the release's
 *     every track paired, and Disc Two is skipped as moved. Accepting it imports both discs;
 *   - "Lonely/A" is matched to disc 1 of the two-disc "Lonely", and waits too while
 *     "Lonely/B" is looked up. But that's a release of its own, imported straight away, so
 *     "Lonely/A" goes to the user after all, as it is, once nothing else may be its disc 2.
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

#define TWIN_ID "a1111111-2222-3333-4444-555555555555"
#define LONELY_ID "b1111111-2222-3333-4444-555555555555"
#define LONELY_B_ID "c1111111-2222-3333-4444-555555555555"

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

/* A tenth of a second of silent 8 kHz mono WAV, tagged (no disc tag). */
static int write_track(const char *path, const char *album, const char *title, int track) {
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
  tags.artist = (char *)"Band";
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* A folder `name` in src with files titles[0..n), album tag `album`. */
static int write_folder(const char *src, const char *name, const char *album, const char *const *titles, int n) {
  char dir[1300], path[1400];
  snprintf(dir, sizeof dir, "%s/%s", src, name);
  if (mc_mkdirs(dir) != MC_OK) return 0;
  for (int i = 0; i < n; i++) {
    snprintf(path, sizeof path, "%s/0%d %s.wav", dir, i + 1, titles[i]);
    if (!write_track(path, album, titles[i], i + 1)) return 0;
  }
  return 1;
}

static const char *const k_twin1[] = {"Kick", "Snare", "Bass"};
static const char *const k_twin2[] = {"Hat", "Clap"};
static const char *const k_lonely1[] = {"Alpha", "Beta", "Gamma"};
static const char *const k_lonely2[] = {"Rain", "Snow"}; /* disc 2 of "Lonely", not here */
static const char *const k_lonely_b[] = {"Sun", "Moon"};

/* --- Fake MusicBrainz --- */

/* The second item of each album is looked up only once the test has seen the first one
 * waiting for it (or after 10 s, failing the test). */
static mc_mutex g_lock;
static int g_seen_waiting[2]; /* Twin, Lonely */

static void wait_seen(int which) {
  uint64_t deadline = mc_mono_ms() + 10000;
  for (;;) {
    mc_mutex_lock(&g_lock);
    int seen = g_seen_waiting[which];
    mc_mutex_unlock(&g_lock);
    if (seen || mc_mono_ms() >= deadline) return;
    mc_sleep_ms(10);
  }
}

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist;
  *n = 0;
  if (max < 2) return MC_OK;
  if (strcmp(album, "Twin") == 0) {
    if (track_count == 2) wait_seen(0);
    memcpy(ids[(*n)++], TWIN_ID, MC_UID_STRLEN);
  }
  if (strcmp(album, "Lonely") == 0) {
    if (track_count == 2) {
      wait_seen(1);
      memcpy(ids[(*n)++], LONELY_B_ID, MC_UID_STRLEN);
    }
    memcpy(ids[(*n)++], LONELY_ID, MC_UID_STRLEN);
  }
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
  int twin = strcmp(id, TWIN_ID) == 0, lonely = strcmp(id, LONELY_ID) == 0, lonely_b = strcmp(id, LONELY_B_ID) == 0;
  if (!twin && !lonely && !lonely_b) return MC_ERR_NOT_FOUND;
  out->id = mc_strdup(id);
  out->title = mc_strdup(twin ? "Twin" : "Lonely");
  out->artist = mc_strdup("Band");
  out->primary_artist = mc_strdup(out->artist);
  out->releasegroup_id = mc_strdup(twin ? "d1111111-2222-3333-4444-555555555555"
                                   : lonely ? "e1111111-2222-3333-4444-555555555555"
                                            : "f1111111-2222-3333-4444-555555555555");
  out->release_type = mc_strdup("Album");
  out->year = out->original_year = 2010;
  out->disc_total = lonely_b ? 1 : 2;
  out->tracks = calloc(5, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  for (int i = 0; twin && i < 3; i++) add_track(out, 1, i + 1, k_twin1[i], '1');
  for (int i = 0; twin && i < 2; i++) add_track(out, 2, i + 1, k_twin2[i], '1');
  for (int i = 0; lonely && i < 3; i++) add_track(out, 1, i + 1, k_lonely1[i], '2');
  for (int i = 0; lonely && i < 2; i++) add_track(out, 2, i + 1, k_lonely2[i], '2');
  for (int i = 0; lonely_b && i < 2; i++) add_track(out, 1, i + 1, k_lonely_b[i], '3');
  return MC_OK;
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, NULL};

/* --- Session --- */

static int busy(mc_import_state s) {
  return s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING;
}

static mc_import_item *get(mc_import *imp, int idx) {
  mc_import_item *it = NULL;
  if (idx < 0 || mc_import_item_get(imp, idx, &it) != MC_OK) return NULL;
  return it;
}

/* Whether path ends in tail, taking '/' and '\\' as the same. */
static int ends_with_path(const char *path, const char *tail) {
  size_t len = path ? strlen(path) : 0, n = strlen(tail);
  if (len < n) return 0;
  for (const char *a = path + len - n; *tail; a++, tail++) {
    int sa = *a == '/' || *a == '\\', st = *tail == '/' || *tail == '\\';
    if (sa != st || (!sa && *a != *tail)) return 0;
  }
  return 1;
}

/* The item whose folder ends in folder, or -1. */
static int find_item(mc_import *imp, const char *folder) {
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it = get(imp, i);
    if (!it) continue;
    int same = ends_with_path(it->folder, folder);
    mc_import_item_free(it);
    if (same) return i;
  }
  return -1;
}

/* Waits until the session has `count` items and none is busy. Meanwhile notes which albums
 * were seen waiting for their other disc, and whether any was put to the user then. */
static int wait_settled(mc_import *imp, int count, const char *waiting, int *early_review) {
  uint64_t deadline = mc_mono_ms() + 30000;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it = get(imp, i);
      if (!it) continue;
      settled += !busy(it->state);
      int held = it->state == MC_IMPORT_PENDING && it->error && strcmp(it->error, waiting) == 0;
      int which = !it->album ? -1 : strcmp(it->album, "Twin") == 0 ? 0 : strcmp(it->album, "Lonely") == 0 ? 1 : -1;
      if (which >= 0) {
        mc_mutex_lock(&g_lock);
        /* In review while its other disc is still being waited for: too early. */
        if (it->state == MC_IMPORT_REVIEW && !g_seen_waiting[which] && it->file_count == 3) (*early_review)++;
        if (held) g_seen_waiting[which] = 1;
        mc_mutex_unlock(&g_lock);
      }
      mc_import_item_free(it);
    }
    if (n == count && settled == n) return 1;
    mc_sleep_ms(5);
  }
  return 0;
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mc_mutex_init(&g_lock);
  mb_set_fake(&k_fake);
  char root[1024], src[1100], lib_dir[1100];
  snprintf(root, sizeof root, "%s/test_import_discs.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(lib_dir) == MC_OK);

  /* In path order, so matched in this order. */
  CHECK(write_folder(src, "Lonely/A", "Lonely", k_lonely1, 3));
  CHECK(write_folder(src, "Lonely/B", "Lonely", k_lonely_b, 2));
  CHECK(write_folder(src, "Twin/Disc One", "Twin", k_twin1, 3));
  CHECK(write_folder(src, "Twin/Disc Two", "Twin", k_twin2, 2));

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};
  const char *paths[] = {src};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  char *waiting = mc_format_alloc(mc_str_plural(MC_STR_IMPORT_WAITING_FOR_DISCS, 1), MC_D(1));
  int early_review = 0;
  CHECK(imp && waiting && wait_settled(imp, 4, waiting, &early_review));
  mc_free(waiting);

  /* Both first discs waited for the item after them rather than going to the user. */
  CHECK(g_seen_waiting[0] && g_seen_waiting[1]);
  CHECK(early_review == 0);

  /* Twin: one item of both discs, waiting for the user with every track paired. */
  int twin = find_item(imp, "Disc One"), twin2 = find_item(imp, "Disc Two");
  CHECK(twin >= 0 && twin2 >= 0);
  mc_import_item *it = get(imp, twin);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_REVIEW);
    CHECK_STR(it->album, "Twin");
    CHECK(it->file_count == 5);
    char *why = mc_format_alloc(mc_str_plural(MC_STR_IMPORT_JOINED_DISCS, 2), MC_D(2));
    CHECK_STR(it->error, why);
    mc_free(why);
    CHECK(it->match_count == 1);
    if (it->match_count) {
      const mc_import_match *m = &it->matches[0];
      CHECK(strcmp(m->mb_albumid, TWIN_ID) == 0 && m->confidence > 0.99 && m->extra_files == 0);
      for (int t = 0; t < m->track_count; t++) CHECK(m->tracks[t].file >= 0);
    }
    /* In disc order, a file with no disc tag taking its track's. */
    if (it->file_count == 5) {
      CHECK(it->files[2].disc == 1 && it->files[3].disc == 2 && it->files[3].track == 1);
      CHECK_STR(it->files[3].title, "Hat");
    }
    mc_import_item_free(it);
  }
  it = get(imp, twin2);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_SKIPPED && it->outcome == MC_IMPORT_OUTCOME_MOVED && it->file_count == 0);
    mc_import_item_free(it);
  }

  /* Lonely: B was a release of its own, so A goes to the user as it is, missing disc 2. */
  int lonely = find_item(imp, "Lonely/A"), lonely_b = find_item(imp, "Lonely/B");
  CHECK(lonely >= 0 && lonely_b >= 0);
  it = get(imp, lonely_b);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_DONE && it->file_count == 2);
    if (it->chosen >= 0 && it->chosen < it->match_count) CHECK(strcmp(it->matches[it->chosen].mb_albumid, LONELY_B_ID) == 0);
    mc_import_item_free(it);
  }
  it = get(imp, lonely);
  CHECK(it != NULL);
  if (it) {
    CHECK(it->state == MC_IMPORT_REVIEW && it->file_count == 3 && it->error == NULL);
    CHECK(it->match_count >= 1);
    if (it->match_count) CHECK(strcmp(it->matches[0].mb_albumid, LONELY_ID) == 0);
    mc_import_item_free(it);
  }

  /* Accepted, Twin goes in as one album of two discs. */
  CHECK(twin >= 0 && mc_import_accept(imp, twin, 0) == MC_OK);
  uint64_t deadline = mc_mono_ms() + 20000;
  int done = 0;
  while (twin >= 0 && !done && mc_mono_ms() < deadline) {
    if ((it = get(imp, twin))) done = it->state == MC_IMPORT_DONE;
    mc_import_item_free(it);
    if (!done) mc_sleep_ms(20);
  }
  CHECK(done);
  mc_import_free(imp);

  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  const char *uid = NULL;
  for (int i = 0; albums && i < albums->count; i++)
    if (albums->items[i].title && strcmp(albums->items[i].title, "Twin") == 0) uid = albums->items[i].uid;
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
  mc_mutex_destroy(&g_lock);
  mc_shutdown();
  return TEST_RESULT();
}
