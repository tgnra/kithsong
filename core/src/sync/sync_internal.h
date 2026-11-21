/* What sync.c, session.c and pairing.c share: the mc_sync object, its peer table, and the
 * hooks tests use to run without discovery. Internal to core.
 *
 * Threads: the caller's (the API), the listener, the scheduler, discovery's, one worker per
 * session or pairing, and the encode pool's (encode.h), which touch no database and no
 * session. Each thread that touches the databases has connections of its own
 * (mc_sync_conn), since a connection is used by one thread at a time; session workers
 * borrow theirs (mc_sync_conn_take()), and hand them on when done. Everything in
 * mc_sync under "lock" is guarded by it; callbacks run under cb_lock, never under lock. */
#ifndef MC_SYNC_INTERNAL_H
#define MC_SYNC_INTERNAL_H

#include <stdint.h>

#include "kithcore/discovery.h"
#include "kithcore/library.h"
#include "kithcore/sync.h"
#include "sync/channel.h"
#include "sync/encode.h"
#include "sync/pair.h"
#include "sync/sync_db.h"
#include "sync/version.h"
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
  MC_TRIGGER_NOW,      /* mc_sync_now(): the only one MC_SYNC_AUTO_INTERVAL_MS and peer_held_ms
                        * don't hold back */
  MC_TRIGGER_AUTO,     /* mc_sync_background() */
  MC_TRIGGER_COPIES    /* this device may now transcode, and the member waits for Opus copies:
                        * MC_SYNC_AUTO_INTERVAL_MS doesn't hold it back, the rest does */
} mc_sync_trigger;

/* The last session attempt with a member, this run of sync (since mc_sync_start()). */
typedef enum mc_sync_contact {
  MC_CONTACT_NONE,    /* not tried yet */
  MC_CONTACT_REACHED, /* a session completed */
  MC_CONTACT_FAILED   /* a session failed, or connecting to it did */
} mc_sync_contact;

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
  /* mono: when mc_channel_cancel() left busy_ch's shutdown to a send under way, which the
   * scheduler aborts if it's still open a moment later; 0 = none. */
  uint64_t cancel_ms;
  /* mono: mc_sync_cancel() holds sessions with it back until this; 0 = none. */
  uint64_t held_ms;
  /* mono: its user cancelled a session with us (its CANCEL), so no automatic session with it
   * starts here before this (MC_SYNC_PEER_CANCEL_HOLD_MS); mc_sync_now() and a session that
   * succeeds clear it, and its own sessions are served as ever. 0 = none. */
  uint64_t peer_held_ms;
  /* The file phase: a signature of the transcodes last asked for on its behalf while it
   * was offline (PREPARE_REQ), so they're asked for once per change. */
  uint64_t prepared_sig;
  /* mono: this device's user pressed Sync for it (mc_sync_now()), which the next session
   * with it carries (shown as asked for; the recycle bin's deletes may go); 0 = not, or
   * already carried. */
  uint64_t user_ms;
  /* The press is to be shown (mc_sync_progress.user): it named this device, or it was for
   * every member and this one was online. The next session this device starts with it on
   * that press carries it; 0 once one has. */
  int asked;
  /* Reachability: mono times of the latest evidence each way; 0 = none. out: we connected
   * to it, or failed to while it was online. in: it connected to us, or a reach check says
   * it couldn't. */
  uint64_t out_ok_ms, out_fail_ms, in_ok_ms, in_fail_ms;
  char in_addr[MC_SYNC_ADDR_MAX]; /* where its last session with us came from */
  uint64_t reach_ms; /* mono: when the last reach check with it started; 0 = never */
  int reaching;      /* a reach check with it is running */
  int reach_now;     /* mc_sync_check_reach() asked for one */
  /* How the last session attempt with it went, for the waiting tracks (waiting.h): only
   * sessions and their connections count, never discovery or a reach check. */
  mc_sync_contact contact;
  /* What was last learnt of its version (kithcore/sync.h, "Versions"). */
  mc_sync_version version;
  /* The hold it gave for refusing a session (ERROR "busy"), and until when (Unix ms, 0 for no
   * end); MC_HOLD_NONE once a session succeeds or the hold lapses. */
  mc_sync_hold refused_hold;
  int64_t refused_until_ms;
  /* Sessions that connected at an address of its but failed the handshake, running, and the
   * last such address ("host:port"); 0 once a handshake succeeds. */
  int wrong_count;
  char wrong_addr[MC_SYNC_ADDR_MAX + 8];
  int64_t held_wall_ms;      /* held_ms, as Unix ms */
  uint64_t seen_written_ms;  /* mono: when peer_contact.seen_ms was last written for it */
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

