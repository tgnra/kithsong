#include "playback.h"

#include <shobjidl.h> /* THBN_CLICKED */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "media_controls.h"
#include "musiccore/musiccore.h"
#include "musiccore/radio.h"
#include "musiccore/smartlist.h"
#include "now_playing.h"
#include "taskbar.h"
#include "track_list.h"
#include "wstr.h"

/* To playback's own message-only window, so they can't clash with the main window's. */
enum {
  WM_PLAYER = WM_APP + 1, /* the player has news */
  WM_MEDIA_BUTTON,        /* from the media controls: a media_button, and lParam */
};

enum { TICK_MS = 250, TICK_TIMER = 1 }; /* moves the seekbar on while playing */

typedef struct item {
  char uid[MC_UID_STRLEN];
  wchar_t *title, *artist, *album, *cover_dir;
  int duration_ms;
} item;

static struct {
  HWND wnd;  /* the main window */
  HWND msgs; /* ours */
  void (*changed)(void);
  void (*recents_changed)(void);
  BOOL station_logged; /* for the end of the next refresh() */
  mc_player *player;
  BOOL media_controls; /* set up on first play, as it loads WinRT */
  mc_library *lib;     /* where plays are counted, or NULL */
  mc_scrobbler *scrobbler;
  BOOL scrobble_off;              /* this device doesn't scrobble (the account is the group's) */
  void (*scrobbler_closing)(void);
  void (*shuffle_requested)(BOOL on);
  int announced;       /* the item last sent as now playing, or -1 */
  item *items;
  int count;
  BOOL shuffle;
  BOOL radio; /* a station is adding to the queue */
  BOOL has_next;
  BOOL queue_tracks; /* mc_settings_queue_tracks() */
  void (*up_next_changed)(void);
  unsigned up_next_changes; /* as the player had them last */
  int volume;
  BOOL muted;

  /* What's shown, to tell what changed. */
  int index;
  mc_player_state state;
  BOOL shown_shuffle;
} s;

static wchar_t *copy_text(const wchar_t *w) {
  return w ? _wcsdup(w) : NULL;
}

static void free_items(void) {
  for (int i = 0; i < s.count; i++) {
    free(s.items[i].title);
    free(s.items[i].artist);
    free(s.items[i].album);
    free(s.items[i].cover_dir);
  }
  free(s.items);
  s.items = NULL;
  s.count = 0;
}

/* "Paranoid Android – Radiohead" while there's a track, so the taskbar and Alt+Tab show it. */
static void set_window_title(const item *it) {
  wchar_t title[512];
  if (!it) {
    swprintf(title, 512, L"%ls", mc_wstr(MC_STR_APP_NAME));
  } else if (it->artist) {
    char *name = wstr_to_utf8(it->title), *artist = wstr_to_utf8(it->artist);
    mc_wformat(title, 512, mc_str(MC_STR_TRACK_BYLINE), MC_S(name), MC_S(artist));
    free(name);
    free(artist);
  } else {
    swprintf(title, 512, L"%ls", it->title);
  }
  SetWindowTextW(s.wnd, title);
}

/* Items for what a station has added to the queue. A track the library doesn't have any more
 * still gets one, as indexes have to stay in step with the player's; it won't open anyway. */
static void add_new_items(void) {
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  if (st.count <= s.count) return;
  item *items = realloc(s.items, (size_t)st.count * sizeof *items);
  if (!items) return;
  s.items = items;
  for (; s.count < st.count; s.count++) {
    item *it = &s.items[s.count];
    memset(it, 0, sizeof *it);
    mc_track_list *list = NULL;
    if (mc_player_queue_uid(s.player, s.count, it->uid) == MC_OK && s.lib &&
        mc_library_get_track(s.lib, it->uid, &list) == MC_OK) {
      const mc_track_info *t = &list->items[0];
      it->title = t->title ? wstr_dup_from_utf8(t->title) : NULL;
      it->artist = t->artist ? wstr_dup_from_utf8(t->artist) : NULL;
      it->album = t->album ? wstr_dup_from_utf8(t->album) : NULL;
      it->cover_dir = t->cover_dir ? wstr_dup_from_utf8(t->cover_dir) : NULL;
      it->duration_ms = t->duration_ms;
      mc_track_list_free(list);
    }
    if (!it->title) it->title = copy_text(mc_wstr(MC_STR_UNTITLED));
  }
}

