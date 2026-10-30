/* What sync.c, session.c and pairing.c share: the mc_sync object, its peer table, and the
 * hooks tests use to run without discovery. Internal to core.
 *
 * Threads: the caller's (the API), the listener, the scheduler, discovery's, and one worker
 * per session or pairing. Each thread that touches the databases opens its own connections
 * (mc_sync_conn), since a connection is used by one thread at a time. Everything in
 * mc_sync under "lock" is guarded by it; callbacks run under cb_lock, never under lock. */
#ifndef MC_SYNC_INTERNAL_H
#define MC_SYNC_INTERNAL_H

#include <stdint.h>

#include "musiccore/discovery.h"
#include "musiccore/library.h"
#include "musiccore/sync.h"
#include "sync/channel.h"
#include "sync/pair.h"
#include "sync/sync_db.h"
#include "util/platform.h"

#define MC_SYNC_NAME_MAX 128
#define MC_SYNC_ADDR_MAX 64
#define MC_SYNC_HINTS_MAX 1024

/* Why a member is due for a session. */
typedef enum mc_sync_trigger {
  MC_TRIGGER_NONE,
  MC_TRIGGER_PERIODIC, /* every few minutes, and at start */
  MC_TRIGGER_ONLINE,   /* discovery just saw it */
  MC_TRIGGER_CHANGED,  /* something changed here that it should hear about */
  MC_TRIGGER_NOW,      /* mc_sync_now(): the only one MC_SYNC_AUTO_INTERVAL_MS doesn't hold back */
  MC_TRIGGER_AUTO      /* mc_sync_background() */
} mc_sync_trigger;

/* What this device knows of another one's whereabouts, and for members, the schedule. */
typedef struct mc_sync_peer {
  char uid[MC_UID_STRLEN];
  char name[MC_SYNC_NAME_MAX];
  char host[MC_SYNC_NAME_MAX];
  char kind[16];
  char grp[MC_GROUP_HASH_STRLEN];
  char address[MC_SYNC_ADDR_MAX]; /* from discovery, else where we last reached it */
  uint16_t port;
  int discovered; /* discovery says it's online */
  int reached;    /* the last session with it succeeded (online without discovery) */
  int member;     /* a non-removed member of our group, as of the scheduler's last look */
  uint64_t seen_ms; /* mono: when it came online, or a session with it last succeeded */
  /* Scheduling. */
  uint64_t due_ms;     /* mono; 0 = not scheduled yet */
  uint64_t trigger_ms; /* mono: when the current trigger was set */
  mc_sync_trigger trigger;
  uint64_t contact_ms; /* mono: the last session with it, in either direction */
  uint32_t backoff_ms;
  /* mono: no automatic session with it starts before this (MC_SYNC_AUTO_INTERVAL_MS after
   * the last complete session either way); 0 = none held back. */
  uint64_t auto_after_ms;
  /* The session running with it, if any. */
  int busy;
  int busy_lower; /* that session was initiated by the lower device uid */
  int abort;      /* a session the lower uid started wants this one gone */
  int cancel;     /* mc_sync_cancel() cut it short */
  mc_channel *busy_ch;
  /* mono: mc_sync_cancel() holds sessions with it back until this; 0 = none. */
  uint64_t held_ms;
  /* The file phase: a signature of the transcodes last asked for on its behalf while it
   * was offline (PREPARE_REQ), so they're asked for once per change. */
  uint64_t prepared_sig;
  /* mono: this device's user pressed Sync for it (mc_sync_confirm_now()), which the next
   * session with it carries; 0 = not, or already carried. */
  uint64_t user_ms;
  /* Reachability: mono times of the latest evidence each way; 0 = none. out: we connected
   * to it, or failed to while it was online. in: it connected to us, or a reach check says
   * it couldn't. */
  uint64_t out_ok_ms, out_fail_ms, in_ok_ms, in_fail_ms;
  char in_addr[MC_SYNC_ADDR_MAX]; /* where its last session with us came from */
  uint64_t reach_ms; /* mono: when the last reach check with it started; 0 = never */
  int reaching;      /* a reach check with it is running */
  int reach_now;     /* mc_sync_check_reach() asked for one */
} mc_sync_peer;

