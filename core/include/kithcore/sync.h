/* Serverless LAN sync between the devices of a group: each device keeps change logs stamped
 * with hybrid logical clocks, and any two members exchange what the other hasn't seen.
 * Devices find each other with mDNS (discovery.h) or by address, and join a group by pairing
 * with a 6-digit code. See docs/sync-protocol.md.
 *
 * Threading: an mc_sync is driven from one thread at a time, but once started owns threads of
 * its own (listener, scheduler, discovery, one per session or pairing). The mc_sync_events
 * callbacks run on those threads, one at a time, never on the caller's. A callback may call
 * the getters, the pairing functions and mc_sync_cancel(), but not mc_sync_stop() or
 * mc_sync_free(). Strings passed to callbacks are valid only during the call. */
#ifndef KITHCORE_SYNC_H
#define KITHCORE_SYNC_H

#include "kithcore/kithcore.h"
#include "kithcore/transcode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;
typedef struct mc_sync mc_sync;

/* TCP; an ephemeral port is used if it is taken. */
#define MC_SYNC_DEFAULT_PORT 47823
/* Rate limit on automatic sync. After a complete session with a member (MC_OK, converged,
 * nothing to follow up), in either direction, this device starts no automatic session with it
 * for this long: scheduled, on discovery, for local changes and mc_sync_background() all wait
 * until it runs out, then go ahead. Not affected: sessions the other device starts and
 * mc_sync_now(). A failed or incomplete session lifts the limit so
 * retries go ahead. Measured from peer_contact.last_sync_ms in sync.db, so it survives
 * restarts. */
#define MC_SYNC_AUTO_INTERVAL_MS (60 * 60 * 1000)
#define MC_PAIR_CODE_LEN 6

typedef enum mc_device_kind {
  MC_DEVICE_DESKTOP,
  MC_DEVICE_PHONE
} mc_device_kind;

typedef enum mc_pair_reason {
  MC_PAIR_PAIRED,           /* both devices are now in one group */
  MC_PAIR_WRONG_CODE,       /* attempts_left says how many tries the code has left */
  MC_PAIR_IN_GROUP,         /* both are in different groups; one must leave first */
  MC_PAIR_LIBRARY_MISMATCH, /* the joining device has music of another library than the
                             * group's (two libraries are never mixed) */
  MC_PAIR_ALREADY_PAIRED,   /* both are already in the same group */
  MC_PAIR_BUSY,             /* the other device is pairing with someone else */
  MC_PAIR_CANCELLED,        /* mc_sync_pair_cancel() here, or the other side gave up */
  MC_PAIR_FAILED,           /* network error or timeout; see status */
  MC_PAIR_VERSION           /* their Kithsong versions can't sync; see version */
} mc_pair_reason;

/* --- Versions ---
 * Two devices sync only when each one's protocol version is at least the oldest the other
 * accepts. Both say theirs in HELLO and in pairing, and a session or pairing between devices
 * that can't sync is refused before any data is exchanged (status MC_ERR_VERSION, reason
 * MC_PAIR_VERSION). Releases up to 0.9.4 say nothing usable, and count as outdated. See
 * "Versions" in docs/sync-protocol.md.
 *
 * Each device sync knows of carries what was last learnt of its version (from a refused
 * session or pairing, or its discovery announcement), kept in memory only; on_version fires
 * when it changes, and a session that succeeds clears it. No automatic session starts with a
 * member marked outdated either way; mc_sync_now() still tries. */

typedef enum mc_sync_outdated {
  MC_SYNC_VERSION_OK,    /* nothing says the two can't sync */
  MC_SYNC_PEER_OUTDATED, /* it runs a Kithsong older than this one syncs with */
  MC_SYNC_SELF_OUTDATED  /* it runs a Kithsong too new for this one */
} mc_sync_outdated;

#define MC_SYNC_APP_VERSION_MAX 32

typedef struct mc_sync_version {
  mc_sync_outdated outdated;
  int protocol;     /* its protocol version; 0 if not known (it sent none) */
  int protocol_min; /* the oldest it syncs with; 0 if not known */
  char app[MC_SYNC_APP_VERSION_MAX]; /* its Kithsong version, "0.9.5"; "" if not known
                                      * (0.9.4 and older don't send it) */
} mc_sync_version;

/* The notice for a device named name (NULL or "" if unknown) whose version is v, in the
 * current language (i18n.h): "Tania's desktop runs Kithsong 0.9.3, which can't sync with this
 * version. Update Kithsong on Tania's desktop.", or "... Update Kithsong on this device."
 * when this device is the older one. Like snprintf: returns the length it wanted; "" when
 * v->outdated is MC_SYNC_VERSION_OK. */
size_t mc_sync_version_text(const char *name, const mc_sync_version *v, char *buf, size_t size);

typedef struct mc_pair_result {
  const char *device_uid; /* the other device; "" if it was never reached */
  const char *name;       /* its name, "" if unknown */
  int initiator;          /* 1 on the device that asked to pair (and enters the code) */
  mc_pair_reason reason;
  int attempts_left;      /* for MC_PAIR_WRONG_CODE */
  mc_status status;       /* MC_OK when paired, else the error behind the reason */
  /* Refused pairings, on the device that was asked: where the other one connected from and
   * listens ("" and 0 if not known), for asking it back when discovery can't see it. */
  const char *host;
  uint16_t port;
  /* MC_PAIR_PAIRED: 1 on the device that joined (see "Join rules" in docs/sync-protocol.md),
   * 0 on the other. The joiner holds no music yet, and fetches none until its user chooses
   * its profile there and presses Sync (see "Choosing a profile"): show it the profile
   * editor, which starts with a budget from its free space. */
  int joined;
  /* MC_PAIR_PAIRED with joined: the group's archive's name (first by name if several) and how
   * many it has. "" and 0 if not known (older versions don't say). */
  const char *archive_name;
  int archives;
  /* MC_PAIR_VERSION: the other device's version, for mc_sync_version_text(). */
  mc_sync_version version;
} mc_pair_result;

typedef enum mc_sync_phase {
  MC_SYNC_PHASE_CONNECTING,
  MC_SYNC_PHASE_GROUP,   /* group data: devices, profiles, status */
  MC_SYNC_PHASE_LIBRARY, /* library metadata */
  MC_SYNC_PHASE_FILES,   /* audio files, after the library data */
  MC_SYNC_PHASE_DONE
} mc_sync_phase;

