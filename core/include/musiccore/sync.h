/* LAN sync between the devices of a group. There is no server: every device keeps change
 * logs stamped with hybrid logical clocks, and any two members exchange what the other
 * hasn't seen. Devices find each other with mDNS (see discovery.h), or by address where
 * multicast is blocked, and join a group by pairing with a 6-digit code. See
 * docs/sync-protocol.md.
 *
 * Threading: an mc_sync is driven from one thread at a time, like every handle, but owns
 * threads of its own once started: a listener, a scheduler, discovery, and one per running
 * session or pairing. Events are reported through the mc_sync_events callbacks, which run on
 * those core threads, never on the caller's, and one at a time (never two at once), so
 * frontends marshal them onto their UI thread. A callback may call the getters and the
 * pairing functions, but not mc_sync_stop() or mc_sync_free(). Strings passed to callbacks
 * are valid only during the call. */
#ifndef MUSICCORE_SYNC_H
#define MUSICCORE_SYNC_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;
typedef struct mc_sync mc_sync;

/* The TCP port sync listens on unless it is taken. */
#define MC_SYNC_DEFAULT_PORT 47823
#define MC_PAIR_CODE_LEN 6

typedef enum mc_device_kind {
  MC_DEVICE_DESKTOP,
  MC_DEVICE_PHONE,
  MC_DEVICE_WATCH
} mc_device_kind;

/* How a pairing attempt ended, on either side. */
typedef enum mc_pair_reason {
  MC_PAIR_PAIRED,           /* both devices are now in one group */
  MC_PAIR_WRONG_CODE,       /* attempts_left says how many tries the code has left */
  MC_PAIR_IN_GROUP,         /* both are in different groups; one must leave first */
  MC_PAIR_LIBRARY_MISMATCH, /* the joining device's library isn't empty or the group's */
  MC_PAIR_ALREADY_PAIRED,   /* both are already in the same group */
  MC_PAIR_BUSY,             /* the other device is pairing with someone else */
  MC_PAIR_CANCELLED,        /* mc_sync_pair_cancel() here, or the other side gave up */
  MC_PAIR_FAILED            /* network error or timeout; see status */
} mc_pair_reason;

typedef struct mc_pair_result {
  const char *device_uid; /* the other device; "" if it was never reached */
  const char *name;       /* its name, "" if unknown */
  int initiator;          /* 1 on the device that asked to pair (and enters the code) */
  mc_pair_reason reason;
  int attempts_left;      /* for MC_PAIR_WRONG_CODE */
  mc_status status;       /* MC_OK when paired, else the error behind the reason */
} mc_pair_result;

typedef enum mc_sync_phase {
  MC_SYNC_PHASE_CONNECTING,
  MC_SYNC_PHASE_GROUP,   /* group data: devices, profiles, status */
  MC_SYNC_PHASE_LIBRARY, /* library metadata */
  MC_SYNC_PHASE_DONE
} mc_sync_phase;

typedef struct mc_sync_progress {
  const char *device_uid;
  mc_sync_phase phase;
  int64_t rows_sent;     /* change rows, for the whole session so far */
  int64_t rows_received;
} mc_sync_progress;

typedef struct mc_sync_result {
  const char *device_uid;
  mc_status status;      /* MC_OK, or why the session failed */
  int library_synced;    /* 0 if only group data was exchanged (see mc_sync_library_state) */
  int converged;         /* both sides ended with the same data */
  int64_t rows_sent;
  int64_t rows_received;
} mc_sync_result;

/* Every member may be NULL. All run on core threads; see the top of this file. */
typedef struct mc_sync_events {
  /* A discovered or group device came online or went offline. member: it's in our group. */
  void (*on_peer)(void *user, const char *device_uid, int online, int member);
  /* This device was asked to pair by requester_name and should show code. Fires again
   * with a new code after MC_PAIR_MAX_FAILURES wrong attempts. */
  void (*on_pair_code)(void *user, const char *code, const char *requester_name,
                       int attempts_left);
  void (*on_pair_result)(void *user, const mc_pair_result *result);
  void (*on_progress)(void *user, const mc_sync_progress *progress);
  void (*on_sync_done)(void *user, const mc_sync_result *result);
} mc_sync_events;

/* Opens this device's sync state in config_dir (the per-device config dir, which holds
 * sync.db next to settings.json) for the library lib. device_name is what other devices
 * show; NULL uses the host name. events (copied) and user receive the callbacks. lib must
 * stay open while the mc_sync exists, but is only read here: sync opens its own
 * connections to the library. Nothing touches the network until mc_sync_start(). */
mc_status mc_sync_new(mc_library *lib, const char *config_dir, const char *device_name,
                      mc_device_kind kind, const mc_sync_events *events, void *user,
                      mc_sync **out);
/* Stops if started, then frees. NULL is a no-op. */
void mc_sync_free(mc_sync *sync);

/* This device's uid, the same on every member of its group. */
const char *mc_sync_device_uid(const mc_sync *sync);

/* Listens on TCP MC_SYNC_DEFAULT_PORT (an ephemeral port if it's taken), starts discovery
 * and starts syncing with group members as they appear, every few minutes, and shortly
 * after local changes. Without multicast discovery fails quietly and members are still
 * reached at their last known addresses. */
