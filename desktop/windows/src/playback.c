#include "playback.h"

#include <shobjidl.h> /* THBN_CLICKED */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "media_controls.h"
#include "musiccore/musiccore.h"
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
  mc_player *player;
  BOOL media_controls; /* set up on first play, as it loads WinRT */
  mc_library *lib;     /* where plays are counted, or NULL */
  mc_scrobbler *scrobbler;
  int announced;       /* the item last sent as now playing, or -1 */
  item *items;
  int count;

  /* What's shown, to tell what changed. */
  int index;
  mc_player_state state;
} s;

static wchar_t *dup(const wchar_t *w) {
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

/* Counts the plays the player says count, and scrobbles them. */
static void count_plays(void) {
  int index;
  int64_t started_ms;
  while (s.player && mc_player_take_played(s.player, &index, &started_ms) == MC_OK) {
    if (index < 0 || index >= s.count || !s.items[index].uid[0]) continue;
    if (s.lib) mc_library_record_play(s.lib, s.items[index].uid);
    if (s.scrobbler) mc_scrobbler_scrobble(s.scrobbler, s.items[index].uid, started_ms);
  }
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
  BOOL has_next = it && st.index + 1 < s.count;

  BOOL new_track = st.index != s.index, new_state = st.state != s.state;
  s.index = st.index;
  s.state = st.state;
  if (st.state != MC_PLAYER_PLAYING) {
    s.announced = -1;
  } else if (it && st.index != s.announced) {
    s.announced = st.index;
    if (s.scrobbler && it->uid[0]) mc_scrobbler_now_playing(s.scrobbler, it->uid);
  }
  if (new_track) {
    now_playing_show(it ? it->title : NULL, it ? it->artist : NULL, duration_ms, it ? it->cover_dir : NULL);
    if (it) media_controls_set_track(it->title, it->artist, it->album, it->cover_dir);
    else media_controls_set_track(NULL, NULL, NULL, NULL);
  }
  now_playing_set_position((int)st.position_ms);
  /* Stopped, the track stays in the now-playing strip, ready to play, but isn't marked. */
  track_list_set_playing(it && st.state != MC_PLAYER_STOPPED ? it->uid : NULL);

  if (new_track || new_state) {
    set_window_title(st.state != MC_PLAYER_STOPPED ? it : NULL);
    taskbar_set_state(it != NULL, st.state == MC_PLAYER_PLAYING, it != NULL, has_next);
    media_controls_set_state(st.state, it != NULL, has_next);
    media_controls_set_timeline(st.position_ms, duration_ms);
    if (st.state == MC_PLAYER_PLAYING) SetTimer(s.msgs, TICK_TIMER, TICK_MS, NULL);
    else KillTimer(s.msgs, TICK_TIMER);
    if (s.changed) s.changed();
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
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = msgs_proc, .hInstance = GetModuleHandleW(NULL),
                    .lpszClassName = L"musicapp_playback"};
  RegisterClassExW(&wc);
  s.msgs = CreateWindowExW(0, L"musicapp_playback", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                           GetModuleHandleW(NULL), NULL);
  taskbar_init(wnd);
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
  count_plays();
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

void playback_clear(void) {
  if (!s.player) return;
  count_plays();
  mc_player_free(s.player); /* closes the file */
  s.player = NULL;
  free_items();
  s.index = s.announced = -1;
  s.state = MC_PLAYER_STOPPED;
  KillTimer(s.msgs, TICK_TIMER);
  now_playing_show(NULL, NULL, 0, NULL);
  now_playing_set_position(0);
  media_controls_set_track(NULL, NULL, NULL, NULL);
  media_controls_set_state(MC_PLAYER_STOPPED, FALSE, FALSE);
  track_list_set_playing(NULL);
  set_window_title(NULL);
  taskbar_set_state(FALSE, FALSE, FALSE, FALSE);
  if (s.changed) s.changed();
}

void playback_play(const playback_track *tracks, int count, int start) {
  if (count <= 0 || start < 0 || start >= count) return;
  if (!s.player && mc_player_new(on_player, NULL, &s.player) != MC_OK) {
    s.player = NULL;
    return;
  }
  if (!s.media_controls) s.media_controls = media_controls_init(s.wnd, s.msgs, WM_MEDIA_BUTTON);

  const char **paths = calloc((size_t)count, sizeof *paths);
  item *items = calloc((size_t)count, sizeof *items);
  if (!paths || !items) {
    free(paths);
    free(items);
    return;
  }
  for (int i = 0; i < count; i++) {
    const playback_track *t = &tracks[i];
    item *it = &items[i];
    if (t->uid && strlen(t->uid) < MC_UID_STRLEN) strcpy(it->uid, t->uid);
    it->title = dup(t->title ? t->title : mc_wstr(MC_STR_UNTITLED));
    it->artist = dup(t->artist);
    it->album = dup(t->album);
    it->cover_dir = dup(t->cover_dir);
    it->duration_ms = t->duration_ms;
    paths[i] = t->path;
  }
  count_plays(); /* the old queue's: the player drops them with it */
  free_items();
  s.items = items;
  s.count = count;
  s.index = s.announced = -1; /* show the track afresh even if it's the same item number */

  mc_player_set_queue(s.player, paths, count, start);
  free(paths);
  refresh();
}

BOOL playback_toggle(void) {
  if (!s.player || !s.count) return FALSE;
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

BOOL playback_is_playing(void) {
  return s.player && s.state == MC_PLAYER_PLAYING;
}

BOOL playback_has_current(void) {
  return s.player && s.index >= 0 && s.index < s.count;
}

BOOL playback_has_next(void) {
  return playback_has_current() && s.index + 1 < s.count;
}