/* Counts the plays the player says count, and scrobbles them. Writes the ones that ended to
 * the play log, or with no library open leaves them for the one that opens next. */
static void count_plays(void) {
  int index;
  int64_t started_ms;
  if (s.player) add_new_items(); /* a play's index may be one of theirs */
  while (s.player && mc_player_take_played(s.player, &index, &started_ms) == MC_OK) {
    if (index < 0 || index >= s.count || !s.items[index].uid[0]) continue;
    if (s.lib) mc_library_record_play(s.lib, s.items[index].uid);
    if (s.scrobbler && !s.scrobble_off) mc_scrobbler_scrobble(s.scrobbler, s.items[index].uid, started_ms);
  }
  mc_play_event ended;
  while (s.player && s.lib && mc_player_take_ended(s.player, &ended) == MC_OK)
    if (mc_library_log_play(s.lib, &ended) == MC_OK && mc_play_event_starts_station(&ended)) s.station_logged = TRUE;
}

/* Brings everything showing playback up to date with the player. */
static void refresh(void) {
  if (!s.player) return;
  count_plays();
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  const item *it = st.index >= 0 && st.index < s.count ? &s.items[st.index] : NULL;
  int duration_ms = it ? it->duration_ms : 0;
  if (!duration_ms) duration_ms = (int)st.duration_ms;
  /* By the order, not st.shuffle: what a station left behind plays in order. */
  int next = -1;
  BOOL has_next = st.up_next || (it && (st.radio || (mc_player_order(s.player, 1, 1, &next) == MC_OK && next >= 0)));
  BOOL up_next_changed = st.up_next_changes != s.up_next_changes;

  BOOL new_track = st.index != s.index, new_state = st.state != s.state;
  /* These change whether there's a next track. */
  BOOL new_shuffle = !st.shuffle != !s.shown_shuffle || !st.radio != !s.radio || !has_next != !s.has_next || up_next_changed;
  s.index = st.index;
  s.state = st.state;
  s.shown_shuffle = st.shuffle;
  s.radio = st.radio;
  s.has_next = has_next;
  s.up_next_changes = st.up_next_changes;
  if (st.state != MC_PLAYER_PLAYING) {
    s.announced = -1;
  } else if (it && st.index != s.announced) {
    s.announced = st.index;
    if (s.scrobbler && !s.scrobble_off && it->uid[0]) mc_scrobbler_now_playing(s.scrobbler, it->uid);
  }
  if (new_track) {
    now_playing_show(it ? it->title : NULL, it ? it->artist : NULL, duration_ms, it ? it->cover_dir : NULL);
    if (it) media_controls_set_track(it->title, it->artist, it->album, it->cover_dir);
    else media_controls_set_track(NULL, NULL, NULL, NULL);
  }
  now_playing_set_position((int)st.position_ms);
  /* Stopped, the track stays in the now-playing strip, ready to play, but isn't marked. */
  track_list_set_playing(it && st.state != MC_PLAYER_STOPPED ? it->uid : NULL);

  if (new_track || new_state || new_shuffle) {
    set_window_title(st.state != MC_PLAYER_STOPPED ? it : NULL);
    BOOL playable = it || st.up_next; /* up next starts on Play with nothing current */
    taskbar_set_state(playable, st.state == MC_PLAYER_PLAYING, it != NULL, has_next);
    media_controls_set_state(st.state, playable, has_next, st.shuffle);
    media_controls_set_timeline(st.position_ms, duration_ms);
    if (st.state == MC_PLAYER_PLAYING) SetTimer(s.msgs, TICK_TIMER, TICK_MS, NULL);
    else KillTimer(s.msgs, TICK_TIMER);
    if (s.changed) s.changed();
  }
  if (up_next_changed && s.up_next_changed) s.up_next_changed();
  if (s.station_logged) {
    s.station_logged = FALSE;
    if (s.recents_changed) s.recents_changed();
  }
}

/* On the player's thread. */
static void on_player(void *user, mc_player_event event) {
  (void)user;
  (void)event;
  PostMessageW(s.msgs, WM_PLAYER, 0, 0);
}

