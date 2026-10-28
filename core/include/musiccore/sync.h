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
#include "musiccore/transcode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;
typedef struct mc_sync mc_sync;

/* The TCP port sync listens on unless it is taken. */
#define MC_SYNC_DEFAULT_PORT 47823
/* Rate limit on automatic sync. After a complete session with a member (status MC_OK, both
 * sides converged, nothing left to follow up), in either direction, this device starts no
 * automatic session with it for this long: not on its periodic schedule, not when discovery
 * sees it come online, not for local changes (they wait, and go out when it runs out), and
 * not for mc_sync_background(). Those are held back until it runs out, then go ahead (the
 * periodic one included, so members still sync about once an interval). Unaffected:
 *  - sessions the other device starts: they are served as ever (and count as complete ones);
 *  - mc_sync_now() and mc_sync_confirm_now(), which start one at once;
 *  - a failed or incomplete session, which lifts the limit, so the usual retries (with
 *    backoff) and follow-ups go ahead.
 * It survives restarts: it is measured from peer_contact.last_sync_ms in sync.db. */
#define MC_SYNC_AUTO_INTERVAL_MS (60 * 60 * 1000)
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
} mc_sync_progress;

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
  int files_deleted;     /* removed by reconcile or the fast check */
  int files_unknown;     /* audio files in the library folder no files row knows; deleted,
                          * except on an archive, which only reports them */
  int files_sent;        /* served to the peer */
  int64_t bytes_received;
  int64_t bytes_sent;
  int files_paused;      /* this device fetched and deleted nothing: its profile isn't
                          * confirmed (see mc_sync_confirm_now()) */
  int covers_received;   /* album covers fetched for folders here that had none */
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
  /* A session's progress. Each that starts ends with MC_SYNC_PHASE_DONE, even one that
   * on_sync_done won't report (a scheduled attempt that found the device unreachable). */
  void (*on_progress)(void *user, const mc_sync_progress *progress);
  void (*on_sync_done)(void *user, const mc_sync_result *result);
  /* What's known of whether device_uid and this device can connect to each other changed
   * (see "Reachability" below). device_uid is this device's own when whether others can
   * reach it changed, and NULL when much changed at once (its network did). */
  void (*on_reach)(void *user, const char *device_uid);
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
 * and on success the first session follows straight away, but it exchanges only group and
 * library data: the new member fetches no files until its profile is confirmed (see
 * mc_sync_confirm_now()). Requires mc_sync_start(). One outgoing pairing at a time:
 * MC_ERR_LOCKED while another is in progress. */

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
/* Syncs with device_uid, or with every reachable member for NULL, as soon as possible,
 * whatever MC_SYNC_AUTO_INTERVAL_MS says. Returns at once; on_sync_done reports each session.
 * Requires mc_sync_start(). It confirms nothing, so a device whose profile isn't confirmed
 * fetches no files. A Sync button calls mc_sync_confirm_now(); background work calls
 * mc_sync_background(). */
mc_status mc_sync_now(mc_sync *sync, const char *device_uid);
/* As mc_sync_now(), for background work (a periodic job, say): an automatic sync, so a member
 * this device had a complete session with less than MC_SYNC_AUTO_INTERVAL_MS ago gets none
 * until that runs out (see mc_sync_device.auto_after_ms), and only if sync still runs then. */
mc_status mc_sync_background(mc_sync *sync, const char *device_uid);

/* --- Confirming profiles ---
 * A device fetches and deletes files only for a profile its user has confirmed: sessions
 * still exchange group and library data, but the file phase on a device whose profile isn't
 * confirmed is skipped (mc_sync_result.files_paused). The user confirms by pressing Sync,
 * on that device or on the other end of a session with it, which records a fingerprint of
 * the device's profile; any later change to it (from any member) pauses the device again
 * until the next press. A new member starts unconfirmed, so pairing alone never moves
 * files. Names don't count as changes, nor do edits that leave what the device holds as it
 * was (the selection while it syncs everything, an archive's fields). */

typedef enum mc_sync_confirm_state {
  MC_SYNC_CONFIRMED,      /* its profile is as its user last confirmed: files sync on their own */
  MC_SYNC_UNCONFIRMED,    /* never confirmed (just paired): no files until Sync is pressed */
  MC_SYNC_PROFILE_CHANGED /* changed since it was confirmed: paused until Sync is pressed */
} mc_sync_confirm_state;

/* The Sync button: as mc_sync_now(), and the user's say-so. It confirms this device's
 * profile at once, and the next session with device_uid (each member for NULL), whichever
 * side starts it within a few minutes, confirms the profiles of both ends as they stand
 * after its group exchange, so it fetches files at once. Requires mc_sync_start(). */
mc_status mc_sync_confirm_now(mc_sync *sync, const char *device_uid);
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

