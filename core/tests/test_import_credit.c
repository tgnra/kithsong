/* Artist credits through an import (see "Artist credits" in docs/tagging-rules.md). What the
 * importer hands to mc_import_db_write is watched (tagger_internal.h), so this checks the
 * credit arrays themselves, whatever the library makes of them:
 *
 *   - a matched album: the release's and each track's MusicBrainz credit, as it is (names as
 *     credited, ids, sort names, join phrases), and the files tagged with ARTISTS,
 *     ALBUMARTISTS and every id, ARTIST and ALBUMARTIST staying the credited strings, under
 *     the whole album credit's folder;
 *   - as-is imports: ARTISTS and MUSICBRAINZ_ARTISTID tags when the file has them, else the
 *     credit string split against the library's artists, and kept whole when the library
 *     doesn't know the parts, or knows the whole as one artist.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/musicbrainz.h"
#include "tagger/tagger_internal.h"
#include "util/platform.h"
#include "test.h"

#define JAY "f82bcf78-5b69-4622-a5ef-73800768d9ac"
#define RIRI "73e5e69d-3554-40d8-8516-00cb38737a1c"
#define KANYE "164f0d73-1234-4e2c-8743-d77bf2191051"
#define REL_ID "11111111-2222-3333-4444-555555555555"
#define XAV "a1111111-1111-4111-8111-111111111111"
#define YOL "a2222222-2222-4222-8222-222222222222"

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

/* A tenth of a second of silent 8 kHz mono WAV with these tags. */
static int write_track(const char *path, const mc_tags *tags) {
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
  return mc_tags_write(path, tags) == MC_OK;
}

/* --- Fake MusicBrainz: a release lookup's JSON, parsed as musicbrainz.org's would be --- */

static const char k_release_json[] =
  "{\"id\":\"" REL_ID "\",\"title\":\"Watch the Throne\",\"date\":\"2011-08-08\","
  "\"artist-credit\":["
  "{\"name\":\"Jay-Z\",\"joinphrase\":\" & \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Kanye West\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" KANYE "\","
  "\"name\":\"Kanye West\",\"sort-name\":\"West, Kanye\"}}],"
  "\"release-group\":{\"id\":\"21111111-2222-3333-4444-555555555555\",\"primary-type\":\"Album\","
  "\"first-release-date\":\"2011-08-08\"},"
  "\"media\":[{\"position\":1,\"format\":\"Digital Media\",\"tracks\":["
  "{\"id\":\"31111111-2222-3333-4444-555555555555\",\"position\":1,\"title\":\"Otis\","
  "\"artist-credit\":[{\"name\":\"Jay-Z\",\"joinphrase\":\"\",\"artist\":{\"id\":\"" JAY "\","
  "\"name\":\"JAY-Z\",\"sort-name\":\"Jay-Z\"}}],"
  "\"recording\":{\"id\":\"41111111-2222-3333-4444-555555555555\"}},"
  "{\"id\":\"32222222-2222-3333-4444-555555555555\",\"position\":2,\"title\":\"Run This Town\","
  "\"artist-credit\":["
  "{\"name\":\"Jay-Z\",\"joinphrase\":\" feat. \",\"artist\":{\"id\":\"" JAY "\",\"name\":\"JAY-Z\","
  "\"sort-name\":\"Jay-Z\"}},"
  "{\"name\":\"Rihanna\",\"artist\":{\"id\":\"" RIRI "\",\"name\":\"Rihanna\","
  "\"sort-name\":\"Rihanna\"}}],"
  "\"recording\":{\"id\":\"42222222-2222-3333-4444-555555555555\"}}]}]}";

static mc_status fake_search(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n) {
  (void)artist;
  (void)track_count;
  *n = 0;
  if (max > 0 && strcmp(album, "Watch the Throne") == 0) memcpy(ids[(*n)++], REL_ID, MC_UID_STRLEN);
  return MC_OK;
}

static mc_status fake_lookup(const char *id, mb_release *out) {
  memset(out, 0, sizeof *out);
  if (strcmp(id, REL_ID) != 0) return MC_ERR_NOT_FOUND;
  return mb_release_parse(k_release_json, sizeof k_release_json - 1, out);
}

static const mb_fake k_fake = {fake_search, fake_lookup, NULL, NULL, NULL};

/* --- What the importer writes --- */

/* A credit as text: "name<id>{sort}join|..." per part, "-" for none. */
static void credit_text(const mc_db_credit_part *c, int n, char *buf, size_t cap) {
  buf[0] = '\0';
  if (n == 0) {
    snprintf(buf, cap, "-");
    return;
  }
  for (int i = 0; i < n; i++) {
    size_t used = strlen(buf);
    snprintf(buf + used, cap - used, "%s%s<%s>{%s}%s", i ? "|" : "", c[i].artist.name ? c[i].artist.name : "?",
             c[i].artist.mb_artistid ? c[i].artist.mb_artistid : "", c[i].artist.sort_name ? c[i].artist.sort_name : "",
             c[i].join ? c[i].join : "");
  }
}

