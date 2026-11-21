/* Sync issues (mc_sync_problems.issues in kithcore/sync.h, "Issues" in docs/sync-protocol.md):
 * what a session's file phase found, kept per peer, and the builder that turns everything the
 * scheduler knows into the list. Internal to core.
 *
 * Sources, each one function in issues.c's mc_issues_build(): the checks (problems.c), the
 * waiting tracks' extras (waiting.h), the findings of the last receive turn with each peer
 * (below), the peer table (refusals' holds, wrong devices), the members' capabilities rows
 * (published holds, low space), and the notices. A new kind gets a source function there and a
 * case in mc_sync_issue_describe(). */
#ifndef MC_SYNC_ISSUES_H
#define MC_SYNC_ISSUES_H

#include "kithcore/kithcore.h"
#include "kithcore/sync.h"

struct mc_sync;
struct mc_sync_conn;

typedef struct mc_uid_list {
  char (*uids)[MC_UID_STRLEN];
  int n, cap;
} mc_uid_list;

/* 0 if out of memory. */
int mc_uid_list_add(mc_uid_list *l, const char *uid);
void mc_uid_list_free(mc_uid_list *l);
/* A deep copy into out; 0 if out of memory (out is then empty). */
int mc_uid_list_copy(mc_uid_list *out, const mc_uid_list *from);

/* Tracks that belong to one device's account. */
typedef struct mc_track_group {
  char uid[MC_UID_STRLEN];
  mc_uid_list tracks;
} mc_track_group;

typedef struct mc_track_groups {
  mc_track_group *items;
  int n;
} mc_track_groups;

/* The group for uid, added if need be; NULL if out of memory. */
mc_track_group *mc_track_groups_get(mc_track_groups *g, const char *uid);
void mc_track_groups_free(mc_track_groups *g);

/* What the last receive turn with a peer found. */
typedef struct mc_findings {
  char peer[MC_UID_STRLEN];
  mc_uid_list refused[MC_REFUSED_COUNT];
  mc_track_groups away; /* tracks left to another transcoder, by that transcoder */
  int files_busy;       /* the turn was skipped: another session was fetching */
  int write_failed;
  char write_error[160];
  int unwritable;       /* the library folder can't be written: folder_error says why */
  char folder_error[160];
  int disk_full;
  int need_tracks;
  int64_t need_bytes, free_bytes;
} mc_findings;

void mc_findings_free(mc_findings *f);

/* The waiting tracks' by-products (waiting.c): tracks with holders but none that may convert
 * them, by holder; tracks nobody holds, by why. */
typedef struct mc_waiting_extra {
  mc_track_groups no_transcoder;
  mc_track_groups lacks; /* by the member that says it no longer has them */
  mc_track_groups left;  /* by the removed device that imported them */
  mc_uid_list unheld;    /* by nobody known, while every member has reported */
} mc_waiting_extra;

void mc_waiting_extra_free(mc_waiting_extra *x);

/* Hands f (heap) to sync, replacing what it kept for f->peer. Not under lock. */
void mc_sync_store_findings(struct mc_sync *sync, mc_findings *f);
/* A notice of something sync did by itself. Not under lock. */
void mc_sync_add_notice(struct mc_sync *sync, mc_sync_issue_kind kind, const char *uid,
                        int64_t bytes);

/* Appends every issue to p (whose failed, unknown and lost are filled in), through c, on the
 * scheduler's thread. *due_ms: the earliest wall time at which an issue lapses by itself (0:
 * none). */
mc_status mc_issues_build(struct mc_sync *sync, struct mc_sync_conn *c, mc_sync_problems *p,
                          int64_t *due_ms);

void mc_issues_free(mc_sync_issue *items, int n);
int mc_issues_equal(const mc_sync_issue *a, int na, const mc_sync_issue *b, int nb);
/* A deep copy, NULL (with *n_out 0) for none; 0 if out of memory. */
int mc_issues_copy(mc_sync_issue **out, const mc_sync_issue *from, int n);

/* Holds as capabilities.state and ERROR "hold" name them ("bin", "cancelled", "auto_off",
 * "not_chosen"; "running" for a session already under way, which isn't one). */
const char *mc_hold_name(mc_sync_hold hold);
mc_sync_hold mc_hold_parse(const char *name);
/* Reads capabilities.state JSON text: its hold and until (0 if none). */
void mc_state_parse_hold(const char *json, mc_sync_hold *hold, int64_t *until_ms);
/* This device's capabilities.state as it should read now (JSON text, malloc'd), NULL when
 * there's nothing to say. Keys other parts of sync publish (the archive's, say) go here. */
char *mc_self_state(struct mc_sync *sync, struct mc_sync_conn *c);

#endif
