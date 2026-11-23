/* Up next, the queue people add to. The list's own workings need no audio device: blocks and
 * tracks, where Queue puts them (albums queued while an album plays in order wait for it), moving,
 * removing, clearing, what a new list does to it, and the setting. The rest plays silent WAVs on
 * the real device, and is skipped (exit 77) without one: up next plays before the list carries on,
 * blocks in their own order with shuffle on, what's left of a block, Next and Previous, a file that
 * won't open, what the play log says, what clears it while playing and what leaves it alone, and
 * albums queued after an album as they play. */
#include <stdatomic.h>
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

/* The titles of what's up next, joined with commas, into a static buffer: the rest of the album's
 * row is R and how many tracks it has. */
static const char *titles(mc_player *player) {
  static char out[256];
  out[0] = '\0';
  mc_up_next_list *list = NULL;
  if (mc_player_up_next(player, &list) != MC_OK) return "?";
  for (int i = 0; i < list->count; i++) {
    if (i) strcat(out, ",");
    if (list->items[i].rest) snprintf(out + strlen(out), sizeof out - strlen(out), "R%d", list->items[i].tracks);
    else strcat(out, list->items[i].title);
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

/* Queues tracks with uids of letters, from context (a block's or the tracks' own); the title makes
 * it a block. */
static mc_status add_from(mc_player *player, mc_play_context context, const char *title, const char *letters,
                          const char *const *paths) {
  mc_up_next_track tracks[8];
  char names[8][2];
  int n = (int)strlen(letters);
  for (int i = 0; i < n; i++) {
    names[i][0] = letters[i];
    names[i][1] = '\0';
    tracks[i] = (mc_up_next_track){paths[i], uid_of(letters[i]), names[i], "Artist", 1000};
  }
  mc_up_next_add a = {tracks, n, title, title ? "Artist" : NULL, context, uid_of('z')};
  return mc_player_up_next_add(player, &a);
}

/* An album's tracks, as add_from() does. */
static mc_status add(mc_player *player, const char *title, const char *letters, const char *const *paths) {
  return add_from(player, MC_PLAY_CONTEXT_ALBUM, title, letters, paths);
}

/* A track by itself. */
static mc_status add_track(mc_player *player, const char *letter, const char *const *paths) {
  return add_from(player, MC_PLAY_CONTEXT_MANUAL, NULL, letter, paths);
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
  CHECK(add(player, "Album", "abc", paths) == MC_OK);
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

  /* What's queued goes after what's there, with nothing playing whatever it is. */
  CHECK(add_track(player, "d", paths) == MC_OK);
  CHECK(add(player, NULL, "e", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "Album,d,e"));
  CHECK(mc_player_up_next(player, &list) == MC_OK);
  CHECK(!list->items[0].after_list && !list->items[2].after_list);
  mc_up_next_list_free(list);

  /* With no title, each track is an entry of its own: a setting's album, in order. */
  CHECK(add(player, NULL, "fg", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "Album,d,e,f,g"));
  CHECK(mc_player_up_next(player, &list) == MC_OK);
  CHECK(!list->items[3].block && list->items[3].tracks == 1 && list->items[0].block);
  uint32_t album = list->items[0].id;
  mc_up_next_list_free(list);

  /* It moves as one, and out-of-range places are clamped. */
  CHECK(mc_player_up_next_move(player, album, 99) == MC_OK);
  CHECK(!strcmp(titles(player), "d,e,f,g,Album"));
  CHECK(mc_player_up_next_move(player, album, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "Album,d,e,f,g"));
  CHECK(mc_player_up_next_move(player, album, 1) == MC_OK);
  CHECK(!strcmp(titles(player), "d,Album,e,f,g"));

  /* Renamed, and then removed, all of it. */
  CHECK(mc_player_up_next_rename(player, album, "Renamed", NULL) == MC_OK);
  CHECK(!strcmp(titles(player), "d,Renamed,e,f,g"));
  CHECK(mc_player_up_next_remove(player, album) == MC_OK);
  CHECK(!strcmp(titles(player), "d,e,f,g"));
  CHECK(mc_player_up_next_remove(player, album) == MC_ERR_NOT_FOUND);
  CHECK(mc_player_up_next_move(player, album, 0) == MC_ERR_NOT_FOUND);
  CHECK(mc_player_up_next_rename(player, album, "x", NULL) == MC_ERR_NOT_FOUND);

  /* The order after the current item has up next in it. */
  const char *one[] = {paths[0]};
  CHECK(mc_player_restore_queue(player, one, 1, 0, NULL) == MC_OK);
  int order[6];
  CHECK(mc_player_order(player, 0, 6, order) == MC_OK);
  CHECK(order[0] == 0 && order[5] == -1);
  for (int i = 1; i <= 4; i++) {
    CHECK(order[i] > 0 && mc_player_queue_uid(player, order[i], uid) == MC_OK && uid[0] == "defg"[i - 1]);
  }

  /* Bringing a list back keeps up next. */
  CHECK(!strcmp(titles(player), "d,e,f,g"));
  mc_player_status_get(player, &st);
  CHECK(st.count == 5);

  CHECK(mc_player_up_next_clear(player) == MC_OK);
  CHECK(!strcmp(titles(player), ""));

  /* Nothing to add is an error, and so is a track with no file. */
  mc_up_next_add none = {NULL, 0, "Empty", NULL, MC_PLAY_CONTEXT_UNKNOWN, NULL};
  CHECK(mc_player_up_next_add(player, &none) == MC_ERR_INVALID_ARG);
  mc_up_next_track no_path = {NULL, NULL, "x", NULL, 0};
  mc_up_next_add bad = {&no_path, 1, NULL, NULL, MC_PLAY_CONTEXT_UNKNOWN, NULL};
  CHECK(mc_player_up_next_add(player, &bad) == MC_ERR_INVALID_ARG);
  mc_player_free(player);
}

static atomic_int g_up_next_events;

static void on_event(void *user, mc_player_event event) {
  (void)user;
  if (event == MC_PLAYER_EVENT_UP_NEXT) atomic_fetch_add(&g_up_next_events, 1);
}

/* Polls until up next's event has come, or ms pass. */
static int wait_event(unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  while (!atomic_load(&g_up_next_events)) {
    if (mc_mono_ms() >= end) return 0;
    mc_sleep_ms(5);
  }
  return 1;
}

/* What a new list does to up next, which the call settles there and then. */
static void test_new_list(const char *const *paths) {
  mc_player *player = NULL;
  CHECK(mc_player_new(on_event, NULL, &player) == MC_OK);
  const char *two[] = {paths[0], paths[1]};
  const char *uids[] = {uid_of('a'), uid_of('b')};
  mc_play_origin origin = {uids, MC_PLAY_CONTEXT_PLAYLIST, uid_of('y')};
  mc_player_status st;
  char uid[MC_UID_STRLEN];

  /* With nothing to clear, up next hasn't changed. */
  mc_player_status_get(player, &st);
  unsigned changes = st.up_next_changes;
  CHECK(mc_player_load_queue(player, two, 2, 0, &origin) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next_changes == changes);

  /* Loading is someone starting something on Android, so it clears, and an open list hears of it.
   * Only the new list's items are left, with what follows the current one the list's. */
  CHECK(add(player, "Album", "cde", paths) == MC_OK);
  CHECK(add(player, NULL, "f", paths) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 2 && st.count == 6);
  changes = st.up_next_changes;
  mc_sleep_ms(50); /* the adds' own events */
  atomic_store(&g_up_next_events, 0);
  CHECK(mc_player_load_queue(player, two, 2, 0, &origin) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 0 && st.count == 2 && st.index == 0 && st.up_next_changes != changes);
  CHECK(!strcmp(titles(player), ""));
  CHECK(wait_event(3000));
  CHECK(mc_player_queue_uid(player, 1, uid) == MC_OK && uid[0] == 'b');
  CHECK(mc_player_queue_uid(player, 2, uid) == MC_ERR_NOT_FOUND);
  int order[3];
  CHECK(mc_player_order(player, 0, 3, order) == MC_OK);
  CHECK(order[0] == 0 && order[1] == 1 && order[2] == -1);

  /* A call that fails has lost nothing, nor has a list of nothing, which starts nothing. */
  CHECK(add(player, NULL, "c", paths) == MC_OK);
  const char *no_file[] = {paths[0], NULL};
  CHECK(mc_player_set_queue(player, two, 2, 2, &origin) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_load_queue(player, two, 2, -2, &origin) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_set_queue(player, no_file, 2, 0, &origin) == MC_ERR_INVALID_ARG);
  CHECK(!strcmp(titles(player), "c"));
  CHECK(mc_player_set_queue(player, NULL, 0, 0, NULL) == MC_OK);
  CHECK(!strcmp(titles(player), "c"));
  CHECK(mc_player_load_queue(player, NULL, 0, 0, NULL) == MC_OK);
  CHECK(!strcmp(titles(player), "c"));
  mc_player_status_get(player, &st);
  CHECK(st.index == -1 && st.count == 1);

  /* A list brought back keeps it, after the list, whichever item is current. */
  CHECK(mc_player_restore_queue(player, two, 2, 1, &origin) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 1 && st.count == 3 && st.index == 1 && st.state == MC_PLAYER_STOPPED);
  CHECK(mc_player_order(player, 0, 3, order) == MC_OK);
  CHECK(order[0] == 1 && order[1] == 2 && order[2] == -1);
  CHECK(mc_player_queue_uid(player, 2, uid) == MC_OK && uid[0] == 'c');

  /* Its own items aren't the list's to skip to, and trying leaves them. */
  CHECK(mc_player_skip_to(player, 2) == MC_ERR_INVALID_ARG);
  CHECK(!strcmp(titles(player), "c"));
  mc_player_free(player);
}

/* Whether up next's entries (not the rest of the album) wait for the list, as 0s and 1s, into a
 * static buffer. */
static const char *waiting(mc_player *player) {
  static char out[64];
  int n = 0;
  mc_up_next_list *list = NULL;
  if (mc_player_up_next(player, &list) != MC_OK) return "?";
  for (int i = 0; i < list->count && n < 63; i++)
    if (!list->items[i].rest) out[n++] = list->items[i].after_list ? '1' : '0';
  out[n] = '\0';
  mc_up_next_list_free(list);
  return out;
}

/* The play order from the current item on, by letters ('-' for none, '?' for one with no uid),
 * into a static buffer. */
static const char *order_of(mc_player *player, int count) {
  static char out[32];
  int order[31];
  if (count > 31 || mc_player_order(player, 0, count, order) != MC_OK) return "?";
  for (int i = 0; i < count; i++) {
    char uid[MC_UID_STRLEN];
    out[i] = order[i] < 0 ? '-' : mc_player_queue_uid(player, order[i], uid) == MC_OK && uid[0] ? uid[0] : '?';
  }
  out[count] = '\0';
  return out;
}

/* Loads a, b and c as from origin's context, stopped at item start. */
static void load_abc(mc_player *player, const char *const *paths, mc_play_context context, int start) {
  const char *uids[] = {uid_of('a'), uid_of('b'), uid_of('c')};
  mc_play_origin origin = {uids, context, uid_of('y')};
  CHECK(mc_player_load_queue(player, paths, 3, start, &origin) == MC_OK);
}

/* Queuing an album while an album plays in order: it waits for the rest of that album, behind any
 * queued the same way, while tracks still go ahead of the album's next track. */
static void test_after_album(const char *const *paths) {
  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);

  /* Albums stack up after the list in the order queued, an album group with them; tracks and a
   * playlist go before the list's next item, after what's ahead of it already. */
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, 0);
  CHECK(add_track(player, "t", paths) == MC_OK);
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(add(player, "C", "s", paths) == MC_OK);
  CHECK(add_track(player, "u", paths) == MC_OK);
  CHECK(add_from(player, MC_PLAY_CONTEXT_ALBUM_GROUP, "G", "g", paths) == MC_OK);
  CHECK(add_from(player, MC_PLAY_CONTEXT_PLAYLIST, "P", "p", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "t,u,P,R2,B,C,G"));
  CHECK(!strcmp(waiting(player), "000111"));
  CHECK(!strcmp(order_of(player, 11), "atupbcqrsg-"));

  /* The rest of the album's row is the album's next track on, and the list's. */
  mc_up_next_list *list = NULL;
  char uid[MC_UID_STRLEN];
  CHECK(mc_player_up_next(player, &list) == MC_OK && list->count == 7);
  const mc_up_next_entry *r = &list->items[3];
  CHECK(r->rest && r->id == MC_UP_NEXT_REST && r->tracks == 2 && !r->after_list && r->block);
  CHECK(r->context == MC_PLAY_CONTEXT_ALBUM && !strcmp(r->context_uid, uid_of('y')) && !strcmp(r->title, ""));
  CHECK(mc_player_queue_uid(player, r->first_item, uid) == MC_OK && uid[0] == 'b');
  CHECK(!list->items[2].rest && !list->items[4].rest);
  mc_up_next_list_free(list);

  /* With the setting's tracks, an album is still an album. */
  CHECK(add(player, NULL, "vw", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "t,u,P,R2,B,C,G,v,w"));
  CHECK(!strcmp(waiting(player), "00011111"));

  /* Moved, an entry plays where it's put, the places being rows as shown: below the rest of the
   * album it waits for it, and above it it doesn't, just below or above included. */
  uint32_t t = id_at(player, 0), c = id_at(player, 5);
  CHECK(mc_player_up_next_move(player, t, 4) == MC_OK);
  CHECK(!strcmp(titles(player), "u,P,R2,B,t,C,G,v,w"));
  CHECK(!strcmp(waiting(player), "00111111"));
  CHECK(mc_player_up_next_move(player, c, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "C,u,P,R2,B,t,G,v,w"));
  CHECK(!strcmp(waiting(player), "00011111"));
  CHECK(mc_player_up_next_move(player, c, 3) == MC_OK);
  CHECK(!strcmp(titles(player), "u,P,R2,C,B,t,G,v,w"));
  CHECK(!strcmp(waiting(player), "00111111"));
  CHECK(mc_player_up_next_move(player, c, 2) == MC_OK);
  CHECK(!strcmp(titles(player), "u,P,C,R2,B,t,G,v,w"));
  CHECK(!strcmp(waiting(player), "00011111"));
  CHECK(!strcmp(order_of(player, 8), "aupsbcqr"));

  /* The rest of the album moves too: what's above it goes first, and what's below waits. Out of
   * range is clamped. It can't be removed. */
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "R2,u,P,C,B,t,G,v,w"));
  CHECK(!strcmp(waiting(player), "11111111"));
  CHECK(!strcmp(order_of(player, 6), "abcups"));
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 99) == MC_OK);
  CHECK(!strcmp(titles(player), "u,P,C,B,t,G,v,w,R2"));
  CHECK(!strcmp(waiting(player), "00000000"));
  CHECK(!strcmp(order_of(player, 14), "aupsqrtgvwbc--"));
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 3) == MC_OK);
  CHECK(!strcmp(titles(player), "u,P,C,R2,B,t,G,v,w"));
  CHECK(mc_player_up_next_remove(player, MC_UP_NEXT_REST) == MC_ERR_NOT_FOUND);
  CHECK(!strcmp(titles(player), "u,P,C,R2,B,t,G,v,w"));

  /* Shuffle on: a shuffled list never ends, so nothing waits any more, nor after it's off again,
   * when the rest of the album comes after everything. Shuffled, there's no row for it. */
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  CHECK(!strcmp(waiting(player), "00000000"));
  CHECK(!strcmp(titles(player), "u,P,C,B,t,G,v,w"));
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 0) == MC_ERR_NOT_FOUND);
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(!strcmp(waiting(player), "00000000"));
  CHECK(!strcmp(titles(player), "u,P,C,B,t,G,v,w,R2"));

  /* Nothing queued, no row. */
  CHECK(mc_player_up_next_clear(player) == MC_OK);
  CHECK(!strcmp(titles(player), ""));

  /* Shuffled, an album queued doesn't wait; started shuffled but in order now, it does, unless the
   * shuffle started on the last track, with nothing left to wait for. */
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, -1);
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(!strcmp(waiting(player), "0"));
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(add(player, "C", "s", paths) == MC_OK);
  mc_player_status st;
  mc_player_status_get(player, &st);
  CHECK(!strcmp(waiting(player), st.index < 2 ? "01" : "00"));

  /* Playing anything but an album, an album queued is like a track. */
  static const mc_play_context others[] = {MC_PLAY_CONTEXT_PLAYLIST, MC_PLAY_CONTEXT_ARTIST,
                                           MC_PLAY_CONTEXT_ALBUM_GROUP, MC_PLAY_CONTEXT_MANUAL,
                                           MC_PLAY_CONTEXT_UNKNOWN};
  for (size_t i = 0; i < sizeof others / sizeof *others; i++) {
    load_abc(player, paths, others[i], 0);
    CHECK(!strcmp(titles(player), ""));
    CHECK(add(player, "B", "qr", paths) == MC_OK);
    CHECK(add_track(player, "t", paths) == MC_OK);
    CHECK(!strcmp(titles(player), "B,t"));
    CHECK(!strcmp(waiting(player), "00"));
    CHECK(!strcmp(order_of(player, 6), "aqrtbc"));
  }

  /* On the album's last track there's nothing left of it to wait for, and no row for it: Queue is
   * simply the end, a track queued after an album going after it. */
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, 2);
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(add_track(player, "t", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "B,t"));
  CHECK(!strcmp(waiting(player), "00"));
  CHECK(!strcmp(order_of(player, 5), "cqrt-"));

  /* Played out, the same; and the order is what Play plays, up next and nothing of the album after
   * it. With nothing queued, Play starts the album again. */
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, 2);
  CHECK(mc_player_next(player) == MC_OK); /* past the end: stopped at its start */
  CHECK(!strcmp(order_of(player, 4), "abc-"));
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(add_track(player, "t", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "B,t"));
  CHECK(!strcmp(waiting(player), "00"));
  CHECK(!strcmp(order_of(player, 6), "aqrt--"));

  /* Queued after an album that waits, with nothing left to go before, it waits behind it. */
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, 1);
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK); /* to the last track, and stopped there */
  CHECK(mc_player_stop(player) == MC_OK);
  CHECK(add_track(player, "t", paths) == MC_OK);
  CHECK(!strcmp(titles(player), "B,t"));
  CHECK(!strcmp(waiting(player), "11"));
  CHECK(!strcmp(order_of(player, 5), "cqrt-"));

  /* The album that's playing, queued, plays again after it. */
  load_abc(player, paths, MC_PLAY_CONTEXT_ALBUM, 1);
  CHECK(add(player, "Same", "abc", paths) == MC_OK);
  CHECK(!strcmp(order_of(player, 6), "bcabc-"));

  /* Bringing the list back keeps them waiting; with nothing, there's no album to wait for. */
  const char *uids[] = {uid_of('a'), uid_of('b'), uid_of('c')};
  mc_play_origin album = {uids, MC_PLAY_CONTEXT_ALBUM, uid_of('y')};
  CHECK(mc_player_restore_queue(player, paths, 3, 0, &album) == MC_OK);
  CHECK(!strcmp(waiting(player), "1"));
  CHECK(!strcmp(order_of(player, 7), "abcabc-"));
  CHECK(mc_player_load_queue(player, NULL, 0, 0, NULL) == MC_OK);
  CHECK(mc_player_up_next_clear(player) == MC_OK);
  CHECK(add(player, "B", "qr", paths) == MC_OK);
  CHECK(!strcmp(waiting(player), "0"));
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
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
  CHECK(add(player, NULL, "t", more) == MC_OK);
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

  /* Something new clears up next, and the list goes straight on with nothing of it in between. */
  CHECK(add(player, NULL, "t", more) == MC_OK);
  play_list(player, list);
  CHECK(!strcmp(titles(player), ""));
  CHECK(wait_for(player, 'a', 3000));
  CHECK(wait_for(player, 'b', 3000));
  CHECK(add(player, NULL, "t", more) == MC_OK);
  CHECK(mc_player_up_next_clear(player) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PLAYING); /* Clear doesn't stop it */
  CHECK(mc_player_stop(player) == MC_OK);

  /* Next goes through up next, then on to the list. Previous goes back within a block, the tracks
   * returning to it; from its first track, to the item it followed. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
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
  CHECK(add(player, "Other", "qrs", block) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_up_next_remove(player, id_at(player, 0)) == MC_OK);
  CHECK(playing(player) == 'q');
  CHECK(wait_for(player, 'b', 3000));
  CHECK(mc_player_stop(player) == MC_OK);

  /* Shuffled, a block still plays in order, and the list's order isn't touched by it. */
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  play_list(player, list);
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
  CHECK(wait_for(player, 'q', 4000));
  CHECK(wait_for(player, 'r', 3000));
  CHECK(wait_for(player, 's', 3000));
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(mc_player_stop(player) == MC_OK);

  /* A list played out: up next added then plays on resume, and nothing starts the list again. */
  CHECK(mc_player_set_queue(player, list, 1, 0, NULL) == MC_OK);
  CHECK(wait_stopped(player, 3000));
  CHECK(add(player, NULL, "t", more) == MC_OK);
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
  CHECK(add(player, "Broken", "qt", broken) == MC_OK);
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

