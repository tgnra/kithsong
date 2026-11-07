/* Shared test helpers: CHECK macros, scratch directories, a fake iPod volume, and a
 * builder for hand-made iTunesDB chunks. */
#ifndef KITHIPOD_TEST_FIXTURES_H
#define KITHIPOD_TEST_FIXTURES_H

#include <stdio.h>
#include <string.h>

#include "util.h"

extern int test_failures;

#define CHECK(cond)                                                              \
  do {                                                                           \
    if (!(cond)) {                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
      test_failures++;                                                           \
    }                                                                            \
  } while (0)
#define CHECK_STR(a, b) CHECK((a) && (b) && strcmp((a), (b)) == 0)
#define CHECK_EQ(a, b)                                                                            \
  do {                                                                                            \
    long long _a = (long long)(a), _b = (long long)(b);                                           \
    if (_a != _b) {                                                                               \
      fprintf(stderr, "%s:%d: CHECK failed: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, \
              _a, _b);                                                                            \
      test_failures++;                                                                            \
    }                                                                                             \
  } while (0)
#define TEST_RESULT() (test_failures ? 1 : 0)

/* A fresh directory under root (made with its parents); free() the result. */
char *test_scratch(const char *root, const char *name);
int test_write(const char *path, const void *data, size_t len);
/* Makes mount/iPod_Control/{iTunes,Device,Music} and a SysInfo naming model_num. */
void test_fake_ipod(const char *mount, const char *model_num);
/* Whether needle occurs in hay. */
int test_contains(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t needle_len);

/* Chunk builders, little-endian. */
size_t tb_open(ipod_buf *b, const char *tag, uint32_t header_len); /* zero-filled header */
void tb_close(ipod_buf *b, size_t at);                              /* total length at +8 */
void tb_string_mhod(ipod_buf *b, uint32_t type, const char *s, int utf8);
void tb_raw_mhod(ipod_buf *b, uint32_t type, size_t body_len, uint8_t fill);

#endif
