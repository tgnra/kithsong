/* What the iPod records itself between syncs, in iPod_Control/iTunes/:
 *
 * - "Play Counts": an mhdp header, then one entry per mhit in the iTunesDB's track order
 *   (the order at the time the iPod last loaded the database). The entry size grew with
 *   the firmware: 0x0C (play count, last played, bookmark), 0x10 (+ rating), 0x14 (+ an
 *   unknown word), 0x1C (+ skip count, last skipped).
 * - "OTGPlaylistInfo" and "OTGPlaylistInfo_1", "_2"...: an mhpo header, then u32 indexes
 *   into the same track order, one file per On-The-Go playlist.
 *
 * Both only make sense against the iTunesDB they were recorded with, so read them before
 * writing a new database, and delete them when the new one is written (the device layer
 * does this; see device.h). */
#ifndef KITHIPOD_PLAYCOUNTS_H
#define KITHIPOD_PLAYCOUNTS_H

#include "kithipod/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipod_playcount {
  uint32_t play_count;   /* plays since the last sync */
  uint32_t last_played;  /* Mac time, iPod local time; 0 if never */
  uint32_t bookmark_ms;
  int32_t rating;        /* stars * 20, or -1 when the entry has no rating field */
  uint32_t skip_count;   /* 0 when the entry has no skip fields */
  uint32_t last_skipped;
} ipod_playcount;

typedef struct ipod_playcounts {
  ipod_playcount *entries; /* entries[i] is for db->tracks[i] */
  size_t n;
  uint32_t entry_len;
} ipod_playcounts;

int ipod_playcounts_parse(const uint8_t *data, size_t len, ipod_playcounts *out, ipod_error *err);
/* IPOD_ERR_NOT_FOUND when the file doesn't exist (nothing played since the last sync). */
int ipod_playcounts_read(const char *path, ipod_playcounts *out, ipod_error *err);
void ipod_playcounts_free(ipod_playcounts *pc);

typedef struct ipod_otg_list {
  uint32_t *indexes; /* into db->tracks */
  size_t n;
} ipod_otg_list;

typedef struct ipod_otg {
  ipod_otg_list *lists; /* OTGPlaylistInfo first, then _1, _2... (empty lists skipped) */
  size_t n;
} ipod_otg;

int ipod_otg_parse(const uint8_t *data, size_t len, ipod_otg_list *out, ipod_error *err);
/* Reads every OTG file in itunes_dir (iPod_Control/iTunes). No files is not an error. */
int ipod_otg_read_all(const char *itunes_dir, ipod_otg *out, ipod_error *err);
void ipod_otg_free(ipod_otg *otg);
/* Deletes OTGPlaylistInfo and every OTGPlaylistInfo_N. */
int ipod_otg_clear(const char *itunes_dir);

#ifdef __cplusplus
}
#endif

#endif