/* How many finished sessions' connections are kept for the next ones: one per session that
 * may run at once with a small group, either way. */
#define MC_SYNC_IDLE_CONNS 4

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
  mc_mutex api_lock;
  mc_sync_conn api; /* the API's connections, under api_lock */
  mc_encode_pool *enc; /* encodes into the transcode cache, for every session */

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
  mc_sync_conn idle[MC_SYNC_IDLE_CONNS]; /* see mc_sync_conn_take() */
  int nidle;
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
  /* mc_sync_set_power(). power_dirty: the scheduler is to write them into this device's
   * capabilities; power_copies: and to sync soon with members waiting for Opus copies. */
  int on_power, may_transcode;
  int power_dirty, power_copies;
  int copying; /* serving turns that have asked the encode pool for something */
  int transcoding_dirty; /* the scheduler is to check mc_sync_transcoding() for on_transcoding */
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
  /* A group of one: being forgotten, which pairing waits out; and the member whose removal
   * left it is being told first, which the scheduler's check waits out. */
  int dissolving;
  int dissolve_telling;
  int no_files_plan; /* test hook: no FILES_PLAN either way, as an older version */
  int no_cancel;     /* test hook: no CANCEL either way, as an older version */
  mc_version_self self_version; /* what this device says of its version; tests change it */
  unsigned last_features; /* the features both HELLOs of the latest session named, for tests */
  /* Tracks waiting for a device (waiting.h), as the scheduler last worked them out (NULL:
   * none yet, which reads as empty); waiting_dirty asks it to work them out again. Members
   * not tried yet count as reachable until waiting_grace_ms (mono), a while after start. */
  struct mc_sync_waiting *waiting;
  mc_sync_outlook outlook; /* worked out with the waiting tracks */
  int waiting_dirty;
  int waiting_announce; /* the answer was dropped (a pairing undone): say so with the next */
  struct mc_sync_problems *problems; /* problems.h, worked out with the waiting tracks */
  /* Issues (issues.h): the last receive turn's findings with each peer, the waiting tracks'
   * by-products, the notices, and when an issue next lapses by itself (wall ms; 0: none). */
  struct mc_findings **findings;
  int nfindings;
  struct mc_waiting_extra *wextra;
  struct mc_sync_issue *notices;
  int nnotices;
  int64_t issues_due_ms;
  char void_noticed[MC_UID_STRLEN]; /* the void demotion a notice was given for */
  int void_seeded;                  /* void_noticed holds what was there at start */
  int network_dirty; /* mc_sync_network_changed(): the scheduler is to look at addresses now */
  /* The hold this device publishes in capabilities.state, as last written. */
  mc_sync_hold pub_hold;
  int64_t pub_until_ms;
  char *pub_state; /* capabilities.state as last published (the scheduler's) */
  /* Taking over from a lost archive (takeover.c): the one under way or last finished. */
  struct mc_takeover_run *takeover;
  /* The weekly deep check: a run under way, whoever asked for it. Under lock. */
  int deep_running;
  int transcoding_shown; /* what on_transcoding last said; under cb_lock */
  uint32_t deep_delay_ms; /* how long after start a desktop first looks whether it's due */
  uint64_t waiting_grace_ms;
  /* The recycle bin (here.c): library exchanges that may be sending rows, which a restore
   * waits out (it's refused meanwhile), and a restore under way, which they wait for. Under
   * lock. */
  int bin_sending, bin_restoring;

  /* Options, set before mc_sync_start(); see the test hooks below. */
  int use_discovery;
  int manual_only; /* sessions only on mc_sync_now(), for tests; under lock */
  int no_auto;     /* mc_sync_set_automatic(0): only mc_sync_now() starts sessions; under lock */
  int listen_port; /* -1: the default with fallback */
  uint32_t auto_interval_ms; /* MC_SYNC_AUTO_INTERVAL_MS, but in tests; under lock */
  uint32_t peer_cancel_hold_ms; /* MC_SYNC_PEER_CANCEL_HOLD_MS, but in tests; under lock */
  char hint_override[MC_SYNC_HINTS_MAX];
  uint16_t reach_port_override; /* under lock */
  uint32_t send_delay_ms; /* under lock */
};