mc_status mc_sync_start(mc_sync *sync);
/* Ends sessions and pairings in progress, stops discovery and the threads. Blocks until
 * they've finished. Safe to call when not started. */
void mc_sync_stop(mc_sync *sync);

/* The port this device listens on once started, else 0. */
uint16_t mc_sync_port(const mc_sync *sync);

/* --- Pairing ---
 * This device (A) asks a nearby device (B) to pair. B shows a code through on_pair_code,
 * and A's user types it into mc_sync_pair_enter(). Both sides then get on_pair_result,
 * and on success the first session follows straight away. Requires mc_sync_start(). One
 * outgoing pairing at a time: MC_ERR_LOCKED while another is in progress. */

/* A device from mc_sync_list_nearby(). MC_ERR_NOT_FOUND if discovery hasn't seen it. */
mc_status mc_sync_pair_request(mc_sync *sync, const char *device_uid);
/* A device given by address, for networks that block multicast. host is an IPv4 or IPv6
 * literal or a host name; port 0 means MC_SYNC_DEFAULT_PORT. */
mc_status mc_sync_pair_request_address(mc_sync *sync, const char *host, uint16_t port);
/* The code B shows, six digits. MC_ERR_NOT_FOUND if no outgoing pairing is waiting. After
 * MC_PAIR_WRONG_CODE the pairing waits for another code. */
mc_status mc_sync_pair_enter(mc_sync *sync, const char *code);
/* Gives up the pairing in progress on this device, outgoing or incoming. */
void mc_sync_pair_cancel(mc_sync *sync);

/* --- Group --- */

/* Leaves the group: marks this device removed, tells one reachable member so the group
 * learns of it (blocking for a few seconds at most), then forgets the group's data here.
 * MC_ERR_LOCKED if this device is the group's only archive and others remain;
 * MC_ERR_NOT_FOUND if not in a group. */
mc_status mc_sync_leave_group(mc_sync *sync);
/* Removes another member (or this device, as mc_sync_leave_group()). Members then refuse
 * it, and it forgets the group when it next talks to one. MC_ERR_LOCKED if it is the
 * group's only archive. */
mc_status mc_sync_remove_device(mc_sync *sync, const char *device_uid);
/* Syncs with device_uid, or with every reachable member for NULL, as soon as possible.
 * Returns at once; on_sync_done reports each session. Requires mc_sync_start(). */
mc_status mc_sync_now(mc_sync *sync, const char *device_uid);
/* Tells sync the library or a profile changed here, so members hear of it in a few
 * seconds. Optional: sync also polls for changes. Any thread. */
void mc_sync_notify_changed(mc_sync *sync);

/* Whether the open library is the one the group syncs. */
typedef enum mc_sync_lib_state {
  MC_SYNC_LIBRARY_NO_GROUP, /* not in a group */
  MC_SYNC_LIBRARY_GROUP,    /* the group's library: it syncs */
  MC_SYNC_LIBRARY_OTHER,    /* another library: only group data syncs; offer to switch
                             * back or to leave the group */
  MC_SYNC_LIBRARY_UNKNOWN   /* in a group whose library this device hasn't learnt yet */
} mc_sync_lib_state;

mc_status mc_sync_library_state(mc_sync *sync, mc_sync_lib_state *out);

/* --- Snapshots ---
 * Lists are malloc'd copies, freed with their _free function. */

typedef struct mc_sync_device {
  char uid[MC_UID_STRLEN];
  char *name;
  char *host;            /* its host name; NULL if unknown */
  mc_device_kind kind;
  int is_self;
  int online;            /* seen by discovery, or synced within the last few minutes */
  int archive;           /* keeps the full library as originals */
  int removed;           /* removed from the group; listed so the UI can show it */
  char *address;         /* where it was last seen or reached; NULL if never */
  uint16_t port;
  int64_t last_sync_ms;  /* last session with it (Unix ms), 0 if never */
  /* Its own report of its files (sync_status); has_status = 0 if it hasn't written one. */
  int has_status;
  int64_t verified_ms, expected, ok, missing, corrupt, bytes;
} mc_sync_device;

typedef struct mc_sync_device_list {
  mc_sync_device *items;
  int count;
} mc_sync_device_list;

/* The group's members, this device included, ordered by name. Empty when not in a group. */
mc_status mc_sync_list_devices(mc_sync *sync, mc_sync_device_list **out);
void mc_sync_device_list_free(mc_sync_device_list *list);

typedef struct mc_sync_nearby {
  char uid[MC_UID_STRLEN];
  char *name;
  char *host;            /* NULL if it didn't announce one */
  mc_device_kind kind;
  int in_other_group;    /* must leave its group before it can pair with us */
  char *address;
  uint16_t port;
} mc_sync_nearby;

typedef struct mc_sync_nearby_list {
  mc_sync_nearby *items;
  int count;
} mc_sync_nearby_list;

/* Devices discovery currently sees that aren't in our group, ordered by name. */
mc_status mc_sync_list_nearby(mc_sync *sync, mc_sync_nearby_list **out);
void mc_sync_nearby_list_free(mc_sync_nearby_list *list);

/* TODO(phase 5): sync profile getters and setters (any member's profile, MC_ERR_LOCKED for
 * an archive's), making a device an archive, and the size estimate. */

#ifdef __cplusplus
}
#endif

#endif