typedef struct mc_sync_progress {
  const char *device_uid;
  mc_sync_phase phase;
  int64_t rows_sent;     /* change rows, for the whole session so far */
  int64_t rows_received;
  /* MC_SYNC_PHASE_FILES: the files this device is fetching from the peer. */
  int files_done;        /* received, failed or refused so far */
  int files_total;       /* asked for in this session */
  int64_t bytes_received; /* file bytes, for the whole session so far */
  int64_t bytes_sent;
  int64_t files_bytes;   /* files_total's size, estimated from this device's plan */
  /* MC_SYNC_PHASE_FILES: the files the peer is fetching from this device, as it announced
   * them at the start of its turn (0 until then, or from versions that don't announce). The
   * turns run one after the other, so (files_done + sends_done) / (files_total + sends_total)
   * is the file phase's progress; the second turn's count is known only once it starts. */
  int sends_done;        /* answered so far, sent or refused */
  int sends_total;
  int64_t sends_bytes;   /* sends_total's size, as the peer estimated it */
  /* MC_SYNC_PHASE_FILES: smoothed and rounded by the core so every frontend shows the same;
   * show them as they are, with mc_sync_pace_text(). 0 means nothing to show (start of a turn,
   * a stall, unknown size). speed is bytes per second; eta_seconds is on a coarse grid and
   * under 60 means less than a minute. */
  int64_t speed;
  int eta_seconds;
  /* See "Showing sync" below. user: this device's user asked for the session. connected: the
   * handshake is done; 0 only in MC_SYNC_PHASE_CONNECTING and in the MC_SYNC_PHASE_DONE of an
   * attempt that never got that far. */
  int user;
  int connected;
} mc_sync_progress;

/* progress's speed and time left as text, "8.3 MB/s · about 12 min left", or as much of it as
 * there is; "" for neither. */
void mc_sync_pace_text(const mc_sync_progress *progress, char *out, size_t cap);

typedef struct mc_sync_result {
  const char *device_uid;
  mc_status status;      /* MC_OK, or why the session failed */
  int library_synced;    /* 0 if only group data was exchanged (see mc_sync_library_state) */
  int converged;         /* both sides ended with the same data */
  int64_t rows_sent;
  int64_t rows_received;
  /* Files, when library_synced. This device as the receiver: */
  int files_received;    /* fetched and verified */
  int files_missing;     /* wanted tracks it still has no file for in the right format */
  int files_corrupt;     /* failed their checksum twice in this session */
  int files_deleted;     /* removed by reconcile, the sweep or the fast check */
  int files_unknown;     /* files in the library folder that nothing explains, found by the
                          * sweep: trashed (deleted on Android), or only reported on an
                          * archive. 0 if the sweep didn't get as far as counting */
  int files_sent;        /* served to the peer */
  int64_t bytes_received;
  int64_t bytes_sent;
  int covers_received;   /* album covers fetched for folders here that had none */
  int cancelled;         /* mc_sync_cancel() here cut the session short; status is not MC_OK */
  int peer_cancelled;    /* the other device's user cancelled it: not a failure to show;
                          * status is not MC_OK */
  int user;              /* as in mc_sync_progress */
  int connected;         /* as in mc_sync_progress: 0 if it failed before the handshake ended */
} mc_sync_result;

/* --- Showing sync ---
 * Most attempts are automatic probes of members at their last known addresses, online or
 * not; a frontend shouldn't show a spinner or error for one nobody asked for. So:
 *  - Show a session once progress has connected set, or from MC_SYNC_PHASE_CONNECTING when
 *    user is set (Sync pressed for that device, pairing's first session, or leaving the
 *    group).
 *  - Report a failed session only when user or connected is set. on_sync_done never reports
 *    an automatic attempt that found the device unreachable or busy; a user-requested one is
 *    reported (MC_ERR_NET if nothing answered). An automatic attempt that failed its
 *    handshake is reported with neither set, for logging.
 * Sessions the other device starts arrive connected, with user set if this device's user
 * pressed Sync for it shortly before. Reach checks send no progress; only on_reach. */

/* Every member may be NULL. All run on core threads (see the top of this file). */
typedef struct mc_sync_events {
  /* A discovered or group device came online or went offline. member: it's in our group. */
  void (*on_peer)(void *user, const char *device_uid, int online, int member);
  /* Show code: requester_name asked to pair. Fires again with a new code after
   * MC_PAIR_MAX_FAILURES wrong attempts. */
  void (*on_pair_code)(void *user, const char *code, const char *requester_name,
                       int attempts_left);
  void (*on_pair_result)(void *user, const mc_pair_result *result);
  /* Every session that starts ends with MC_SYNC_PHASE_DONE, even one on_sync_done won't
   * report. See "Showing sync" for which to show. */
  void (*on_progress)(void *user, const mc_sync_progress *progress);
  void (*on_sync_done)(void *user, const mc_sync_result *result);
  /* Reachability with device_uid changed (see "Reachability"). device_uid is this device's
   * own uid when others' reach to it changed, NULL when much changed at once (network
   * change). */
  void (*on_reach)(void *user, const char *device_uid);
  /* mc_sync_get_waiting()'s answer changed (only then; see "Waiting for a device"). */
  void (*on_waiting)(void *user);
  /* mc_sync_get_problems()'s answer changed. */
  void (*on_problems)(void *user);
  /* mc_sync_transcoding() changed to active. */
  void (*on_transcoding)(void *user, int active);
  /* This device was the last member of its group, so the group was forgotten (see "Group"
   * below): it's no longer in a group. device_name is this device's. Once per group. */
  void (*on_group_dissolved)(void *user, const char *device_name);
  /* A device's mc_sync_version changed: a session or pairing with it was refused for its
   * version, discovery heard it announce another, or a session with it succeeded. */
  void (*on_version)(void *user, const char *device_uid);
  /* mc_sync_get_outlook()'s answer changed. */
  void (*on_outlook)(void *user);
} mc_sync_events;

/* Opens this device's sync state (sync.db in config_dir, the per-device config dir) for lib.
 * device_name is what other devices show; NULL uses the host name. events is copied. lib
 * must stay open while the mc_sync exists (sync opens its own connections to it). Nothing
 * touches the network until mc_sync_start(). */
mc_status mc_sync_new(mc_library *lib, const char *config_dir, const char *device_name,
                      mc_device_kind kind, const mc_sync_events *events, void *user,
                      mc_sync **out);
/* Stops if started, then frees. NULL is a no-op. */
void mc_sync_free(mc_sync *sync);

/* This device's uid, the same on every member of its group. */
const char *mc_sync_device_uid(const mc_sync *sync);

/* Listens on TCP MC_SYNC_DEFAULT_PORT (an ephemeral port if taken), starts discovery and
 * automatic sync. Without multicast, discovery fails quietly and members are still tried at
 * their last known addresses. */
mc_status mc_sync_start(mc_sync *sync);
/* Ends sessions and pairings, stops discovery and the threads; blocks until they have
 * finished. Safe when not started. */
void mc_sync_stop(mc_sync *sync);

/* The port this device listens on once started, else 0. */
uint16_t mc_sync_port(const mc_sync *sync);

/* --- Pairing ---
 * A asks B to pair; B shows a code (on_pair_code) which A's user enters with
 * mc_sync_pair_enter(). Both get on_pair_result; on success the first session follows, but
 * the device that joined fetches no files until its user has chosen its profile (see
 * "Choosing a profile"). Requires mc_sync_start().
 * One outgoing pairing at a time (MC_ERR_LOCKED). */