/* --- Shared helpers (sync.c) --- */

mc_status mc_sync_conn_open(mc_sync *sync, mc_sync_conn *c);
void mc_sync_conn_close(mc_sync_conn *c);
/* A session's connections: ones an earlier session gave back, else new ones (opening both
 * databases costs tens of ms, over 100 on Windows, before HELLO). Not under lock. */
mc_status mc_sync_conn_take(mc_sync *sync, mc_sync_conn *c);
/* Keeps c for the next session (or closes it: when enough are kept, when sync is stopping,
 * or if a transaction was left open on it) and clears it. Not under lock. */
void mc_sync_conn_give(mc_sync *sync, mc_sync_conn *c);

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
 * carried it yet; clears it. For a session that has just claimed the peer: it is shown as
 * asked for. */
int mc_sync_take_user(mc_sync *sync, const char *peer_uid);
/* Whether a session with peer_uid now would be one this device's user asked for: a press of
 * Sync (mc_sync_now()) no session has carried yet, or an mc_sync_now() still to be
 * acted on. Clears nothing. The recycle bin's deletes go only in such a session. */
int mc_sync_pressed(mc_sync *sync, const char *peer_uid);

/* --- The recycle bin (here.c; kithcore/delete.h) --- */

/* Before a library exchange: waits for a restore under way, then empties the bin through
 * lib, as its deletes may reach the peer from now on. With pressed 0 (the user didn't ask
 * for this session) a bin that holds anything stays as it is, and MC_ERR_LOCKED says the
 * exchange mustn't run. On MC_OK, call mc_sync_bin_send_end() after the exchange, whatever
 * became of it. */
mc_status mc_sync_bin_send_begin(mc_sync *sync, mc_library *lib, int pressed);
/* Empties the bin again (deletes made while the rows were being read may have gone too),
 * then lets restores go ahead. */
void mc_sync_bin_send_end(mc_sync *sync, mc_library *lib);

/* Something the waiting tracks depend on changed (files arrived or went, the group's data
 * changed): the scheduler works them out again soon. Any thread, not under lock. */
void mc_sync_waiting_changed(mc_sync *sync);

/* This device's mc_sync_set_power() values. Any thread, not under lock. */
void mc_sync_power(mc_sync *sync, int *on_power, int *may_transcode);
/* How many encodes the pool runs at once while it may transcode: the logical cores less one,
 * at least one, on power or not. */
int mc_sync_pool_size(void);
/* Test hook: the pool size instead of the cores', if above 0. Set before syncing. */
extern int mc_sync_test_pool_size;
/* A serving turn started (delta 1) or stopped (-1) using the encode pool: on_transcoding
 * fires if mc_sync_transcoding() changed. Any thread, not under lock. */
void mc_sync_copying(mc_sync *sync, int delta);

/* What holds sync back on this device's side, as it publishes it (capabilities.state) and
 * refuses sessions for, in this order: the recycle bin holds deletes, the user cancelled
 * (until the latest hold of any member), automatic sync is off, its profile isn't chosen.
 * Reads c; not under lock. */
void mc_sync_self_hold(mc_sync *sync, mc_sync_conn *c, mc_sync_hold *hold, int64_t *until_ms);

/* Sessions that failed the handshake at a member's address this many times running show as
 * MC_ISSUE_WRONG_DEVICE. */
#define MC_SYNC_WRONG_SHOW 3

/* Whether a member is online: discovery sees it, or the last session with it succeeded. */
int mc_sync_peer_online(mc_sync *sync, const char *uid);
/* Whether uid is a current member, as of the scheduler's last look. */
int mc_sync_peer_member(mc_sync *sync, const char *uid);
/* Stores sig as uid's prepared_sig and returns the one it replaces (0 if none). */
uint64_t mc_sync_peer_swap_prepared(mc_sync *sync, const char *uid, uint64_t sig);

/* What this device says of its version, under lock. */
void mc_sync_self_version(mc_sync *sync, mc_version_self *out);
/* Records what a session, a pairing or discovery learnt of uid's version, and says so
 * (on_version) if it changed. Not under lock. */
void mc_sync_note_version(mc_sync *sync, const char *uid, const mc_sync_version *v);