/* Going through what's playing leaves up next alone. The tracks are long enough to stay put while
 * it's looked at. */
static void test_going_through(mc_player *player) {
  const char *list[] = {wav("long_a", 4000), wav("long_b", 4000), wav("long_c", 4000)};
  const char *block[] = {wav("q", 300), wav("r", 300), wav("s", 300)};
  const char *more[] = {wav("t", 300)};

  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
  CHECK(add(player, NULL, "t", more) == MC_OK);

  /* Jumping within the list, there and back. */
  CHECK(mc_player_skip_to(player, 1) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(!strcmp(titles(player), "Block,t"));
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(!strcmp(titles(player), "Block,t"));

  /* Seeking, pausing, stopping, and shuffle on and off. */
  CHECK(mc_player_seek_ms(player, 100) == MC_OK);
  CHECK(mc_player_pause(player) == MC_OK);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "Block,t"));
  CHECK(mc_player_stop(player) == MC_OK);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(!strcmp(titles(player), "Block,t"));

  /* Next takes one track of it, and no more. */
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  mc_up_next_list *left = NULL;
  CHECK(mc_player_up_next(player, &left) == MC_OK && left->count == 2 && left->items[0].tracks == 2);
  mc_up_next_list_free(left);
  CHECK(mc_player_up_next_clear(player) == MC_OK);
  CHECK(mc_player_stop(player) == MC_OK);
}