static void on_media_button(media_button button, LPARAM lp) {
  if (!s.player) return;
  switch (button) {
    case MEDIA_PLAY: mc_player_resume(s.player); break;
    case MEDIA_PAUSE: mc_player_pause(s.player); break;
    case MEDIA_STOP: mc_player_stop(s.player); break;
    case MEDIA_NEXT: mc_player_next(s.player); break;
    case MEDIA_PREVIOUS: mc_player_previous(s.player); break;
    case MEDIA_SEEK: playback_seek((int)lp); return;
    case MEDIA_SHUFFLE:
      if (s.shuffle_requested) s.shuffle_requested(lp != 0);
      else playback_set_shuffle(lp != 0);
      return;
  }
  refresh();
}

static LRESULT CALLBACK msgs_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PLAYER:
    case WM_TIMER:
      refresh();
      return 0;
    case WM_MEDIA_BUTTON:
      on_media_button((media_button)wp, lp);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

void playback_init(HWND wnd, void (*changed)(void)) {
  memset(&s, 0, sizeof s);
  s.wnd = wnd;
  s.changed = changed;
  s.index = s.announced = -1;
  s.volume = 100;
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = msgs_proc, .hInstance = GetModuleHandleW(NULL),
                    .lpszClassName = L"kithsong_playback"};
  RegisterClassExW(&wc);
  s.msgs = CreateWindowExW(0, L"kithsong_playback", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                           GetModuleHandleW(NULL), NULL);
  taskbar_init(wnd);
}

void playback_on_recents_changed(void (*changed)(void)) {
  s.recents_changed = changed;
}

void playback_free(void) {
  playback_set_library(NULL, NULL);
  mc_player_free(s.player); /* first, so nothing more is posted */
  if (s.media_controls) media_controls_free();
  taskbar_free();
  if (s.msgs) DestroyWindow(s.msgs);
  free_items();
  memset(&s, 0, sizeof s);
}

void playback_set_library(mc_library *lib, const char *config_dir) {
  if (s.player && lib != s.lib) { /* their tracks are that library's */
    mc_player_stop_radio(s.player);
    mc_player_up_next_clear(s.player);
  }
  if (!lib && s.player) mc_player_end_play(s.player); /* the library, or the app, is closing */
  count_plays();
  if (s.scrobbler && s.scrobbler_closing) s.scrobbler_closing();
  mc_scrobbler_free(s.scrobbler);
  s.scrobbler = NULL;
  s.lib = lib;
  if (lib && config_dir && mc_scrobbler_new(lib, config_dir, &s.scrobbler) != MC_OK) s.scrobbler = NULL;
}

BOOL playback_window_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
  if (taskbar_message(msg, wp, lp)) {
    *result = 0;
    return TRUE;
  }
  switch (msg) {
    case WM_APPCOMMAND: /* media keys, while the window or one of its children has the focus */
      /* With nothing queued, leave the key to Windows, which passes it on to another player. */
      if (!s.count) return FALSE;
      switch (GET_APPCOMMAND_LPARAM(lp)) {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: playback_toggle(); break;
        case APPCOMMAND_MEDIA_PLAY: on_media_button(MEDIA_PLAY, 0); break;
        case APPCOMMAND_MEDIA_PAUSE: on_media_button(MEDIA_PAUSE, 0); break;
        case APPCOMMAND_MEDIA_STOP: on_media_button(MEDIA_STOP, 0); break;
        case APPCOMMAND_MEDIA_NEXTTRACK: playback_next(); break;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: playback_previous(); break;
        default: return FALSE;
      }
      *result = TRUE;
      return TRUE;
    case WM_COMMAND:
      if (HIWORD(wp) != THBN_CLICKED) return FALSE;
      switch (LOWORD(wp)) {
        case TASKBAR_PREVIOUS: playback_previous(); break;
        case TASKBAR_PLAY: playback_toggle(); break;
        case TASKBAR_NEXT: playback_next(); break;
        default: return FALSE;
      }
      *result = 0;
      return TRUE;
  }
  return FALSE;
}

void playback_end_session(void) {
  if (!s.player) return;
  mc_player_end_play(s.player);
  count_plays();
}

/* Rows of up next keep up with what an edit changed: a track's, and an album's title. A playlist
 * or group goes by what it was called when it was added. */
