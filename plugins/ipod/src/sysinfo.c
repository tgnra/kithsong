#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "kithipod/sysinfo.h"

#include <ctype.h>
#include <string.h>

#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/statvfs.h>
#endif

/* Cover-art sizes per generation (from the ArtworkDB formats iTunes writes). */
static const ipod_artwork_format art_photo[] = {{1017, 56, 56}, {1016, 140, 140}};
static const ipod_artwork_format art_nano[] = {{1031, 42, 42}, {1027, 100, 100}};
static const ipod_artwork_format art_video[] = {{1028, 100, 100}, {1029, 200, 200}};

#define NOART NULL, 0
#define PHOTO art_photo, 2
#define NANO art_nano, 2
#define VIDEO art_video, 2

static const char n1[] = "iPod (1st generation)", n2[] = "iPod (2nd generation)",
                  n3[] = "iPod (3rd generation)", n4[] = "iPod (4th generation)",
                  nphoto[] = "iPod photo", nmini1[] = "iPod mini (1st generation)",
                  nmini2[] = "iPod mini (2nd generation)", nnano1[] = "iPod nano (1st generation)",
                  nnano2[] = "iPod nano (2nd generation)", nvid1[] = "iPod (5th generation)",
                  nvid2[] = "iPod (5.5th generation)", nshuf[] = "iPod shuffle",
                  nhash[] = "iPod with iTunesDB hash";

