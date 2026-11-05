#include "playback.h"

#include <string.h>

#include "artist_view.h"
#include "mpris.h"
#include "musiccore/radio.h"
#include "now_playing.h"
#include "track_list.h"
#include "volume_button.h"

enum { TICK_MS = 250 }; /* moves the seekbar on while playing */

typedef struct item {
  char uid[MC_UID_STRLEN];
  char *title, *artist, *album, *cover_dir;
  int duration_ms;
} item;

static struct {
  GtkWindow *win;
  void (*changed)(void *user);
  void (*recents_changed)(void *user);
  void (*radio_failed)(void *user);
  gboolean station_logged; /* for the end of the next refresh() */
  void *user;
  mc_player *player;
  mc_library *lib; /* where plays are counted, or NULL */
  mc_scrobbler *scrobbler;
  gboolean scrobble_off;              /* this device doesn't scrobble (the account is the group's) */
  void (*scrobbler_closing)(void);
  int announced;   /* the item last sent as now playing, or -1 */
  item *items;
  int count;
  gboolean shuffle;
  int volume;
  gboolean muted;
  guint tick;   /* timer source while playing */
  gint pending; /* an idle is queued for the player's news; set on its thread */

  /* What's shown, to tell what changed. */
  int index;
  mc_player_state state;
  gboolean has_next;
} s;

static void free_items(void) {
  for (int i = 0; i < s.count; i++) {
    g_free(s.items[i].title);
    g_free(s.items[i].artist);
    g_free(s.items[i].album);
    g_free(s.items[i].cover_dir);
  }
  g_free(s.items);
  s.items = NULL;
  s.count = 0;
}

/* "Paranoid Android – Radiohead" while there's a track, for the window list and Alt+Tab. */
static void set_window_title(const item *it) {
  if (!s.win) return;
  char *title = it && it->artist ? mc_format_alloc(mc_str(MC_STR_TRACK_BYLINE), MC_S(it->title), MC_S(it->artist))
                                 : NULL;
  gtk_window_set_title(s.win, title ? title : it ? it->title : mc_str(MC_STR_APP_NAME));
  mc_free(title);
}

static const item *current(const mc_player_status *st) {
  return st->index >= 0 && st->index < s.count ? &s.items[st->index] : NULL;
}

static int duration_of(const item *it, const mc_player_status *st) {
  int ms = it ? it->duration_ms : 0;
  return ms ? ms : (int)st->duration_ms;
}

void playback_art_changed(mc_library *lib, const char *album_uid) {
  mc_track_list *tracks = NULL;
  if (!s.player || !s.count || mc_library_list_album_tracks(lib, album_uid, &tracks) != MC_OK) return;
  for (int i = 0; i < tracks->count; i++)
    for (int k = 0; k < s.count; k++)
      if (!strcmp(s.items[k].uid, tracks->items[i].uid)) {
        g_free(s.items[k].cover_dir);
        s.items[k].cover_dir = g_strdup(tracks->items[i].cover_dir);
      }
  mc_track_list_free(tracks);
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  const item *it = current(&st);
  if (!it) return;
  now_playing_art_changed(it->cover_dir);
  mpris_set_track(it->uid, it->title, it->artist, it->album, it->cover_dir, duration_of(it, &st));
}

static gboolean on_tick(gpointer data);

/* Items for what a station has added to the queue. A track the library doesn't have any more
 * still gets one, as indexes have to stay in step with the player's; it won't open anyway. */
static void add_new_items(void) {
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  if (st.count <= s.count) return;
  s.items = g_renew(item, s.items, (gsize)st.count);
  for (; s.count < st.count; s.count++) {
    item *it = &s.items[s.count];
    memset(it, 0, sizeof *it);
    mc_track_list *list = NULL;
    if (mc_player_queue_uid(s.player, s.count, it->uid) == MC_OK && s.lib &&
        mc_library_get_track(s.lib, it->uid, &list) == MC_OK) {
      const mc_track_info *t = &list->items[0];
      it->title = g_strdup(t->title);
      it->artist = g_strdup(t->artist);
      it->album = g_strdup(t->album);
      it->cover_dir = g_strdup(t->cover_dir);
      it->duration_ms = t->duration_ms;
      mc_track_list_free(list);
    }
    if (!it->title) it->title = g_strdup(mc_str(MC_STR_UNTITLED));
  }
}