void mc_sync_emit_pair_code(mc_sync *sync, const char *code, const char *name, int attempts);
void mc_sync_emit_pair_result(mc_sync *sync, const mc_pair_result *r);
void mc_sync_emit_progress(mc_sync *sync, const mc_sync_progress *p);
/* KITHSONG_SYNC_TRACE=1: mc_sync_trace() writes timestamped lines to stderr, for timing a
 * session's steps. A thread flushes them every 20 ms so tracing doesn't slow what it times;
 * other stderr output may overtake them. */
int mc_sync_tracing(void);
void mc_sync_trace(const char *fmt, ...);

/* --- Choosing a profile (profile.c) --- */

/* Whether mc_sync_stop() or mc_sync_free() is under way. Any thread. */
int mc_sync_is_stopping(mc_sync *sync);
/* Whether this device's user has chosen its profile (local.profile_chosen), or it is an
 * archive, whose profile is fixed: only then does its file phase fetch or delete. 1, 0, -1 on
 * a database error. */
int mc_profile_chosen(mc_sync_db *sdb);
/* Records that this device's user chose its profile (a press of Sync). *wrote (may be NULL):
 * it wasn't before. MC_ERR_NOT_FOUND if not in a group. */
mc_status mc_profile_choose(mc_sync_db *sdb, int *wrote);
/* Pairing, on the device that joins, inside the caller's write: its profile isn't chosen
 * yet, and if it has no profiles row, one is written with the budget on at nine tenths of
 * free_bytes in whole GiB (at least 1), so the editor it chooses in starts from what fits.
 * free_bytes -1 (unknown) writes no row. *budget (may be NULL): the limit it set, 0 if none. */
mc_status mc_profile_prefill(mc_sync_db *sdb, int64_t free_bytes, int64_t *budget);
/* Profiles from before ownership: logs this device's own profile, as it stands here, in its
 * own name where another device's change is the newest of a field, an item another device
 * added, or a delete of its item another device made, so every device reads the same
 * profile once only the owner's changes count. In a write of its own; a no-op when there is
 * nothing to claim. */
mc_status mc_profile_claim(mc_sync_db *sdb);

/* This device's free space in bytes, on its library folder's volume; -1 if unknown. Every
 * plan for this device, and its capabilities, measure it through here. */
int64_t mc_sync_free_space(mc_library *lib);
/* Test hook for mc_sync_free_space(): stands in for mc_free_space() when set, given the
 * library folder (so a test running two devices in one process can answer for one of
 * them). Set before the devices start; NULL measures. */
extern mc_status (*mc_sync_test_free_space)(const char *dir, int64_t *out);

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

/* How long after mc_sync_start() the scheduler first looks whether the weekly deep check is due. */
void mc_sync_set_test_deep_delay(mc_sync *sync, uint32_t ms);

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
/* no_files_plan = 1: this device syncs as a version without FILES_PLAN would (no
 * "files_plan" in HELLO, and one sent all the same ends the session). Any time; takes
 * effect at the next session. */
void mc_sync_set_test_no_files_plan(mc_sync *sync, int no_files_plan);
/* no_cancel = 1: this device syncs as a version without CANCEL would (no "cancel" in HELLO,
 * and its mc_sync_cancel() sends none). Any time; takes effect at the next session. */
void mc_sync_set_test_no_cancel(mc_sync *sync, int no_cancel);
/* This device poses as another version from the next session or pairing: protocol and
 * protocol_min (< 0: not sent), app ("" or NULL: not sent), check (0: refuses nobody and
 * doesn't know the "version" refusals, as releases up to 0.9.4), and the features it
 * announces (version.h). Any time. */
void mc_sync_set_test_version(mc_sync *sync, int protocol, int protocol_min, const char *app,
                              int check, unsigned features);
/* The optional features (version.h) both HELLOs named in the latest session to get past them. */
unsigned mc_sync_test_last_features(mc_sync *sync);
/* Shortens (or lengthens) MC_SYNC_PEER_CANCEL_HOLD_MS for CANCELs received from now on.
 * Any time. */
void mc_sync_set_test_peer_cancel_hold(mc_sync *sync, uint32_t ms);
/* As if discovery had reported a peer online (online = 1) or gone (0). */
void mc_sync_test_peer(mc_sync *sync, const char *device_uid, const char *name, const char *kind,
                       const char *group_hash, const char *address, uint16_t port, int online);

#endif
