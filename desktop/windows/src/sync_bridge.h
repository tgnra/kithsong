/* Sync for the app: owns this device's mc_sync and carries its events onto the UI thread.
 * Core runs the sync callbacks on its own threads; each is copied into a flat malloc'd
 * sync_event (strings copied, names converted to UTF-16) and posted to the main window as
 * WM_APP_SYNC. Progress is throttled to one event every 150 ms per session, except a change
 * of phase and the one that finishes the session's files, which always go through.
 *
 * Stopping bumps a generation counter carried in wParam, so events still queued from the
 * old mc_sync are dropped by sync_bridge_receive(). Every function runs on the UI thread. */
#ifndef KITHSONG_SYNC_BRIDGE_H
#define KITHSONG_SYNC_BRIDGE_H

#include <stdint.h>
#include <windows.h>

#include "musiccore/musiccore.h"

/* Posted to the main window. wParam: the generation; lParam: a sync_event * (malloc'd).
 * Pass both to sync_bridge_receive(). */
#define WM_APP_SYNC (WM_APP + 6)

typedef enum {
  SYNC_EV_PEER,        /* on_peer: uid, online, member */
  SYNC_EV_PAIR_CODE,   /* on_pair_code: code, name (the requester), attempts_left */
  SYNC_EV_PAIR_RESULT, /* on_pair_result: uid, name, initiator, reason, attempts_left, status,
                        * joined, archive_name, archives */
  SYNC_EV_PROGRESS,    /* on_progress: uid, phase, rows_*, files_done, files_total, bytes_*,
                        * library_changed */
  SYNC_EV_DONE,        /* on_sync_done: uid, status, library_synced, converged, rows_*,
                        * files_received, files_sent, bytes_*, cancelled */
  SYNC_EV_PAIR_MERGE,  /* on_pair_merge: uid, name, initiator, kept_here, needs_confirm,
                        * tracks_*, scrobble_sign_out, scrobble_user, forget_* */
  SYNC_EV_MERGE_PROGRESS, /* on_merge_progress: merge_phase, merge_done, merge_total */
  SYNC_EV_MERGE_DONE,  /* on_merge_done: status, merge (counts), backup_path, scrobble_user */
} sync_event_kind;

/* One core event. No pointers inside: free() it whole. Fields that don't apply to the kind
 * are zero. */
typedef struct sync_event {
  sync_event_kind kind;
  char uid[MC_UID_STRLEN]; /* the other device; "" if unknown */
  wchar_t name[128];
  int online, member;
  char code[MC_PAIR_CODE_LEN + 1];
  int attempts_left;
  int initiator;
  mc_pair_reason reason;
  mc_status status;
  mc_sync_phase phase;
  int64_t rows_sent, rows_received;
  int files_done, files_total;   /* the files this device is fetching in the session */
  int files_received, files_sent;
  int64_t bytes_received, bytes_sent;
  int library_synced, converged;
  int cancelled; /* mc_sync_cancel() here cut the session short */
  /* Progress: library rows or a file arrived since the last event posted for the device,
   * so the library views should reload. */
  int library_changed;
  /* Pair result: this PC joined, and the group's archive (the first of archives). */
  int joined, archives;
  wchar_t archive_name[128];
  /* Pair result: they paired by merging libraries, and this PC's was the one kept. Pair
   * merge: kept_here as there, and needs_confirm (= !kept_here). */
  int merged, kept_here, needs_confirm;
  /* Pair merge: each library's track count; Last.fm is signed out on the merged side
   * (scrobble_sign_out), and the paired devices this PC forgets (needs_confirm only). */
  int64_t tracks_here, tracks_there;
  int scrobble_sign_out;
  int forget_count;
  char forget_names[512]; /* UTF-8, joined with import_list_separator, cut short if long */
  /* UTF-8. Pair merge: the Last.fm user signed out here, if this PC confirms. Pair result and
   * merge done: the one signed out (on either device for the result); "" if none. */
  char scrobble_user[128];
  /* Merge progress. */
  mc_merge_phase merge_phase;
  int merge_done, merge_total;
  /* Merge done: the counts (its two string pointers are NULL; see the fields below), and the
   * set-aside library's path (UTF-8). */
  mc_merge_result merge;
  char backup_path[1024];
} sync_event;

/* Creates and starts sync for lib, with sync.db in config_dir, reporting to wnd. device_name
 * (UTF-8) is what other devices show; NULL uses the host name. On failure sync stays off
 * and sync_bridge_error() says why. Stops any sync already running first. */
void sync_bridge_start(HWND wnd, mc_library *lib, const char *config_dir, const char *device_name);
/* Stops and frees sync, blocking until its threads end, and drops events still queued.
 * Safe to call when not started. Call before closing the library. */
void sync_bridge_stop(void);
/* The running mc_sync, or NULL if sync is off. */
mc_sync *sync_bridge_get(void);
/* Why the last sync_bridge_start() failed; MC_OK while running or never started. */
mc_status sync_bridge_error(void);
/* Takes ownership of a WM_APP_SYNC's event: returns it (free() it when done), or frees it
 * and returns NULL if it's from an older generation. */
sync_event *sync_bridge_receive(WPARAM wp, LPARAM lp);

#endif