/* Whether next() has anywhere to go. Shuffle can't say: a station's queue stays in its own order
 * when the station ends, so with shuffle on it still has a last item. */
static gboolean next_exists(const mc_player_status *st) {
  int next = -1;
  return st->radio || (mc_player_order(s.player, 1, 1, &next) == MC_OK && next >= 0);
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
  add_new_items(); /* the player may have moved on to one that's only just come */
  const item *it = current(&st);
  int duration_ms = duration_of(it, &st);
  gboolean has_next = it && next_exists(&st);

  gboolean new_track = st.index != s.index, new_state = st.state != s.state;
  gboolean new_next = has_next != s.has_next;
  s.index = st.index;
  s.state = st.state;
  s.has_next = has_next;
  if (st.state != MC_PLAYER_PLAYING) {
    s.announced = -1;
  } else if (it && st.index != s.announced) {
    s.announced = st.index;
    if (s.scrobbler && !s.scrobble_off && it->uid[0]) mc_scrobbler_now_playing(s.scrobbler, it->uid);
  }
  if (new_track) {
    now_playing_show(it ? it->title : NULL, it ? it->artist : NULL, duration_ms, it ? it->cover_dir : NULL);
    if (it) mpris_set_track(it->uid, it->title, it->artist, it->album, it->cover_dir, duration_ms);
    else mpris_set_track(NULL, NULL, NULL, NULL, NULL, 0);
  }
  now_playing_set_position((int)st.position_ms);
  /* Stopped, the track stays in the now-playing strip, ready to play, but isn't marked. */
  track_list_set_playing(it && st.state != MC_PLAYER_STOPPED ? it->uid : NULL);
  artist_view_set_playing(it && st.state != MC_PLAYER_STOPPED ? it->uid : NULL);

  if (new_track || new_state || new_next) {
    set_window_title(st.state != MC_PLAYER_STOPPED ? it : NULL);
    mpris_set_state(st.state, it != NULL, has_next);
    if (st.state == MC_PLAYER_PLAYING && !s.tick) s.tick = g_timeout_add(TICK_MS, on_tick, NULL);
    else if (st.state != MC_PLAYER_PLAYING && s.tick) g_clear_handle_id(&s.tick, g_source_remove);
    if (s.changed) s.changed(s.user);
  }
  if (s.station_logged) {
    s.station_logged = FALSE;
    if (s.recents_changed) s.recents_changed(s.user);
  }
}

static gboolean on_tick(gpointer data) {
  (void)data;
  refresh();
  return G_SOURCE_CONTINUE;
}

static gboolean on_player_idle(gpointer data) {
  (void)data;
  g_atomic_int_set(&s.pending, 0);
  refresh();
  return G_SOURCE_REMOVE;
}

/* On the player's thread. Events that come in a burst share one refresh. */
static void on_player(void *user, mc_player_event event) {
  (void)user;
  (void)event;
  if (g_atomic_int_compare_and_exchange(&s.pending, 0, 1)) g_idle_add(on_player_idle, &s);
}

void playback_init(GtkWindow *win, void (*changed)(void *user), void *user) {
  memset(&s, 0, sizeof s);
  s.win = win;
  s.changed = changed;
  s.user = user;
  s.index = s.announced = -1;
  s.volume = 100;
}

void playback_on_recents_changed(void (*changed)(void *user)) {
  s.recents_changed = changed;
}

void playback_on_radio_failed(void (*failed)(void *user)) {
  s.radio_failed = failed;
}

void playback_free(void) {
  playback_set_library(NULL, NULL);
  mc_player_free(s.player); /* first, so nothing more is posted */
  s.player = NULL;
  if (s.tick) g_source_remove(s.tick);
  g_idle_remove_by_data(&s); /* a refresh still queued */
  free_items();
  memset(&s, 0, sizeof s);
}

