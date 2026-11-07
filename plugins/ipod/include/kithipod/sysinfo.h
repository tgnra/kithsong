/* Device identity: iPod_Control/Device/SysInfo, the model table, and disk space. */
#ifndef KITHIPOD_SYSINFO_H
#define KITHIPOD_SYSINFO_H

#include "kithipod/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ipod_generation {
  IPOD_GEN_UNKNOWN = 0,
  IPOD_GEN_1,       /* scroll wheel */
  IPOD_GEN_2,       /* touch wheel */
  IPOD_GEN_3,       /* dock connector, buttons above the wheel */
  IPOD_GEN_4,       /* click wheel, grayscale */
  IPOD_GEN_PHOTO,   /* iPod photo / color (4th generation, colour screen) */
  IPOD_GEN_MINI_1,
  IPOD_GEN_MINI_2,
  IPOD_GEN_NANO_1,
  IPOD_GEN_NANO_2,
  IPOD_GEN_VIDEO_1, /* 5th generation; no hash, but untested */
  IPOD_GEN_VIDEO_2, /* 5.5th generation; no hash, but untested */
  IPOD_GEN_SHUFFLE, /* any shuffle: uses iTunesSD, not supported */
  IPOD_GEN_HASHED   /* nano 3G and later, classic, touch: iTunesDB hash, not supported */
} ipod_generation;

typedef enum ipod_support {
  IPOD_SUPPORTED = 0,
  IPOD_SUPPORT_UNKNOWN_MODEL, /* no SysInfo, or a model number not in the table */
  IPOD_UNSUPPORTED_HASH,      /* needs the iTunesDB hash */
  IPOD_UNSUPPORTED_SHUFFLE    /* a shuffle (iTunesSD) */
} ipod_support;

/* One cover-art size the firmware reads from the ArtworkDB, RGB565 little-endian. */
typedef struct ipod_artwork_format {
  uint32_t id;    /* correlation id: the ithmb file is F<id>_1.ithmb */
  uint16_t width, height;
} ipod_artwork_format;

typedef struct ipod_model {
  const char *number;    /* 4 characters, e.g. "9282" */
  double capacity_gb;
  ipod_generation generation;
  const char *name;      /* English, e.g. "iPod (4th generation)" */
  int music_dirs;        /* iPod_Control/Music/F00..F(n-1) */
  const ipod_artwork_format *artwork; /* NULL when the model shows no cover art */
  int n_artwork;
} ipod_model;

typedef struct ipod_sysinfo {
  char model_num[16];     /* ModelNumStr as written, e.g. "M9282" */
  char serial[32];        /* pszSerialNumber */
  char firewire_guid[24]; /* FirewireGuid, upper-case hex without "0x"; "" if absent */
  char board[48];         /* BoardHwName */
  char build_id[48];      /* visibleBuildID */
  const ipod_model *model; /* NULL when unknown */
} ipod_sysinfo;

/* Parses SysInfo text ("Key: Value" lines). Missing keys stay "". Always succeeds. */
void ipod_sysinfo_parse(const char *text, size_t len, ipod_sysinfo *out);
/* Reads <mount>/iPod_Control/Device/SysInfo. IPOD_ERR_NOT_FOUND if it's absent. */
int ipod_sysinfo_read(const char *mount, ipod_sysinfo *out, ipod_error *err);

/* Looks up a model number, with or without its region letter ("M9282", "9282", "MA350"). */
const ipod_model *ipod_model_lookup(const char *model_num);
/* Whether the library can write to a model. NULL means unknown. A model number missing from
 * the table that starts with B or C (after the region letter) is taken to be hashed. */
ipod_support ipod_model_support(const ipod_model *model, const char *model_num);
const char *ipod_generation_name(ipod_generation g);

/* A stable id for the device: "guid:<FirewireGuid>", else "serial:<serial>", else NULL. */
const char *ipod_sysinfo_stable_id(const ipod_sysinfo *si, char *buf, size_t buflen);

/* Total and free bytes of the volume holding path. */
int ipod_disk_space(const char *path, uint64_t *capacity, uint64_t *free_bytes, ipod_error *err);

#ifdef __cplusplus
}
#endif

#endif
