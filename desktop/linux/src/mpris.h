/* MPRIS (org.mpris.MediaPlayer2 on the session bus, as org.mpris.MediaPlayer2.kithsong):
 * the desktop's media controls, in GNOME's top bar and lock screen, KDE's applet,
 * playerctl, and Bluetooth headsets. It's also how media keys reach the app, whichever
 * window has the focus. The Linux side of what media_controls.c does on Windows.
 *
 * Shows the playing track (title, artist, album, length, cover) and its position, and
 * passes the controls on to playback.c. Call everything on the UI thread.
 */
#ifndef KITHSONG_MPRIS_H
#define KITHSONG_MPRIS_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

/* Takes the bus name. Raise presents win; Quit quits gapp. */
void mpris_init(GtkApplication *gapp, GtkWindow *win);
void mpris_free(void);

/* A NULL uid shows nothing playing. cover_dir is the album folder, or NULL. */
void mpris_set_track(const char *uid, const char *title, const char *artist, const char *album,
                     const char *cover_dir, int64_t duration_ms);
void mpris_set_state(mc_player_state state, gboolean has_current, gboolean has_next);
/* After the position jumped: a seek, from here or anywhere else. */
void mpris_seeked(int64_t position_ms);

#endif
