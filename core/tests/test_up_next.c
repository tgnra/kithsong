/* Up next, the queue people add to. The list's own workings need no audio device: blocks and
 * tracks, Play Next against Play Last, moving, removing, clearing, and the setting. The rest plays
 * silent WAVs on the real device, and is skipped (exit 77) without one: up next plays before the
 * list carries on, blocks in their own order with shuffle on, what's left of a block, Next and
 * Previous, a file that won't open, and what the play log says. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "kithcore/settings.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A silent 16-bit PCM WAV. */
static void write_wav(const char *path, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  unsigned rate = 8000, channels = 1;
  uint32_t frames = rate * ms / 1000, bytes = frames * channels * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * 2);
  put16(f, channels * 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < bytes; i++) fputc(0, f);
  fclose(f);
}

static char g_dir[1024];

/* The name of a test file, and the WAV of that length. Good for a handful at once. */
static const char *wav(const char *name, unsigned ms) {
  static char paths[32][1100];
  static int next;
  char *path = paths[next++ % 32];
  snprintf(path, 1100, "%s/up_next_%s.wav", g_dir, name);
  write_wav(path, ms);
  return path;
}

/* The uid of the track with this letter. */
static const char *uid_of(char letter) {
  static char uids[32][MC_UID_STRLEN];
  static int next;
  char *uid = uids[next++ % 32];
  memset(uid, letter, MC_UID_STRLEN - 1);
  uid[MC_UID_STRLEN - 1] = '\0';
  return uid;
}

/* What's playing, by the letter of its uid, or '-' with nothing. */
static char playing(mc_player *player) {
  mc_player_status st;
  mc_player_status_get(player, &st);
  char uid[MC_UID_STRLEN];
  if (st.index < 0 || mc_player_queue_uid(player, st.index, uid) != MC_OK || !uid[0]) return '-';
  return uid[0];
}

/* Polls until the track with letter is the one playing, or ms pass. */
static int wait_for(mc_player *player, char letter, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  for (;;) {
    mc_player_status st;
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_PLAYING && playing(player) == letter) return 1;
    if (mc_mono_ms() >= end) return 0;
    mc_sleep_ms(5);
  }
}

static int wait_stopped(mc_player *player, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  for (;;) {
    mc_player_status st;
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_STOPPED) return 1;
    if (mc_mono_ms() >= end) return 0;
    mc_sleep_ms(5);
  }
}

/* The titles of what's up next, joined with commas, into a static buffer. */
static const char *titles(mc_player *player) {
  static char out[256];
  out[0] = '\0';
  mc_up_next_list *list = NULL;
  if (mc_player_up_next(player, &list) != MC_OK) return "?";
  for (int i = 0; i < list->count; i++) {
    if (i) strcat(out, ",");
    strcat(out, list->items[i].title);
  }
  mc_up_next_list_free(list);
  return out;
}

/* Entry at place i's id, or 0. */
static uint32_t id_at(mc_player *player, int i) {
  mc_up_next_list *list = NULL;
  if (mc_player_up_next(player, &list) != MC_OK) return 0;
  uint32_t id = i < list->count ? list->items[i].id : 0;
  mc_up_next_list_free(list);
  return id;
}

/* Adds tracks with uids of letters; the title makes it a block. */
static mc_status add(mc_player *player, int next, const char *title, const char *letters, const char *const *paths) {
  mc_up_next_track tracks[8];
  char names[8][2];
  int n = (int)strlen(letters);
  for (int i = 0; i < n; i++) {
    names[i][0] = letters[i];
    names[i][1] = '\0';
    tracks[i] = (mc_up_next_track){paths[i], uid_of(letters[i]), names[i], "Artist", 1000};
  }
  mc_up_next_add a = {tracks, n, title, title ? "Artist" : NULL, MC_PLAY_CONTEXT_ALBUM, uid_of('z')};
  return next ? mc_player_play_next(player, &a) : mc_player_play_last(player, &a);
}

