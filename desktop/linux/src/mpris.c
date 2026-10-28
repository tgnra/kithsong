#include "mpris.h"

#include <string.h>

#include "cover.h"
#include "playback.h"

#define BUS_NAME "org.mpris.MediaPlayer2.kithsong"
#define OBJECT_PATH "/org/mpris/MediaPlayer2"
#define ROOT_IFACE "org.mpris.MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"
#define NO_TRACK "/org/mpris/MediaPlayer2/TrackList/NoTrack"

static const char introspection[] =
  "<node>"
  "  <interface name='org.mpris.MediaPlayer2'>"
  "    <method name='Raise'/>"
  "    <method name='Quit'/>"
  "    <property name='CanQuit' type='b' access='read'/>"
  "    <property name='CanRaise' type='b' access='read'/>"
  "    <property name='HasTrackList' type='b' access='read'/>"
  "    <property name='Identity' type='s' access='read'/>"
  "    <property name='DesktopEntry' type='s' access='read'/>"
  "    <property name='SupportedUriSchemes' type='as' access='read'/>"
  "    <property name='SupportedMimeTypes' type='as' access='read'/>"
  "  </interface>"
  "  <interface name='org.mpris.MediaPlayer2.Player'>"
  "    <method name='Next'/>"
  "    <method name='Previous'/>"
  "    <method name='Pause'/>"
  "    <method name='PlayPause'/>"
  "    <method name='Stop'/>"
  "    <method name='Play'/>"
  "    <method name='Seek'><arg direction='in' name='Offset' type='x'/></method>"
  "    <method name='SetPosition'>"
  "      <arg direction='in' name='TrackId' type='o'/>"
  "      <arg direction='in' name='Position' type='x'/>"
  "    </method>"
  "    <method name='OpenUri'><arg direction='in' name='Uri' type='s'/></method>"
  "    <signal name='Seeked'><arg name='Position' type='x'/></signal>"
  "    <property name='PlaybackStatus' type='s' access='read'/>"
  "    <property name='Rate' type='d' access='read'/>"
  "    <property name='Metadata' type='a{sv}' access='read'/>"
  "    <property name='Volume' type='d' access='read'/>"
  "    <property name='Position' type='x' access='read'>"
  "      <annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='false'/>"
  "    </property>"
  "    <property name='MinimumRate' type='d' access='read'/>"
  "    <property name='MaximumRate' type='d' access='read'/>"
  "    <property name='CanGoNext' type='b' access='read'/>"
  "    <property name='CanGoPrevious' type='b' access='read'/>"
  "    <property name='CanPlay' type='b' access='read'/>"
  "    <property name='CanPause' type='b' access='read'/>"
  "    <property name='CanSeek' type='b' access='read'/>"
  "    <property name='CanControl' type='b' access='read'/>"
  "  </interface>"
  "</node>";

static struct {
  GtkApplication *gapp;
  GtkWindow *win;
  GDBusConnection *conn;
  GDBusNodeInfo *node;
  guint owner, root_id, player_id;

  /* The track shown; trackid is NULL for none. */
  char *trackid, *title, *artist, *album, *art_url;
  int64_t length_us;
  mc_player_state state;
  gboolean has_current, has_next;
} s;

/* --- Properties --- */

static const char *status_name(mc_player_state state) {
  return state == MC_PLAYER_PLAYING ? "Playing" : state == MC_PLAYER_PAUSED ? "Paused" : "Stopped";
}

static gboolean can_seek(void) {
  return s.has_current && s.length_us > 0;
}

static GVariant *metadata(void) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(s.trackid ? s.trackid : NO_TRACK));
  if (s.trackid) {
    if (s.length_us > 0) g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(s.length_us));
    if (s.title) g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(s.title));
    if (s.artist) {
      const char *artists[] = {s.artist, NULL};
      g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(artists, 1));
      g_variant_builder_add(&b, "{sv}", "xesam:albumArtist", g_variant_new_strv(artists, 1));
    }
    if (s.album) g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(s.album));
    if (s.art_url) g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(s.art_url));
  }
  return g_variant_builder_end(&b);
}

static GVariant *get_root(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                          const char *name, GError **error, gpointer user) {
  (void)conn;
  (void)sender;
  (void)path;
  (void)iface;
  (void)error;
  (void)user;
  if (!strcmp(name, "CanQuit") || !strcmp(name, "CanRaise")) return g_variant_new_boolean(TRUE);
  if (!strcmp(name, "HasTrackList")) return g_variant_new_boolean(FALSE);
  if (!strcmp(name, "Identity")) return g_variant_new_string(mc_str(MC_STR_APP_NAME));
  if (!strcmp(name, "DesktopEntry")) return g_variant_new_string(g_application_get_application_id(G_APPLICATION(s.gapp)));
  if (!strcmp(name, "SupportedUriSchemes") || !strcmp(name, "SupportedMimeTypes"))
    return g_variant_new_strv(NULL, 0);
  return NULL;
}