typedef struct mc_sync_worker {
  mc_thread thread;
  int done;
  struct mc_sync_worker *next;
} mc_sync_worker;

/* One thread's connections to the two databases. */
typedef struct mc_sync_conn {
  mc_sync_db *sdb;
  mc_library *lib;
} mc_sync_conn;

struct mc_sync {
  /* Fixed at mc_sync_new(), but for name. */
  char *config_dir;
  char *lib_dir;
  char device_uid[MC_UID_STRLEN];
  char name[MC_SYNC_NAME_MAX]; /* under lock: it changes when the device is renamed */
  char host[MC_SYNC_NAME_MAX];
  mc_device_kind kind;
  mc_sync_events ev;
  void *user;

  mc_mutex cb_lock; /* one callback at a time */
  mc_mutex transcode_lock; /* one transcode at a time: mc_transcode_cached() needs it per file */
  mc_mutex api_lock;
  mc_sync_conn api; /* the API's connections, under api_lock */

  mc_mutex lock;
  mc_cond cond; /* the scheduler waits here; also session and pairing hand-offs */
  /* Under lock. */
  int started, stopping;
  mc_listener *listener;
  uint16_t port;
  mc_discovery *disc;
  mc_thread listen_thread, sched_thread;
  mc_sync_peer *peers;
  int npeers;
  mc_sync_worker *workers;
  struct mc_sync_tracked {
    mc_channel *ch;
    int role; /* 0 session, 1 outgoing pairing, 2 incoming pairing */
  } *chans;   /* open channels, aborted by stop */
  int nchans, chans_cap;
  int absorb; /* sessions applied rows: the changelog poll shouldn't take them for edits */
  char group_hash[MC_GROUP_HASH_STRLEN];
  uint64_t changed_ms; /* mono: a local change to tell members about; 0 = none */
  int files_busy; /* a session is fetching files or reconciling, or a verify runs */
  /* This device's sync_status as a file phase last found it, waiting to be written before
   * the next session's group exchange (a write after it would leave the pair unconverged). */
  int status_pending;
  struct mc_sync_status_row {
    int64_t verified_ms, expected, ok, missing, corrupt, bytes;
  } status;
  char hints[MC_SYNC_HINTS_MAX]; /* our address hints, JSON */
  /* Outgoing pairing: this device is A. */
  int out_active;
  int out_has_code;
  int out_cancel;
  char out_code[MC_PAIR_CODE_DIGITS + 1];
  /* Incoming pairing: this device is B, showing a code. */
  int in_active;  /* a requester has a code */
  int in_conn;    /* one of its connections is being served */
  int in_cancel;
  char in_requester[MC_UID_STRLEN];
  char in_requester_name[MC_SYNC_NAME_MAX];
  uint64_t in_expires_ms;
  mc_pair_responder in_r;
  /* Merging libraries (pairing.c): whether each pairing waits for the user's answer
   * (mc_sync_pair_merge_confirm()), and the answer: 1 merge, -1 declined, 0 none yet. */
  int out_merge_wait, out_merge_answer;
  int in_merge_wait, in_merge_answer;
  int no_merge; /* test hook: offer no merging, as an older version */
  int no_files_plan; /* test hook: no FILES_PLAN either way, as an older version */
  /* The merge that runs after a session syncs the library (sync.c), with what the pairing
   * learnt: the kept device's originals (sorted; owned) and the account signed out. */
  int merge_running;
  struct mc_kept_file *merge_kept;
  int merge_nkept;
  char merge_signed_out[MC_SYNC_NAME_MAX];
  int merge_sent, merge_dropped;