/* --- Reachability ---
 * Firewalls often let discovery (UDP 5353) through but block the TCP port sync listens on,
 * so a device shows up but can't be synced with, or syncs only when the other side calls.
 * Sync tests both directions with each device it can see: every connection it makes or
 * fails to make while the device is online, every one the device makes to it, and a reach
 * check, in which this device connects to the other and asks it to connect back. Checks run
 * when a device is first seen online (a member, or a nearby device outside the group), every
 * 15 minutes while it stays online, and when this device's network changes, which forgets
 * what was known. Nothing about it is stored: it starts out unknown after mc_sync_start(),
 * and on_reach reports changes. A device running an older version answers no check, so
 * whether it can reach this one stays unknown until it connects. */

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

/* "Can we be seen": whether the devices sync has tested with can connect to this one.
 * MC_ERR_INVALID_ARG unless started. */
mc_status mc_sync_reach_self(mc_sync *sync, mc_sync_self_reach *out);
/* "Can we see them": whether this device can connect to device_uid (*to_them), and it to
 * this one (*to_us); either may be NULL. Any device sync knows of, member or nearby.
 * MC_ERR_NOT_FOUND if it knows nothing of it. */
mc_status mc_sync_reach_peer(mc_sync *sync, const char *device_uid, mc_sync_reach *to_them,
                             mc_sync_reach *to_us);
/* Checks device_uid (every device online, for NULL) again now, as after the user fixed a
 * firewall. Returns at once; on_reach reports what changes. Requires mc_sync_start();
 * MC_ERR_NOT_FOUND for a device sync knows nothing of. */
mc_status mc_sync_check_reach(mc_sync *sync, const char *device_uid);

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
  int64_t auto_after_ms; /* Unix ms before which this device starts no automatic session
                          * with it (see MC_SYNC_AUTO_INTERVAL_MS); 0 if it may now */
  /* Its own report of its files (sync_status); has_status = 0 if it hasn't written one. */
  int has_status;
  int64_t verified_ms, expected, ok, missing, corrupt, bytes;
  /* Whether its files sync on their own; see mc_sync_confirm_now(). Another device's is as
   * this one sees it: an edit here it hasn't received yet already counts. */
  mc_sync_confirm_state confirm;
  /* As mc_sync_reach_peer(): reach, whether this device can connect to it; reach_us, it to
   * this one. On this device's own row both are mc_sync_reach_self()'s state. */
  mc_sync_reach reach, reach_us;
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
  int in_other_group;    /* both of us are in groups, different ones: pairing will be
                          * refused until one leaves (MC_PAIR_IN_GROUP) */
  char *address;
  uint16_t port;
  mc_sync_reach reach, reach_us; /* as in mc_sync_device: BLOCKED reach means pairing
                                  * with it will fail */
} mc_sync_nearby;

typedef struct mc_sync_nearby_list {
  mc_sync_nearby *items;
  int count;
} mc_sync_nearby_list;

/* Devices discovery currently sees that aren't in our group, ordered by name. */
mc_status mc_sync_list_nearby(mc_sync *sync, mc_sync_nearby_list **out);
void mc_sync_nearby_list_free(mc_sync_nearby_list *list);

/* --- Names and addresses --- */

/* Length of a numeric IPv6 address (the longest kind) including the terminating NUL. */
#define MC_SYNC_ADDR_STRLEN 46
/* Longest device name in bytes, excluding the NUL. Longer names are cut at a UTF-8
 * character boundary. */
#define MC_SYNC_DEVICE_NAME_MAX 127

/* Renames this device. In a group this sets its profile name, which the group then shows
 * everywhere; outside one it only changes the name discovery announces. Leading and
 * trailing spaces are trimmed; MC_ERR_INVALID_ARG if nothing is left. The frontend saves
 * the name (mc_settings_set_device_name()) to pass to mc_sync_new() next time. */
mc_status mc_sync_set_device_name(mc_sync *sync, const char *name);
/* This device's current name into out (cap bytes, NUL-terminated, cut if it doesn't fit).
 * A device takes on the name its group gives it, so this can change after a session. */
mc_status mc_sync_get_device_name(mc_sync *sync, char *out, size_t cap);
/* This machine's numeric IPv4 and IPv6 addresses on interfaces that are up, loopback left
 * out, IPv4 first. Writes at most max and returns how many; 0 if there are none. */
int mc_sync_local_addresses(char (*out)[MC_SYNC_ADDR_STRLEN], int max);

/* --- Profiles ---
 * What each device holds: everything or a selection, as originals or Opus, or within a size
 * budget that picks the format. Any member may edit any member's profile; the device applies
 * it when it next syncs. */

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
  /* Original, Opus + bitrate (one of mc_opus_bitrates), or MC_FORMAT_AUTO (bitrate 0) when
   * and only when budget_enabled: the format and the size budget exclude each other. Auto
   * takes the best of originals, then each of mc_opus_bitrates, that fits the budget; if
   * even the lowest doesn't, the profile is over budget and nothing syncs (see
   * mc_sync_estimate). */
  mc_transcode_opts format;
  int budget_enabled;
  int64_t budget_bytes;     /* the budget; 0 = the device's free space alone. Kept while
                             * !budget_enabled so it comes back. */
  mc_profile_item *items;   /* the selection; kept while sync_all so it comes back */
  int count;
} mc_sync_profile;

