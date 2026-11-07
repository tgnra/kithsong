/* Tethered devices: devices that can't run Kithsong, such as a click-wheel iPod, which a
 * plugin's device target (kithcore/plugin.h) writes to while they're plugged into this
 * computer. Core works out what each should hold, from a sync profile of its own, and does
 * the encoding, verifying and bookkeeping; the plugin only reads and writes the device. See
 * "Device targets" in docs/plugins.md.
 *
 * A tethered device belongs to the computer it's plugged into: its profile is in this
 * computer's sync.db, never replicated, and it works with no sync group. It receives only
 * tracks this computer has an original of; the rest of what its profile wants is counted as
 * missing. Tracks on it that Kithsong didn't write are never changed or removed.
 *
 * Frontends:
 *  - make one mc_tethered after mc_plugins_load(), and free it before mc_plugins_unload();
 *  - tell it when a volume mounts or unmounts (WM_DEVICECHANGE on Windows, GVolumeMonitor on
 *    Linux), and at startup once for every volume already mounted;
 *  - show mc_tethered_list() in the device list, as devices of kind "tethered", with a What
 *    to sync editor over mc_tethered_get_profile() and mc_tethered_set_profile(), and a Sync
 *    button for mc_tethered_sync();
 *  - hand it the scrobbler (mc_tethered_set_scrobbler()) while this device scrobbles, so
 *    plays made on a tethered device are scrobbled too.
 *
 * Threading: an mc_tethered is driven from one thread (the UI's). Each sync runs on a thread
 * of its own, and events come from those threads, one at a time, as sync's do (sync.h):
 * frontends marshal them onto their UI thread. A callback may call the getters and
 * mc_tethered_cancel(), but not mc_tethered_free(). Strings passed to callbacks are valid
 * only during the call. Not on Android, where no plugin is ever loaded. */
#ifndef KITHCORE_TETHERED_H
#define KITHCORE_TETHERED_H

#include "kithcore/kithcore.h"
#include "kithcore/scrobbler.h"
#include "kithcore/sync.h"
#include "kithcore/transcode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_tethered mc_tethered;

typedef enum mc_tethered_phase {
  MC_TETHERED_PHASE_READING,   /* opening the device and reading what it holds */
  MC_TETHERED_PHASE_IMPORTING, /* taking in its plays and playlists */
  MC_TETHERED_PHASE_WRITING,   /* removing, encoding and writing tracks */
  MC_TETHERED_PHASE_FINISHING, /* play counts, playlists and the device's database */
  MC_TETHERED_PHASE_DONE
} mc_tethered_phase;

typedef struct mc_tethered_progress {
  const char *device_uid;
  mc_tethered_phase phase;
  int done;          /* MC_TETHERED_PHASE_WRITING: tracks written or failed so far */
  int total;         /* of this many to write */
  int64_t bytes_done; /* their estimated sizes */
  int64_t bytes_total;
  const char *title; /* the track under way, or NULL */
} mc_tethered_progress;

typedef struct mc_tethered_result {
  const char *device_uid;
  mc_status status;   /* MC_OK; MC_ERR_CANCELLED after mc_tethered_cancel(); MC_ERR_NOT_FOUND
                       * if its plugin is off or gone; MC_ERR_IO if the device went or
                       * failed; else why */
  int over_budget;    /* nothing was written or removed: what the profile wants doesn't fit
                       * its budget (or the device), by shortfall_bytes */
  int64_t shortfall_bytes;
  int full;           /* writing stopped: the device ran out of room */
  int written;        /* tracks put on the device (verified) */
  int removed;        /* Kithsong's tracks the profile no longer wants */
  int updated;        /* tags or play counts rewritten without copying the file */
  int failed;         /* couldn't be encoded or written, or failed verification twice */
  int missing;        /* wanted, but this computer has no original of them */
  int foreign;        /* tracks on it Kithsong didn't write, left as they are */
  int plays_imported; /* plays taken in from the device */
  int plays_unmatched; /* "plays of tracks not in the library": not taken in */
  int plays_not_scrobbled; /* taken in, but over 14 days old, so too old to scrobble */
  int playlists_imported;  /* On-The-Go playlists made into the library's */
} mc_tethered_result;

/* Every member may be NULL. */
typedef struct mc_tethered_events {
  /* What mc_tethered_list() returns changed: a device came, went, or finished a sync. */
  void (*on_devices)(void *user);
  void (*on_progress)(void *user, const mc_tethered_progress *progress);
  /* A sync ended, however it did. */
  void (*on_done)(void *user, const mc_tethered_result *result);
} mc_tethered_events;

/* Tethered devices for library lib, with this computer's sync.db in config_dir (as for
 * mc_sync_new()). lib must stay open while it exists, but is only read here: syncs open
 * connections of their own. events (copied) and user receive the callbacks. */
mc_status mc_tethered_new(mc_library *lib, const char *config_dir, const mc_tethered_events *events,
                          void *user, mc_tethered **out);
/* Cancels syncs under way and waits for them. NULL is a no-op. */
void mc_tethered_free(mc_tethered *t);

/* The scrobbler plays taken in from devices are scrobbled to, or NULL for none (the
 * default). Set it while scrobbling is on for this device (mc_settings_scrobble()); it must
 * stay valid until it's replaced, set to NULL, or the mc_tethered freed. */
void mc_tethered_set_scrobbler(mc_tethered *t, mc_scrobbler *sc);

/* A volume was mounted at mount_path. Asks each device target of the plugins that are on
 * whether it's one of theirs (cheap reads, no writes; call it off the UI thread if the
 * volume may be slow) and, if one says so, opens it to read its id, name and size, and adds
 * it to the list (on_devices). out_uid, unless NULL, receives its uid. MC_ERR_NOT_FOUND if
 * no target knows it, which is the usual answer for a USB stick. A device seen for the
 * first time gets a profile of everything, in the target's default formats. */
