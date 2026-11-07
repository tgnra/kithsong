#include "kithipod/artwork.h"

#include <string.h>

#include "util.h"

typedef struct art_entry {
  uint64_t dbid;
  uint32_t image_id;
  uint8_t *mhii;    /* the original chunk, for entries read from the file */
  uint32_t mhii_len;
  uint8_t **pixels; /* new entries: one picture per format, not yet in an .ithmb */
  uint32_t source_size;
} art_entry;

struct ipod_artdb {
  char *dir; /* iPod_Control/Artwork */
  ipod_artwork_format formats[4];
  int n_formats;
  art_entry *e;
  size_t n, cap;
  uint8_t *mhfd_header;
  uint32_t mhfd_header_len;
  uint32_t next_id;
  int dirty;
};

static void entry_free(art_entry *e, int n_formats) {
  free(e->mhii);
  if (e->pixels) {
    for (int i = 0; i < n_formats; i++) free(e->pixels[i]);
    free(e->pixels);
  }
  memset(e, 0, sizeof *e);
}

void ipod_artdb_free(ipod_artdb *a) {
  if (!a) return;
  for (size_t i = 0; i < a->n; i++) entry_free(&a->e[i], a->n_formats);
  free(a->e);
  free(a->dir);
  free(a->mhfd_header);
  free(a);
}

int ipod_artdb_enabled(const ipod_artdb *a) { return a && a->n_formats > 0; }
int ipod_artdb_dirty(const ipod_artdb *a) { return a && a->dirty; }

static art_entry *push(ipod_artdb *a) {
  if (a->n == a->cap) {
    size_t cap = a->cap ? a->cap * 2 : 64;
    art_entry *n = realloc(a->e, cap * sizeof *n);
    if (!n) return NULL;
    a->e = n;
    a->cap = cap;
  }
  art_entry *e = &a->e[a->n++];
  memset(e, 0, sizeof *e);
  return e;
}

/* Reads the mhii list of an existing ArtworkDB. Anything unexpected and the old covers are
 * simply not kept: the iPod then shows no cover for those tracks until they're rewritten. */
static void parse_artdb(ipod_artdb *a, const uint8_t *d, size_t len) {
  if (len < 0x20 || memcmp(d, "mhfd", 4) != 0) return;
  uint32_t hlen = ipod_rd32(d + 4), total = ipod_rd32(d + 8);
  if (hlen < 0x20 || hlen > len || total > len) return;
  a->mhfd_header = malloc(hlen);
  if (a->mhfd_header) {
    memcpy(a->mhfd_header, d, hlen);
    a->mhfd_header_len = hlen;
  }
  uint32_t next = ipod_rd32(d + 28);
  if (next > a->next_id) a->next_id = next;
  uint32_t n_sd = ipod_rd32(d + 20);
  size_t off = hlen;
  for (uint32_t s = 0; s < n_sd && off + 16 <= total; s++) {
    const uint8_t *sd = d + off;
    if (memcmp(sd, "mhsd", 4) != 0) return;
    uint32_t sh = ipod_rd32(sd + 4), st = ipod_rd32(sd + 8);
    if (sh < 16 || st < sh || off + st > total) return;
    uint16_t index = ipod_rd16(sd + 12);
    if (index == 1 && off + sh + 12 <= total && memcmp(sd + sh, "mhli", 4) == 0) {
      const uint8_t *li = sd + sh;
      uint32_t lh = ipod_rd32(li + 4), count = ipod_rd32(li + 8);
      size_t p = off + sh + lh;
      for (uint32_t i = 0; i < count && p + 0x1C <= off + st; i++) {
        const uint8_t *ii = d + p;
        if (memcmp(ii, "mhii", 4) != 0) break;
        uint32_t it = ipod_rd32(ii + 8);
        if (it < 0x1C || p + it > off + st) break;
        art_entry *e = push(a);
        if (!e) return;
        e->image_id = ipod_rd32(ii + 16);
        e->dbid = ipod_rd64(ii + 20);
        e->mhii = malloc(it);
        if (!e->mhii) {
          a->n--;
          return;
        }
        memcpy(e->mhii, ii, it);
        e->mhii_len = it;
        if (e->image_id >= a->next_id) a->next_id = e->image_id + 1;
        p += it;
      }
    }
    off += st;
  }
}