static void rename_up_next(void) {
  mc_up_next_list *list = playback_up_next();
  for (int i = 0; list && i < list->count; i++) {
    const mc_up_next_entry *e = &list->items[i];
    if (e->first_item < 0 || e->first_item >= s.count) continue;
    const item *it = &s.items[e->first_item];
    const wchar_t *title = e->block ? (e->context == MC_PLAY_CONTEXT_ALBUM ? it->album : NULL) : it->title;
    char *name = title ? wstr_to_utf8(title) : NULL, *artist = !e->block && it->artist ? wstr_to_utf8(it->artist) : NULL;
    if (name) mc_player_up_next_rename(s.player, e->id, name, e->block ? e->artist : artist);
    free(name);
    free(artist);
  }
  mc_up_next_list_free(list);
}

void playback_tracks_changed(void) {
  if (!s.player || !s.lib) return;
  for (int i = 0; i < s.count; i++) {
    item *it = &s.items[i];
    mc_track_list *list = NULL;
    if (!it->uid[0] || mc_library_get_track(s.lib, it->uid, &list) != MC_OK) continue;
    const mc_track_info *t = &list->items[0];
    free(it->title);
    free(it->artist);
    free(it->album);
    free(it->cover_dir);
    it->title = t->title ? wstr_dup_from_utf8(t->title) : copy_text(mc_wstr(MC_STR_UNTITLED));
    it->artist = t->artist ? wstr_dup_from_utf8(t->artist) : NULL;
    it->album = t->album ? wstr_dup_from_utf8(t->album) : NULL;
    it->cover_dir = t->cover_dir ? wstr_dup_from_utf8(t->cover_dir) : NULL;
    if (t->path) mc_player_queue_set_path(s.player, i, t->path);
    mc_track_list_free(list);
  }
  rename_up_next();
  refresh();
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  const item *it = st.index >= 0 && st.index < s.count ? &s.items[st.index] : NULL;
  if (!it) return;
  now_playing_show(it->title, it->artist, it->duration_ms ? it->duration_ms : (int)st.duration_ms, it->cover_dir);
  now_playing_set_position((int)st.position_ms);
  media_controls_set_track(it->title, it->artist, it->album, it->cover_dir);
}

void playback_clear(void) {
  if (!s.player) return;
  mc_player_end_play(s.player);
  count_plays();
  mc_player_free(s.player); /* closes the file */
  s.player = NULL;
  free_items();
  s.index = s.announced = -1;
  s.state = MC_PLAYER_STOPPED;
  s.has_next = FALSE;
  s.up_next_changes = 0;
  KillTimer(s.msgs, TICK_TIMER);
  now_playing_show(NULL, NULL, 0, NULL);
  now_playing_set_position(0);
  media_controls_set_track(NULL, NULL, NULL, NULL);
  media_controls_set_state(MC_PLAYER_STOPPED, FALSE, FALSE, s.shuffle);
  track_list_set_playing(NULL);
  set_window_title(NULL);
  taskbar_set_state(FALSE, FALSE, FALSE, FALSE);
  if (s.changed) s.changed();
  if (s.up_next_changed) s.up_next_changed();
}

/* The player, set up for use. FALSE if it can't be made. */
static BOOL ensure_player(void) {
  if (!s.player && mc_player_new(on_player, NULL, &s.player) != MC_OK) {
    s.player = NULL;
    return FALSE;
  }
  mc_player_set_shuffle(s.player, s.shuffle); /* a new player starts off, at full volume */
  mc_player_set_volume(s.player, s.volume);
  mc_player_set_muted(s.player, s.muted);
  if (!s.media_controls) s.media_controls = media_controls_init(s.wnd, s.msgs, WM_MEDIA_BUTTON);
  return TRUE;
}