  /* Options, set before mc_sync_start(); see the test hooks below. */
  int use_discovery;
  int manual_only; /* sessions only on mc_sync_now(), for tests; under lock */
  int listen_port; /* -1: the default with fallback */
  uint32_t auto_interval_ms; /* MC_SYNC_AUTO_INTERVAL_MS, but in tests; under lock */
  char hint_override[MC_SYNC_HINTS_MAX];
  uint16_t reach_port_override; /* under lock */
  uint32_t send_delay_ms; /* under lock */
};

/* --- Shared helpers (sync.c) --- */

mc_status mc_sync_conn_open(mc_sync *sync, mc_sync_conn *c);
void mc_sync_conn_close(mc_sync_conn *c);

const char *mc_sync_kind_str(mc_device_kind kind);
mc_device_kind mc_sync_kind_parse(const char *s);

/* Starts fn(arg) on a worker thread, joined by stop. */
mc_status mc_sync_spawn(mc_sync *sync, void (*fn)(void *), void *arg);
/* Channels in this registry are aborted by mc_sync_stop(), and pairing ones also by
 * mc_sync_pair_cancel(). incoming: this device is B. Tracking a tracked channel changes its
 * role; untracking an untracked one is a no-op. */
void mc_sync_track(mc_sync *sync, mc_channel *ch);
void mc_sync_track_pairing(mc_sync *sync, mc_channel *ch, int incoming);
void mc_sync_untrack(mc_sync *sync, mc_channel *ch);

/* At most one session per peer. lower: the session was initiated by the lower device uid.
 * A session the lower uid initiated displaces one the higher did (which is aborted);
 * otherwise the newcomer loses, as does any while mc_sync_cancel() holds the peer back.
 * 1 if claimed. */
int mc_sync_claim(mc_sync *sync, const char *peer_uid, int lower, mc_channel *ch);
/* 1 if mc_sync_cancel() cut the claimed session short. */
int mc_sync_release(mc_sync *sync, const char *peer_uid);

/* After this device joined or left a group: the announced group hash and the members. */
void mc_sync_group_changed(mc_sync *sync, mc_sync_conn *c);
/* This device was removed: forget the group. */
void mc_sync_forget_group(mc_sync *sync, mc_sync_conn *c);
/* A session ended. rows_new: rows that were new here, which other members may lack too.
 * follow_up: it wrote group rows the peer hasn't seen yet, so sync with it again soon.
 * address and port, if given, are where we reached the peer. */
void mc_sync_session_ended(mc_sync *sync, mc_sync_conn *c, const char *peer_uid,
                           const mc_sync_result *result, int64_t rows_new, int follow_up,
                           const char *address, uint16_t port);

/* Sets this device's profiles.name through c in a write transaction, adding the profile
 * row if there's none. only_if_unset: keep a name that's there. *wrote (may be NULL): it
 * changed something. */
mc_status mc_sync_write_profile_name(mc_sync_conn *c, const char *name, int only_if_unset,
                                     int *wrote);
/* Takes on the name the group gives this device (its profile name) if it has one and it
 * differs: renames, re-announces and writes its devices row. c is the caller's connection
 * (the API's under api_lock); not under lock. Done at start, after each session and after a
 * rename here; call it after setting this device's profile too. */
void mc_sync_take_group_name(mc_sync *sync, mc_sync_conn *c);
/* After joining or making a group: gives this device's profile its current name unless the
 * group already has one for it. 1 if it wrote. */
int mc_sync_seed_profile_name(mc_sync *sync, mc_sync_conn *c);

/* Whether this device's user pressed Sync for peer_uid a short while ago and no session has
 * carried it yet; clears it. For a session that has just claimed the peer. */
int mc_sync_take_user(mc_sync *sync, const char *peer_uid);

/* Whether a member is online: discovery sees it, or the last session with it succeeded. */
int mc_sync_peer_online(mc_sync *sync, const char *uid);
/* Stores sig as uid's prepared_sig and returns the one it replaces (0 if none). */
uint64_t mc_sync_peer_swap_prepared(mc_sync *sync, const char *uid, uint64_t sig);