static void test_list(const char *const *paths) {
  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);

  mc_player_status st;
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 0);
  unsigned changes = st.up_next_changes;
  CHECK(!strcmp(titles(player), ""));

  /* A block is one entry however many tracks it has, with their length. */
  CHECK(add(player, 0, "Album", "abc", paths) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 1 && st.count == 3 && st.up_next_changes != changes);
  mc_up_next_list *list = NULL;
  CHECK(mc_player_up_next(player, &list) == MC_OK && list->count == 1);
  CHECK(!strcmp(list->items[0].title, "Album") && !strcmp(list->items[0].artist, "Artist"));
  CHECK(list->items[0].block && list->items[0].tracks == 3 && list->items[0].duration_ms == 3000);
  CHECK(list->items[0].context == MC_PLAY_CONTEXT_ALBUM && !strcmp(list->items[0].context_uid, uid_of('z')));
  char uid[MC_UID_STRLEN];
  CHECK(mc_player_queue_uid(player, list->items[0].first_item, uid) == MC_OK && uid[0] == 'a');
  mc_up_next_list_free(list);

  /* Play Last goes after it, and Play Next before everything. */
  CHECK(add(player, 0, NULL, "d", paths) == MC_OK);
  CHECK(add(player, 1, NULL, "e", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "e,Album,d"));

  /* With no title, each track is an entry of its own: a setting's album. Play Next keeps them in
   * order, in front of the rest. */
  CHECK(add(player, 1, NULL, "fg", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "f,g,e,Album,d"));
  CHECK(mc_player_up_next(player, &list) == MC_OK);
  CHECK(!list->items[0].block && list->items[0].tracks == 1 && list->items[3].block);
  uint32_t album = list->items[3].id;
  mc_up_next_list_free(list);

  /* It moves as one, and out-of-range places are clamped. */
  CHECK(mc_player_up_next_move(player, album, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "Album,f,g,e,d"));
  CHECK(mc_player_up_next_move(player, album, 99) == MC_OK);
  CHECK(!strcmp(titles(player), "f,g,e,d,Album"));
  CHECK(mc_player_up_next_move(player, album, 1) == MC_OK);
  CHECK(!strcmp(titles(player), "f,Album,g,e,d"));

  /* Renamed, and then removed, all of it. */
  CHECK(mc_player_up_next_rename(player, album, "Renamed", NULL) == MC_OK);
  CHECK(!strcmp(titles(player), "f,Renamed,g,e,d"));
  CHECK(mc_player_up_next_remove(player, album) == MC_OK);
  CHECK(!strcmp(titles(player), "f,g,e,d"));
  CHECK(mc_player_up_next_remove(player, album) == MC_ERR_NOT_FOUND);
  CHECK(mc_player_up_next_move(player, album, 0) == MC_ERR_NOT_FOUND);
  CHECK(mc_player_up_next_rename(player, album, "x", NULL) == MC_ERR_NOT_FOUND);

  /* The order after the current item has up next in it. */
  const char *one[] = {paths[0]};
  CHECK(mc_player_load_queue(player, one, 1, 0, NULL) == MC_OK);
  int order[6];
  CHECK(mc_player_order(player, 0, 6, order) == MC_OK);
  CHECK(order[0] == 0 && order[5] == -1);
  for (int i = 1; i <= 4; i++) {
    CHECK(order[i] > 0 && mc_player_queue_uid(player, order[i], uid) == MC_OK && uid[0] == "fged"[i - 1]);
  }

  /* Replacing the list keeps up next. */
  CHECK(!strcmp(titles(player), "f,g,e,d"));
  mc_player_status_get(player, &st);
  CHECK(st.count == 5);

  CHECK(mc_player_up_next_clear(player) == MC_OK);
  CHECK(!strcmp(titles(player), ""));

  /* Nothing to add is an error, and so is a track with no file. */
  mc_up_next_add none = {NULL, 0, "Empty", NULL, MC_PLAY_CONTEXT_UNKNOWN, NULL};
  CHECK(mc_player_play_next(player, &none) == MC_ERR_INVALID_ARG);
  mc_up_next_track no_path = {NULL, NULL, "x", NULL, 0};
  mc_up_next_add bad = {&no_path, 1, NULL, NULL, MC_PLAY_CONTEXT_UNKNOWN, NULL};
  CHECK(mc_player_play_last(player, &bad) == MC_ERR_INVALID_ARG);
  mc_player_free(player);
}

static void test_setting(void) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/up_next_settings", g_dir);
  mc_remove_tree(dir);
  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(!mc_settings_queue_tracks(s));
  CHECK(mc_settings_set_queue_tracks(s, 1) == MC_OK && mc_settings_save(s) == MC_OK);
  mc_settings_free(s);
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  CHECK(mc_settings_queue_tracks(s));
  mc_settings_free(s);
  mc_remove_tree(dir);
}

/* The list of a, b and c, playing from a, each track with its letter as its uid. */
static void play_list(mc_player *player, const char *const *paths) {
  const char *uids[] = {uid_of('a'), uid_of('b'), uid_of('c')};
  mc_play_origin origin = {uids, MC_PLAY_CONTEXT_PLAYLIST, uid_of('y')};
  CHECK(mc_player_set_queue(player, paths, 3, 0, &origin) == MC_OK);
}

/* The next row of the play log, skipping those of other tracks. */
static int logged(mc_player *player, char letter, mc_play_event *out) {
  uint64_t end = mc_mono_ms() + 3000;
  while (mc_mono_ms() < end) {
    if (mc_player_take_ended(player, out) != MC_OK) {
      mc_sleep_ms(5);
      continue;
    }
    if (out->track_uid[0] == letter) return 1;
  }
  return 0;
}