/* A device from mc_sync_list_nearby(). MC_ERR_NOT_FOUND if discovery hasn't seen it. */
mc_status mc_sync_pair_request(mc_sync *sync, const char *device_uid);
/* A device given by address, for networks that block multicast. host is an IPv4 or IPv6
 * literal or a host name; port 0 means MC_SYNC_DEFAULT_PORT. */
mc_status mc_sync_pair_request_address(mc_sync *sync, const char *host, uint16_t port);
/* MC_ERR_NOT_FOUND if no outgoing pairing is waiting. After MC_PAIR_WRONG_CODE the pairing
 * waits for another code. */
mc_status mc_sync_pair_enter(mc_sync *sync, const char *code);
/* Gives up the pairing in progress here, outgoing or incoming. */
void mc_sync_pair_cancel(mc_sync *sync);

/* --- Group ---
 * A group of one goes: once every other member has left or been removed, this device
 * forgets the group as leaving does (its library, keys and scrobbling account stay) and
 * reports on_group_dissolved. It checks at start, after group data changes, and when it
 * removes the last other member, never while a pairing or session is under way here. */

/* Leaves the group: marks this device removed, tells one reachable member so the group
 * learns of it (blocking for a few seconds at most), then forgets the group's data here.
 * MC_ERR_LOCKED if this device is the group's only archive and others remain;
 * MC_ERR_NOT_FOUND if not in a group. */
mc_status mc_sync_leave_group(mc_sync *sync);
/* Removes another member (or this device, as mc_sync_leave_group()). Members then refuse
 * it, and it forgets the group when it next talks to one. MC_ERR_LOCKED if it is the
 * group's only archive. Returns at once; if it was the last other member, sync tells it in
 * the background, best effort, and then forgets the group. */
mc_status mc_sync_remove_device(mc_sync *sync, const char *device_uid);
/* The Sync button: syncs with device_uid (every reachable member for NULL) now, ignoring
 * MC_SYNC_AUTO_INTERVAL_MS and lifting cancel holds. The session it starts, or one the member
 * starts within a few minutes, has user = 1, and may send the recycle bin's deletes. It also
 * chooses this device's profile as it stands, if it wasn't chosen (see "Choosing a
 * profile"). Returns at once; on_sync_done reports each session. Requires mc_sync_start().
 * Background work calls mc_sync_background() instead. */
mc_status mc_sync_now(mc_sync *sync, const char *device_uid);
/* As mc_sync_now(), but an automatic sync: it waits out MC_SYNC_AUTO_INTERVAL_MS and cancel
 * holds (mc_sync_device.auto_after_ms), and only happens if sync still runs then. */
mc_status mc_sync_background(mc_sync *sync, const char *device_uid);
/* The Cancel button: ends the session with device_uid (every member for NULL), whichever side
 * started it, and holds sessions with it back for MC_SYNC_CANCEL_HOLD_MS: none starts here
 * and ones the other device starts are refused as busy. mc_sync_now() lifts the hold;
 * mc_sync_background() doesn't. A pending Sync press is
 * dropped; a partly fetched file is deleted, whole ones kept. on_sync_done reports it with
 * cancelled set. Returns at once; the hold survives mc_sync_stop()/mc_sync_start() but not
 * mc_sync_free().
 * The other device is told, best effort, once past the HELLOs (older versions just see the
 * session fail): it reports peer_cancelled and starts no automatic session with this one for
 * MC_SYNC_PEER_CANCEL_HOLD_MS, unless its user syncs or a session with it succeeds. */
#define MC_SYNC_CANCEL_HOLD_MS MC_SYNC_AUTO_INTERVAL_MS
#define MC_SYNC_PEER_CANCEL_HOLD_MS (10 * 60 * 1000)
mc_status mc_sync_cancel(mc_sync *sync, const char *device_uid);
/* on = 0 stops this device starting sessions or reach checks by itself (for an app that may
 * only start them while on screen). Running sessions continue, incoming ones are served, and
 * mc_sync_now() still works; whatever falls due waits for on = 1,
 * the default. Survives mc_sync_stop()/mc_sync_start(). Any thread. */
void mc_sync_set_automatic(mc_sync *sync, int on);

/* --- Power and transcoding for others ---
 * Members wanting Opus get it only from a member that may transcode for them (see "Lossy
 * copies" in docs/sync-protocol.md); a receiver never fetches an original to encode itself.
 * Each device publishes in its capabilities whether it is on mains power (or charging) and
 * whether it may transcode for others now; the platform reports both. Until it does, a desktop
 * counts as on_power = may_transcode = 1, a phone as 0, 0. */

/* Reports this device's power and whether it may transcode for other members now. Changes
 * reach this device's capabilities, and so the members, within moments (in the next session
 * with each). Turning may_transcode on starts a session soon with each member that wants Opus
 * and is missing tracks, whatever MC_SYNC_AUTO_INTERVAL_MS says, so it can fetch what this
 * device can make; automatic sync off (mc_sync_set_automatic()) still holds it back.
 * Turning it off takes effect at once: queued encodes are dropped, and requests for Opus this
 * device would have to encode, including those waiting on an encode that is running, are
 * refused as no_transcode, which the receiver counts as missing, not failed. A running
 * encode finishes into the cache (it can't be cut short), from which it may be sent later
 * without encoding. While it may transcode, the encode pool runs as many encodes at once as
 * the device has logical cores less one (at least one), on battery too: on_power changes
 * nothing here but what is published. Any thread; returns at once. */
void mc_sync_set_power(mc_sync *sync, int on_power, int may_transcode);
/* Whether this device is transcoding for other members now: a session it serves has encoded
 * or asked for an encode in its turn, or encodes are queued or running. on_transcoding says
 * when it changes. Any thread. */
int mc_sync_transcoding(mc_sync *sync);

/* --- Choosing a profile ---
 * Each device sets its own profile; other devices' profiles are read-only (MC_ERR_LOCKED from
 * the setters), and changes another device logs to them don't count. A device fetches and
 * deletes files only once its user has chosen its profile: the device that joins a group by
 * pairing starts unchosen, with the budget on at nine tenths of its free space, and its first
 * press of Sync (mc_sync_now()) chooses whatever the profile is then. An archive's profile is
 * fixed, so there's nothing to choose. mc_sync_outlook.chosen says which. */

/* --- Outlook ---
 * What the next sync would change, cheaply, for a one-line summary ("↑ 10 GB · ↓ 100
 * tracks"): worked out with the waiting tracks, from this device's plan and the members'
 * profiles and own reports, with nothing asked over the network. */

