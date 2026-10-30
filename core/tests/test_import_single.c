/* Importing loose tracks (singles) against a fake MusicBrainz (mb_set_fake). A loose file is
 * matched to a recording, credited to one of its releases, and filed as that release's track
 * like any album track:
 *
 *   - a recording whose release has one track: the release's album, album artist, ids, track
 *     and disc numbers and date, under <album artist>/<album> (<year>)/;
 *   - one track of a three-track release, imported on its own: tagged as its track there, of
 *     three;
 *   - a recording whose release can't be looked up: what the search had, the release's title
 *     and ids and the track's number, with no track total;
 *   - a file that matches nothing, with no album tag, imported as-is: no album, in <artist>/.
 *
 * None makes a "<artist> Singles" album; the albums of one track aren't in the album list but
 * in the artist's singles.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/musicbrainz.h"
#include "util/platform.h"
#include "test.h"

#define ARTIST_ID "a1111111-2222-3333-4444-555555555555"
#define SINGLE_REL "b1111111-2222-3333-4444-555555555555" /* "Hey Ya!", one track */
#define ALBUM_REL "b2222222-2222-3333-4444-555555555555"  /* "Speakerboxxx", three */
#define GONE_REL "b3333333-2222-3333-4444-555555555555"   /* "Stankonia", not found */

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

/* A tenth of a second of silent 8 kHz mono WAV, tagged with an artist and title only. */
static int write_track(const char *path, const char *title) {
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
  tags.artist = "OutKast";
  tags.title = (char *)title;
  return mc_tags_write(path, &tags) == MC_OK;
}

/* --- Fake MusicBrainz --- */

/* Track i of a release: ids made from the release's first letters and i. */
static void fake_track(mb_track *t, char rel, int i, const char *title, int disc, int number) {
  char tid[] = "c0000000-2222-3333-4444-555555555555", rid[] = "d0000000-2222-3333-4444-555555555555";
  tid[1] = rid[1] = rel;
  tid[7] = rid[7] = (char)('1' + i);
  t->id = mc_strdup(tid);
  t->recording_id = mc_strdup(rid);
  t->title = mc_strdup(title);
  t->artist = mc_strdup("OutKast");
  t->artist_id = mc_strdup(ARTIST_ID);
  t->disc = disc;
  t->number = number;
  t->length_ms = 0; /* unknown, so only titles and artists count */
}

static void fake_release(mb_release *out, const char *id, const char *title, int year) {
  memset(out, 0, sizeof *out);
  out->id = mc_strdup(id);
  out->title = mc_strdup(title);
  out->artist = mc_strdup("OutKast");
  out->artist_id = mc_strdup(ARTIST_ID);
  out->primary_artist = mc_strdup("OutKast");
  out->primary_artist_id = mc_strdup(ARTIST_ID);
  char rg[] = "e0000000-2222-3333-4444-555555555555";
  rg[1] = id[1];
  out->releasegroup_id = mc_strdup(rg);
  out->year = out->original_year = year;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  if (strcmp(id, SINGLE_REL) == 0) {
    fake_release(out, id, "Hey Ya!", 2003);
    out->release_type = mc_strdup("Single");
    out->disc_total = 1;
    out->tracks = calloc(1, sizeof *out->tracks);
    if (!out->tracks) return MC_ERR_NOMEM;
    out->track_count = 1;
    fake_track(&out->tracks[0], '1', 0, "Hey Ya!", 1, 1);
    return MC_OK;
  }
  if (strcmp(id, ALBUM_REL) == 0) {
    static const char *const titles[] = {"GhettoMusick", "Roses", "Bowtie"};
    fake_release(out, id, "Speakerboxxx", 2003);
    out->release_type = mc_strdup("Album");
    out->disc_total = 1;
    out->tracks = calloc(3, sizeof *out->tracks);
    if (!out->tracks) return MC_ERR_NOMEM;
    out->track_count = 3;
    for (int i = 0; i < 3; i++) fake_track(&out->tracks[i], '2', i, titles[i], 1, i + 1);
    return MC_OK;
  }
  return MC_ERR_NOT_FOUND;
}

