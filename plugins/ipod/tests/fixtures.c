#include "fixtures.h"

#include <stdlib.h>

int test_failures;

char *test_scratch(const char *root, const char *name) {
  char *d = ipod_asprintf("%s/%s-%08x", root, name, (unsigned)ipod_random_u64());
  if (!d || ipod_mkdirs(d) != 0) {
    fprintf(stderr, "can't make scratch dir under %s\n", root);
    exit(2);
  }
  return d;
}

int test_write(const char *path, const void *data, size_t len) {
  FILE *f = ipod_fopen(path, "wb");
  if (!f) return -1;
  int ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok ? 0 : -1;
}

void test_fake_ipod(const char *mount, const char *model_num) {
  static const char *dirs[] = {"iPod_Control/iTunes", "iPod_Control/Device", "iPod_Control/Music/F00",
                               "iPod_Control/Music/F01"};
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    char *p = ipod_path_join(mount, dirs[i]);
    ipod_mkdirs(p);
    free(p);
  }
  char *si = ipod_path_join(mount, "iPod_Control/Device/SysInfo");
  char *text = ipod_asprintf("BoardHwName: iPod Q22\r\npszSerialNumber: JQ5123ABCDE\r\nModelNumStr: %s\r\n"
                             "FirewireGuid: 0x000A2700137F5D2A\r\nvisibleBuildID: 0x03118000 (3.1.1)\r\n",
                             model_num);
  test_write(si, text, strlen(text));
  free(text);
  free(si);
}

int test_contains(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t needle_len) {
  if (needle_len == 0) return 1;
  for (size_t i = 0; i + needle_len <= hay_len; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, needle_len) == 0) return 1;
  return 0;
}

size_t tb_open(ipod_buf *b, const char *tag, uint32_t header_len) {
  size_t at = b->len;
  uint8_t *h = ipod_buf_grow(b, header_len);
  memcpy(h, tag, 4);
  ipod_wr32(h + 4, header_len);
  return at;
}

void tb_close(ipod_buf *b, size_t at) { ipod_buf_set_le32(b, at + 8, (uint32_t)(b->len - at)); }

void tb_string_mhod(ipod_buf *b, uint32_t type, const char *s, int utf8) {
  size_t n;
  uint8_t *u = utf8 ? (uint8_t *)ipod_strdup(s) : ipod_utf8_to_utf16le(s, &n);
  if (utf8) n = strlen(s);
  size_t at = tb_open(b, "mhod", 24);
  ipod_buf_set_le32(b, at + 12, type);
  ipod_buf_le32(b, utf8 ? 2 : 1);
  ipod_buf_le32(b, (uint32_t)n);
  ipod_buf_le32(b, 0);
  ipod_buf_le32(b, 0);
  ipod_buf_put(b, u, n);
  tb_close(b, at);
  free(u);
}

void tb_raw_mhod(ipod_buf *b, uint32_t type, size_t body_len, uint8_t fill) {
  size_t at = tb_open(b, "mhod", 24);
  ipod_buf_set_le32(b, at + 12, type);
  uint8_t *p = ipod_buf_grow(b, body_len);
  for (size_t i = 0; i < body_len; i++) p[i] = (uint8_t)(fill + i);
  tb_close(b, at);
}