static void test_playing(mc_player *player) {
  /* The list has short tracks, and up next's are named by letters from q on. */
  const char *list[] = {wav("a", 600), wav("b", 600), wav("c", 600)};
  const char *block[] = {wav("q", 300), wav("r", 300), wav("s", 300)};
  const char *more[] = {wav("t", 300)};
  mc_player_status st;

  /* Up next plays straight after the current item, a block in order, and the list then carries
   * on from where it was. Each track leaves as it starts, and the block goes with its last. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, 0, "Block", "qrs", block) == MC_OK);
  CHECK(add(player, 0, NULL, "t", more) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  mc_up_next_list *left = NULL;
  CHECK(mc_player_up_next(player, &left) == MC_OK && left->count == 2);
  CHECK(!strcmp(left->items[0].title, "Block") && left->items[0].tracks == 2 && left->items[0].duration_ms == 2000);
  mc_up_next_list_free(left);
  CHECK(wait_for(player, 'r', 3000));
  CHECK(wait_for(player, 's', 3000));
  CHECK(!strcmp(titles(player), "t"));
  CHECK(wait_for(player, 't', 3000));
  CHECK(!strcmp(titles(player), ""));
  CHECK(wait_for(player, 'b', 3000));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(wait_stopped(player, 3000));

  /* Something new playing leaves up next as it is, and up next follows the track that was picked. */
  CHECK(add(player, 0, NULL, "t", more) == MC_OK);
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(!strcmp(titles(player), "t"));
  CHECK(mc_player_up_next_clear(player) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PLAYING); /* Clear doesn't stop it */
  CHECK(mc_player_stop(player) == MC_OK);

  /* Next goes through up next, then on to the list. Previous goes back within a block, the tracks
   * returning to it; from its first track, to the item it followed. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, 0, "Block", "qrs", block) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'r', 3000));
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_up_next(player, &left) == MC_OK && left->count == 1 && left->items[0].tracks == 2);
  mc_up_next_list_free(left);
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(mc_player_up_next(player, &left) == MC_OK && left->count == 1 && left->items[0].tracks == 3);
  mc_up_next_list_free(left);

  /* From the last track, gone from the list, the block comes back. */
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'r', 3000));
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 's', 3000));
  CHECK(!strcmp(titles(player), ""));
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'r', 3000));
  CHECK(!strcmp(titles(player), "Block"));
  CHECK(mc_player_up_next_clear(player) == MC_OK);

  /* Removing what's left of the playing block leaves the track playing, and the list goes on. */
  CHECK(add(player, 1, "Other", "qrs", block) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_up_next_remove(player, id_at(player, 0)) == MC_OK);
  CHECK(playing(player) == 'q');
  CHECK(wait_for(player, 'b', 3000));
  CHECK(mc_player_stop(player) == MC_OK);

  /* Shuffled, a block still plays in order, and the list's order isn't touched by it. */
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  play_list(player, list);
  CHECK(add(player, 1, "Block", "qrs", block) == MC_OK);
  CHECK(wait_for(player, 'q', 4000));
  CHECK(wait_for(player, 'r', 3000));
  CHECK(wait_for(player, 's', 3000));
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(mc_player_stop(player) == MC_OK);

  /* A list played out: up next added then plays on resume, and nothing starts the list again. */
  CHECK(mc_player_set_queue(player, list, 1, 0, NULL) == MC_OK);
  CHECK(wait_stopped(player, 3000));
  CHECK(add(player, 0, NULL, "t", more) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_STOPPED);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(wait_for(player, 't', 3000));
  CHECK(wait_stopped(player, 3000));
  CHECK(!strcmp(titles(player), ""));

  /* A file that won't open leaves up next, and the next one plays. */
  char gone[1100];
  snprintf(gone, sizeof gone, "%s/up_next_gone.wav", g_dir);
  const char *broken[] = {gone, more[0]};
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, 0, "Broken", "qt", broken) == MC_OK);
  CHECK(wait_for(player, 't', 3000));
  CHECK(!strcmp(titles(player), ""));

  /* The log has a queued track's own context, and the list's has the list's. */
  mc_play_event e;
  while (mc_player_take_ended(player, &e) == MC_OK) {}
  CHECK(wait_for(player, 'b', 3000));
  CHECK(logged(player, 't', &e));
  CHECK(e.context == MC_PLAY_CONTEXT_ALBUM && !strcmp(e.context_uid, uid_of('z')));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(logged(player, 'b', &e));
  CHECK(e.context == MC_PLAY_CONTEXT_PLAYLIST && !strcmp(e.context_uid, uid_of('y')));
  CHECK(mc_player_stop(player) == MC_OK);
}

int main(int argc, char **argv) {
  snprintf(g_dir, sizeof g_dir, "%s", argc > 1 ? argv[1] : ".");
  const char *paths[] = {wav("1", 100), wav("2", 100), wav("3", 100)};
  test_list(paths);
  test_setting();

  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);
  /* Is there a device? The position only moves once it pulls audio. */
  const char *probe[] = {wav("probe", 2000)};
  CHECK(mc_player_set_queue(player, probe, 1, 0, NULL) == MC_OK);
  int has_device = 0;
  mc_player_status st;
  for (uint64_t end = mc_mono_ms() + 3000; !has_device && mc_mono_ms() < end; mc_sleep_ms(10)) {
    mc_player_status_get(player, &st);
    has_device = st.state == MC_PLAYER_PLAYING && st.position_ms > 0;
  }
  if (!has_device) {
    fprintf(stderr, "no audio device; skipping what plays\n");
    mc_player_free(player);
    return SKIP;
  }
  test_playing(player);
  mc_player_free(player);
  return TEST_RESULT();
}