void mc_sync_emit_pair_code(mc_sync *sync, const char *code, const char *name, int attempts);
void mc_sync_emit_pair_result(mc_sync *sync, const mc_pair_result *r);
void mc_sync_emit_progress(mc_sync *sync, const mc_sync_progress *p);
void mc_sync_emit_pair_merge(mc_sync *sync, const mc_pair_merge *m);

/* --- Confirming profiles (profile.c) --- */

/* Hex SHA-256 and its NUL. */
#define MC_PROFILE_FP_STRLEN 65

/* uid's profile fingerprint: a hash of what decides what it holds (archive, mode, format,
 * budget and selection, as stored), the same on every device holding the same rows. */
mc_status mc_profile_fingerprint(sqlite3 *db, const char *uid, char out[MC_PROFILE_FP_STRLEN]);
/* uid's mc_sync_confirm_state: its fingerprint against its devices.confirmed_profile. -1 on
 * a database error. */
int mc_profile_confirm_state(sqlite3 *db, const char *uid);
/* Records this device's current fingerprint as confirmed, in a write of its own. *wrote
 * (may be NULL): it changed. MC_ERR_NOT_FOUND if not in a group. */
mc_status mc_profile_confirm(mc_sync_db *sdb, int *wrote);

/* --- Pairing (pairing.c) --- */

/* The thread of an outgoing pairing; arg is a heap mc_pair_target, freed here. */
typedef struct mc_pair_target {
  mc_sync *sync;
  char host[256];
  uint16_t port;
  char device_uid[MC_UID_STRLEN]; /* "" when pairing by address */
} mc_pair_target;
void mc_pairing_initiate(void *arg);
/* Serves an accepted pairing-mode connection (this device is B); owns ch, and untracks it
 * before closing it. */
void mc_pairing_respond(mc_sync *sync, mc_sync_conn *c, mc_channel *ch);

/* --- Test hooks --- */

/* Before mc_sync_start(): listen on port (0: ephemeral) instead of the default, and run
 * without discovery. */
void mc_sync_set_test_options(mc_sync *sync, uint16_t port, int use_discovery);
/* manual_only: the scheduler starts sessions only for mc_sync_now(), and reach checks only
 * for mc_sync_check_reach(), so tests decide when devices talk. Any time. */
void mc_sync_set_test_manual(mc_sync *sync, int manual_only);
/* Shortens (or lengthens) MC_SYNC_AUTO_INTERVAL_MS for sessions that end from now on; 0
 * turns the limit off. Any time. */
void mc_sync_set_test_auto_interval(mc_sync *sync, uint32_t ms);
/* Publishes these address hints (JSON array text) instead of the interfaces' addresses. */
void mc_sync_set_test_hints(mc_sync *sync, const char *json);
/* Reach checks ask to be called back at port instead of the listener's (0: the listener's),
 * as if a firewall blocked it. Any time. */
void mc_sync_set_test_reach_port(mc_sync *sync, uint16_t port);
/* Sleeps ms before each frame of a file this device serves, so that a test can catch a
 * transfer halfway (0: none). Any time. */
void mc_sync_set_test_send_delay(mc_sync *sync, uint32_t ms);
/* no_merge = 1: this device pairs as a version without merging libraries would (no "merge"
 * in PAIR_HELLO/PAIR_READY), so two libraries with music are refused as library_mismatch.
 * Any time; takes effect at the next pairing. */
void mc_sync_set_test_no_merge(mc_sync *sync, int no_merge);
/* no_files_plan = 1: this device syncs as a version without FILES_PLAN would (no
 * "files_plan" in HELLO, and one sent all the same ends the session). Any time; takes
 * effect at the next session. */
void mc_sync_set_test_no_files_plan(mc_sync *sync, int no_files_plan);
/* As if discovery had reported a peer online (online = 1) or gone (0). */
void mc_sync_test_peer(mc_sync *sync, const char *device_uid, const char *name, const char *kind,
                       const char *group_hash, const char *address, uint16_t port, int online);

#endif