void playback_set_library(mc_library *lib, const char *config_dir) {
  if (s.player && lib != s.lib) mc_player_stop_radio(s.player); /* its tracks are that library's */
  if (!lib && s.player) mc_player_end_play(s.player); /* the library, or the app, is closing */
  count_plays();
  if (s.scrobbler && s.scrobbler_closing) s.scrobbler_closing();
  mc_scrobbler_free(s.scrobbler);
  s.scrobbler = NULL;
  s.lib = lib;
  if (lib && config_dir && mc_scrobbler_new(lib, config_dir, &s.scrobbler) != MC_OK) s.scrobbler = NULL;
}

void playback_clear(void) {
  if (!s.player) return;
  mc_player_end_play(s.player);
  count_plays();
  mc_player_free(s.player); /* closes the file */
  s.player = NULL;
  g_idle_remove_by_data(&s);
  g_atomic_int_set(&s.pending, 0);
  if (s.tick) g_clear_handle_id(&s.tick, g_source_remove);
  free_items();
  s.index = s.announced = -1;
  s.state = MC_PLAYER_STOPPED;
  s.has_next = FALSE;
  now_playing_show(NULL, NULL, 0, NULL);
  now_playing_set_position(0);
  mpris_set_track(NULL, NULL, NULL, NULL, NULL, 0);
  mpris_set_state(MC_PLAYER_STOPPED, FALSE, FALSE);
  track_list_set_playing(NULL);
  artist_view_set_playing(NULL);
  set_window_title(NULL);
  if (s.changed) s.changed(s.user);
}

/* playback_play(), or with radio, the one track as a station's seed. */
static void play_queue(const playback_track *tracks, int count, int start, mc_play_context context,
                       const char *context_uid, gboolean radio) {
  if (count <= 0 || start < -1 || start >= count) return;
  if (!s.player && mc_player_new(on_player, NULL, &s.player) != MC_OK) {
    s.player = NULL;
    return;
  }
  mc_player_set_shuffle(s.player, s.shuffle); /* a new player starts off, at full volume */
  mc_player_set_volume(s.player, s.volume);
  mc_player_set_muted(s.player, s.muted);
  const char **paths = g_new0(const char *, (gsize)count);
  const char **uids = g_new0(const char *, (gsize)count);
  item *items = g_new0(item, (gsize)count);
  for (int i = 0; i < count; i++) {
    const playback_track *t = &tracks[i];
    item *it = &items[i];
    if (t->uid && strlen(t->uid) < MC_UID_STRLEN) strcpy(it->uid, t->uid);
    it->title = g_strdup(t->title ? t->title : mc_str(MC_STR_UNTITLED));
    it->artist = g_strdup(t->artist);
    it->album = g_strdup(t->album);
    it->cover_dir = g_strdup(t->cover_dir);
    it->duration_ms = t->duration_ms;
    paths[i] = t->path;
    uids[i] = t->uid;
  }
  count_plays(); /* the old queue's: the player drops them with it */
  free_items();
  s.items = items;
  s.count = count;
  s.index = s.announced = -1; /* show the track afresh even if it's the same item number */

  if (radio) mc_player_play_radio(s.player, mc_library_dir(s.lib), paths[0], uids[0]);
  else mc_player_set_queue(s.player, paths, count, start, &(mc_play_origin){uids, context, context_uid});
  g_free(paths);
  g_free(uids);
  refresh();
}

void playback_play(const playback_track *tracks, int count, int start, mc_play_context context,
                   const char *context_uid) {
  play_queue(tracks, count, start, context, context_uid, FALSE);
}

gboolean playback_can_radio(const char *seed_track_uid) {
  mc_radio_options opts = {.seed_track = seed_track_uid};
  return s.lib && seed_track_uid && seed_track_uid[0] && mc_radio_seed_check(s.lib, &opts) == MC_OK;
}

/* Not playback_can_radio()'s business: that's a menu asking, before anything was chosen. */
static void radio_failed(void) {
  if (s.radio_failed) s.radio_failed(s.user);
}

