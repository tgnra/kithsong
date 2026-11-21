/* Tiny test harness: CHECK records a failure and keeps going; the test's main returns
 * TEST_RESULT() so CTest sees a non-zero exit code if anything failed. */
#ifndef MC_TEST_H
#define MC_TEST_H

#include <stdio.h>
#include <string.h>

static int test_failures;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      test_failures++;                                                    \
    }                                                                     \
  } while (0)

#define CHECK_STR(a, b) CHECK((a) && (b) && strcmp((a), (b)) == 0)

#define TEST_RESULT() (test_failures ? 1 : 0)

#endif