typedef struct mc_sync_outlook {
  int chosen;             /* this device's profile is chosen (or it is an archive): it fetches */
  int receive_tracks;     /* wanted here, not held in the right form, and some member holds
                           * them (reachable or not) */
  int64_t receive_bytes;  /* their estimated size */
  int send_tracks;        /* what the other members' own reports say they still lack; some
                           * may come from a member other than this device */
  int64_t send_bytes;     /* estimated from their plans' average track size */
  int send_devices;       /* members lacking something */
} mc_sync_outlook;

/* The outlook as last worked out; all 0 outside a group, when the open library isn't the
 * group's, and before the first computation. Doesn't block on the databases. */
mc_status mc_sync_get_outlook(mc_sync *sync, mc_sync_outlook *out);
/* o as the summary, in the current language: "↑ 10 GB · ↓ 100 tracks", one side alone,
 * or "Up to date". Like snprintf: returns the length it wanted. */
size_t mc_sync_outlook_text(const mc_sync_outlook *o, char *buf, size_t size);

/* The library or a profile changed here; members hear in a few seconds. Optional (sync
 * also polls). Any thread. */
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

/* --- Reachability ---
 * Firewalls often pass discovery (UDP 5353) but block sync's TCP port, so a device shows up
 * but can't be synced with, or only when it calls. Sync tracks both directions with each
 * device it sees: from every connection made or failed while it's online, and from reach
 * checks (connect, ask it to connect back), run when it first comes online, every 15 minutes
 * and on a network change (which forgets what was known). Not stored: unknown after
 * mc_sync_start(). Older versions answer no check. */

typedef enum mc_sync_reach {
  MC_REACH_UNKNOWN, /* not tested yet */
  MC_REACH_OK,      /* the latest attempt connected */
  MC_REACH_BLOCKED  /* the latest attempt failed while the device was online */
} mc_sync_reach;

/* Can other devices connect to this one? */
typedef struct mc_sync_self_reach {
  mc_sync_reach state; /* BLOCKED if any device found it couldn't, OK if some could and
                        * none couldn't, else UNKNOWN */
  int ok, blocked;     /* how many devices found each */
  uint16_t port;       /* the TCP port to allow, as mc_sync_port() */
  int default_port;    /* port is MC_SYNC_DEFAULT_PORT; else that was taken, and the port
                        * changes from one start to the next */
} mc_sync_self_reach;

/* MC_ERR_INVALID_ARG unless started. */
mc_status mc_sync_reach_self(mc_sync *sync, mc_sync_self_reach *out);
/* Either out may be NULL. Any device sync knows of, member or nearby; MC_ERR_NOT_FOUND
 * otherwise. */
mc_status mc_sync_reach_peer(mc_sync *sync, const char *device_uid, mc_sync_reach *to_them,
                             mc_sync_reach *to_us);
/* Re-checks device_uid (every online device for NULL) now, e.g. after a firewall fix. Returns
 * at once; on_reach reports changes. Requires mc_sync_start(); MC_ERR_NOT_FOUND for an
 * unknown device. */
mc_status mc_sync_check_reach(mc_sync *sync, const char *device_uid);

/* --- Holds ---
 * Why automatic sync between this device and another doesn't happen now. A press of Sync
 * goes past every hold of this device's own. */

typedef enum mc_sync_hold {
  MC_HOLD_NONE,
  MC_HOLD_RATE,           /* this device synced with it lately (MC_SYNC_AUTO_INTERVAL_MS) */
  MC_HOLD_CANCELLED,      /* the user cancelled (MC_SYNC_CANCEL_HOLD_MS); others refuse as busy */
  MC_HOLD_PEER_CANCELLED, /* the other device's user cancelled (MC_SYNC_PEER_CANCEL_HOLD_MS) */
  MC_HOLD_BIN,            /* the recycle bin holds deletes: only a press of Sync there goes */
  MC_HOLD_AUTO_OFF,       /* automatic sync is off (a phone with Kithsong closed) */
  MC_HOLD_NOT_CHOSEN,     /* it copies no music until its user chooses what it keeps */
  MC_HOLD_VERSION,        /* their versions can't sync (mc_sync_device.version) */
  MC_HOLD_COUNT
} mc_sync_hold;

/* Text flags. MC_SYNC_TEXT_APP_OPEN: this app syncs only while it's open (Android), so times
 * read "the next time Kithsong is open after 14:30". */
#define MC_SYNC_TEXT_APP_OPEN 1

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
  int64_t auto_after_ms; /* Unix ms before which no automatic session with it starts here
                          * (MC_SYNC_AUTO_INTERVAL_MS, MC_SYNC_PEER_CANCEL_HOLD_MS); 0 if
                          * none */
  /* Its own report of its files (sync_status); has_status = 0 if it hasn't written one. */
  int has_status;
  int64_t verified_ms, expected, ok, missing, corrupt, bytes;
  /* As mc_sync_reach_peer()'s to_them and to_us. On this device's own row both are
   * mc_sync_reach_self()'s state. */
  mc_sync_reach reach, reach_us;
  /* See "Versions". MC_SYNC_VERSION_OK on this device's own row. */
  mc_sync_version version;
  /* Why this device starts no automatic session with it now (MC_HOLD_NONE: nothing holds it
   * back), and when it next would on its own (Unix ms; 0 if not known, as while automatic
   * sync is off or a session runs). MC_HOLD_NONE on this device's own row. */
  mc_sync_hold hold;
  int64_t next_auto_ms;
  /* What it says holds back sync on its side (its published state, or the reason it gave
   * for refusing a session), and until when (Unix ms, 0 for no end). On this device's own
   * row: what this device publishes. */
  mc_sync_hold peer_hold;
  int64_t peer_hold_until_ms;
  /* It may convert for others but doesn't: under MC_SYNC_TRANSCODE_MIN_FREE free. */
  int low_space;
  /* Something else answered at this address when sessions tried it, wrong_count times
   * running; NULL if not. */
  char *wrong_address;
  int wrong_count;
  /* The newest of its last complete session, discovery seeing it and a connection with it
   * (Unix ms, kept in sync.db); 0 if never. */
  int64_t last_seen_ms;
} mc_sync_device;

typedef struct mc_sync_device_list {
  mc_sync_device *items;
  int count;
} mc_sync_device_list;

/* The group's members, this device included, ordered by name. Empty when not in a group. */
mc_status mc_sync_list_devices(mc_sync *sync, mc_sync_device_list **out);
void mc_sync_device_list_free(mc_sync_device_list *list);

/* The text a device's row shows, in the current language; "" when there's nothing. Like
 * snprintf, each returns the length it wanted. flags: MC_SYNC_TEXT_*.
 *
 * mc_sync_device_warning(): what's wrong, for a warning icon's tooltip (desktops) or the
 * row's subtext (Android), first of: its version can't sync; it holds sync back
 * (peer_hold: "Tania's phone is waiting for a press of Sync. Open Kithsong on Tania's phone
 * and press Sync."); it can't be reached (mc_sync_reach_text(), warning_only, while it's
 * online); something else answers at its address; it's an archive away for a long while
 * ("Keeps the only full copy of the library and last synced 12 days ago."); it doesn't
 * convert for others for want of space ("Not converting for other devices: under 256 MB
 * free"). Nothing for this device's own row but its reach and low space. */
