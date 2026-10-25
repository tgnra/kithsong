#include <stdint.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "test.h"
#include "util/hlc.h"

static uint64_t fake_now;
static uint64_t fake_clock(void) {
  return fake_now;
}

static int is_hex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

static void test_uid(void) {
  char a[MC_UID_STRLEN], b[MC_UID_STRLEN];
  CHECK(mc_uid_new(a) == MC_OK);
  CHECK(mc_uid_new(b) == MC_OK);

  CHECK(strlen(a) == MC_UID_STRLEN - 1);
  CHECK(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
  CHECK(a[14] == '7');                                      /* version */
  CHECK(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b'); /* variant */
  for (int i = 0; i < MC_UID_STRLEN - 1; i++)
    if (i != 8 && i != 13 && i != 18 && i != 23) CHECK(is_hex(a[i]));

  CHECK(strcmp(a, b) != 0);
  /* The first 48 bits are the timestamp, so a later uid never sorts before an earlier one. */
  CHECK(strncmp(a, b, 13) <= 0);
}

static void test_hlc(void) {
  mc_hlc hlc;
  fake_now = 1000;
  mc_hlc_init(&hlc, fake_clock, 0);

  uint64_t t1 = mc_hlc_now(&hlc);
  CHECK(mc_hlc_physical_ms(t1) == 1000);

  /* Same millisecond, or a clock that went backwards: still strictly increasing. */
  uint64_t t2 = mc_hlc_now(&hlc);
  CHECK(t2 == t1 + 1);
  fake_now = 900;
  uint64_t t3 = mc_hlc_now(&hlc);
  CHECK(t3 == t2 + 1);

  /* A peer ahead of us pulls our clock forward. */
  uint64_t remote = (uint64_t)5000 << MC_HLC_COUNTER_BITS;
  uint64_t t4 = mc_hlc_observe(&hlc, remote);
  CHECK(t4 == remote + 1);
  CHECK(mc_hlc_now(&hlc) > t4);

  /* Once wall time passes everything seen, the counter resets. */
  fake_now = 6000;
  CHECK(mc_hlc_now(&hlc) == (uint64_t)6000 << MC_HLC_COUNTER_BITS);
}

int main(void) {
  CHECK(strcmp(mc_version(), "") != 0);
  test_uid();
  test_hlc();
  return TEST_RESULT();
}