/* playback_play(), or with radio, the one track as a station's seed. */
static void play_queue(const playback_track *tracks, int count, int start, mc_play_context context,
                       const char *context_uid, BOOL radio) {
  if (count <= 0 || start < -1 || start >= count || !ensure_player()) return;

  const char **paths = calloc((size_t)count, sizeof *paths);
  const char **uids = calloc((size_t)count, sizeof *uids);
  item *items = calloc((size_t)count, sizeof *items);
  if (!paths || !uids || !items) {
    free(paths);
    free(uids);
    free(items);
    return;
  }
  for (int i = 0; i < count; i++) {
    const playback_track *t = &tracks[i];
    item *it = &items[i];
    if (t->uid && strlen(t->uid) < MC_UID_STRLEN) strcpy(it->uid, t->uid);
    it->title = copy_text(t->title ? t->title : mc_wstr(MC_STR_UNTITLED));
    it->artist = copy_text(t->artist);
    it->album = copy_text(t->album);
    it->cover_dir = copy_text(t->cover_dir);
    it->duration_ms = t->duration_ms;
    paths[i] = t->path;
    uids[i] = t->uid;
  }
  count_plays(); /* the old queue's: the player drops them with it */
  free_items();
  s.items = items;
  s.count = count;
  s.index = s.announced = -1; /* show the track afresh even if it's the same item number */

  if (radio) mc_player_play_station(s.player, mc_library_dir(s.lib), paths[0], uids[0], context_uid);
  else mc_player_set_queue(s.player, paths, count, start, &(mc_play_origin){uids, context, context_uid});
  free(paths);
  free(uids);
  refresh();
}

void playback_play(const playback_track *tracks, int count, int start, mc_play_context context,
                   const char *context_uid) {
  play_queue(tracks, count, start, context, context_uid, FALSE);
}

/* Plays station seed from the track start. FALSE if it can't, with nothing changed. */
static BOOL start_station(const char *seed, const char *start) {
  mc_radio_options opts = {.seed_track = start};
  mc_track_list *list = NULL;
  if (mc_radio_seed_check(s.lib, &opts) != MC_OK || mc_library_get_track(s.lib, start, &list) != MC_OK) return FALSE;
  const mc_track_info *t = &list->items[0];
  wchar_t *title = t->title ? wstr_dup_from_utf8(t->title) : NULL;
  wchar_t *artist = t->artist ? wstr_dup_from_utf8(t->artist) : NULL;
  wchar_t *album = t->album ? wstr_dup_from_utf8(t->album) : NULL;
  wchar_t *cover_dir = t->cover_dir ? wstr_dup_from_utf8(t->cover_dir) : NULL;
  playback_track track = {t->uid, t->path, title, artist, album, cover_dir, t->duration_ms};
  /* The seed check only asks the database: a file that's gone would be skipped without a word. */
  wchar_t *wide = t->path ? wstr_dup_from_utf8(t->path) : NULL;
  wchar_t *path = wide ? wstr_long_path(wide) : NULL;
  free(wide);
  BOOL playable = path && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
  if (playable) {
    play_queue(&track, 1, 0, MC_PLAY_CONTEXT_RADIO, seed, TRUE);
    /* Now, as opening an album is: the play log's row only comes once the seed has ended. */
    if (mc_library_record_opened(s.lib, MC_RECENT_STATION, seed) == MC_OK && s.recents_changed)
      s.recents_changed();
  }
  free(path);
  free(title);
  free(artist);
  free(album);
  free(cover_dir);
  mc_track_list_free(list);
  return playable;
}

void playback_play_station(const char *seed_track_uid, const char *start_uid) {
  if (!s.lib || !seed_track_uid) return;
  char seed[MC_UID_STRLEN]; /* it can be the queue's own, which playing frees */
  snprintf(seed, sizeof seed, "%s", seed_track_uid);
  if (start_uid && start_uid[0] && start_station(seed, start_uid)) return;
  if (!start_station(seed, seed)) now_playing_notice(mc_wstr(MC_STR_STATION_CANT_PLAY));
}

void playback_play_radio(const char *seed_track_uid) {
  playback_play_station(seed_track_uid, NULL);
}

void playback_set_shuffle(BOOL on) {
  s.shuffle = on;
  if (s.player) {
    mc_player_set_shuffle(s.player, on);
    refresh();
  } else if (s.changed) {
    s.changed();
  }
}

BOOL playback_shuffle(void) {
  return s.shuffle;
}

BOOL playback_toggle(void) {
  if (!playback_has_current() && !playback_up_next_count()) return FALSE;
  mc_player_toggle(s.player);
  refresh();
  return TRUE;
}

void playback_next(void) {
  if (!s.player) return;
  mc_player_next(s.player);
  refresh();
}

void playback_previous(void) {
  if (!s.player) return;
  mc_player_previous(s.player);
  refresh();
}

