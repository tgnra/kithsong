/* Tethered devices for the GTK app (kithcore/tethered.h; the counterpart of
 * desktop/windows/src/tethered_bridge.c): owns the mc_tethered for the open library, tells it
 * about volumes, and carries its events onto the main thread, as sync_bridge.c does for sync.
 * Core runs the callbacks on sync threads; each is copied into a flat tethered_event and handed
 * to the main loop with g_idle_add(). Progress is throttled to one event every 150 ms, except a
 * change of phase and the last track's, which always go through.
 *
 * Volumes: a GVolumeMonitor's mount-added and mount-removed, and every mount there is at start
 * and when the plugins that are on change (tethered_bridge_scan()). Only local mounts are
 * announced: one without a path, or under gvfs (a phone over MTP, a network share), would be
 * slow or pointless to probe. For testing without a device, KITHSONG_TEST_VOLUMES lists folders
 * (separated by :) that are announced as volumes too.
 *
 * Stopping bumps a generation counter carried with each event, so events still queued from the
 * old mc_tethered are dropped rather than delivered. Every function runs on the main thread,
 * and so does the event callback. */
#ifndef KITHSONG_TETHERED_BRIDGE_H
#define KITHSONG_TETHERED_BRIDGE_H

#include <glib.h>

#include "kithcore/tethered.h"

typedef enum {
  TETHERED_EV_DEVICES,  /* on_devices: read mc_tethered_list() again */
  TETHERED_EV_PROGRESS, /* on_progress: uid, phase, done, total, bytes_*, title */
  TETHERED_EV_DONE,     /* on_done: uid, result */
} tethered_event_kind;

/* One core event. Fields that don't apply to the kind are zero. */
typedef struct tethered_event {
  tethered_event_kind kind;
  char uid[MC_UID_STRLEN];
  mc_tethered_phase phase;
  int done, total;
  int64_t bytes_done, bytes_total;
  char title[256];           /* UTF-8, cut at a character boundary; "" if none */
  mc_tethered_result result; /* its device_uid is NULL: see uid */
} tethered_event;

/* Receives each event on the main thread. ev is valid during the call only. */
typedef void (*tethered_bridge_event_fn)(void *user, const tethered_event *ev);

/* Makes the mc_tethered for lib (sync.db in config_dir), delivering events to on_event, and
 * announces the volumes mounted now. Stops any running first. After mc_plugins_load(). */
void tethered_bridge_start(mc_library *lib, const char *config_dir, tethered_bridge_event_fn on_event, void *user);
/* Frees it, cancelling syncs under way and waiting for them, and drops events still queued.
 * Safe to call when not started. Before the library closes and before mc_plugins_unload(). */
void tethered_bridge_stop(void);
/* The mc_tethered, or NULL. */
mc_tethered *tethered_bridge_get(void);
/* The scrobbler plays from devices go to: NULL while scrobbling is off. */
void tethered_bridge_set_scrobbler(mc_scrobbler *sc);
/* Announces every volume mounted now (again): after a plugin is turned on, say. */
void tethered_bridge_scan(void);
/* Whether uid is a tethered device's, as of now. */
gboolean tethered_bridge_has(const char *uid);

/* Ejects the volume at mount_path (or unmounts it, if it can't be ejected), then calls
 * done(user, status, message) on the main thread: message is the system's reason, or NULL.
 * A folder that isn't a mount (a test folder) needs nothing, and done is called at once. */
void tethered_bridge_eject_volume(const char *mount_path, void (*done)(void *user, mc_status st, const char *message),
                                  void *user);

#endif
