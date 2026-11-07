/* Shared types for the iPod library: errors, Mac timestamps, PCM formats and tags.
 *
 * Every string is UTF-8, and every path is a UTF-8 path (converted to UTF-16 for Windows
 * internally). Functions that can fail take an optional ipod_error * (NULL is fine) and
 * return 0 / a pointer on success, or a negative code / NULL. */
#ifndef KITHIPOD_COMMON_H
#define KITHIPOD_COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ipod_status {
  IPOD_OK = 0,
  IPOD_ERR_IO = -1,          /* reading or writing a file failed */
  IPOD_ERR_FORMAT = -2,      /* a file isn't what it should be (corrupt, truncated) */
  IPOD_ERR_UNSUPPORTED = -3, /* a model or format the library doesn't handle */
  IPOD_ERR_NOMEM = -4,
  IPOD_ERR_ARG = -5,         /* bad argument */
  IPOD_ERR_NOT_FOUND = -6,
  IPOD_ERR_FULL = -7,        /* no space left on the device */
  IPOD_ERR_ENCODER = -8      /* the ALAC or MP3 encoder failed */
} ipod_status;

typedef struct ipod_error {
  int code;          /* an ipod_status */
  char message[256]; /* English, for logs */
} ipod_error;

/* Seconds between the Mac epoch (1904-01-01) and the Unix epoch. iPod timestamps are
 * unsigned 32-bit seconds since 1904, in the iPod's local time (see ipod_mac_to_unix). */
#define IPOD_MAC_EPOCH_OFFSET 2082844800u

/* Mac timestamp <-> Unix time. tz_offset is the iPod's offset from UTC in seconds (east
 * positive): the iPod writes local time, so pass the computer's offset for the time the
 * play happened, or 0 to treat the stamps as UTC. 0 maps to 0 both ways (never set). */
int64_t ipod_mac_to_unix(uint32_t mac, int32_t tz_offset);
uint32_t ipod_unix_to_mac(int64_t unix_time, int32_t tz_offset);

/* The computer's current offset from UTC in seconds, east positive. */
int32_t ipod_local_tz_offset(void);

/* PCM pushed into the encoders: interleaved, native-endian signed integers. bits is 16
 * (int16_t samples) or 24 (int32_t samples holding a sign-extended 24-bit value). */
typedef struct ipod_pcm_format {
  uint32_t sample_rate;
  uint16_t channels; /* 1 or 2 */
  uint16_t bits;     /* 16 or 24 */
} ipod_pcm_format;

/* Tags written into encoded files and into the iTunesDB. Strings may be NULL; numbers
 * 0 for unset. */
typedef struct ipod_tags {
  const char *title;
  const char *artist;
  const char *album;
  const char *album_artist;
  const char *genre;
  const char *composer;
  const char *comment;
  const char *grouping;
  const char *sort_title;
  const char *sort_artist;
  const char *sort_album;
  const char *sort_album_artist;
  const char *sort_composer;
  int year;
  int track_number, track_total;
  int disc_number, disc_total;
  int compilation; /* 0 or 1 */
  int bpm;
  /* Cover art, JPEG or PNG bytes; may be NULL. */
  const uint8_t *cover;
  size_t cover_len;
} ipod_tags;

#ifdef __cplusplus
}
#endif

#endif