void playback_play_radio(const char *seed_track_uid) {
  if (!s.lib || !seed_track_uid) return;
  mc_track_list *list = NULL;
  if (!playback_can_radio(seed_track_uid) || mc_library_get_track(s.lib, seed_track_uid, &list) != MC_OK) {
    radio_failed();
    return;
  }
  const mc_track_info *t = &list->items[0];
  playback_track track = {t->uid, t->path, t->title, t->artist, t->album, t->cover_dir, t->duration_ms};
  /* The seed check only asks the database: a file that's gone would be skipped without a word. */
  if (t->path && g_file_test(t->path, G_FILE_TEST_EXISTS)) {
    play_queue(&track, 1, 0, MC_PLAY_CONTEXT_RADIO, t->uid, TRUE);
    /* Now, as opening an album is: the play log's row only comes once the seed has ended. */
    if (mc_library_record_opened(s.lib, MC_RECENT_STATION, t->uid) == MC_OK && s.recents_changed)
      s.recents_changed(s.user);
  } else {
    radio_failed();
  }
  mc_track_list_free(list);
}

void playback_set_shuffle(gboolean on) {
  gboolean changed = !on != !s.shuffle;
  s.shuffle = on;
  now_playing_set_shuffle(on);
  if (changed) mpris_shuffle_changed();
  if (s.player) {
    mc_player_set_shuffle(s.player, on);
    refresh();
  } else if (s.changed) {
    s.changed(s.user);
  }
}

gboolean playback_shuffle(void) {
  return s.shuffle;
}

gboolean playback_toggle(void) {
  if (!s.player || !s.count) return FALSE;
  mc_player_toggle(s.player);
  refresh();
  return TRUE;
}

void playback_resume(void) {
  if (!s.player || !s.count) return;
  mc_player_resume(s.player);
  refresh();
}

void playback_pause(void) {
  if (!s.player) return;
  mc_player_pause(s.player);
  refresh();
}

void playback_stop(void) {
  if (!s.player) return;
  mc_player_stop(s.player);
  refresh();
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

void playback_seek(int64_t ms) {
  if (!s.player) return;
  mc_player_seek_ms(s.player, ms < 0 ? 0 : ms);
  refresh();
  mpris_seeked(playback_position_ms());
}

void playback_seek_by(int64_t delta_ms) {
  if (!playback_has_current()) return;
  int64_t to = playback_position_ms() + delta_ms, duration_ms = playback_duration_ms();
  if (duration_ms > 0 && to > duration_ms) to = duration_ms;
  playback_seek(to);
}

void playback_set_volume(int volume) {
  volume = CLAMP(volume, 0, 100);
  if (volume == s.volume) return;
  s.volume = volume;
  if (s.player) mc_player_set_volume(s.player, s.volume);
  mpris_volume_changed();
  volume_button_update();
}

int playback_volume(void) {
  return s.volume;
}

void playback_set_muted(gboolean muted) {
  if (!muted == !s.muted) return;
  s.muted = muted;
  if (s.player) mc_player_set_muted(s.player, muted);
  mpris_volume_changed();
  volume_button_update();
}

gboolean playback_muted(void) {
  return s.muted;
}

gboolean playback_is_playing(void) {
  return s.player && s.state == MC_PLAYER_PLAYING;
}

mc_player_state playback_state(void) {
  return s.player ? s.state : MC_PLAYER_STOPPED;
}

int playback_queue_count(void) {
  return s.player ? s.count : 0;
}

const char *playback_queue_uid(int i) {
  return s.player && i >= 0 && i < s.count ? s.items[i].uid : "";
}

gboolean playback_has_current(void) {
  return s.player && s.index >= 0 && s.index < s.count;
}

const char *playback_current_uid(void) {
  return playback_has_current() ? s.items[s.index].uid : "";
}

gboolean playback_has_next(void) {
  return playback_has_current() && s.has_next;
}

int64_t playback_position_ms(void) {
  if (!s.player) return 0;
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  return st.position_ms;
}

int64_t playback_duration_ms(void) {
  if (!s.player) return 0;
  mc_player_status st;
  mc_player_status_get(s.player, &st);
  return duration_of(current(&st), &st);
}

mc_scrobbler *playback_scrobbler(void) {
  return s.scrobbler;
}

void playback_on_scrobbler_closing(void (*closing)(void)) {
  s.scrobbler_closing = closing;
}

void playback_set_scrobble(gboolean on) {
  s.scrobble_off = !on;
}