void playback_seek(int ms) {
  if (!s.player) return;
  mc_player_seek_ms(s.player, ms);
  refresh();
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  int duration_ms = st.index >= 0 && st.index < s.count ? s.items[st.index].duration_ms : 0;
  media_controls_set_timeline(st.position_ms, duration_ms ? duration_ms : st.duration_ms);
}

void playback_seek_by(int delta_ms) {
  if (!playback_has_current()) return;
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  int duration_ms = s.items[s.index].duration_ms ? s.items[s.index].duration_ms : (int)st.duration_ms;
  int64_t to = st.position_ms + delta_ms;
  if (duration_ms > 0 && to > duration_ms) to = duration_ms;
  playback_seek(to > 0 ? (int)to : 0);
}

void playback_set_volume(int volume) {
  s.volume = max(0, min(volume, 100));
  if (s.player) mc_player_set_volume(s.player, s.volume);
}

int playback_volume(void) {
  return s.volume;
}

void playback_set_muted(BOOL muted) {
  s.muted = muted;
  if (s.player) mc_player_set_muted(s.player, muted);
}

BOOL playback_muted(void) {
  return s.muted;
}

BOOL playback_is_playing(void) {
  return s.player && s.state == MC_PLAYER_PLAYING;
}

int playback_queue_count(void) {
  return s.player ? s.count : 0;
}

const char *playback_queue_uid(int i) {
  return s.player && i >= 0 && i < s.count ? s.items[i].uid : "";
}

BOOL playback_has_current(void) {
  return s.player && s.index >= 0 && s.index < s.count;
}

const char *playback_current_uid(void) {
  return playback_has_current() ? s.items[s.index].uid : "";
}

BOOL playback_has_next(void) {
  return s.player && s.has_next;
}

mc_scrobbler *playback_scrobbler(void) {
  return s.scrobbler;
}

void playback_on_scrobbler_closing(void (*closing)(void)) {
  s.scrobbler_closing = closing;
}

void playback_on_shuffle_requested(void (*requested)(BOOL on)) {
  s.shuffle_requested = requested;
}

void playback_set_scrobble(BOOL on) {
  s.scrobble_off = !on;
}

/* --- Up next --- */

void playback_set_queue_tracks(BOOL on) {
  s.queue_tracks = on;
}

void playback_on_up_next_changed(void (*changed)(void)) {
  s.up_next_changed = changed;
}

int playback_up_next_count(void) {
  mc_player_status st = {0};
  if (s.player) mc_player_status_get(s.player, &st);
  return st.up_next;
}

mc_up_next_list *playback_up_next(void) {
  mc_up_next_list *list = NULL;
  if (s.player && mc_player_up_next(s.player, &list) != MC_OK) list = NULL;
  return list;
}

void playback_up_next_move(uint32_t id, int to) {
  if (!s.player) return;
  mc_player_up_next_move(s.player, id, to);
  refresh();
}

void playback_up_next_remove(uint32_t id) {
  if (!s.player) return;
  mc_player_up_next_remove(s.player, id);
  refresh();
}

void playback_up_next_clear(void) {
  if (!s.player) return;
  mc_player_up_next_clear(s.player);
  refresh();
}

/* An album's artist credit as shown, for its row. Malloc'd, or NULL. */
static char *album_credit(const char *album_uid) {
  mc_credit_list *credit = NULL;
  if (mc_library_list_credit(s.lib, album_uid, &credit) != MC_OK) return NULL;
  size_t n = 1;
  for (int i = 0; i < credit->count; i++) n += strlen(credit->items[i].name) + strlen(credit->items[i].join);
  char *text = credit->count ? calloc(n, 1) : NULL;
  for (int i = 0; text && i < credit->count; i++) {
    strcat(text, credit->items[i].name);
    strcat(text, credit->items[i].join);
  }
  mc_credit_list_free(credit);
  return text;
}

/* Every album of the group, in its order: their tracks with a file here (see
 * mc_library_list_albums_tracks()). NULL if it can't be listed. */