size_t mc_sync_device_warning(mc_sync *sync, const mc_sync_device *d, int flags, char *buf,
                              size_t size);
/* A calm note on this device's own holds with it: "Next automatic sync after 14:30", "Sync
 * held after Cancel until 15:10. Press Sync to sync now.", "Next sync the next time Kithsong
 * is open after 14:30" with MC_SYNC_TEXT_APP_OPEN. Only while d->hold isn't MC_HOLD_NONE,
 * unless always, which gives the next automatic sync whenever it's known. */
size_t mc_sync_device_note(const mc_sync_device *d, int flags, int always, char *buf, size_t size);
/* How this device and device_uid reach each other, in words, as the desktops' device pages
 * show it ("This computer can't connect to Tania's phone. A firewall on it may be blocking
 * sync."); for this device's own uid, whether others can reach it. warning_only: only a
 * problem, else "" (a peer's failure to reach this device shows on this device's row
 * instead). Worded for a phone on a phone. name: the device's, NULL for unnamed. */
size_t mc_sync_reach_text(mc_sync *sync, const char *device_uid, const char *name,
                          int warning_only, char *buf, size_t size);

/* The network changed (an interface came up or went, Wi-Fi joined another network): reopens
 * discovery's sockets and announces this device again, forgets what was known of
 * reachability (on_reach with NULL), and tries members due for a retry now rather than
 * after their backoff. Call it from the platform's network monitor; any thread, returns at
 * once. Nothing if not started. */
void mc_sync_network_changed(mc_sync *sync);

typedef struct mc_sync_nearby {
  char uid[MC_UID_STRLEN];
  char *name;
  char *host;            /* NULL if it didn't announce one */
  mc_device_kind kind;
  int in_other_group;    /* both in different groups: pairing is refused (MC_PAIR_IN_GROUP) */
  char *address;
  uint16_t port;
  mc_sync_reach reach, reach_us; /* as in mc_sync_device: BLOCKED reach means pairing
                                  * with it will fail */
  mc_sync_version version;       /* as in mc_sync_device: not MC_SYNC_VERSION_OK means
                                  * pairing with it will be refused */
} mc_sync_nearby;

typedef struct mc_sync_nearby_list {
  mc_sync_nearby *items;
  int count;
} mc_sync_nearby_list;

/* Devices discovery currently sees that aren't in our group, ordered by name. */
mc_status mc_sync_list_nearby(mc_sync *sync, mc_sync_nearby_list **out);
void mc_sync_nearby_list_free(mc_sync_nearby_list *list);

/* --- Waiting for a device ---
 * Tracks this device's profile wants but doesn't hold in the right form, which only members
 * it can't reach now could give it ("12 tracks are waiting for Tania's desktop to come
 * online."). A member is a source if it holds the original, or a copy at exactly the Opus
 * bitrate wanted (copies are only made from originals). Tracks no member holds don't count.
 *
 * "Can't reach" is what the last real session attempt said, kept across restarts: completed
 * or running counts as reached; never connected counts as not; answered-then-failed changes
 * nothing. Discovery and reach checks don't count, so it doesn't flicker. A member not tried
 * yet counts as reached while automatic sync is off, for the first minute after start (so
 * sync's own sessions go first), and after that if its last complete session was within
 * MC_SYNC_AUTO_INTERVAL_MS.
 *
 * Worked out on a sync thread when its inputs change. Empty outside a group, when the open
 * library isn't the group's, while this profile isn't chosen or is over budget, and before
 * the first computation. */

typedef struct mc_sync_waiting_device {
  char uid[MC_UID_STRLEN];
  char *name;            /* the name the group shows for it */
  mc_device_kind kind;
  int tracks;            /* of the waiting tracks, those it holds */
} mc_sync_waiting_device;

typedef struct mc_sync_waiting {
  int tracks;                      /* waiting tracks, each counted once */
  char (*track_uids)[MC_UID_STRLEN]; /* those tracks, as many as tracks, sorted */
  mc_sync_waiting_device *devices; /* the unreachable members holding them, most tracks
                                    * first (then by name); a track several of them hold
                                    * counts for each */
  int count;
} mc_sync_waiting;

/* The waiting tracks as last worked out; never NULL on MC_OK. Doesn't block on the
 * databases. Free with mc_sync_waiting_free(). */
mc_status mc_sync_get_waiting(mc_sync *sync, mc_sync_waiting **out);
void mc_sync_waiting_free(mc_sync_waiting *waiting);
/* The banner's sentence for w in the current language (i18n.h), naming the member holding
 * the most and how many others ("... for Tania's desktop and 2 other devices ..."). Like
 * snprintf: returns the length it wanted; "" for an empty w. */
size_t mc_sync_waiting_text(const mc_sync_waiting *w, char *buf, size_t size);

/* --- Names and addresses --- */

/* A numeric IPv6 address (the longest kind) with its NUL. */
#define MC_SYNC_ADDR_STRLEN 46
/* Longest device name in bytes, excluding the NUL. Longer names are cut at a UTF-8
 * character boundary. */
#define MC_SYNC_DEVICE_NAME_MAX 127

/* In a group this sets its profile name; outside one, only the name discovery announces.
 * Trimmed; MC_ERR_INVALID_ARG if nothing is left. The frontend saves it
 * (mc_settings_set_device_name()) to pass to mc_sync_new() next time. */
mc_status mc_sync_set_device_name(mc_sync *sync, const char *name);
/* Truncated to cap. Can change after a session: a device takes the name its group gives it. */
mc_status mc_sync_get_device_name(mc_sync *sync, char *out, size_t cap);
/* Numeric addresses of interfaces that are up, no loopback, IPv4 first. Returns how many
 * (at most max). */
int mc_sync_local_addresses(char (*out)[MC_SYNC_ADDR_STRLEN], int max);

/* --- Profiles ---
 * What each device holds: everything or a selection, as originals or Opus, or within a size
 * budget that picks the format. Every member's can be read; only this device's can be set
 * (see "Choosing a profile"). */

typedef enum mc_profile_item_kind {
  MC_PROFILE_ALBUM,
  MC_PROFILE_ARTIST,      /* every album and track credited to the artist */
  MC_PROFILE_PLAYLIST,    /* a manual playlist */
  MC_PROFILE_ALBUM_GROUP  /* a manual album group */
} mc_profile_item_kind;

typedef struct mc_profile_item {
  mc_profile_item_kind kind;
  char ref_uid[MC_UID_STRLEN]; /* the album, artist, playlist or album group */
} mc_profile_item;