typedef struct seen {
  char title[128];
  char artist[128];    /* the whole credit */
  char artist_id[64];  /* its id, if any */
  char credit[512];
} seen;

static seen g_albums[8], g_tracks[16];
static int g_album_count, g_track_count;

static void note(seen *s, const char *title, const mc_db_artist *a, const mc_db_credit_part *c, int n) {
  snprintf(s->title, sizeof s->title, "%s", title ? title : "");
  snprintf(s->artist, sizeof s->artist, "%s", a->name ? a->name : "");
  snprintf(s->artist_id, sizeof s->artist_id, "%s", a->mb_artistid ? a->mb_artistid : "");
  credit_text(c, n, s->credit, sizeof s->credit);
}

static mc_status spy_write(mc_library *lib, const mc_db_album *album, const mc_db_track *tracks, int count) {
  if (g_album_count < 8) note(&g_albums[g_album_count++], album->title, &album->artist, album->credit, album->credit_count);
  for (int i = 0; i < count && g_track_count < 16; i++)
    note(&g_tracks[g_track_count++], tracks[i].title, &tracks[i].artist, tracks[i].credit, tracks[i].credit_count);
  return mc_import_db_write(lib, album, tracks, count);
}

static const seen *track_seen(const char *title) {
  for (int i = 0; i < g_track_count; i++)
    if (strcmp(g_tracks[i].title, title) == 0) return &g_tracks[i];
  return NULL;
}

static void check_track(const char *title, const char *artist, const char *artist_id, const char *credit) {
  const seen *s = track_seen(title);
  if (!s) {
    fprintf(stderr, "no track \"%s\" written\n", title);
    CHECK(0);
    return;
  }
  CHECK_STR(s->artist, artist);
  CHECK_STR(s->artist_id, artist_id);
  if (strcmp(s->credit, credit) != 0) {
    fprintf(stderr, "\"%s\": credit %s, expected %s\n", title, s->credit, credit);
    CHECK(0);
  }
}

/* --- Sessions --- */

static mc_import_state item_state(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return MC_IMPORT_FAILED;
  mc_import_state s = it->state;
  mc_import_item_free(it);
  return s;
}

/* Imports the folder; every item must end DONE. */
static void import_all(mc_library *lib, const char *dir, int skip_tagging, int items) {
  const char *paths[] = {dir};
  mc_import_options opts = {.threshold = 0, .fetch_art = 0, .skip_tagging = skip_tagging};
  mc_import *imp = NULL;
  CHECK(mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (!imp) return;
  uint64_t deadline = mc_mono_ms() + 20000;
  int done = 0;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp);
    done = 0;
    for (int i = 0; i < n; i++) done += item_state(imp, i) == MC_IMPORT_DONE;
    if (n == items && done == n) break;
    mc_sleep_ms(20);
  }
  CHECK(done == items);
  mc_import_free(imp);
}

