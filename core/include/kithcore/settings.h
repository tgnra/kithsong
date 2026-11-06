/* Per-device preferences: where the library folder is, the device's name and a frontend's
 * own yes/no answers. Stored as settings.json in a folder the frontend picks
 * (%LOCALAPPDATA%\kithsong on Windows, ~/.config/kithsong on Linux). Never synced: each
 * device has its own library folder. */
#ifndef KITHCORE_SETTINGS_H
#define KITHCORE_SETTINGS_H

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_settings mc_settings;

/* Loads dir/settings.json. A missing or unreadable file gives empty settings; nothing is
 * written until mc_settings_save(). */
mc_status mc_settings_load(const char *dir, mc_settings **out);
void mc_settings_free(mc_settings *s);

/* Moves the folder old_dir, where an earlier version kept its settings, to dir, if dir
 * doesn't exist yet. Call before mc_settings_load(dir). If the move fails, dir starts out
 * empty. */
void mc_settings_move_old_dir(const char *old_dir, const char *dir);

/* Resetting the app on this device (Settings' Reset Kithsong…): clearing everything in dir,
 * the per-device folder, which holds settings.json, sync.db (this device's pairing), the
 * trash's deleted.db and the playlists' pictures. The library folder isn't touched.
 *
 * mc_settings_request_reset() marks dir to be cleared: the frontend then quits, and the next
 * start clears it with mc_settings_reset_if_requested() before anything opens it, so nothing
 * in it is open while it goes. */
mc_status mc_settings_request_reset(const char *dir);
/* Clears dir if mc_settings_request_reset() marked it; call before mc_settings_load(dir)
 * and before anything else opens a file in it. Sets *reset to whether it was marked. dir is
 * first renamed aside (to dir.old) and made again empty, then the old one is deleted, so the
 * app starts afresh even if some of it can't be deleted. On failure *left_behind (mc_free
 * it; may be NULL) is the folder that still holds old files: dir itself if it couldn't be
 * renamed (it stays marked, for the next start to try again), else dir.old. A dir.old left
 * by an earlier reset is deleted here too, quietly. */
mc_status mc_settings_reset_if_requested(const char *dir, int *reset, char **left_behind);

/* Writes the settings back, creating dir if needed. Keys this version doesn't know about
 * are kept. */
mc_status mc_settings_save(const mc_settings *s);

/* The library folder, or NULL if none has been chosen yet (first run). */
const char *mc_settings_library_dir(const mc_settings *s);
mc_status mc_settings_set_library_dir(mc_settings *s, const char *dir);

/* The name this device shows to others (see mc_sync_new()), or NULL if none is saved (the
 * host name is used). */
const char *mc_settings_device_name(const mc_settings *s);
/* NULL or "" clears it. */
mc_status mc_settings_set_device_name(mc_settings *s, const char *name);

/* A frontend's own yes/no preference under key (e.g. Windows' "firewall_dont_ask"), or def
 * if none is saved or it isn't a boolean. */
int mc_settings_bool(const mc_settings *s, const char *key, int def);
mc_status mc_settings_set_bool(mc_settings *s, const char *key, int value);

/* A frontend's own text under key, or NULL if none is saved. Valid until the settings next
 * change. NULL or "" clears it. */
const char *mc_settings_string(const mc_settings *s, const char *key);
mc_status mc_settings_set_string(mc_settings *s, const char *key, const char *value);

/* Whether playback shuffles (mc_player_set_shuffle()), as it was left. Off if never set. */
int mc_settings_shuffle(const mc_settings *s);
mc_status mc_settings_set_shuffle(mc_settings *s, int on);

/* Whether playing a track from an album's track list turns shuffle off, so the album plays
 * in order from it. Only an album's: not a playlist's, an artist's, search results or the
 * recents. On if never set. */
int mc_settings_album_in_order(const mc_settings *s);
mc_status mc_settings_set_album_in_order(mc_settings *s, int on);

/* Whether an album or anything else added to the queue as a whole goes in as one entry per track
 * (mc_up_next_add.title NULL) rather than as one block. Only what's added from then on. Off if
 * never set. */
int mc_settings_queue_tracks(const mc_settings *s);
mc_status mc_settings_set_queue_tracks(mc_settings *s, int on);

/* The playback volume (mc_player_set_volume()), 0 to 100, as it was left. 100 if never set. */
int mc_settings_volume(const mc_settings *s);
mc_status mc_settings_set_volume(mc_settings *s, int volume);

/* Whether this device scrobbles its plays while the group is signed in to a scrobbling
 * service (see scrobbler.h). The account is the group's; this switch is each device's own.
 * On if never set. */
int mc_settings_scrobble(const mc_settings *s);
mc_status mc_settings_set_scrobble(mc_settings *s, int on);

#ifdef __cplusplus
}
#endif

#endif