typedef struct mc_sync_profile {
  char *name;               /* friendly name; NULL = unset (shows devices.name) */
  int archive;              /* keeps the full library as originals; read-only here, set
                             * with mc_sync_set_archive() */
  int sync_all;             /* everything, else just items */
  /* Original, Opus at one of mc_opus_bitrates, or MC_FORMAT_AUTO (bitrate 0) exactly when
   * budget_enabled. Auto takes the best format that fits the budget; if even the lowest
   * doesn't, the profile is over budget and nothing syncs. */
  mc_transcode_opts format;
  int budget_enabled;
  int64_t budget_bytes;     /* 0 = the device's free space alone. Kept while !budget_enabled */
  mc_profile_item *items;   /* the selection; kept while sync_all so it comes back */
  int count;
  /* Albums and artists removed from this device (kithcore/sync_here.h), sorted: what Select
   * all leaves out. Filled in by mc_sync_get_profile(); ignored by mc_sync_set_profile() and
   * mc_sync_estimate_profile(). */
  mc_profile_item *removed;
  int removed_count;
} mc_sync_profile;

/* Any member's profile. An archive's reads as everything, originals, no budget, whatever its
 * row holds. With none stored (briefly, after pairing) returns an editor's starting point:
 * everything, originals, no budget, no name. A stored budget reads as MC_FORMAT_AUTO whatever
 * format was stored with it (older versions kept both). Unknown item kinds are left out;
 * exclusions show only in removed. MC_ERR_NOT_FOUND if not in a group or not a current
 * member. Free with mc_sync_profile_free(). */
mc_status mc_sync_get_profile(mc_sync *sync, const char *device_uid, mc_sync_profile **out);
/* Frees a profile from mc_sync_get_profile(). NULL is a no-op. */
void mc_sync_profile_free(mc_sync_profile *profile);
/* Sets this device's profile: MC_ERR_LOCKED for another device's. Writes only what differs
 * from the stored profile (changed fields, items added or removed; duplicates merged, stored
 * unknown kinds kept). archive is ignored. An archive is locked to
 * everything, originals, no budget (MC_ERR_LOCKED otherwise); its name, budget size and
 * items stay editable. MC_ERR_INVALID_ARG for an unknown kind, a bitrate not in
 * mc_opus_bitrates, or MC_FORMAT_AUTO not matching budget_enabled; MC_ERR_NOT_FOUND as for
 * the getter. */
mc_status mc_sync_set_profile(mc_sync *sync, const char *device_uid, const mc_sync_profile *profile);
/* Select all (on) / Select none: (un)selects the uids of kind in one write, leaving other
 * items alone. Selecting skips mc_sync_profile.removed (only ticking one by hand, via
 * mc_sync_set_profile(), drops an exclusion); unselecting keeps exclusions. sync_all is not
 * changed. Store pending edits first. Errors as for mc_sync_set_profile(). */
mc_status mc_sync_select_items(mc_sync *sync, const char *device_uid, mc_profile_item_kind kind,
                               const char (*uids)[MC_UID_STRLEN], int count, int on);
/* Unticking (on) under Sync everything: excludes the albums or artists as Remove from this
 * device would (kithcore/sync_here.h), leaving the files to the next sync; !on takes them out
 * of mc_sync_profile.removed. An album removed only through its artist comes back alone.
 * MC_ERR_LOCKED for an archive; other errors as for mc_sync_set_profile(). */
mc_status mc_sync_exclude_items(mc_sync *sync, const char *device_uid, mc_profile_item_kind kind,
                                const char (*uids)[MC_UID_STRLEN], int count, int on);

/* --- Archives ---
 * An archive keeps the whole library as originals, and the group always has one. */

/* Only a device sets its own role: MC_ERR_LOCKED for another device. Any device may make
 * itself an archive. It may stop being one only while another archive's own report shows it
 * verified complete since the library's latest change (MC_ERR_LOCKED otherwise;
 * MC_ERR_LIBRARY_MISMATCH if the open library isn't the group's). Only the flag is written.
 * If two archives clear themselves at once, the group keeps the one that cleared first (see
 * "Archives" in docs/sync-protocol.md). MC_ERR_NOT_FOUND as for mc_sync_get_profile(). */
mc_status mc_sync_set_archive(mc_sync *sync, const char *device_uid, int archive);

/* --- Size estimate --- */

/* Originals, then each of mc_opus_bitrates. */
#define MC_SYNC_ESTIMATE_STEPS (1 + MC_OPUS_BITRATE_COUNT)

typedef struct mc_sync_estimate {
  int tracks;              /* tracks the profile wants */
  int64_t total_bytes;     /* their size at kbps; when over_budget, at 64 kbps */
  int kbps;                /* what the device would hold them as: 0 for originals, else the
                            * Opus bitrate; for MC_FORMAT_AUTO the best that fits; 0 when
                            * over_budget */
  int archive;             /* the device is an archive: everything, originals, no budget */
  int over_budget;         /* even 64 kbps doesn't fit: nothing would sync */
  int64_t shortfall_bytes; /* when over_budget: how much too big at 64 kbps */
  int64_t limit_bytes;     /* with a budget, what they must fit in: the budget, or free space
                            * plus what the device already holds of them if that's less;
                            * -1 without a budget or a limit */
  int64_t free_bytes;      /* the device's free space used, -1 if unknown */
  int imports_dropped;     /* tracks imported here the profile leaves out (removed once an
                            * archive has them); 0 for another device */
  /* Size as originals ([0]) and at each of mc_opus_bitrates ([1 + i]), whatever the format. */
  int64_t step_bytes[MC_SYNC_ESTIMATE_STEPS];
} mc_sync_estimate;

/* How much device_uid would hold with profile (need not be stored), as its next sync would
 * work it out: Opus sizes from durations, originals from known files (others at 1000 kbps).
 * Free space is measured here for this device, else taken from the device's last report.
 * profile->archive previews making it an archive. Blocks for a read of every track. Errors as
 * for mc_sync_set_profile() and mc_sync_get_profile(); MC_ERR_LIBRARY_MISMATCH if the open
 * library isn't the group's. */
mc_status mc_sync_estimate_profile(mc_sync *sync, const char *device_uid,
                                   const mc_sync_profile *profile, mc_sync_estimate *out);

/* --- Verification ---
 * Received files are checked against files.sha256 on arrival. A fast check runs after each
 * session, and a deep one every MC_SYNC_DEEP_CHECK_DAYS: by itself on a desktop on mains
 * power, and on a phone through mc_sync_verify_weekly() from a charging job. */

#define MC_SYNC_DEEP_CHECK_DAYS 7

typedef enum mc_verify_mode {
  MC_VERIFY_FAST, /* each file's size and modification time against its files row */
  MC_VERIFY_DEEP  /* re-hashes each file against files.sha256 (recording it if unset) */
} mc_verify_mode;

typedef struct mc_verify_result {
  int64_t checked;
  int64_t ok;
  int64_t failed;  /* missing, changed or corrupt */
  int64_t deleted; /* failed copies from peers, deleted with their rows so they are fetched
                    * again; originals imported here are never deleted */
} mc_verify_result;