ipod_artdb *ipod_artdb_load(const char *mount, const ipod_artwork_format *formats, int n_formats,
                            ipod_error *err) {
  ipod_artdb *a = calloc(1, sizeof *a);
  if (!a) {
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  a->next_id = 0x64;
  if (n_formats > 4) n_formats = 4;
  for (int i = 0; i < n_formats; i++) a->formats[i] = formats[i];
  a->n_formats = formats ? n_formats : 0;
  a->dir = ipod_path_join(mount, "iPod_Control/Artwork");
  if (!a->dir) {
    ipod_artdb_free(a);
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  if (!a->n_formats) return a;
  char *path = ipod_path_join(a->dir, "ArtworkDB");
  size_t len;
  uint8_t *d = path && ipod_file_exists(path) ? ipod_read_file(path, &len) : NULL;
  free(path);
  if (d) {
    parse_artdb(a, d, len);
    free(d);
  }
  return a;
}

static art_entry *find(const ipod_artdb *a, uint64_t dbid) {
  for (size_t i = 0; i < a->n; i++)
    if (a->e[i].dbid == dbid) return &a->e[i];
  return NULL;
}

int ipod_artdb_has(const ipod_artdb *a, uint64_t dbid) { return a && find(a, dbid) != NULL; }

void ipod_artdb_remove(ipod_artdb *a, uint64_t dbid) {
  if (!a) return;
  for (size_t i = 0; i < a->n; i++) {
    if (a->e[i].dbid == dbid) {
      entry_free(&a->e[i], a->n_formats);
      memmove(a->e + i, a->e + i + 1, (a->n - i - 1) * sizeof *a->e);
      a->n--;
      a->dirty = 1;
      return;
    }
  }
}

int ipod_artdb_set(ipod_artdb *a, uint64_t dbid, const uint8_t *image, size_t len, ipod_error *err) {
  if (!ipod_artdb_enabled(a)) return 0;
  uint8_t **pixels = calloc((size_t)a->n_formats, sizeof *pixels);
  if (!pixels) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  for (int i = 0; i < a->n_formats; i++) {
    const ipod_artwork_format *f = &a->formats[i];
    pixels[i] = malloc((size_t)f->width * f->height * 2);
    int r = pixels[i] ? ipod_image_to_rgb565(image, len, f->width, f->height, pixels[i]) : IPOD_ERR_NOMEM;
    if (r != 0) {
      for (int k = 0; k <= i; k++) free(pixels[k]);
      free(pixels);
      return ipod_seterr(err, r, r == IPOD_ERR_NOMEM ? "out of memory" : "can't decode the cover picture");
    }
  }
  ipod_artdb_remove(a, dbid);
  art_entry *e = push(a);
  if (!e) {
    for (int k = 0; k < a->n_formats; k++) free(pixels[k]);
    free(pixels);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  e->dbid = dbid;
  e->image_id = a->next_id++;
  e->pixels = pixels;
  e->source_size = (uint32_t)len;
  a->dirty = 1;
  return 0;
}

/* --- Writing ------------------------------------------------------------------------- */

static void put_name_mhod(ipod_buf *b, const char *name) {
  size_t n;
  uint8_t *u = ipod_utf8_to_utf16le(name, &n);
  if (!u) {
    b->oom = 1;
    return;
  }
  size_t pad = (4 - ((36 + n) % 4)) % 4;
  size_t at = b->len;
  ipod_buf_tag(b, "mhod");
  ipod_buf_le32(b, 0x18);
  ipod_buf_le32(b, (uint32_t)(36 + n + pad));
  ipod_buf_le16(b, 3);
  ipod_buf_u8(b, 0);
  ipod_buf_u8(b, (uint8_t)pad);
  ipod_buf_zero(b, 8);
  ipod_buf_le32(b, (uint32_t)n);
  ipod_buf_u8(b, 2); /* UTF-16LE */
  ipod_buf_zero(b, 7);
  ipod_buf_put(b, u, n);
  ipod_buf_zero(b, pad);
  free(u);
  (void)at;
}

static void put_mhii(ipod_buf *b, const ipod_artdb *a, const art_entry *e, const uint32_t *offsets) {
  size_t at = b->len;
  ipod_buf_tag(b, "mhii");
  ipod_buf_le32(b, 0x98);
  ipod_buf_le32(b, 0);
  ipod_buf_le32(b, (uint32_t)a->n_formats);
  ipod_buf_le32(b, e->image_id);
  ipod_buf_le64(b, e->dbid);
  ipod_buf_zero(b, 20);          /* unknown, rating, unknown, original and digitized dates */
  ipod_buf_le32(b, e->source_size);
  ipod_buf_zero(b, 0x98 - 0x34);
  for (int i = 0; i < a->n_formats; i++) {
    const ipod_artwork_format *f = &a->formats[i];
    size_t mo = b->len;
    ipod_buf_tag(b, "mhod");
    ipod_buf_le32(b, 0x18);
    ipod_buf_le32(b, 0);
    ipod_buf_le16(b, 2); /* thumbnail container */
    ipod_buf_zero(b, 10);
    size_t no = b->len;
    ipod_buf_tag(b, "mhni");
    ipod_buf_le32(b, 0x4C);
    ipod_buf_le32(b, 0);
    ipod_buf_le32(b, 1);
    ipod_buf_le32(b, f->id);
    ipod_buf_le32(b, offsets[i]);
    ipod_buf_le32(b, (uint32_t)f->width * f->height * 2);
    ipod_buf_le16(b, 0); /* vertical padding */
    ipod_buf_le16(b, 0); /* horizontal padding */
    ipod_buf_le16(b, f->height);
    ipod_buf_le16(b, f->width);
    ipod_buf_zero(b, 0x4C - 0x24);
    char name[32];
    snprintf(name, sizeof name, ":F%u_1.ithmb", (unsigned)f->id);
    put_name_mhod(b, name);
    ipod_buf_set_le32(b, no + 8, (uint32_t)(b->len - no));
    ipod_buf_set_le32(b, mo + 8, (uint32_t)(b->len - mo));
  }
  ipod_buf_set_le32(b, at + 8, (uint32_t)(b->len - at));
}

static int in_db(const ipod_db *db, uint64_t dbid) { return ipod_db_find_dbid(db, dbid) != NULL; }

int ipod_artdb_write(ipod_artdb *a, const ipod_db *db, ipod_error *err) {
  if (!ipod_artdb_enabled(a)) return 0;
  if (ipod_mkdirs(a->dir) != 0) return ipod_seterr(err, IPOD_ERR_IO, "can't create %s", a->dir);
  /* Drop covers of tracks that are gone. */
  size_t w = 0;
  for (size_t i = 0; i < a->n; i++) {
    if (in_db(db, a->e[i].dbid)) a->e[w++] = a->e[i];
    else entry_free(&a->e[i], a->n_formats);
  }
  a->n = w;

  /* Append new pictures to the .ithmb files, noting where each went. */
  uint32_t *offsets = calloc(a->n ? a->n * (size_t)a->n_formats : 1, sizeof *offsets);
  if (!offsets) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  for (int f = 0; f < a->n_formats; f++) {
    char name[32];
    snprintf(name, sizeof name, "F%u_1.ithmb", (unsigned)a->formats[f].id);
    char *path = ipod_path_join(a->dir, name);
    if (!path) {
      free(offsets);
      return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    }
    int64_t size = ipod_file_size(path);
    if (size < 0) size = 0;
    FILE *fp = NULL;
    size_t bytes = (size_t)a->formats[f].width * a->formats[f].height * 2;
    for (size_t i = 0; i < a->n; i++) {
      if (!a->e[i].pixels || !in_db(db, a->e[i].dbid)) continue;
      if (!fp && !(fp = ipod_fopen(path, "ab"))) break;
      if (fwrite(a->e[i].pixels[f], 1, bytes, fp) != bytes) {
        fclose(fp);
        fp = NULL;
        size = -1;
        break;
      }
      offsets[i * (size_t)a->n_formats + (size_t)f] = (uint32_t)size;
      size += (int64_t)bytes;
    }
    int bad = size < 0;
    if (fp && fclose(fp) != 0) bad = 1;
    free(path);
    if (bad) {
      free(offsets);
      return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s (disk full?)", name);
    }
  }

  ipod_buf b = {0};
  if (a->mhfd_header && a->mhfd_header_len >= 0x84) {
    ipod_buf_put(&b, a->mhfd_header, a->mhfd_header_len);
  } else {
    uint8_t *h = ipod_buf_grow(&b, 0x84);
    if (h) {
      memcpy(h, "mhfd", 4);
      ipod_wr32(h + 4, 0x84);
      ipod_wr32(h + 16, 2); /* iTunes 4.9 and later; iTunes 7 drops the file if it's 1 */
      h[48] = 2;
    }
  }
  ipod_buf_set_le32(&b, 20, 3);
  ipod_buf_set_le32(&b, 28, a->next_id);

  /* mhsd 1: images. */
  size_t sd = b.len;
  ipod_buf_tag(&b, "mhsd");
  ipod_buf_le32(&b, 0x60);
  ipod_buf_le32(&b, 0);
  ipod_buf_le16(&b, 1);
  ipod_buf_zero(&b, 0x60 - 14);
  size_t li = b.len;
  ipod_buf_tag(&b, "mhli");
  ipod_buf_le32(&b, 0x5C);
  ipod_buf_le32(&b, 0);
  ipod_buf_zero(&b, 0x5C - 12);
  uint32_t count = 0;
  for (size_t i = 0; i < a->n; i++) {
    art_entry *e = &a->e[i];
    if (!in_db(db, e->dbid)) continue;
    if (e->pixels) put_mhii(&b, a, e, offsets + i * (size_t)a->n_formats);
    else ipod_buf_put(&b, e->mhii, e->mhii_len);
    count++;
  }
  ipod_buf_set_le32(&b, li + 8, count);
  ipod_buf_set_le32(&b, sd + 8, (uint32_t)(b.len - sd));

  /* mhsd 2: no albums. */
  sd = b.len;
  ipod_buf_tag(&b, "mhsd");
  ipod_buf_le32(&b, 0x60);
  ipod_buf_le32(&b, 0x60 + 0x5C);
  ipod_buf_le16(&b, 2);
  ipod_buf_zero(&b, 0x60 - 14);
  ipod_buf_tag(&b, "mhla");
  ipod_buf_le32(&b, 0x5C);
  ipod_buf_le32(&b, 0);
  ipod_buf_zero(&b, 0x5C - 12);

  /* mhsd 3: one file per size. */
  sd = b.len;
  ipod_buf_tag(&b, "mhsd");
  ipod_buf_le32(&b, 0x60);
  ipod_buf_le32(&b, 0);
  ipod_buf_le16(&b, 3);
  ipod_buf_zero(&b, 0x60 - 14);
  ipod_buf_tag(&b, "mhlf");
  ipod_buf_le32(&b, 0x5C);
  ipod_buf_le32(&b, (uint32_t)a->n_formats);
  ipod_buf_zero(&b, 0x5C - 12);
  for (int f = 0; f < a->n_formats; f++) {
    ipod_buf_tag(&b, "mhif");
    ipod_buf_le32(&b, 0x7C);
    ipod_buf_le32(&b, 0x7C);
    ipod_buf_le32(&b, 0);
    ipod_buf_le32(&b, a->formats[f].id);
    ipod_buf_le32(&b, (uint32_t)a->formats[f].width * a->formats[f].height * 2);
    ipod_buf_zero(&b, 0x7C - 24);
  }
  ipod_buf_set_le32(&b, sd + 8, (uint32_t)(b.len - sd));
  ipod_buf_set_le32(&b, 8, (uint32_t)b.len);

  if (b.oom) {
    free(offsets);
    ipod_buf_free(&b);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  char *path = ipod_path_join(a->dir, "ArtworkDB");
  int r = path ? ipod_write_file_atomic(path, b.data, b.len, err) : ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  free(path);
  ipod_buf_free(&b);
  if (r != 0) {
    free(offsets);
    return r;
  }

  /* The new pictures are now in the files: keep them as plain entries from here on, with the
   * mhii as written, so a second write doesn't append them again. */
  for (size_t i = 0; i < a->n; i++) {
    art_entry *e = &a->e[i];
    if (!e->pixels) continue;
    ipod_buf one = {0};
    put_mhii(&one, a, e, offsets + i * (size_t)a->n_formats);
    for (int f = 0; f < a->n_formats; f++) free(e->pixels[f]);
    free(e->pixels);
    e->pixels = NULL;
    if (one.oom) {
      e->dbid = 0; /* lost from memory, but on disk; dropped at the next write */
      ipod_buf_free(&one);
      continue;
    }
    e->mhii = one.data;
    e->mhii_len = (uint32_t)one.len;
  }
  free(offsets);
  a->dirty = 0;
  return 0;
}