static mc_track_list *group_tracks(const char *uid, BOOL smart) {
  mc_album_list *albums = NULL;
  mc_status st = smart ? mc_library_list_smart_albums(s.lib, uid, &albums)
                       : mc_library_list_album_group_albums(s.lib, uid, &albums);
  if (st != MC_OK) return NULL;
  const char **uids = albums->count ? calloc((size_t)albums->count, sizeof *uids) : NULL;
  for (int i = 0; uids && i < albums->count; i++) uids[i] = albums->items[i].uid;
  mc_track_list *list = NULL;
  if (uids && mc_library_list_albums_tracks(s.lib, uids, albums->count, &list) != MC_OK) list = NULL;
  free(uids);
  mc_album_list_free(albums);
  return list;
}

static mc_track_list *tracks_of(playback_what what, const char *uid) {
  mc_track_list *list = NULL;
  mc_status st = MC_ERR_NOT_FOUND;
  switch (what) {
    case PLAYBACK_TRACK: st = mc_library_get_track(s.lib, uid, &list); break;
    case PLAYBACK_ALBUM: st = mc_library_list_album_tracks(s.lib, uid, &list); break;
    case PLAYBACK_ARTIST: st = mc_library_list_artist_tracks(s.lib, uid, &list); break;
    case PLAYBACK_PLAYLIST: st = mc_library_list_playlist_tracks(s.lib, uid, &list); break;
    case PLAYBACK_SMART_PLAYLIST: st = mc_library_list_smart_tracks(s.lib, uid, &list); break;
    case PLAYBACK_GROUP: return group_tracks(uid, FALSE);
    case PLAYBACK_SMART_GROUP: return group_tracks(uid, TRUE);
  }
  return st == MC_OK ? list : NULL;
}

void playback_add(playback_what what, const char *uid, const wchar_t *name, BOOL next) {
  if (!s.lib || !uid || !ensure_player()) return;
  mc_track_list *list = tracks_of(what, uid);
  mc_up_next_track *tracks = list && list->count ? calloc((size_t)list->count, sizeof *tracks) : NULL;
  int count = 0;
  for (int i = 0; tracks && i < list->count; i++) {
    const mc_track_info *t = &list->items[i];
    if (t->path) tracks[count++] = (mc_up_next_track){t->path, t->uid, t->title, t->artist, t->duration_ms};
  }
  if (count) {
    /* The second line of a block's row: what it is, or with none given, its tracks left. */
    static const struct {
      mc_play_context context;
      int kind; /* a string for the second line, or 0 */
    } k_whats[] = {[PLAYBACK_TRACK] = {MC_PLAY_CONTEXT_MANUAL, 0},
                   [PLAYBACK_ALBUM] = {MC_PLAY_CONTEXT_ALBUM, 0},
                   [PLAYBACK_ARTIST] = {MC_PLAY_CONTEXT_ARTIST, 0},
                   [PLAYBACK_PLAYLIST] = {MC_PLAY_CONTEXT_PLAYLIST, MC_STR_PLAYLIST},
                   [PLAYBACK_SMART_PLAYLIST] = {MC_PLAY_CONTEXT_PLAYLIST, MC_STR_SMART_PLAYLIST},
                   [PLAYBACK_GROUP] = {MC_PLAY_CONTEXT_ALBUM_GROUP, MC_STR_ALBUM_GROUP},
                   [PLAYBACK_SMART_GROUP] = {MC_PLAY_CONTEXT_ALBUM_GROUP, MC_STR_ALBUM_GROUP}};
    char *title = what != PLAYBACK_TRACK && !s.queue_tracks && name ? wstr_to_utf8(name) : NULL;
    char *credit = title && what == PLAYBACK_ALBUM ? album_credit(uid) : NULL;
    const char *artist = credit ? credit : title && k_whats[what].kind ? mc_str((enum mc_str_id)k_whats[what].kind) : NULL;
    mc_up_next_add add = {tracks, count, title, artist, k_whats[what].context,
                          what == PLAYBACK_TRACK ? NULL : uid};
    if ((next ? mc_player_play_next(s.player, &add) : mc_player_play_last(s.player, &add)) == MC_OK)
      now_playing_flash(mc_wstr(MC_STR_WINDOWS_QUEUE_ADDED));
    free(title);
    free(credit);
    refresh();
  } else {
    now_playing_notice(mc_wstr(MC_STR_WINDOWS_QUEUE_NOTHING_HERE));
  }
  free(tracks);
  mc_track_list_free(list);
}