static void add_artist(mc_library *lib, const char *uid, const char *name, const char *mbid) {
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(mc_library_db(lib), "INSERT INTO artists (uid, name, mb_artistid) VALUES (?1, ?2, ?3)",
                           -1, &stmt, NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
  if (mbid) sqlite3_bind_text(stmt, 3, mbid, -1, SQLITE_STATIC);
  CHECK(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void check_matched(const char *lib_dir) {
  CHECK(g_album_count == 1);
  if (g_album_count >= 1) {
    CHECK_STR(g_albums[0].artist, "Jay-Z & Kanye West");
    CHECK_STR(g_albums[0].artist_id, "");
    CHECK_STR(g_albums[0].credit, "Jay-Z<" JAY ">{Jay-Z} & |Kanye West<" KANYE ">{West, Kanye}");
  }
  check_track("Otis", "Jay-Z", JAY, "Jay-Z<" JAY ">{Jay-Z}");
  check_track("Run This Town", "Jay-Z feat. Rihanna", "", "Jay-Z<" JAY ">{Jay-Z} feat. |Rihanna<" RIRI ">{Rihanna}");

  /* The files: ARTIST and ALBUMARTIST credited, the lists beside them, folder unchanged. */
  char path[1300];
  snprintf(path, sizeof path, "%s/Jay-Z & Kanye West/Watch the Throne (2011)/02 Run This Town.wav", lib_dir);
  mc_tags t;
  CHECK(mc_tags_read(path, &t) == MC_OK);
  CHECK_STR(t.artist, "Jay-Z feat. Rihanna");
  CHECK_STR(t.artists, "Jay-Z\nRihanna");
  CHECK_STR(t.mb_artistids, JAY "\n" RIRI);
  CHECK_STR(t.album_artist, "Jay-Z & Kanye West");
  CHECK_STR(t.album_artists, "Jay-Z\nKanye West");
  CHECK_STR(t.mb_albumartistids, JAY "\n" KANYE);
  mc_tags_clear(&t);
  snprintf(path, sizeof path, "%s/Jay-Z & Kanye West/Watch the Throne (2011)/01 Otis.wav", lib_dir);
  CHECK(mc_tags_read(path, &t) == MC_OK);
  CHECK_STR(t.artist, "Jay-Z");
  CHECK_STR(t.artists, "Jay-Z");
  CHECK_STR(t.mb_artistids, JAY);
  CHECK_STR(t.mb_artistid, JAY);
  mc_tags_clear(&t);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  mb_set_fake(&k_fake);
  mc_import_set_db_write(spy_write);
  char root[1024], album_src[1100], loose_src[1100], path[1300], lib_dir[1100];
  snprintf(root, sizeof root, "%s/test_import_credit.d", argc > 1 ? argv[1] : ".");
  remove_tree(root);
  snprintf(album_src, sizeof album_src, "%s/src/Throne", root);
  snprintf(loose_src, sizeof loose_src, "%s/src2/Loose", root);
  snprintf(lib_dir, sizeof lib_dir, "%s/lib", root);
  CHECK(mc_mkdirs(album_src) == MC_OK && mc_mkdirs(loose_src) == MC_OK && mc_mkdirs(lib_dir) == MC_OK);

  /* An album with stale tags: matched, then tagged from MusicBrainz. */
  static const char *const titles[] = {"Otis", "Run This Town"};
  for (int i = 0; i < 2; i++) {
    mc_tags t = {0};
    t.title = (char *)titles[i];
    t.artist = "Jay Z";
    t.album = "Watch the Throne";
    t.album_artist = "Jay Z and Kanye West";
    t.artists = "Somebody\nElse"; /* replaced */
    t.track = i + 1;
    snprintf(path, sizeof path, "%s/%d.wav", album_src, i + 1);
    CHECK(write_track(path, &t));
  }

  /* Loose files, imported as they are. */
  struct {
    const char *file, *title, *artist, *artists, *ids, *id;
  } loose[] = {
    {"duet.wav", "Duet", "Xavier feat. Yolanda", "Xavier\nYolanda", XAV "\n" YOL, NULL},
    {"split.wav", "Split", "Pat & Quinn", NULL, NULL, NULL},
    {"case.wav", "Case", "pat x QUINN", NULL, NULL, NULL},
    {"unknown.wav", "Unknown", "Ray & Sam", NULL, NULL, NULL},
    {"pair.wav", "Pair", "Simon & Garfunkel", NULL, NULL, NULL},
    {"old.wav", "Old", "Old & Combined", NULL, NULL, NULL},
    {"solo.wav", "Solo", "Xavier", "Xavier", XAV, XAV},
    {"feat.wav", "Feat", "Pat (feat. Ray)", NULL, NULL, NULL},
  };
  int loose_count = (int)(sizeof loose / sizeof loose[0]);
  for (int i = 0; i < loose_count; i++) {
    mc_tags t = {0};
    t.title = (char *)loose[i].title;
    t.artist = (char *)loose[i].artist;
    t.artists = (char *)loose[i].artists;
    t.mb_artistids = (char *)loose[i].ids;
    snprintf(path, sizeof path, "%s/%s", loose_src, loose[i].file);
    CHECK(write_track(path, &t));
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  if (lib) {
    import_all(lib, album_src, 0, 1);
    check_matched(lib_dir);

    add_artist(lib, "00000000-0000-7000-8000-000000000001", "Pat", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000002", "Quinn", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000003", "Simon", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000004", "Garfunkel", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000005", "Simon & Garfunkel",
               "5d02f264-e225-41ff-83f7-d9b1f0b1874a");
    add_artist(lib, "00000000-0000-7000-8000-000000000006", "Old", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000007", "Combined", NULL);
    add_artist(lib, "00000000-0000-7000-8000-000000000008", "Old & Combined", NULL); /* not one */
    g_album_count = g_track_count = 0;
    import_all(lib, loose_src, 1, loose_count);
    check_track("Duet", "Xavier feat. Yolanda", "", "Xavier<" XAV ">{} feat. |Yolanda<" YOL ">{}");
    check_track("Split", "Pat & Quinn", "", "Pat<>{} & |Quinn<>{}");
    check_track("Case", "pat x QUINN", "", "pat<>{} x |QUINN<>{}");
    check_track("Unknown", "Ray & Sam", "", "-");
    check_track("Pair", "Simon & Garfunkel", "", "-");
    check_track("Old", "Old & Combined", "", "Old<>{} & |Combined<>{}");
    check_track("Solo", "Xavier", XAV, "-");
    check_track("Feat", "Pat (feat. Ray)", "", "Pat<>{} (feat. |Ray<>{}");
    mc_library_close(lib);
  }

  mc_import_set_db_write(NULL);
  mb_set_fake(NULL);
  if (!test_failures) remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