mc_status mc_tethered_volume_added(mc_tethered *t, const char *mount_path,
                                   char out_uid[MC_UID_STRLEN]);
/* The volume at mount_path went: its device stays listed, not connected. A sync under way
 * on it is cancelled (it fails anyway). */
mc_status mc_tethered_volume_removed(mc_tethered *t, const char *mount_path);

typedef struct mc_tethered_device {
  char uid[MC_UID_STRLEN];
  char *name;       /* the device's own name */
  char *model;      /* "iPod nano (2nd generation)"; NULL if the plugin doesn't say */
  char *kind;       /* the target's name: "iPod" */
  char *plugin_id;
  char *mount_path; /* NULL while it isn't plugged in */
  int connected;
  int syncing;
  int usable;       /* its plugin is loaded and on: without it, it can't be synced */
  int64_t capacity_bytes; /* as it was last seen */
  int64_t free_bytes;
  int64_t last_sync_ms;   /* 0 if never */
} mc_tethered_device;

typedef struct mc_tethered_device_list {
  mc_tethered_device *items; /* connected first, then by name */
  int count;
} mc_tethered_device_list;

/* Every tethered device this computer knows, plugged in or not. Free with
 * mc_tethered_device_list_free(). */
mc_status mc_tethered_list(mc_tethered *t, mc_tethered_device_list **out);
void mc_tethered_device_list_free(mc_tethered_device_list *list);

/* --- Profiles ---
 * A tethered device's profile is an mc_sync_profile, edited in the same What to sync editor
 * as a Kithsong device's: everything or a selection, and a budget. Its format isn't Opus or
 * originals but its format policy (below), so mc_sync_profile.format reads as
 * MC_FORMAT_ORIGINAL (MC_FORMAT_AUTO with a budget) and is ignored when set; name, archive
 * and removed are unused. With a budget, what the profile wants must fit in the budget (0:
 * the device's free space, plus what Kithsong's tracks on it take up already), or the sync
 * writes nothing and says by how much it's over. */

/* The format policy (see "Format policy" in docs/plugins.md): encoder format and setting
 * ids, as the target's formats and the loaded encoders offer them (mc_tethered_choices()). */
typedef struct mc_tethered_policy {
  /* What lossless originals are written as: "alac" (setting ""), or a lossy format such as
   * "mp3" at a setting, to save space. */
  char lossless_format[MC_ENCODER_ID_LEN];
  char lossless_setting[MC_ENCODER_ID_LEN];
  /* What lossy originals in a format the device doesn't play are re-encoded to, and those it
   * plays when over lossy_cap_kbps. */
  char lossy_format[MC_ENCODER_ID_LEN];
  char lossy_setting[MC_ENCODER_ID_LEN];
  int lossy_cap_kbps; /* 0: lossy files the device plays are copied as they are */
} mc_tethered_policy;

/* device_uid's profile (free with mc_sync_profile_free()) and, unless policy is NULL, its
 * format policy. MC_ERR_NOT_FOUND for a uid that isn't a tethered device. */
mc_status mc_tethered_get_profile(mc_tethered *t, const char *device_uid, mc_sync_profile **out,
                                  mc_tethered_policy *policy);
/* Stores device_uid's profile (and policy, unless NULL), as given: what's stored is replaced.
 * MC_ERR_INVALID_ARG for an unknown item kind, or a policy naming an id that isn't
 * [a-z0-9]+; a format no loaded encoder has is stored, and the sync then fails those tracks.
 * It takes effect at the next sync. */
mc_status mc_tethered_set_profile(mc_tethered *t, const char *device_uid,
                                  const mc_sync_profile *profile, const mc_tethered_policy *policy);

/* A format policy choice, for the editor's menus. */
typedef struct mc_tethered_choice {
  char format[MC_ENCODER_ID_LEN];
  char setting[MC_ENCODER_ID_LEN];
  char *label;  /* "ALAC", "MP3 · V0 (about 245 kbps)" */
  int lossless;
  int kbps;     /* for size estimates; 0 for lossless */
} mc_tethered_choice;

typedef struct mc_tethered_choice_list {
  mc_tethered_choice *items;
  int count;
} mc_tethered_choice_list;

/* The encodes the plugins that are on offer for device_uid: each setting of each encoder
 * format the device plays (as it was last seen). Free with mc_tethered_choice_list_free(). */
mc_status mc_tethered_choices(mc_tethered *t, const char *device_uid, mc_tethered_choice_list **out);
void mc_tethered_choice_list_free(mc_tethered_choice_list *list);

/* --- Syncing --- */

/* Syncs device_uid now, on a thread of its own: on_progress, then on_done. MC_ERR_NOT_FOUND
 * unless it's plugged in, MC_ERR_IN_USE if it's syncing already, MC_ERR_UNIMPLEMENTED if its
 * plugin isn't loaded and on. */
mc_status mc_tethered_sync(mc_tethered *t, const char *device_uid);
/* Stops device_uid's sync: what's written so far is committed to the device, and on_done
 * reports MC_ERR_CANCELLED. A no-op if it isn't syncing. */
mc_status mc_tethered_cancel(mc_tethered *t, const char *device_uid);
/* Gets device_uid ready to unplug, for the device's own needs (the OS's eject is the
 * frontend's). MC_ERR_IN_USE while it's syncing. */
mc_status mc_tethered_eject(mc_tethered *t, const char *device_uid);
/* Forgets a device that isn't plugged in: its profile and imports. MC_ERR_IN_USE if it is. */
mc_status mc_tethered_forget(mc_tethered *t, const char *device_uid);

#ifdef __cplusplus
}
#endif

#endif