static GVariant *get_player(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                            const char *name, GError **error, gpointer user) {
  (void)conn;
  (void)sender;
  (void)path;
  (void)iface;
  (void)error;
  (void)user;
  if (!strcmp(name, "PlaybackStatus")) return g_variant_new_string(status_name(s.state));
  if (!strcmp(name, "Rate") || !strcmp(name, "MinimumRate") || !strcmp(name, "MaximumRate") ||
      !strcmp(name, "Volume"))
    return g_variant_new_double(1.0);
  if (!strcmp(name, "Metadata")) return metadata();
  if (!strcmp(name, "Position")) return g_variant_new_int64(s.has_current ? playback_position_ms() * 1000 : 0);
  if (!strcmp(name, "CanGoNext")) return g_variant_new_boolean(s.has_next);
  if (!strcmp(name, "CanGoPrevious") || !strcmp(name, "CanPlay") || !strcmp(name, "CanPause"))
    return g_variant_new_boolean(s.has_current);
  if (!strcmp(name, "CanSeek")) return g_variant_new_boolean(can_seek());
  if (!strcmp(name, "CanControl")) return g_variant_new_boolean(TRUE);
  return NULL;
}

/* --- Methods --- */

static void call_root(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user) {
  (void)conn;
  (void)sender;
  (void)path;
  (void)iface;
  (void)params;
  (void)user;
  if (!strcmp(method, "Raise")) gtk_window_present(s.win);
  else if (!strcmp(method, "Quit")) g_application_quit(G_APPLICATION(s.gapp));
  g_dbus_method_invocation_return_value(inv, NULL);
}

/* Offset is relative, in µs. Past the end means the next track, as the spec says. */
static void seek_by(int64_t offset_us) {
  if (!can_seek()) return;
  int64_t to = playback_position_ms() * 1000 + offset_us;
  if (to >= s.length_us) playback_next();
  else playback_seek((to < 0 ? 0 : to) / 1000);
}

static void call_player(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user) {
  (void)conn;
  (void)sender;
  (void)path;
  (void)iface;
  (void)user;
  if (!strcmp(method, "Next")) {
    playback_next();
  } else if (!strcmp(method, "Previous")) {
    playback_previous();
  } else if (!strcmp(method, "Pause")) {
    if (playback_state() == MC_PLAYER_PLAYING) playback_pause();
  } else if (!strcmp(method, "PlayPause")) {
    playback_toggle();
  } else if (!strcmp(method, "Stop")) {
    playback_stop();
  } else if (!strcmp(method, "Play")) {
    if (playback_state() != MC_PLAYER_PLAYING) playback_resume();
  } else if (!strcmp(method, "Seek")) {
    gint64 offset = 0;
    g_variant_get(params, "(x)", &offset);
    seek_by(offset);
  } else if (!strcmp(method, "SetPosition")) {
    const char *track = NULL;
    gint64 position = 0;
    g_variant_get(params, "(&ox)", &track, &position);
    /* Ignored if it's for a track that's no longer playing, or out of range. */
    if (can_seek() && s.trackid && !strcmp(track, s.trackid) && position >= 0 && position <= s.length_us)
      playback_seek(position / 1000);
  } else if (!strcmp(method, "OpenUri")) {
    g_dbus_method_invocation_return_error_literal(inv, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                                  "Opening URIs isn't supported");
    return;
  }
  g_dbus_method_invocation_return_value(inv, NULL);
}

static const GDBusInterfaceVTable root_vtable = {call_root, get_root, NULL, {0}};
static const GDBusInterfaceVTable player_vtable = {call_player, get_player, NULL, {0}};

/* --- Setup --- */

static void on_name_lost(GDBusConnection *conn, const char *name, gpointer user) {
  (void)conn;
  (void)user;
  g_warning("Couldn't take the D-Bus name %s; media controls won't show Kithsong", name);
}

