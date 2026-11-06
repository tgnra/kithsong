/* A file's bytes between the disk and a session's channel, with the disk side on a thread of
 * its own, so that reading or hashing and writing overlap the channel's encryption and the
 * socket (transfer.c). Each side hands chunks of up to MC_CHANNEL_MAX_PAYLOAD bytes, one
 * FILE_DATA message's worth, through a small ring of buffers, in order. Internal to core;
 * each reader or writer is used by one session thread, besides its own. */
#ifndef MC_SYNC_FILEPIPE_H
#define MC_SYNC_FILEPIPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "kithcore/kithcore.h"

typedef struct mc_file_reader mc_file_reader;
typedef struct mc_file_writer mc_file_writer;

/* Reads size bytes of f ahead of the caller. Should f end early, the rest reads as zeros. f
 * stays the caller's, to close after mc_file_reader_free(). */
mc_status mc_file_reader_start(FILE *f, int64_t size, mc_file_reader **out);
/* The next chunk, waiting for it if need be, and *len its size; NULL once size bytes have
 * been given. The chunk stays valid until the next call. */
const unsigned char *mc_file_reader_next(mc_file_reader *r, size_t *len);
/* Stops reading (at once, if the caller gives up midway) and frees r. NULL is ignored. */
void mc_file_reader_free(mc_file_reader *r);

/* Hashes (SHA-256) and writes to f what it is given. f stays the caller's, to close after
 * mc_file_writer_finish(). */
mc_status mc_file_writer_start(FILE *f, mc_file_writer **out);
/* Queues a copy of len <= MC_CHANNEL_MAX_PAYLOAD bytes, waiting for room if need be. */
void mc_file_writer_put(mc_file_writer *w, const void *data, size_t len);
/* Waits until everything queued is written, and frees w. Returns whether every write
 * succeeded, and if so digest is the hash of all the bytes given. */
int mc_file_writer_finish(mc_file_writer *w, unsigned char digest[32]);

#endif
