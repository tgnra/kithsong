/* Private helpers: errors, growable byte buffers, little-endian access, UTF-8/UTF-16, files
 * with UTF-8 paths, randomness and a process-wide lock. */
#ifndef KITHIPOD_UTIL_H
#define KITHIPOD_UTIL_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kithipod/common.h"

#if defined(__GNUC__) || defined(__clang__)
#define IPOD_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define IPOD_PRINTF(f, a)
#endif

/* Sets err (if not NULL) and returns code. */
int ipod_seterr(ipod_error *err, int code, const char *fmt, ...) IPOD_PRINTF(3, 4);

/* --- Byte buffers ----------------------------------------------------------- */

typedef struct ipod_buf {
  uint8_t *data;
  size_t len, cap;
  int oom; /* set once an allocation failed; later writes are dropped */
} ipod_buf;

void ipod_buf_free(ipod_buf *b);
/* Makes room for n more bytes; returns a pointer to them (zeroed) or NULL. */
uint8_t *ipod_buf_grow(ipod_buf *b, size_t n);
void ipod_buf_put(ipod_buf *b, const void *p, size_t n);
void ipod_buf_zero(ipod_buf *b, size_t n);
void ipod_buf_u8(ipod_buf *b, uint8_t v);
void ipod_buf_le16(ipod_buf *b, uint16_t v);
void ipod_buf_le32(ipod_buf *b, uint32_t v);
void ipod_buf_le64(ipod_buf *b, uint64_t v);
void ipod_buf_be16(ipod_buf *b, uint16_t v);
void ipod_buf_be32(ipod_buf *b, uint32_t v);
void ipod_buf_be64(ipod_buf *b, uint64_t v);
void ipod_buf_tag(ipod_buf *b, const char tag[4]);
/* Overwrites a value written earlier, at an absolute offset. */
void ipod_buf_set_le32(ipod_buf *b, size_t at, uint32_t v);
void ipod_buf_set_be32(ipod_buf *b, size_t at, uint32_t v);

/* --- Endian access ------------------------------------------------------------ */

static inline uint16_t ipod_rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t ipod_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t ipod_rd64(const uint8_t *p) {
  return (uint64_t)ipod_rd32(p) | ((uint64_t)ipod_rd32(p + 4) << 32);
}
static inline void ipod_wr16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
static inline void ipod_wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}
static inline void ipod_wr64(uint8_t *p, uint64_t v) {
  ipod_wr32(p, (uint32_t)v);
  ipod_wr32(p + 4, (uint32_t)(v >> 32));
}
static inline uint32_t ipod_rdbe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void ipod_wrbe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

/* --- Strings ------------------------------------------------------------------- */

char *ipod_strdup(const char *s);  /* NULL-safe */
char *ipod_strndup(const char *s, size_t n);
char *ipod_asprintf(const char *fmt, ...) IPOD_PRINTF(1, 2);
/* NULL and "" compare equal. */
int ipod_streq(const char *a, const char *b);

/* UTF-16LE bytes (len in bytes) to a new UTF-8 string; invalid units become U+FFFD. */
char *ipod_utf16le_to_utf8(const uint8_t *p, size_t len);
/* UTF-8 to new UTF-16LE bytes; *out_len receives the length in bytes (no terminator).
 * Invalid input bytes become U+FFFD. */
uint8_t *ipod_utf8_to_utf16le(const char *s, size_t *out_len);
/* Decodes one code point, advancing *s; returns 0 at the end. */
uint32_t ipod_utf8_next(const char **s);
/* Whether every byte is ASCII. */
int ipod_is_ascii(const char *s);

/* --- Files ----------------------------------------------------------------------- */

FILE *ipod_fopen(const char *path, const char *mode);
int ipod_file_exists(const char *path);
int ipod_dir_exists(const char *path);
int ipod_mkdir(const char *path);   /* 0 if made or already there */
int ipod_mkdirs(const char *path);  /* every missing parent too */
int ipod_remove(const char *path);  /* 0 if removed or already absent */
/* Replaces dst with src atomically where the OS allows it. */
int ipod_rename_replace(const char *src, const char *dst);
/* Reads a whole file; NULL on error. *out_len receives the size. */
uint8_t *ipod_read_file(const char *path, size_t *out_len);
/* Writes path.tmp, flushes it to disk, then renames it over path. */
int ipod_write_file_atomic(const char *path, const void *data, size_t len, ipod_error *err);
int ipod_copy_file(const char *src, const char *dst, ipod_error *err);
int64_t ipod_file_size(const char *path);
/* Joins with '/', which Windows accepts too. */
char *ipod_path_join(const char *a, const char *b);

/* --- Randomness and locking --------------------------------------------------------- */

/* Fills buf from the OS's CSPRNG (falls back to a time-seeded generator). */
void ipod_random_bytes(void *buf, size_t n);
uint64_t ipod_random_u64(void);

/* One process-wide lock, for library init that isn't thread-safe (LAME's tables). */
void ipod_global_lock(void);
void ipod_global_unlock(void);

#endif
