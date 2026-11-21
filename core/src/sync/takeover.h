/* Lost archives and taking over from one ("Taking over an archive" in docs/sync-protocol.md):
 * when an archive was last seen, what an archive publishes of itself, which archive this
 * device should worry about, the offer, and the takeover's worker. Internal to core. */
#ifndef MC_SYNC_TAKEOVER_H
#define MC_SYNC_TAKEOVER_H

#include <sqlite3.h>

#include "kithcore/kithcore.h"
#include "kithcore/sync.h"
#include "sync/issues.h"

struct mc_sync;
struct mc_sync_conn;
struct yyjson_mut_doc;
struct yyjson_mut_val;

/* An archive away this long shows as MC_ISSUE_ARCHIVE_AWAY. */
#define MC_SYNC_ARCHIVE_AWAY_DAYS 7
#define MC_DAY_MS ((int64_t)24 * 60 * 60 * 1000)

/* What an archive publishes in capabilities.state beyond its hold: when it was last up
 * ("seen", Unix ms to the day), what doesn't fit on it ("full": {"tracks", "bytes"}), and
 * its tracks whose files are missing or damaged ("damaged": up to MC_ARCHIVE_DAMAGED_MAX uids,
 * "damaged_n": how many in all). */
#define MC_ARCHIVE_DAMAGED_MAX 50

typedef struct mc_archive_state {
  int64_t seen_ms;
  int full_tracks;
  int64_t full_bytes;
  int damaged_n;
  mc_uid_list damaged;
} mc_archive_state;

void mc_archive_state_parse(const char *json, mc_archive_state *out);
void mc_archive_state_free(mc_archive_state *s);
/* Adds this device's own keys to root, if it is an archive. Reads c; not under lock. */
void mc_archive_state_add(struct mc_sync *sync, struct mc_sync_conn *c, struct yyjson_mut_doc *doc,
                          struct yyjson_mut_val *root);

/* The newest of uid's last complete session with this device, this device seeing it, and
 * what it last published of itself; 0 if never. */
int64_t mc_archive_last_seen(sqlite3 *db, const char *uid);

/* The archive this device should worry about: the group's only archive, another device, not
 * online. */
typedef struct mc_lost_archive {
  char uid[MC_UID_STRLEN];
  int64_t last_seen_ms;
  int away;      /* unseen MC_SYNC_ARCHIVE_AWAY_DAYS */
  int lost;      /* unseen MC_SYNC_ARCHIVE_LOST_DAYS */
  int reinstall; /* this device looks like it, reinstalled */
} mc_lost_archive;

/* 1 with *out filled in if there is one that is away, lost, or that this device looks like
 * reinstalled; 0 if not (or this device is taking over). Through c. */
int mc_archive_find_lost(struct mc_sync *sync, struct mc_sync_conn *c, mc_lost_archive *out);

/* The offer for la, through c: NULL in *out if there's none to make. */
mc_status mc_takeover_offer(struct mc_sync *sync, struct mc_sync_conn *c,
                            mc_sync_archive_offer **out);

/* Of tracks (the damaged tracks of archive), those no device but archive holds the original
 * of, into out. */
mc_status mc_archive_only_there(struct mc_sync_conn *c, const char *archive, const mc_uid_list *tracks,
                                mc_uid_list *out);

/* The takeover under way here, for the issues: 0 if none. */
typedef struct mc_takeover_view {
  mc_takeover_state state;
  char uid[MC_UID_STRLEN];
  int total, done;
  mc_status status;
} mc_takeover_view;
int mc_takeover_view_get(struct mc_sync *sync, mc_takeover_view *out);
/* The tracks a finished takeover couldn't have as originals, as uids (malloc'd copy). */
int mc_takeover_unrecovered(struct mc_sync *sync, mc_uid_list *out);

/* After start: resumes a takeover that local.takeover_uid says was under way. */
void mc_takeover_resume(struct mc_sync *sync);
/* In mc_sync_free(). */
void mc_takeover_free(struct mc_sync *sync);

#endif
