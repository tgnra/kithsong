/* Sync for the GTK app (the counterpart of desktop/windows/src/sync_bridge.c): owns this
 * device's mc_sync and carries its events onto the main thread. Core runs the sync callbacks
 * on its own threads; each is copied into a flat sync_event (strings copied) and handed to
 * the main loop with g_idle_add(). Progress is throttled to one event every 150 ms per
 * device, except a change of phase and the one that finishes the session's files, which
 * always go through.
 *
 * Stopping bumps a generation counter carried with each event, so events still queued from
 * the old mc_sync are dropped rather than delivered. Every function runs on the main thread,
 * and so does the event callback. */
#ifndef KITHSONG_SYNC_BRIDGE_H
#define KITHSONG_SYNC_BRIDGE_H

#include <glib.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

typedef enum {
  SYNC_EV_PEER,        /* on_peer: uid, online, member */
  SYNC_EV_PAIR_CODE,   /* on_pair_code: code, name (the requester), attempts_left */
  SYNC_EV_PAIR_RESULT, /* on_pair_result: uid, name, initiator, reason, attempts_left, status */
  SYNC_EV_PROGRESS,    /* on_progress: uid, phase, rows_*, files_done, files_total, bytes_*,
                        * library_changed */
  SYNC_EV_DONE,        /* on_sync_done: uid, status, library_synced, converged, rows_*,
                        * files_received, files_sent, bytes_* */
  SYNC_EV_REACH,       /* on_reach: uid ("" when much changed at once) */
} sync_event_kind;

/* One core event. Fields that don't apply to the kind are zero. */
typedef struct sync_event {
  sync_event_kind kind;
  char uid[MC_UID_STRLEN]; /* the other device; "" if unknown */
  char name[MC_SYNC_DEVICE_NAME_MAX + 1]; /* UTF-8, cut at a character boundary */
  int online, member;
  char code[MC_PAIR_CODE_LEN + 1];
  int attempts_left;
  int initiator;
  mc_pair_reason reason;
  mc_status status;
  mc_sync_phase phase;
  int64_t rows_sent, rows_received;
  int files_done, files_total; /* the files this device is fetching in the session */
  int files_received, files_sent;
  int64_t bytes_received, bytes_sent;
  int library_synced, converged;
  /* Progress: library rows or a file arrived since the last event delivered for the
   * device, so the library views should reload. */
  int library_changed;
} sync_event;

/* Receives each event on the main thread. ev is valid during the call only. */
typedef void (*sync_bridge_event_fn)(void *user, const sync_event *ev);

/* Creates and starts sync for lib, with sync.db in config_dir, delivering events to on_event.
 * device_name (UTF-8) is what other devices show; NULL uses the host name. On failure sync
 * stays off and sync_bridge_error() says why. Stops any sync already running first. */
void sync_bridge_start(mc_library *lib, const char *config_dir, const char *device_name,
                       sync_bridge_event_fn on_event, void *user);
/* Stops and frees sync, blocking until its threads end, and drops events still queued.
 * Safe to call when not started. Call before closing the library. */
void sync_bridge_stop(void);
/* The running mc_sync, or NULL if sync is off. */
mc_sync *sync_bridge_get(void);
/* Why the last sync_bridge_start() failed; MC_OK while running or never started. */
mc_status sync_bridge_error(void);

#endif