/* device_uid's profile (any member's, this device's included). An archive's reads as
 * everything, originals, no budget, whatever its row holds. A device with no profile stored
 * receives no files until one is (pairing stores one at once, so this is brief); for it the
 * getter returns what an editor should start from: everything, originals, no budget, no
 * name. A stored budget reads as MC_FORMAT_AUTO whatever format it was stored with (older
 * versions kept both, the budget walking down from the format). Items of kinds this version
 * doesn't know are left out. MC_ERR_NOT_FOUND if not in a group or it isn't a current
 * member. Free with mc_sync_profile_free(). */
mc_status mc_sync_get_profile(mc_sync *sync, const char *device_uid, mc_sync_profile **out);
/* Frees a profile from mc_sync_get_profile(). NULL is a no-op. */
void mc_sync_profile_free(mc_sync_profile *profile);
/* Stores device_uid's profile, writing only what differs from the stored one: changed
 * fields, and items added or removed (duplicates are merged; stored items of unknown kinds
 * are kept). archive is ignored. Callers read-modify-write: mc_sync_get_profile(), change
 * the fields, then set straight away, so edits made meanwhile on other devices to other
 * fields or items aren't overwritten. An archive's profile is locked to everything,
 * originals and no budget (MC_ERR_LOCKED for anything else); its name, budget size and
 * items stay editable, and its stored mode, format and budget switch are kept for when it
 * stops being one. MC_ERR_INVALID_ARG for an unknown kind, a bitrate not in
 * mc_opus_bitrates, or MC_FORMAT_AUTO without budget_enabled or another format with it;
 * MC_ERR_NOT_FOUND as for the getter. Members hear of the change in a few
 * seconds. */
mc_status mc_sync_set_profile(mc_sync *sync, const char *device_uid, const mc_sync_profile *profile);

/* --- Archives ---
 * An archive keeps the whole library as originals, and the group always has one. */

/* archive = 1 makes device_uid an archive: any member may do it for any device. Its
 * profile then reads as everything, originals, no budget (see mc_sync_set_profile()).
 * archive = 0 clears it, which only the device itself may do (MC_ERR_LOCKED from any other),
 * and only while another archive's own report (mc_sync_device's status) shows it verified
 * complete: nothing missing or corrupt, checked after the library's latest change
 * (MC_ERR_LOCKED otherwise; MC_ERR_LIBRARY_MISMATCH if the open library isn't the group's,
 * which is what says when it last changed). Setting what it already is does nothing. Only
 * the flag is written, so concurrent edits to the rest of the profile all apply. If two
 * archives clear themselves at once, each relying on the other, the group keeps the one
 * that cleared first (see "Archives" in docs/sync-protocol.md); every getter already counts
 * it as one. MC_ERR_NOT_FOUND as for mc_sync_get_profile(). */
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
  /* The wanted tracks' size as originals ([0]) and at each of mc_opus_bitrates ([1 + i]),
   * whatever the profile's format, so an editor can show every option. */
  int64_t step_bytes[MC_SYNC_ESTIMATE_STEPS];
} mc_sync_estimate;

/* How much device_uid would hold with profile, which needn't be stored (the profile editor's
 * candidate; a device with no profile stored yet is estimated as if it were). Worked out as
 * the device's next sync would: Opus sizes are estimated from durations, originals from the
 * files this device knows (others at 1000 kbps). Free space is measured here for this
 * device, and for another one comes from what it last reported, without what it already
 * holds (only it knows that). profile->archive previews making it an archive. Blocks for a
 * read of every track. MC_ERR_INVALID_ARG as for mc_sync_set_profile(); MC_ERR_NOT_FOUND as
 * for mc_sync_get_profile(); MC_ERR_LIBRARY_MISMATCH if the open library isn't the
 * group's. */
mc_status mc_sync_estimate_profile(mc_sync *sync, const char *device_uid,
                                   const mc_sync_profile *profile, mc_sync_estimate *out);

/* --- Verification ---
 * Every file sync receives is checked against its SHA-256 on arrival (files.sha256). A fast
 * check also runs after each session. */

typedef enum mc_verify_mode {
  MC_VERIFY_FAST, /* each file's size and modification time against its files row */
  MC_VERIFY_DEEP  /* re-hashes each file against files.sha256 (recording it if unset) */
} mc_verify_mode;

typedef struct mc_verify_result {
  int64_t checked;
  int64_t ok;
  int64_t failed;  /* missing, changed or corrupt */
  int64_t deleted; /* failed copies received from peers: deleted with their rows, so the
                    * next session fetches them again. Originals imported here are only
                    * counted, never deleted. */
} mc_verify_result;

/* Checks this device's files, then writes its sync_status when the open library is the
 * group's. Blocks for as long as it takes (a deep check reads every file), waiting first for
 * a session's file phase to finish. out may be NULL. */
mc_status mc_sync_verify(mc_sync *sync, mc_verify_mode mode, mc_verify_result *out);

#ifdef __cplusplus
}
#endif

#endif
