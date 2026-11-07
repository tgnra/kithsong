/* Tethered devices for the app (kithcore/tethered.h): owns the mc_tethered for the open
 * library, tells it about volumes, and carries its events onto the UI thread, as sync_bridge.c
 * does for sync. Core runs the callbacks on sync threads; each is copied into a flat malloc'd
 * tethered_event and posted to the main window as WM_APP_TETHERED. Progress is throttled to
 * one event every 150 ms, except a change of phase, which always goes through.
 *
 * Volumes: the main window passes WM_DEVICECHANGE on (volume arrivals and removals, by drive
 * letter), and tethered_bridge_scan() announces every drive mounted now, at start and when the
 * plugins that are on change. Network and optical drives are left out, so probing never waits
 * on them. For testing without a device, KITHSONG_TEST_VOLUMES lists folders (separated by ;)
 * that are announced as volumes too.
 *
 * Stopping bumps a generation counter carried in wParam, so events still queued from the old
 * mc_tethered are dropped by tethered_bridge_receive(). Every function runs on the UI thread. */
#ifndef KITHSONG_TETHERED_BRIDGE_H
#define KITHSONG_TETHERED_BRIDGE_H

#include <windows.h>

#include "kithcore/tethered.h"

/* Posted to the main window. wParam: the generation; lParam: a tethered_event * (malloc'd).
 * Pass both to tethered_bridge_receive(). */
#define WM_APP_TETHERED (WM_APP + 12)

typedef enum {
  TETHERED_EV_DEVICES,  /* on_devices: read mc_tethered_list() again */
  TETHERED_EV_PROGRESS, /* on_progress: uid, phase, done, total, bytes_*, title */
  TETHERED_EV_DONE,     /* on_done: uid, result */
} tethered_event_kind;

/* One core event. No pointers inside: free() it whole. */
typedef struct tethered_event {
  tethered_event_kind kind;
  char uid[MC_UID_STRLEN];
  mc_tethered_phase phase;
  int done, total;
  int64_t bytes_done, bytes_total;
  char title[256];           /* UTF-8, cut short if long; "" if none */
  mc_tethered_result result; /* its device_uid is NULL: see uid */
} tethered_event;

/* Makes the mc_tethered for lib (sync.db in config_dir), reporting to wnd, and announces the
 * volumes mounted now. Stops any running first. After mc_plugins_load(). */
void tethered_bridge_start(HWND wnd, mc_library *lib, const char *config_dir);
/* Frees it, cancelling syncs under way and waiting for them, and drops events still queued.
 * Safe to call when not started. Before the library closes and before mc_plugins_unload(). */
void tethered_bridge_stop(void);
/* The mc_tethered, or NULL. */
mc_tethered *tethered_bridge_get(void);
/* The scrobbler plays from devices go to: NULL while scrobbling is off. */
void tethered_bridge_set_scrobbler(mc_scrobbler *sc);
/* Announces every volume mounted now (again): after a plugin is turned on, say. */
void tethered_bridge_scan(void);
/* For the main window's WM_DEVICECHANGE. */
void tethered_bridge_device_change(WPARAM wp, LPARAM lp);
/* Whether uid is a tethered device's, as of the last mc_tethered_list(). */
BOOL tethered_bridge_has(const char *uid);
/* Takes ownership of a WM_APP_TETHERED's event: returns it (free() it when done), or frees it
 * and returns NULL if it's from an older generation. */
tethered_event *tethered_bridge_receive(WPARAM wp, LPARAM lp);

/* Ejects the volume at mount_path (a drive root such as "E:\"): flushes, dismounts and asks the
 * device to eject. Anything else (a test folder) needs nothing. */
mc_status tethered_bridge_eject_volume(const char *mount_path);

#endif
