#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "util/hex.h"

int main(void) {
  char out[17];
  const unsigned char b[] = {0x00, 0x01, 0x7f, 0x80, 0xab, 0xcd, 0xef, 0xff};
  mc_hex(b, sizeof b, out);
  CHECK(!strcmp(out, "00017f80abcdefff"));
  mc_hex(b, 0, out);
  CHECK(!strcmp(out, ""));

  char *e = mc_percent_encode("a-Z_0.~ /&\xc3\xa9", NULL);
  CHECK_STR(e, "a-Z_0.~%20%2F%26%C3%A9");
  free(e);
  e = mc_percent_encode("/a b/c", "/");
  CHECK_STR(e, "/a%20b/c");
  free(e);
  e = mc_percent_encode("", NULL);
  CHECK_STR(e, "");
  free(e);
  return TEST_RESULT();
}