/* Then writes sync_status if the open library is the group's. Blocks (a deep check reads
 * every file), first waiting for any file phase to finish. out may be NULL. */
mc_status mc_sync_verify(mc_sync *sync, mc_verify_mode mode, mc_verify_result *out);

/* Continues the weekly deep check on the caller's thread for up to budget_ms (0: to the end),
 * resuming where it stopped; nothing if not due. Unlike mc_sync_verify() it blocks nothing
 * else. *done: finished for this week. For devices that don't run it by themselves. */
mc_status mc_sync_verify_weekly(mc_sync *sync, int64_t budget_ms, int *done);

/* --- Problems ---
 * For a warning card at the top of the library.
 *
 * A track is lost when this device, an archive that fetches files, holds nothing of it and
 * every other current member said in a session within MC_SYNC_LOST_ASKED_DAYS that it holds
 * no file of it at all. A member that hasn't said keeps the track off the list, as it may be
 * the last to hold it. See "Verification" in docs/sync-protocol.md. */

#define MC_SYNC_LOST_ASKED_DAYS 7

typedef struct mc_sync_lost_track {
  char uid[MC_UID_STRLEN];
  char *title;
  char *artist; /* its credit as shown; NULL if none */
} mc_sync_lost_track;

/* --- Issues ---
 * Everything that keeps sync from doing what the profiles ask, or that it did on its own,
 * each with what happened and how to fix it: one list (mc_sync_problems.issues), shown by one
 * dialog per platform, opened from the problems banner, a device's row or page, and the bar
 * of a sync that finished with tracks still missing. Worked out by the scheduler with the
 * waiting tracks and the problems, from the group's data, the last session with each member
 * and what is known of each one's whereabouts; on_problems says when it changed. See
 * "Issues" in docs/sync-protocol.md. */

/* A device needs this much free space to convert (transcode) for others. */
#define MC_SYNC_TRANSCODE_MIN_FREE ((int64_t)256 << 20)
/* An archive unseen this long is offered for takeover (see "Taking over an archive"). */
#define MC_SYNC_ARCHIVE_LOST_DAYS 30

typedef enum mc_sync_issue_kind {
  /* This device. */
  MC_ISSUE_FOLDER_UNWRITABLE, /* the library folder (detail) can't be written: error says why;
                               * nothing was fetched */
  MC_ISSUE_DISK_FULL,         /* the planned fetch (bytes) doesn't fit in bytes_free at detail;
                               * nothing was fetched */
  MC_ISSUE_WRITE_FAILED,      /* count files from device couldn't be written; error: the last */
  MC_ISSUE_FILES_BUSY,        /* the last session with device fetched nothing: another session
                               * here was fetching */
  MC_ISSUE_FAILED_FILES,      /* count files here failed a check (mc_sync_problems.failed) */
  MC_ISSUE_UNKNOWN_FILES,     /* count files the archive's sweep couldn't place */
  /* Tracks this device wants that it doesn't get. */
  MC_ISSUE_NO_TRANSCODER,     /* device holds them as originals but doesn't convert for others
                               * now, and nobody else can */
  MC_ISSUE_TRANSCODER_AWAY,   /* they're left for device to convert, but it can't be reached */
  MC_ISSUE_REFUSED,           /* device refused them; refusal says why */
  MC_ISSUE_DEVICE_LACKS,      /* no device holds them: device says it no longer has them */
  MC_ISSUE_DEVICE_LEFT,       /* no device holds them: device had them and left the group */
  MC_ISSUE_NO_HOLDER,         /* no device is known to hold them */
  MC_ISSUE_LOST,              /* on an archive: gone for good (mc_sync_problems.tracks);
                               * offer mc_sync_remove_lost() */
  /* Other devices. */
  MC_ISSUE_HELD,              /* device holds sync back on its side: hold, until_ms */
  MC_ISSUE_LOW_SPACE,         /* device doesn't convert for others: under
                               * MC_SYNC_TRANSCODE_MIN_FREE free (bytes_free) */
  MC_ISSUE_WRONG_DEVICE,      /* something else answered at device's address detail, count times */
  /* Archives. */
  MC_ISSUE_ARCHIVE_AWAY,      /* device keeps the only full copy and was last seen until_ms */
  MC_ISSUE_ARCHIVE_DAMAGED,   /* device, an archive, is missing or failed to check tracks; those
                               * no other device has the original of are named */
  MC_ISSUE_ARCHIVE_FULL,      /* device, an archive, is full: tracks aren't backed up, it needs
                               * bytes more */
  MC_ISSUE_ARCHIVE_LOST,      /* device, an archive, unseen MC_SYNC_ARCHIVE_LOST_DAYS: offer
                               * mc_sync_get_archive_offer() (count: 1 if this device has room) */
  MC_ISSUE_ARCHIVE_REINSTALL, /* this device looks like device reinstalled: offer to take over */
  MC_ISSUE_TAKEOVER,          /* this device is taking over from device (mc_sync_takeover_get()):
                               * count of bytes tracks are here so far; error if it failed; a
                               * notice once done, naming the tracks it couldn't have as
                               * originals */
  /* Notices: things sync did by itself. Gone once mc_sync_dismiss_notices(). */
  MC_ISSUE_NOTICE_ARCHIVE,    /* device became the group's archive by itself */
  MC_ISSUE_NOTICE_BUDGET,     /* this device's storage limit was set to bytes when it joined */
  MC_ISSUE_KIND_COUNT
} mc_sync_issue_kind;

/* Why a device refused tracks (FILE_ERR). */
typedef enum mc_sync_refusal {
  MC_REFUSED_IO,           /* it couldn't read or convert the file */
  MC_REFUSED_MISSING,      /* it no longer had the file */
  MC_REFUSED_NO_TRANSCODE, /* it stopped converting partway */
  MC_REFUSED_FORMAT,       /* it didn't understand the request */
  MC_REFUSED_COUNT
} mc_sync_refusal;

#define MC_SYNC_ISSUE_EXAMPLES 3

typedef struct mc_sync_issue {
  mc_sync_issue_kind kind;
  char device_uid[MC_UID_STRLEN]; /* the device it's about; this device's own uid for its own */
  char *device_name;              /* the name the group shows for it */
  mc_device_kind device_kind;
  int notice;                     /* a notice of something done, not a problem */
  int tracks;                     /* the tracks it's about; 0 if none */
  int albums;                     /* > 0: examples are album titles, of this many albums */
  char *examples[MC_SYNC_ISSUE_EXAMPLES]; /* names of what it's about: albums, else tracks */
  int nexamples;
  int count;                      /* as the kind says */
  mc_sync_refusal refusal;        /* MC_ISSUE_REFUSED */
  mc_sync_hold hold;              /* MC_ISSUE_HELD */
  int64_t bytes, bytes_free;      /* -1: not known */
  int64_t until_ms;               /* Unix ms as the kind says; 0 if none */
  char *detail;                   /* a folder or an address; NULL if none */
  char *error;                    /* in words; NULL if none */
} mc_sync_issue;

