/* Hash-ahead: the SHA-256 of files a sender is about to send, worked out on a thread of its
 * own while it sends the ones before, so that FILE_HDR, which carries the hash, needn't wait
 * for a whole read of its file. One per serving turn (transfer.c); internal to core.
 *
 * Files are queued in the order they will be asked for (HAVE_REQ's, which is the receiver's
 * fetch order), each under a key. The worker hashes at most `ahead` of them beyond the last
 * one taken, so a receiver that asks about many files but fetches few doesn't cost a read
 * of each. Taking a file passes over those queued before it, which are then never hashed if
 * they weren't yet. A job holds its own copy of the path: the worker touches only the file,
 * never the library, so keeping a hash is the taker's business. */
#ifndef MC_SYNC_HASHAHEAD_H
#define MC_SYNC_HASHAHEAD_H

#include <stdint.h>

#include "musiccore/musiccore.h"

#define MC_HASH_AHEAD_STRLEN 65 /* hex SHA-256 plus NUL */

typedef struct mc_hash_ahead mc_hash_ahead;

/* A hash worked out ahead, with the file's size and mtime, the same before and after. */
typedef struct mc_hash_ahead_result {
  char sha[MC_HASH_AHEAD_STRLEN];
  int64_t size, mtime;
} mc_hash_ahead_result;

/* Starts the worker; it hashes up to ahead (at least 1) files past the last one taken. */
mc_status mc_hash_ahead_new(int ahead, mc_hash_ahead **out);
/* Stops the worker (waiting for a hash it is in the middle of) and frees everything. */
void mc_hash_ahead_free(mc_hash_ahead *ha);
/* Queues path under key, after everything queued before. */
mc_status mc_hash_ahead_queue(mc_hash_ahead *ha, const char *key, const char *path);
/* The first file queued under key after the last one taken: 1 and its hash in *out, waiting
 * for the worker if it is hashing it now. 0 if there is none, or the worker hadn't started on
 * it (it won't now), or it couldn't be read whole or changed while it was hashed: the caller
 * hashes it itself. */
int mc_hash_ahead_take(mc_hash_ahead *ha, const char *key, mc_hash_ahead_result *out);
/* Stops the worker, then calls fn for each file hashed but never taken, so its hash can be
 * kept anyway. Takes nothing afterwards. */
void mc_hash_ahead_finish(mc_hash_ahead *ha,
                          void (*fn)(void *user, const char *key, const char *path,
                                     const mc_hash_ahead_result *r),
                          void *user);
/* For tests: how many files the worker hashed, and how many of those were taken. */
void mc_hash_ahead_stats(mc_hash_ahead *ha, int *hashed, int *taken);

#endif