void mpris_init(GtkApplication *gapp, GtkWindow *win) {
  s.gapp = gapp;
  s.win = win;
  s.conn = g_application_get_dbus_connection(G_APPLICATION(gapp));
  if (!s.conn) return; /* no session bus */
  g_object_ref(s.conn);
  s.node = g_dbus_node_info_new_for_xml(introspection, NULL);
  s.root_id = g_dbus_connection_register_object(s.conn, OBJECT_PATH, s.node->interfaces[0], &root_vtable,
                                                NULL, NULL, NULL);
  s.player_id = g_dbus_connection_register_object(s.conn, OBJECT_PATH, s.node->interfaces[1], &player_vtable,
                                                  NULL, NULL, NULL);
  s.owner = g_bus_own_name_on_connection(s.conn, BUS_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, NULL, on_name_lost,
                                         NULL, NULL);
}

static void clear_track(void) {
  g_clear_pointer(&s.trackid, g_free);
  g_clear_pointer(&s.title, g_free);
  g_clear_pointer(&s.artist, g_free);
  g_clear_pointer(&s.album, g_free);
  g_clear_pointer(&s.art_url, g_free);
  s.length_us = 0;
}

void mpris_free(void) {
  if (s.owner) g_bus_unown_name(s.owner);
  if (s.conn) {
    if (s.root_id) g_dbus_connection_unregister_object(s.conn, s.root_id);
    if (s.player_id) g_dbus_connection_unregister_object(s.conn, s.player_id);
    g_object_unref(s.conn);
  }
  if (s.node) g_dbus_node_info_unref(s.node);
  clear_track();
  memset(&s, 0, sizeof s);
}

/* --- Updates --- */

/* Sends PropertiesChanged for the player, if anything's in b; ends b either way. */
static void emit_changed(GVariantBuilder *b, gboolean any) {
  GVariant *changed = g_variant_builder_end(b);
  if (any && s.conn)
    g_dbus_connection_emit_signal(s.conn, NULL, OBJECT_PATH, "org.freedesktop.DBus.Properties",
                                  "PropertiesChanged", g_variant_new("(s@a{sv}as)", PLAYER_IFACE, changed, NULL),
                                  NULL);
  else
    g_variant_unref(g_variant_ref_sink(changed));
}

void mpris_set_track(const char *uid, const char *title, const char *artist, const char *album,
                     const char *cover_dir, int64_t duration_ms) {
  gboolean could_seek = can_seek();
  clear_track();
  if (uid) {
    /* Object path elements allow [A-Za-z0-9_] only; uids are hex and '-'. */
    s.trackid = g_strdup_printf("/app/kithsong/track/%s", *uid ? uid : "none");
    for (char *p = s.trackid; *p; p++)
      if (*p == '-') *p = '_';
    if (!g_variant_is_object_path(s.trackid)) {
      g_free(s.trackid);
      s.trackid = g_strdup("/app/kithsong/track/current");
    }
    s.title = g_strdup(title);
    s.artist = g_strdup(artist);
    s.album = g_strdup(album);
    char *cover = cover_path(cover_dir);
    s.art_url = cover ? g_filename_to_uri(cover, NULL, NULL) : NULL;
    g_free(cover);
    s.length_us = duration_ms > 0 ? duration_ms * 1000 : 0;
  }
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "Metadata", metadata());
  if (can_seek() != could_seek) g_variant_builder_add(&b, "{sv}", "CanSeek", g_variant_new_boolean(can_seek()));
  emit_changed(&b, TRUE);
}

void mpris_set_state(mc_player_state state, gboolean has_current, gboolean has_next) {
  gboolean could_seek = can_seek();
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  gboolean any = FALSE;
  if (state != s.state) {
    g_variant_builder_add(&b, "{sv}", "PlaybackStatus", g_variant_new_string(status_name(state)));
    any = TRUE;
  }
  if (has_current != s.has_current) {
    const char *names[] = {"CanGoPrevious", "CanPlay", "CanPause"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
      g_variant_builder_add(&b, "{sv}", names[i], g_variant_new_boolean(has_current));
    any = TRUE;
  }
  if (has_next != s.has_next) {
    g_variant_builder_add(&b, "{sv}", "CanGoNext", g_variant_new_boolean(has_next));
    any = TRUE;
  }
  s.state = state;
  s.has_current = has_current;
  s.has_next = has_next;
  if (can_seek() != could_seek) {
    g_variant_builder_add(&b, "{sv}", "CanSeek", g_variant_new_boolean(can_seek()));
    any = TRUE;
  }
  emit_changed(&b, any);
}

void mpris_seeked(int64_t position_ms) {
  if (!s.conn || !s.trackid) return;
  g_dbus_connection_emit_signal(s.conn, NULL, OBJECT_PATH, PLAYER_IFACE, "Seeked",
                                g_variant_new("(x)", (gint64)(position_ms * 1000)), NULL);
}