static const ipod_model models[] = {
    {"8513", 5, IPOD_GEN_1, n1, 20, NOART},
    {"8541", 5, IPOD_GEN_1, n1, 20, NOART},
    {"8697", 5, IPOD_GEN_1, n1, 20, NOART},
    {"8709", 10, IPOD_GEN_1, n1, 20, NOART},
    {"8737", 10, IPOD_GEN_2, n2, 20, NOART},
    {"8740", 10, IPOD_GEN_2, n2, 20, NOART},
    {"8738", 20, IPOD_GEN_2, n2, 50, NOART},
    {"8741", 20, IPOD_GEN_2, n2, 50, NOART},
    {"8976", 10, IPOD_GEN_3, n3, 20, NOART},
    {"8946", 15, IPOD_GEN_3, n3, 50, NOART},
    {"9460", 15, IPOD_GEN_3, n3, 50, NOART},
    {"9244", 20, IPOD_GEN_3, n3, 50, NOART},
    {"8948", 30, IPOD_GEN_3, n3, 50, NOART},
    {"9245", 40, IPOD_GEN_3, n3, 50, NOART},
    {"9282", 20, IPOD_GEN_4, n4, 50, NOART},
    {"9787", 25, IPOD_GEN_4, n4, 50, NOART}, /* U2 */
    {"9268", 40, IPOD_GEN_4, n4, 50, NOART},
    {"9160", 4, IPOD_GEN_MINI_1, nmini1, 6, NOART},
    {"9436", 4, IPOD_GEN_MINI_1, nmini1, 6, NOART},
    {"9435", 4, IPOD_GEN_MINI_1, nmini1, 6, NOART},
    {"9434", 4, IPOD_GEN_MINI_1, nmini1, 6, NOART},
    {"9437", 4, IPOD_GEN_MINI_1, nmini1, 6, NOART},
    {"9800", 4, IPOD_GEN_MINI_2, nmini2, 6, NOART},
    {"9802", 4, IPOD_GEN_MINI_2, nmini2, 6, NOART},
    {"9804", 4, IPOD_GEN_MINI_2, nmini2, 6, NOART},
    {"9806", 4, IPOD_GEN_MINI_2, nmini2, 6, NOART},
    {"9801", 6, IPOD_GEN_MINI_2, nmini2, 20, NOART},
    {"9803", 6, IPOD_GEN_MINI_2, nmini2, 20, NOART},
    {"9805", 6, IPOD_GEN_MINI_2, nmini2, 20, NOART},
    {"9807", 6, IPOD_GEN_MINI_2, nmini2, 20, NOART},
    {"A079", 20, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"A127", 20, IPOD_GEN_PHOTO, nphoto, 50, PHOTO}, /* U2 */
    {"9829", 30, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"9585", 40, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"9830", 60, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"9586", 60, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"S492", 30, IPOD_GEN_PHOTO, nphoto, 50, PHOTO},
    {"A350", 1, IPOD_GEN_NANO_1, nnano1, 3, NANO},
    {"A352", 1, IPOD_GEN_NANO_1, nnano1, 3, NANO},
    {"A004", 2, IPOD_GEN_NANO_1, nnano1, 3, NANO},
    {"A099", 2, IPOD_GEN_NANO_1, nnano1, 3, NANO},
    {"A005", 4, IPOD_GEN_NANO_1, nnano1, 6, NANO},
    {"A107", 4, IPOD_GEN_NANO_1, nnano1, 6, NANO},
    {"A477", 2, IPOD_GEN_NANO_2, nnano2, 3, NANO},
    {"A426", 4, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A428", 4, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A487", 4, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A489", 4, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A725", 4, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A726", 8, IPOD_GEN_NANO_2, nnano2, 6, NANO},
    {"A497", 8, IPOD_GEN_NANO_2, nnano2, 14, NANO},
    {"A002", 30, IPOD_GEN_VIDEO_1, nvid1, 50, VIDEO},
    {"A146", 30, IPOD_GEN_VIDEO_1, nvid1, 50, VIDEO},
    {"A003", 60, IPOD_GEN_VIDEO_1, nvid1, 50, VIDEO},
    {"A147", 60, IPOD_GEN_VIDEO_1, nvid1, 50, VIDEO},
    {"A452", 30, IPOD_GEN_VIDEO_1, nvid1, 50, VIDEO},
    {"A444", 30, IPOD_GEN_VIDEO_2, nvid2, 50, VIDEO},
    {"A446", 30, IPOD_GEN_VIDEO_2, nvid2, 50, VIDEO},
    {"A664", 30, IPOD_GEN_VIDEO_2, nvid2, 50, VIDEO},
    {"A448", 80, IPOD_GEN_VIDEO_2, nvid2, 50, VIDEO},
    {"A450", 80, IPOD_GEN_VIDEO_2, nvid2, 50, VIDEO},
    /* Shuffles (iTunesSD). */
    {"9724", 0.5, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"9725", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"A546", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"A947", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"A949", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"A951", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"A953", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B225", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B233", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B231", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B227", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B228", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B229", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B518", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B520", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B522", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B524", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B526", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C167", 1, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C306", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C323", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C381", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C384", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C387", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"B867", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C164", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C303", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C307", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C328", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C331", 4, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C584", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C585", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C749", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C750", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    {"C751", 2, IPOD_GEN_SHUFFLE, nshuf, 3, NOART},
    /* Hashed: nano 3G, classic. Listed so they're refused by name; anything else starting
     * with B or C is refused too. */
    {"A978", 4, IPOD_GEN_HASHED, nhash, 6, NOART},
    {"A980", 8, IPOD_GEN_HASHED, nhash, 14, NOART},
    {"B261", 8, IPOD_GEN_HASHED, nhash, 14, NOART},
    {"B249", 8, IPOD_GEN_HASHED, nhash, 14, NOART},
    {"B253", 8, IPOD_GEN_HASHED, nhash, 14, NOART},
    {"B257", 8, IPOD_GEN_HASHED, nhash, 14, NOART},
    {"B029", 80, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"B147", 80, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"B145", 160, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"B150", 160, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"B562", 120, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"B565", 120, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"C293", 160, IPOD_GEN_HASHED, nhash, 50, NOART},
    {"C297", 160, IPOD_GEN_HASHED, nhash, 50, NOART},
};

/* The four-character part of a model number: "M9282" -> "9282", "MA350" -> "A350",
 * "xA079" -> "A079". */
static void model_key(const char *num, char key[5]) {
  key[0] = 0;
  if (!num) return;
  while (*num == ' ') num++;
  size_t n = strlen(num);
  while (n && (num[n - 1] == ' ' || num[n - 1] == '\r')) n--;
  if (n >= 5 && isalpha((unsigned char)num[0])) {
    num++;
    n--;
  }
  if (n < 4) return;
  for (int i = 0; i < 4; i++) key[i] = (char)toupper((unsigned char)num[i]);
  key[4] = 0;
}

const ipod_model *ipod_model_lookup(const char *model_num) {
  char key[5];
  model_key(model_num, key);
  if (!key[0]) return NULL;
  for (size_t i = 0; i < sizeof models / sizeof models[0]; i++)
    if (strcmp(models[i].number, key) == 0) return &models[i];
  return NULL;
}

ipod_support ipod_model_support(const ipod_model *model, const char *model_num) {
  if (!model) {
    char key[5];
    model_key(model_num, key);
    if (key[0] == 'B' || key[0] == 'C') return IPOD_UNSUPPORTED_HASH;
    return IPOD_SUPPORT_UNKNOWN_MODEL;
  }
  switch (model->generation) {
    case IPOD_GEN_SHUFFLE: return IPOD_UNSUPPORTED_SHUFFLE;
    case IPOD_GEN_HASHED: return IPOD_UNSUPPORTED_HASH;
    case IPOD_GEN_UNKNOWN: return IPOD_SUPPORT_UNKNOWN_MODEL;
    default: return IPOD_SUPPORTED;
  }
}

const char *ipod_generation_name(ipod_generation g) {
  switch (g) {
    case IPOD_GEN_1: return n1;
    case IPOD_GEN_2: return n2;
    case IPOD_GEN_3: return n3;
    case IPOD_GEN_4: return n4;
    case IPOD_GEN_PHOTO: return nphoto;
    case IPOD_GEN_MINI_1: return nmini1;
    case IPOD_GEN_MINI_2: return nmini2;
    case IPOD_GEN_NANO_1: return nnano1;
    case IPOD_GEN_NANO_2: return nnano2;
    case IPOD_GEN_VIDEO_1: return nvid1;
    case IPOD_GEN_VIDEO_2: return nvid2;
    case IPOD_GEN_SHUFFLE: return nshuf;
    case IPOD_GEN_HASHED: return nhash;
    default: return "iPod";
  }
}

static void copy_value(char *dst, size_t cap, const char *v, size_t n) {
  while (n && (v[0] == ' ' || v[0] == '\t')) {
    v++;
    n--;
  }
  while (n && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) n--;
  if (n >= cap) n = cap - 1;
  memcpy(dst, v, n);
  dst[n] = 0;
}

void ipod_sysinfo_parse(const char *text, size_t len, ipod_sysinfo *out) {
  memset(out, 0, sizeof *out);
  size_t i = 0;
  while (i < len) {
    size_t eol = i;
    while (eol < len && text[eol] != '\n') eol++;
    const char *line = text + i;
    size_t n = eol - i;
    const char *colon = memchr(line, ':', n);
    if (colon) {
      size_t klen = (size_t)(colon - line);
      const char *v = colon + 1;
      size_t vlen = n - klen - 1;
#define KEY(k) (klen == sizeof(k) - 1 && memcmp(line, k, klen) == 0)
      if (KEY("ModelNumStr")) copy_value(out->model_num, sizeof out->model_num, v, vlen);
      else if (KEY("pszSerialNumber")) copy_value(out->serial, sizeof out->serial, v, vlen);
      else if (KEY("BoardHwName")) copy_value(out->board, sizeof out->board, v, vlen);
      else if (KEY("visibleBuildID")) copy_value(out->build_id, sizeof out->build_id, v, vlen);
      else if (KEY("FirewireGuid")) {
        char tmp[40];
        copy_value(tmp, sizeof tmp, v, vlen);
        const char *h = tmp;
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) h += 2;
        size_t k = 0;
        for (; h[k] && k < sizeof out->firewire_guid - 1; k++) {
          if (!isxdigit((unsigned char)h[k])) break;
          out->firewire_guid[k] = (char)toupper((unsigned char)h[k]);
        }
        out->firewire_guid[k] = 0;
        /* An all-zero GUID identifies nothing. */
        if (strspn(out->firewire_guid, "0") == k) out->firewire_guid[0] = 0;
      }
#undef KEY
    }
    i = eol + 1;
  }
  out->model = ipod_model_lookup(out->model_num);
}

int ipod_sysinfo_read(const char *mount, ipod_sysinfo *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  char *path = ipod_path_join(mount, "iPod_Control/Device/SysInfo");
  if (!path) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  size_t len = 0;
  uint8_t *data = ipod_read_file(path, &len);
  free(path);
  if (!data) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no iPod_Control/Device/SysInfo");
  ipod_sysinfo_parse((const char *)data, len, out);
  free(data);
  return 0;
}

const char *ipod_sysinfo_stable_id(const ipod_sysinfo *si, char *buf, size_t buflen) {
  if (si->firewire_guid[0]) snprintf(buf, buflen, "guid:%s", si->firewire_guid);
  else if (si->serial[0]) snprintf(buf, buflen, "serial:%s", si->serial);
  else return NULL;
  return buf;
}

int ipod_disk_space(const char *path, uint64_t *capacity, uint64_t *free_bytes, ipod_error *err) {
#ifdef _WIN32
  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  if (n <= 0) return ipod_seterr(err, IPOD_ERR_ARG, "bad path");
  wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
  if (!w) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
  ULARGE_INTEGER avail, total, freeb;
  BOOL ok = GetDiskFreeSpaceExW(w, &avail, &total, &freeb);
  free(w);
  if (!ok) return ipod_seterr(err, IPOD_ERR_IO, "can't read free space of %s", path);
  if (capacity) *capacity = total.QuadPart;
  if (free_bytes) *free_bytes = avail.QuadPart;
#else
  struct statvfs st;
  if (statvfs(path, &st) != 0) return ipod_seterr(err, IPOD_ERR_IO, "can't read free space of %s", path);
  if (capacity) *capacity = (uint64_t)st.f_blocks * st.f_frsize;
  if (free_bytes) *free_bytes = (uint64_t)st.f_bavail * st.f_frsize;
#endif
  return 0;
}
