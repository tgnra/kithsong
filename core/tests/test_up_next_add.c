/* What Play Next and Play Last add for something in the library: which tracks, one block or a
 * track each, and a block's second line. Needs no audio device: it goes no further than what's
 * handed to the player. */
#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"

static void populate(mc_library *lib) {
  CHECK(sqlite3_exec(mc_library_db(lib),
                     "INSERT INTO artists (uid, name) VALUES ('ar1', 'Abba'), ('ar2', 'Low');"
                     "INSERT INTO albums (uid, title, artist_uid, kind) VALUES ('al1', 'Arrival', 'ar1', 'album'),"
                     " ('al2', 'Away', 'ar2', 'album');"
                     "INSERT INTO tracks (uid, album_uid, title, track_no, duration_ms) VALUES"
                     " ('t1', 'al1', 'One', 1, 1000), ('t2', 'al1', 'Two', 2, 2000), ('t3', 'al1', 'Three', 3, 3000),"
                     " ('t4', 'al2', 'Elsewhere', 1, 4000);"
                     "INSERT INTO files (uid, track_uid, path, format) VALUES"
                     " ('f1', 't1', 'a/1.flac', 'flac'), ('f3', 't3', 'a/3.flac', 'flac');",
                     NULL, NULL, NULL) == SQLITE_OK);
}

/* Loads it and checks what's there: the titles of its tracks joined with '|'. */
static void check_tracks(mc_library *lib, mc_up_next_kind kind, const char *uid, const char *want) {
  mc_up_next_add *add = NULL;
  CHECK(mc_up_next_add_load(lib, kind, uid, "Name", 0, &add) == MC_OK);
  if (!add) return;
  char got[256] = "";
  for (int i = 0; i < add->count; i++) {
    if (i) strcat(got, "|");
    strcat(got, add->tracks[i].title);
  }
  CHECK(!strcmp(got, want));
  mc_up_next_add_free(add);
}

/* Loads it and checks its block's title and second line, either of which may be NULL. */
static void check_row(mc_library *lib, mc_up_next_kind kind, const char *uid, const char *name, int as_tracks,
                      const char *title, const char *artist) {
  mc_up_next_add *add = NULL;
  CHECK(mc_up_next_add_load(lib, kind, uid, name, as_tracks, &add) == MC_OK);
  if (!add) return;
  CHECK(!title == !add->title);
  if (title) CHECK_STR(add->title, title);
  CHECK(!artist == !add->artist);
  if (artist) CHECK_STR(add->artist, artist);
  mc_up_next_add_free(add);
}

int main(int argc, char **argv) {
  char path[1024], file[1100];
  snprintf(path, sizeof path, "%s/test_up_next_add.d", argc > 1 ? argv[1] : ".");
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm"};
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", path, names[i]);
    remove(file);
  }
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();
  populate(lib);

  char entry[MC_UID_STRLEN], playlist[MC_UID_STRLEN], group[MC_UID_STRLEN], smart[MC_UID_STRLEN], smart_group[MC_UID_STRLEN];
  CHECK(mc_playlist_create(lib, "Mix", playlist) == MC_OK);
  CHECK(mc_playlist_append(lib, playlist, "t3", entry) == MC_OK);
  CHECK(mc_playlist_append(lib, playlist, "t2", entry) == MC_OK); /* no file here */
  CHECK(mc_playlist_append(lib, playlist, "t1", entry) == MC_OK);
  CHECK(mc_album_group_create(lib, "Swedes", group) == MC_OK);
  CHECK(mc_album_group_add(lib, group, "al1") == MC_OK);
  CHECK(mc_smart_create(lib, "Ones", MC_SMART_TRACKS, "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"One\"}}",
                        smart) == MC_OK);
  CHECK(mc_smart_create(lib, "Arrivals", MC_SMART_ALBUMS,
                        "{\"v\":1,\"match\":{\"field\":\"title\",\"op\":\"is\",\"value\":\"Arrival\"}}", smart_group) == MC_OK);

  /* Only what has a file here, in its own order. */
  check_tracks(lib, MC_UP_NEXT_TRACK, "t1", "One");
  check_tracks(lib, MC_UP_NEXT_ALBUM, "al1", "One|Three");
  check_tracks(lib, MC_UP_NEXT_ARTIST, "ar1", "One|Three");
  check_tracks(lib, MC_UP_NEXT_PLAYLIST, playlist, "Three|One");
  check_tracks(lib, MC_UP_NEXT_SMART_PLAYLIST, smart, "One");
  check_tracks(lib, MC_UP_NEXT_ALBUM_GROUP, group, "One|Three");
  check_tracks(lib, MC_UP_NEXT_SMART_ALBUM_GROUP, smart_group, "One|Three");

  /* A block's rows. A track is never one, and an artist's second line is the frontend's. */
  check_row(lib, MC_UP_NEXT_TRACK, "t1", "Name", 0, NULL, NULL);
  check_row(lib, MC_UP_NEXT_ALBUM, "al1", "Arrival", 0, "Arrival", "Abba");
  check_row(lib, MC_UP_NEXT_ARTIST, "ar1", "Abba", 0, "Abba", NULL);
  check_row(lib, MC_UP_NEXT_PLAYLIST, playlist, "Mix", 0, "Mix", mc_str(MC_STR_PLAYLIST));
  check_row(lib, MC_UP_NEXT_SMART_PLAYLIST, smart, "Ones", 0, "Ones", mc_str(MC_STR_SMART_PLAYLIST));
  check_row(lib, MC_UP_NEXT_ALBUM_GROUP, group, "Swedes", 0, "Swedes", mc_str(MC_STR_ALBUM_GROUP));
  check_row(lib, MC_UP_NEXT_SMART_ALBUM_GROUP, smart_group, "Arrivals", 0, "Arrivals", mc_str(MC_STR_ALBUM_GROUP));

  /* A track each, by the setting or with no name. */
  check_row(lib, MC_UP_NEXT_ALBUM, "al1", "Arrival", 1, NULL, NULL);
  check_row(lib, MC_UP_NEXT_PLAYLIST, playlist, NULL, 0, NULL, NULL);

  /* What the play log gets. */
  mc_up_next_add *add = NULL;
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_ALBUM, "al1", "Arrival", 0, &add) == MC_OK);
  CHECK(add && add->context == MC_PLAY_CONTEXT_ALBUM);
  if (add) CHECK_STR(add->context_uid, "al1");
  mc_up_next_add_free(add);
  add = NULL;
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_TRACK, "t1", NULL, 0, &add) == MC_OK);
  CHECK(add && add->context == MC_PLAY_CONTEXT_MANUAL && !add->context_uid);
  mc_up_next_add_free(add);

  /* Nothing with a file here, or nothing there. */
  add = (mc_up_next_add *)1;
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_ALBUM, "al2", "Away", 0, &add) == MC_ERR_NOT_FOUND);
  CHECK(!add);
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_TRACK, "t2", NULL, 0, &add) == MC_ERR_NOT_FOUND);
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_TRACK, "nope", NULL, 0, &add) == MC_ERR_NOT_FOUND);
  CHECK(mc_up_next_add_load(lib, MC_UP_NEXT_PLAYLIST, "nope", "Gone", 0, &add) == MC_ERR_NOT_FOUND);
  CHECK(mc_up_next_add_load(NULL, MC_UP_NEXT_TRACK, "t1", NULL, 0, &add) == MC_ERR_INVALID_ARG);

  mc_library_close(lib);
  return TEST_RESULT();
}