/* A recording as mb_search_recordings gives it: the release it's credited to, with only the
 * recording as its track, numbered as there. */
static mc_status fake_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                                        int *n) {
  (void)artist;
  *n = 0;
  if (max < 1) return MC_OK;
  const char *rel = NULL, *album = NULL;
  char key = 0;
  int index = 0;
  if (strcmp(title, "Hey Ya!") == 0) rel = SINGLE_REL, album = "Hey Ya!", key = '1', index = 0;
  else if (strcmp(title, "Roses") == 0) rel = ALBUM_REL, album = "Speakerboxxx", key = '2', index = 1;
  else if (strcmp(title, "Ms. Jackson") == 0) rel = GONE_REL, album = "Stankonia", key = '3', index = 3;
  if (!rel) return MC_OK;
  fake_release(out, rel, album, 2003);
  out->is_single = 1;
  free(out->artist_id); /* the recording's credit; the first credited artist stands in */
  out->artist_id = NULL;
  out->tracks = calloc(1, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  out->track_count = 1;
  fake_track(&out->tracks[0], key, index, title, 1, index + 1);
  *n = 1;
  return MC_OK;
}

static const mb_fake k_fake = {NULL, fake_lookup, fake_search_recordings, NULL, NULL};

/* --- Sessions --- */

static mc_import_state item_state(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return MC_IMPORT_FAILED;
  mc_import_state s = it->state;
  mc_import_item_free(it);
  return s;
}

/* Waits until every item has left PENDING, MATCHING and WRITING, or timeout_ms passes. */
static int wait_settled(mc_import *imp, int count, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_state s = item_state(imp, i);
      settled += s != MC_IMPORT_PENDING && s != MC_IMPORT_MATCHING && s != MC_IMPORT_WRITING;
    }
    if (n == count && settled == n) return 1;
    mc_sleep_ms(20);
  }
  return 0;
}

/* The item whose album (a single's title) is title, or -1. */
static int find_item(mc_import *imp, const char *title) {
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it;
    if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
    int same = it->album && strcmp(it->album, title) == 0;
    mc_import_item_free(it);
    if (same) return i;
  }
  return -1;
}

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

/* The file at rel in the library has these tags (album NULL: none). A track_total of -1
 * isn't checked. */
