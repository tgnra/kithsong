/* SysInfo parsing, the model table, and disk space. */
#include <stdlib.h>

#include "fixtures.h"
#include "kithipod/sysinfo.h"

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";

  /* A 4th generation iPod's SysInfo, as iTunes leaves it (CRLF, padded values). */
  const char *text =
      "BoardHwName: iPod Q22\r\n"
      "pszSerialNumber: JQ5123ABCDE\r\n"
      "ModelNumStr: M9282\r\n"
      "FirewireGuid: 0x000a2700137f5d2a\r\n"
      "HddFirmwareRev:  BU005A  \r\n"
      "visibleBuildID: 0x03118000 (3.1.1)\r\n";
  ipod_sysinfo si;
  ipod_sysinfo_parse(text, strlen(text), &si);
  CHECK_STR(si.model_num, "M9282");
  CHECK_STR(si.serial, "JQ5123ABCDE");
  CHECK_STR(si.firewire_guid, "000A2700137F5D2A");
  CHECK_STR(si.board, "iPod Q22");
  CHECK_STR(si.build_id, "0x03118000 (3.1.1)");
  CHECK(si.model != NULL);
  if (si.model) {
    CHECK_EQ(si.model->generation, IPOD_GEN_4);
    CHECK_EQ(si.model->music_dirs, 50);
    CHECK(si.model->artwork == NULL); /* grayscale: no covers */
  }
  CHECK_EQ(ipod_model_support(si.model, si.model_num), IPOD_SUPPORTED);
  char id[64];
  CHECK_STR(ipod_sysinfo_stable_id(&si, id, sizeof id), "guid:000A2700137F5D2A");

  /* No GUID (or an all-zero one): the serial number. */
  const char *text2 = "pszSerialNumber: 4H5ABC\nFirewireGuid: 0x0000000000000000\n";
  ipod_sysinfo_parse(text2, strlen(text2), &si);
  CHECK_STR(si.firewire_guid, "");
  CHECK_STR(ipod_sysinfo_stable_id(&si, id, sizeof id), "serial:4H5ABC");
  ipod_sysinfo_parse("", 0, &si);
  CHECK(ipod_sysinfo_stable_id(&si, id, sizeof id) == NULL);
  CHECK_EQ(ipod_model_support(si.model, si.model_num), IPOD_SUPPORT_UNKNOWN_MODEL);

  /* Region letters, colour models and their cover sizes. */
  const ipod_model *m = ipod_model_lookup("MA079");
  CHECK(m && m->generation == IPOD_GEN_PHOTO && m->n_artwork == 2);
  if (m && m->artwork) {
    CHECK_EQ(m->artwork[0].id, 1017);
    CHECK_EQ(m->artwork[0].width, 56);
    CHECK_EQ(m->artwork[1].id, 1016);
    CHECK_EQ(m->artwork[1].width, 140);
  }
  m = ipod_model_lookup("PA350");
  CHECK(m && m->generation == IPOD_GEN_NANO_1 && m->artwork && m->artwork[1].width == 100);
  m = ipod_model_lookup("xA497");
  CHECK(m && m->generation == IPOD_GEN_NANO_2);
  m = ipod_model_lookup("M9800");
  CHECK(m && m->generation == IPOD_GEN_MINI_2 && m->artwork == NULL);
  CHECK_EQ(ipod_model_support(ipod_model_lookup("M8513"), "M8513"), IPOD_SUPPORTED);

  /* Refused: hashed models (listed, or any unknown B/C number) and shuffles. */
  CHECK_EQ(ipod_model_support(ipod_model_lookup("MB029"), "MB029"), IPOD_UNSUPPORTED_HASH);
  CHECK_EQ(ipod_model_support(ipod_model_lookup("MA978"), "MA978"), IPOD_UNSUPPORTED_HASH);
  CHECK(ipod_model_lookup("MC999") == NULL);
  CHECK_EQ(ipod_model_support(NULL, "MC999"), IPOD_UNSUPPORTED_HASH);
  CHECK_EQ(ipod_model_support(NULL, "MA999"), IPOD_SUPPORT_UNKNOWN_MODEL);
  CHECK_EQ(ipod_model_support(ipod_model_lookup("M9724"), "M9724"), IPOD_UNSUPPORTED_SHUFFLE);

  /* From a volume. */
  char *mount = test_scratch(root, "ipod");
  CHECK_EQ(ipod_sysinfo_read(mount, &si, NULL), IPOD_ERR_NOT_FOUND);
  test_fake_ipod(mount, "MA079");
  CHECK_EQ(ipod_sysinfo_read(mount, &si, NULL), 0);
  CHECK(si.model && si.model->generation == IPOD_GEN_PHOTO);

  uint64_t cap = 0, avail = 0;
  CHECK_EQ(ipod_disk_space(mount, &cap, &avail, NULL), 0);
  CHECK(cap > 0 && avail > 0 && avail <= cap);
  free(mount);
  return TEST_RESULT();
}