/* Albums queued while an album plays, as they play. */
static void test_after_album_playing(mc_player *player) {
  const char *list[] = {wav("a", 600), wav("b", 600), wav("c", 600)};
  const char *block[] = {wav("q", 300), wav("r", 300)};
  const char *more[] = {wav("t", 300)};
  const char *other[] = {wav("u", 300)};
  const char *uids[] = {uid_of('a'), uid_of('b'), uid_of('c')};
  mc_play_origin album = {uids, MC_PLAY_CONTEXT_ALBUM, uid_of('y')};

  /* The track goes first, then the rest of the album, then the album queued before it. That one,
   * once it starts, is like any other entry: a track queued now goes after what's left of it. */
  CHECK(mc_player_set_queue(player, list, 3, 0, &album) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, "Block", "qr", block) == MC_OK);
  CHECK(add_track(player, "t", more) == MC_OK);
  CHECK(!strcmp(titles(player), "t,R2,Block"));
  CHECK(wait_for(player, 't', 3000));
  CHECK(wait_for(player, 'b', 3000));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(waiting(player), "1"));
  CHECK(wait_for(player, 'q', 3000));
  CHECK(!strcmp(waiting(player), "0"));
  CHECK(add_track(player, "u", other) == MC_OK);
  CHECK(!strcmp(titles(player), "Block,u"));
  CHECK(wait_for(player, 'r', 3000));
  CHECK(wait_for(player, 'u', 3000));
  CHECK(wait_stopped(player, 3000));
  CHECK(!strcmp(titles(player), ""));

  /* Next goes through the album first, and on to the queued one after its last track. Previous from
   * that one's first track goes back to the album's last, and it's next again, waiting. */
  CHECK(mc_player_set_queue(player, list, 3, 0, &album) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, "Block", "qr", block) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(titles(player), "Block"));
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(titles(player), "Block"));
  CHECK(!strcmp(waiting(player), "1"));
  CHECK(!strcmp(order_of(player, 4), "cqr-"));

  /* It waits again, so going back further into the album doesn't bring it in before the album's
   * last track. */
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(!strcmp(order_of(player, 5), "bcqr-"));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_stop(player) == MC_OK);

  /* The rest of the album, moved, goes on from where it was when its turn comes: here after the
   * first track queued and before the second, which waits for it. */
  CHECK(mc_player_set_queue(player, list, 3, 0, &album) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add_track(player, "t", more) == MC_OK);
  CHECK(add_track(player, "u", other) == MC_OK);
  CHECK(!strcmp(titles(player), "t,u,R2"));
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 1) == MC_OK);
  CHECK(!strcmp(titles(player), "t,R2,u"));
  CHECK(wait_for(player, 't', 3000));
  CHECK(!strcmp(titles(player), "R2,u"));
  CHECK(!strcmp(order_of(player, 5), "tbcu-"));
  CHECK(wait_for(player, 'b', 3000));
  CHECK(!strcmp(titles(player), "R1,u"));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(titles(player), "u"));
  CHECK(wait_for(player, 'u', 3000));
  CHECK(wait_stopped(player, 3000));

  /* Moved to the top while a track queued plays, nothing cuts in any more: the album goes on. */
  CHECK(mc_player_set_queue(player, list, 3, 0, &album) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add_track(player, "t", more) == MC_OK);
  CHECK(add_track(player, "u", other) == MC_OK);
  CHECK(wait_for(player, 't', 3000));
  CHECK(mc_player_up_next_move(player, MC_UP_NEXT_REST, 0) == MC_OK);
  CHECK(!strcmp(titles(player), "R2,u"));
  CHECK(wait_for(player, 'b', 3000));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(wait_for(player, 'u', 3000));
  CHECK(mc_player_stop(player) == MC_OK);

  /* Something queued while a queued album plays goes after it, and still does once Previous from
   * its first track has it wait again. */
  const char *slow[] = {wav("slow_q", 2000), wav("slow_r", 2000)};
  CHECK(mc_player_set_queue(player, list, 3, 1, &album) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(add(player, "Block", "qr", slow) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(add_track(player, "t", more) == MC_OK);
  CHECK(!strcmp(titles(player), "Block,t"));
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(titles(player), "Block,t"));
  CHECK(!strcmp(waiting(player), "11"));
  CHECK(!strcmp(order_of(player, 5), "cqrt-"));
  CHECK(mc_player_stop(player) == MC_OK);
  CHECK(mc_player_up_next_clear(player) == MC_OK);

  /* Stopped at the album's end, what's queued plays on resume. */
  CHECK(mc_player_set_queue(player, list, 3, 2, &album) == MC_OK);
  CHECK(wait_stopped(player, 3000));
  CHECK(add(player, "Block", "qr", block) == MC_OK);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  CHECK(mc_player_stop(player) == MC_OK);
  CHECK(mc_player_up_next_clear(player) == MC_OK);
}

