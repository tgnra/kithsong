/* The file phase of a session (step 6 of "Session" in docs/sync-protocol.md): each side in
 * turn fetches the files its profile wants from the other, then reconciles its library
 * folder with its plan, runs the fast check and writes its sync_status. Also the transcode
 * cache that serves others, and mc_sync_verify(). Internal to core.
 *
 * Turns. The library exchange leaves the two devices in step, and one receives first
 * (session.h's receive_first: an archive facing a non-archive, else the initiator): it sends
 * HAVE_REQ, FILE_REQ and PREPARE_REQ while the other serves them, and ends its turn with
 * FILES_DONE. Then the roles swap. Each device's own reconcile waits
 * until after BYE, when the peer can no longer ask for a file it would delete. */
#ifndef MC_SYNC_TRANSFER_H
#define MC_SYNC_TRANSFER_H

#include "sync/session.h"

/* The transcode cache, in the per-device config dir, and the size the LRU trim keeps it to. */
#define MC_TRANSCODE_CACHE_DIR "transcode-cache"
#define MC_TRANSCODE_CACHE_MAX ((int64_t)8 << 30)

/* Before the library exchange: starts noting the files rows it deletes (their tracks were
 * deleted elsewhere), whose files mc_transfer_end() then removes. */
void mc_transfer_begin(mc_session *s);
/* After the library exchange: both turns. */
mc_status mc_transfer_run(mc_session *s);
/* After BYE (ok), or once the session failed: removes deleted tracks' files, and when this
 * side fetched files, reconciles, runs the fast check and writes sync_status. Always frees
 * what mc_transfer_begin() set up. */
void mc_transfer_end(mc_session *s, int ok);

/* Before a session's HELLO, with this device's other own rows: writes the sync_status the
 * last file phase left pending, so the group exchange carries it. */
void mc_transfer_write_status(mc_sync *sync, mc_sync_conn *c);

/* How a session moves a file to the trash: deleted tracks' originals, and unknown audio
 * files. mc_trash(), unless a test swaps it before syncing. */
extern mc_status (*mc_transfer_trash)(const char *path);

/* Copies the session's file counts into r. */
void mc_transfer_fill_result(const mc_session *s, mc_sync_result *r);

/* For removing files outside a session (here.c): rel's absolute path if sync may delete it
 * (inside the library folder, no links on the way), else NULL; and, after a file at rel
 * went, removing its album and artist folders if only a cover is left in them. */
char *mc_transfer_deletable_path(mc_library *lib, const char *rel);
void mc_transfer_prune_dirs(mc_library *lib, const char *rel);

#endif