static void check_file(const char *lib_dir, const char *rel, const char *album, const char *album_id,
                       int track, int track_total, int year) {
  char path[1300];
  snprintf(path, sizeof path, "%s/%s", lib_dir, rel);
  CHECK(mc_file_exists(path));
  mc_tags tags;
  if (mc_tags_read(path, &tags) != MC_OK) {
    fprintf(stderr, "no tags at %s\n", path);
    CHECK(0);
    return;
  }
  if (album) {
    CHECK_STR(tags.album, album);
    CHECK_STR(tags.album_artist, "OutKast");
    CHECK_STR(tags.mb_albumid, album_id);
    CHECK(tags.mb_releasegroupid != NULL);
  } else {
    CHECK(!tags.album && !tags.album_artist && !tags.mb_albumid && !tags.mb_releasegroupid);
  }
  CHECK(tags.track == track);
  if (track_total >= 0) CHECK(tags.track_total == track_total);
  CHECK(tags.year == year);
  mc_tags_clear(&tags);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  char root[1024], src[1100], path[1300], lib_dir[1100];
  snprintf(root, sizeof root, "%s/test_import_single.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(src, sizeof src, "%s/src/Loose", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(src) == MC_OK && mc_mkdirs(lib_dir) == MC_OK);
  static const char *const titles[] = {"Hey Ya!", "Roses", "Ms. Jackson", "Qwxzzy Demo"};
  for (int i = 0; i < 4; i++) {
    snprintf(path, sizeof path, "%s/%d.wav", src, i + 1);
    CHECK(write_track(path, titles[i]));
  }

  mc_library *lib = NULL;
  mc_import *imp = NULL;
  const char *paths[] = {src};
  mc_import_options opts = {.threshold = 0, .fetch_art = 0};
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  CHECK(lib && mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    /* Four singles: three matched and imported by themselves, one left in review. */
    CHECK(wait_settled(imp, 4, 20000));
    for (int i = 0; i < 3; i++) {
      int idx = find_item(imp, titles[i]);
      CHECK(idx >= 0 && item_state(imp, idx) == MC_IMPORT_DONE);
      mc_import_item *it;
      if (idx >= 0 && mc_import_item_get(imp, idx, &it) == MC_OK) {
        CHECK(it->kind == MC_IMPORT_SINGLE && it->match_count == 1);
        if (it->match_count == 1) CHECK(it->matches[0].is_single && it->matches[0].album);
        mc_import_item_free(it);
      }
    }
    int demo = find_item(imp, "Qwxzzy Demo");
    CHECK(demo >= 0 && item_state(imp, demo) == MC_IMPORT_REVIEW);
    CHECK(mc_import_accept_as_is(imp, demo) == MC_OK);
    uint64_t deadline = mc_mono_ms() + 20000;
    while (mc_mono_ms() < deadline && item_state(imp, demo) != MC_IMPORT_DONE) mc_sleep_ms(20);
    CHECK(item_state(imp, demo) == MC_IMPORT_DONE);
    mc_import_free(imp);
  }

  /* The single's own release, of one track. */
  check_file(lib_dir, "OutKast/Hey Ya! (2003)/01 Hey Ya!.wav", "Hey Ya!", SINGLE_REL, 1, 1, 2003);
  /* One track of three, as on its release. */
  check_file(lib_dir, "OutKast/Speakerboxxx (2003)/02 Roses.wav", "Speakerboxxx", ALBUM_REL, 2, 3, 2003);
  /* The release isn't there to look up: the search's title, ids and number. */
  check_file(lib_dir, "OutKast/Stankonia (2003)/04 Ms. Jackson.wav", "Stankonia", GONE_REL, 4, 0, 2003);
  /* No match and no album tag: no album. */
  check_file(lib_dir, "OutKast/Qwxzzy Demo.wav", NULL, NULL, 0, -1, 0);

  if (lib) {
    /* Three ordinary albums of one track each, and a track with none; no singles albums. */
    CHECK(query_int(lib, "SELECT count(*) FROM albums") == 3);
    CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind <> 'album' OR title LIKE '%Singles'") == 0);
    CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE mb_albumid IS NOT NULL"
                         " AND mb_releasegroupid IS NOT NULL AND year = 2003") == 3);
    CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE album_uid IS NULL") == 1);
    CHECK(query_int(lib, "SELECT track_no FROM tracks WHERE title = 'Roses'") == 2);
    CHECK(query_int(lib, "SELECT count(*) FROM albums a JOIN artists ar ON ar.uid = a.artist_uid"
                         " WHERE ar.mb_artistid = '" ARTIST_ID "'") == 3);

    /* None is in the album list; all four are OutKast's singles. */
    mc_album_list *albums = NULL;
    CHECK(mc_library_list_albums(lib, &albums) == MC_OK && albums && albums->count == 0);
    mc_album_list_free(albums);
    mc_artist_list *artists = NULL;
    CHECK(mc_library_list_artists(lib, &artists) == MC_OK && artists && artists->count == 1);
    if (artists && artists->count == 1) {
      CHECK(artists->items[0].album_count == 0);
      mc_track_list *singles = NULL;
      CHECK(mc_library_list_artist_singles(lib, artists->items[0].uid, &singles) == MC_OK && singles &&
            singles->count == 4);
      for (int i = 0; singles && i < singles->count; i++) {
        const mc_track_info *t = &singles->items[i];
        CHECK(t->path && mc_file_exists(t->path));
        if (strcmp(t->title, "Qwxzzy Demo") == 0) CHECK(!t->album && !t->album_uid[0]);
        else CHECK(t->album && t->album_uid[0] && t->year == 2003);
      }
      mc_track_list_free(singles);
      albums = NULL;
      CHECK(mc_library_list_artist_albums(lib, artists->items[0].uid, &albums) == MC_OK && albums &&
            albums->count == 0);
      mc_album_list_free(albums);
    }
    mc_artist_list_free(artists);
    mc_library_close(lib);
  }

  mb_set_fake(NULL);
  if (!test_failures) remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