/* An issue in words, in the current language: a one-line title ("Tania's phone isn't
 * converting for other devices"), what happened, naming the examples, and how to fix it.
 * flags: MC_SYNC_TEXT_*. */
typedef struct mc_sync_issue_text {
  char title[256];
  char body[1024];
  char fix[768];
} mc_sync_issue_text;
void mc_sync_issue_describe(const mc_sync_issue *issue, int flags, mc_sync_issue_text *out);

typedef struct mc_sync_problems {
  int failed;  /* files here that are missing, changed or corrupt, not counting lost tracks' */
  int unknown; /* files in the library folder that an archive's latest sweep couldn't place */
  int lost;
  mc_sync_lost_track *tracks; /* the lost ones, as many as lost, sorted by uid */
  /* Every issue, the ones above included: problems first, then notices; in each, this
   * device's own, then by device name. */
  mc_sync_issue *issues;
  int issue_count;
} mc_sync_problems;

/* The problems as last worked out; never NULL on MC_OK. Doesn't block on the databases. Free
 * with mc_sync_problems_free(). */
mc_status mc_sync_get_problems(mc_sync *sync, mc_sync_problems **out);
void mc_sync_problems_free(mc_sync_problems *problems);
/* The banner's sentence for p in the current language, "" if there's nothing to say: the
 * first issue's title, and how many more ("… (2 more problems)"), notices left out when
 * there are problems. Like snprintf: returns the length it wanted. */
size_t mc_sync_problems_text(const mc_sync_problems *p, char *buf, size_t size);
/* Forgets the notices, once the user has seen them (the dialog closed). on_problems follows. */
void mc_sync_dismiss_notices(mc_sync *sync);
/* "Finished, but 3 tracks are still missing", for a session's result (files_missing), or ""
 * when none are. */
size_t mc_sync_finished_text(const mc_sync_result *r, char *buf, size_t size);

/* --- Taking over an archive ---
 * When a member that keeps the only full copy (an archive) hasn't been seen for
 * MC_SYNC_ARCHIVE_LOST_DAYS, every other member offers, once as a dialog and then as a notice
 * on that device's row and page, to make itself the archive in its place. Only a device with
 * room for the whole library as originals may; if none has, the offer says to pair one that
 * has. Taking over makes this device an archive, fetches everything it can, checks it, and
 * then removes the lost device from the group, leaving a list of what couldn't be had in
 * full quality. The first device to finish wins; the offer goes elsewhere once another
 * device is a verified archive. A device that looks like the lost one reinstalled (same
 * library with its files here, the same kind and the same host or device name) is offered
 * the same, checking the files it has instead of fetching them. See "Taking over an archive"
 * in docs/sync-protocol.md. */

typedef struct mc_sync_archive_offer {
  char device_uid[MC_UID_STRLEN]; /* the lost archive */
  char *name;
  mc_device_kind kind;
  int64_t last_seen_ms;     /* Unix ms; 0 if never */
  int64_t bytes_needed;     /* the whole library as originals, as far as known */
  int64_t bytes_free;       /* here; -1 if not known */
  int can_take;             /* this device has room: offer the button */
  int nobody_can;           /* no current member has room: say to pair one with bytes_needed */
  int reinstall;            /* this device looks like the lost one reinstalled */
  int asked;                /* the one-time dialog was shown (mc_sync_archive_offer_seen()) */
  int unrecoverable;        /* tracks no device left holds the original of: they can only be
                             * had as lossy copies, or not at all */
  mc_sync_lost_track *tracks; /* some of those (at most 100), sorted by title */
  int ntracks;
} mc_sync_archive_offer;

/* The offer to show now, NULL in *out if none (no archive lost, or another device is a
 * verified archive now). Blocks for a read of every track. Free with
 * mc_sync_archive_offer_free(). */
mc_status mc_sync_get_archive_offer(mc_sync *sync, mc_sync_archive_offer **out);
void mc_sync_archive_offer_free(mc_sync_archive_offer *offer);
/* The dialog for device_uid was shown: never as a popup again (kept in sync.db). */
mc_status mc_sync_archive_offer_seen(mc_sync *sync, const char *device_uid);
/* The offer in words: the dialog's title ("Make this computer the archive?"), its text
 * ("Studio PC hasn't synced for 30 days and keeps the only full copy of your library. …",
 * with the space needed, or the advice to pair a device with room), and the button ("Make
 * This Computer the Archive"; "" when this device can't). */
typedef struct mc_sync_offer_text {
  char title[256];
  char body[1536];
  char button[128];
} mc_sync_offer_text;
void mc_sync_archive_offer_describe(const mc_sync_archive_offer *offer, mc_device_kind self,
                                    mc_sync_offer_text *out);

/* Starts taking over from lost_uid: MC_ERR_NOT_FOUND unless it's on offer here, MC_ERR_LOCKED
 * without room or while one runs. Returns at once; progress comes with on_problems
 * (an MC_ISSUE_TAKEOVER issue) and mc_sync_takeover_get(). Survives restarts. */
mc_status mc_sync_take_over_archive(mc_sync *sync, const char *lost_uid);

typedef enum mc_takeover_state {
  MC_TAKEOVER_NONE,
  MC_TAKEOVER_FETCHING,  /* an archive now, fetching what it can */
  MC_TAKEOVER_CHECKING,  /* checking the files it has */
  MC_TAKEOVER_DONE,      /* the lost device was removed; see unrecovered */
  MC_TAKEOVER_FAILED     /* status says why */
} mc_takeover_state;

typedef struct mc_sync_takeover {
  mc_takeover_state state;
  char device_uid[MC_UID_STRLEN]; /* the lost archive */
  char *name;
  int tracks_total, tracks_done;  /* of the library, held here in full quality */
  mc_status status;
  int unrecovered;                /* tracks that couldn't be had as originals */
  mc_sync_lost_track *tracks;     /* some of those (at most 100) */
  int ntracks;
} mc_sync_takeover;

/* The takeover under way or last finished (state MC_TAKEOVER_NONE if none since start). Free
 * with mc_sync_takeover_free(). */
mc_status mc_sync_takeover_get(mc_sync *sync, mc_sync_takeover **out);
void mc_sync_takeover_free(mc_sync_takeover *t);

/* Deletes those of uids still lost from the library (so from every device), as a delete here
 * would. MC_ERR_LOCKED, doing nothing, while a file phase runs. Tracks no longer lost are
 * skipped silently: compare out->tracks_deleted with count. Irreversible: call only after the
 * user confirms. out may be NULL. */
struct mc_delete_result; /* delete.h, which may be what brought this file in */
mc_status mc_sync_remove_lost(mc_sync *sync, const char *const *uids, int count,
                              struct mc_delete_result *out);

#ifdef __cplusplus
}
#endif

#endif
