/* The music server a Cast device fetches what it plays from, as Android's CastServer.kt does:
 * it runs only while a cast is connected, on this computer's address on the network the
 * device was reached on, at MC_CAST_MUSIC_PORT (or a port the system picks, if that's
 * taken). Every URL starts with a token, random for each server, and serves only what has
 * been put up (the tracks the device has been given and their covers), so nothing else on
 * this computer can be fetched, by the device or anyone else on the network. Tracks are served with their type and length, and with byte
 * ranges (206), which the device uses to seek. Each connection is answered on a thread of
 * its own (a few at once), a request at a time, and closed.
 *
 * What Cast can't play is converted as it's served: see cast_http_content_type(). Internal
 * to core; the parsing functions are separate so they can be tested. */
#ifndef MC_CAST_CAST_HTTP_H
#define MC_CAST_CAST_HTTP_H

#include <stddef.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

enum {
  CAST_HTTP_PATH_LEN = 256,
  CAST_HTTP_RANGE_LEN = 128,
  CAST_HTTP_MAX_HEAD = 16 * 1024, /* request line and headers */
  CAST_WAV_HEADER = 44
};

typedef struct cast_http_request {
  char method[8];
  char path[CAST_HTTP_PATH_LEN]; /* as sent, query included */
  int has_range;
  char range[CAST_HTTP_RANGE_LEN];
} cast_http_request;

/* Parses a request's head from buf: 1 once it has the blank line that ends it, 0 while more
 * is needed, -1 if it's malformed or longer than CAST_HTTP_MAX_HEAD. */
int cast_http_parse_request(const char *buf, size_t len, cast_http_request *out);

/* What a Range header asks for of length bytes, as Android's CastServer.parseRange: "bytes=a-b",
 * "bytes=a-" or the last n, "bytes=-n"; of several ranges, only the first. 1 with the first
 * and last byte in *from and *to; 0 if it's none of those, which is ignored and the whole
 * file sent, as HTTP has it; -1 if it starts past the end (416). */
int cast_http_range(const char *header, int64_t length, int64_t *from, int64_t *to);

/* The type a track is served as, by its extension: audio/flac, audio/mpeg, audio/ogg (Opus),
 * audio/wav. Cast can't play AIFF, so *as_wav says to serve that as 24-bit PCM WAV, decoded
 * as it's sent: the same samples, so nothing is lost, and the length is known beforehand, so
 * ranges work as for a file. NULL for a file the player can't play either. */
const char *cast_http_content_type(const char *path, int *as_wav);

/* The header of a PCM WAV holding frames frames, into out. Frames past 4 GiB of data
 * overflow the format's sizes, which are then cut to the maximum. */
void cast_http_wav_header(unsigned char out[CAST_WAV_HEADER], unsigned channels, unsigned rate,
                          unsigned bits, uint64_t frames);

/* --- The server --- */

typedef struct cast_server cast_server;

/* Starts serving on address (numeric, the local end of the connection to the device). */
mc_status cast_server_start(const char *address, cast_server **out);
/* "http://<address>:<port>/<token>/": what every URL starts with. */
const char *cast_server_base(const cast_server *server);
/* The port it listens on. */
uint16_t cast_server_port(const cast_server *server);
/* Whether anything has connected to it yet. A device that says a track failed without
 * having connected couldn't reach it at all: a firewall, most likely. */
int cast_server_reached(cast_server *server);
/* Puts up a track file as name (letters, digits and dots), with its type, converted to WAV if
 * as_wav. Replaces what name had. */
mc_status cast_server_add_file(cast_server *server, const char *name, const char *path,
                               const char *content_type, int as_wav);
/* Puts up an image (copied) as name. */
mc_status cast_server_add_bytes(cast_server *server, const char *name, const unsigned char *data,
                                size_t len, const char *content_type);
/* Takes down all but the n names. */
void cast_server_keep(cast_server *server, const char *const *names, int n);
/* Stops listening, drops every connection, waits for its threads and frees it. NULL is a
 * no-op. */
void cast_server_stop(cast_server *server);

#endif