/* A list of a, b and c from an artist. uid_of() goes round, so this is for using straight away. */
static mc_play_origin from_artist(const char **uids) {
  uids[0] = uid_of('a');
  uids[1] = uid_of('b');
  uids[2] = uid_of('c');
  return (mc_play_origin){uids, MC_PLAY_CONTEXT_ARTIST, uid_of('x')};
}

/* Starting something new, whatever's playing. */
static void test_starting(mc_player *player) {
  const char *list[] = {wav("a", 600), wav("b", 600), wav("c", 600)};
  const char *block[] = {wav("q", 300), wav("r", 300), wav("s", 300)};
  const char *more[] = {wav("t", 300)};
  const char *uids[3];
  mc_play_origin artist;
  mc_player_status st;
  mc_play_event e;
  char uid[MC_UID_STRLEN];
  while (mc_player_take_ended(player, &e) == MC_OK) {}

  /* From a track of up next. That play's row says what it was, the new list's say the list, and
   * the items are the list's alone. Previous has nothing of the block to go back into. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
  CHECK(mc_player_next(player) == MC_OK);
  CHECK(wait_for(player, 'q', 3000));
  mc_sleep_ms(50); /* a command it replaced would leave no row */
  artist = from_artist(uids);
  CHECK(mc_player_set_queue(player, list, 3, 1, &artist) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(!strcmp(titles(player), ""));
  mc_player_status_get(player, &st);
  CHECK(st.count == 3 && st.index == 1);
  CHECK(mc_player_queue_uid(player, 3, uid) == MC_ERR_NOT_FOUND);
  CHECK(logged(player, 'q', &e));
  CHECK(e.context == MC_PLAY_CONTEXT_ALBUM && !strcmp(e.context_uid, uid_of('z')) && e.end == MC_PLAY_END_REPLACED);
  CHECK(mc_player_previous(player) == MC_OK);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(logged(player, 'b', &e));
  CHECK(e.context == MC_PLAY_CONTEXT_ARTIST && !strcmp(e.context_uid, uid_of('x')));
  CHECK(mc_player_stop(player) == MC_OK);

  /* What was already buffered of up next goes too, unheard. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, NULL, "t", more) == MC_OK);
  mc_sleep_ms(200);
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(wait_for(player, 'b', 3000));
  while (mc_player_take_ended(player, &e) == MC_OK) CHECK(e.track_uid[0] != 't');
  CHECK(mc_player_stop(player) == MC_OK);

  /* A track of the list that's playing, pressed, is new as well. */
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(add(player, NULL, "t", more) == MC_OK);
  artist = from_artist(uids);
  CHECK(mc_player_set_queue(player, list, 3, 2, &artist) == MC_OK);
  CHECK(wait_for(player, 'c', 3000));
  CHECK(!strcmp(titles(player), ""));
  CHECK(mc_player_stop(player) == MC_OK);

  /* With a list played out, and something waiting for Play. */
  CHECK(mc_player_set_queue(player, list, 1, 0, NULL) == MC_OK);
  CHECK(wait_stopped(player, 3000));
  CHECK(add(player, NULL, "t", more) == MC_OK);
  play_list(player, list);
  CHECK(wait_for(player, 'a', 3000));
  CHECK(!strcmp(titles(player), ""));
  CHECK(mc_player_stop(player) == MC_OK);

  /* Shuffled, from any track: the order is the new list's, each item once a round. */
  CHECK(add(player, "Block", "qrs", block) == MC_OK);
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  artist = from_artist(uids);
  CHECK(mc_player_set_queue(player, list, 3, -1, &artist) == MC_OK);
  CHECK(!strcmp(titles(player), ""));
  mc_player_status_get(player, &st);
  CHECK(st.count == 3);
  int order[3], seen = 0;
  CHECK(mc_player_order(player, 0, 3, order) == MC_OK);
  for (int i = 0; i < 3; i++) {
    CHECK(order[i] >= 0 && order[i] < 3);
    seen |= 1 << order[i];
  }
  CHECK(seen == 7);
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  CHECK(mc_player_stop(player) == MC_OK);

  /* A list brought back, though, plays up next after the item it stopped on, and the list after
   * that, with up next's row still its own. */
  CHECK(add(player, NULL, "t", more) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.up_next == 1);
  artist = from_artist(uids);
  CHECK(mc_player_restore_queue(player, list, 3, 1, &artist) == MC_OK);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(wait_for(player, 'b', 3000));
  CHECK(wait_for(player, 't', 3000));
  CHECK(wait_for(player, 'c', 3000));
  CHECK(logged(player, 't', &e));
  CHECK(e.context == MC_PLAY_CONTEXT_ALBUM && !strcmp(e.context_uid, uid_of('z')));
  CHECK(mc_player_stop(player) == MC_OK);
}

int main(int argc, char **argv) {
  snprintf(g_dir, sizeof g_dir, "%s", argc > 1 ? argv[1] : ".");
  const char *paths[] = {wav("1", 100), wav("2", 100), wav("3", 100)};
  test_list(paths);
  test_new_list(paths);
  test_after_album(paths);
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
  test_going_through(player);
  test_after_album_playing(player);
  test_starting(player);
  mc_player_free(player);
  return TEST_RESULT();
}
