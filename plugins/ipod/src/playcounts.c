#include "kithipod/playcounts.h"

#include <string.h>

#include "util.h"

int ipod_playcounts_parse(const uint8_t *data, size_t len, ipod_playcounts *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  if (len < 16 || memcmp(data, "mhdp", 4) != 0)
    return ipod_seterr(err, IPOD_ERR_FORMAT, "not a Play Counts file (no mhdp)");
  uint32_t hlen = ipod_rd32(data + 4), elen = ipod_rd32(data + 8), n = ipod_rd32(data + 12);
  if (hlen < 16 || hlen > len) return ipod_seterr(err, IPOD_ERR_FORMAT, "Play Counts: bad header length %u", hlen);
  if (elen < 0x0C) return ipod_seterr(err, IPOD_ERR_FORMAT, "Play Counts: entries too short (%u)", elen);
  if ((uint64_t)n * elen > len - hlen) return ipod_seterr(err, IPOD_ERR_FORMAT, "Play Counts: truncated");
  out->entries = calloc(n ? n : 1, sizeof *out->entries);
  if (!out->entries) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  out->n = n;
  out->entry_len = elen;
  for (uint32_t i = 0; i < n; i++) {
    const uint8_t *e = data + hlen + (size_t)i * elen;
    ipod_playcount *pc = &out->entries[i];
    pc->play_count = ipod_rd32(e);
    pc->last_played = ipod_rd32(e + 4);
    pc->bookmark_ms = ipod_rd32(e + 8);
    pc->rating = elen >= 0x10 ? (int32_t)ipod_rd32(e + 12) : -1;
    if (elen >= 0x1C) {
      pc->skip_count = ipod_rd32(e + 20);
      pc->last_skipped = ipod_rd32(e + 24);
    }
  }
  return 0;
}

int ipod_playcounts_read(const char *path, ipod_playcounts *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  if (!ipod_file_exists(path)) return ipod_seterr(err, IPOD_ERR_NOT_FOUND, "no Play Counts file");
  size_t len;
  uint8_t *data = ipod_read_file(path, &len);
  if (!data) return ipod_seterr(err, IPOD_ERR_IO, "can't read %s", path);
  int r = ipod_playcounts_parse(data, len, out, err);
  free(data);
  return r;
}

void ipod_playcounts_free(ipod_playcounts *pc) {
  free(pc->entries);
  memset(pc, 0, sizeof *pc);
}

int ipod_otg_parse(const uint8_t *data, size_t len, ipod_otg_list *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  if (len < 16 || memcmp(data, "mhpo", 4) != 0)
    return ipod_seterr(err, IPOD_ERR_FORMAT, "not an OTG playlist file (no mhpo)");
  uint32_t hlen = ipod_rd32(data + 4), elen = ipod_rd32(data + 8), n = ipod_rd32(data + 12);
  if (hlen < 16 || hlen > len) return ipod_seterr(err, IPOD_ERR_FORMAT, "OTG: bad header length %u", hlen);
  if (elen < 4) return ipod_seterr(err, IPOD_ERR_FORMAT, "OTG: entries too short (%u)", elen);
  if ((uint64_t)n * elen > len - hlen) return ipod_seterr(err, IPOD_ERR_FORMAT, "OTG: truncated");
  out->indexes = malloc((n ? n : 1) * sizeof *out->indexes);
  if (!out->indexes) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  out->n = n;
  for (uint32_t i = 0; i < n; i++) out->indexes[i] = ipod_rd32(data + hlen + (size_t)i * elen);
  return 0;
}

/* OTG files beyond _N with gaps aren't expected; looking this far covers renumbering. */
#define OTG_MAX_FILES 100

int ipod_otg_read_all(const char *itunes_dir, ipod_otg *out, ipod_error *err) {
  memset(out, 0, sizeof *out);
  out->lists = calloc(OTG_MAX_FILES + 1, sizeof *out->lists);
  if (!out->lists) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  int misses = 0;
  for (int i = 0; i <= OTG_MAX_FILES && misses < 5; i++) {
    char name[32];
    if (i == 0) snprintf(name, sizeof name, "OTGPlaylistInfo");
    else snprintf(name, sizeof name, "OTGPlaylistInfo_%d", i);
    char *path = ipod_path_join(itunes_dir, name);
    if (!path) {
      ipod_otg_free(out);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
    size_t len;
    uint8_t *data = ipod_file_exists(path) ? ipod_read_file(path, &len) : NULL;
    free(path);
    if (!data) {
      if (i > 0) misses++;
      continue;
    }
    misses = 0;
    ipod_otg_list l;
    int r = ipod_otg_parse(data, len, &l, err);
    free(data);
    if (r != 0) {
      ipod_otg_free(out);
      return r;
    }
    if (l.n) out->lists[out->n++] = l;
    else free(l.indexes);
  }
  return 0;
}

void ipod_otg_free(ipod_otg *otg) {
  for (size_t i = 0; otg->lists && i < otg->n; i++) free(otg->lists[i].indexes);
  free(otg->lists);
  memset(otg, 0, sizeof *otg);
}

int ipod_otg_clear(const char *itunes_dir) {
  int r = 0;
  for (int i = 0; i <= OTG_MAX_FILES; i++) {
    char name[32];
    if (i == 0) snprintf(name, sizeof name, "OTGPlaylistInfo");
    else snprintf(name, sizeof name, "OTGPlaylistInfo_%d", i);
    char *path = ipod_path_join(itunes_dir, name);
    if (!path) return IPOD_ERR_NOMEM;
    if (ipod_remove(path) != 0) r = IPOD_ERR_IO;
    free(path);
  }
  return r;
}
