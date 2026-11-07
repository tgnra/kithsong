/* iTunesDB reader and writer. Layout notes (all little-endian):
 *
 *   mhbd  header, then mhsd children
 *   mhsd  type 1: mhlt + mhit*        (tracks)
 *         type 2: mhlp + mhyp*        (playlists, flat)
 *         type 3: mhlp + mhyp*        (the same playlists, with Podcasts grouped by show)
 *         other types (4 album list, 5 smart playlists...) are kept as they were
 *   mhit  header, then mhod children (strings and others)
 *   mhyp  header, then mhods (name, prefs, smart rules, library indexes 52/53), then mhip*
 *   mhip  header, then one mhod 100 (position); before iTunes 4.9 that mhod followed the
 *         mhip instead of being inside it, which the reader accepts.
 *
 * Each chunk starts with a 4-byte tag and its header length; most have the total length
 * (header + children) at offset 8. mhlt and mhlp have their child count there instead. */

#include "kithipod/itunesdb.h"

#include <stddef.h>
#include <string.h>
#include <time.h>

#include "util.h"

typedef struct raw {
  uint8_t *p;
  uint32_t len;
} raw;

static raw raw_copy(const uint8_t *p, uint32_t len) {
  raw r = {NULL, 0};
  r.p = malloc(len ? len : 1);
  if (r.p) {
    memcpy(r.p, p, len);
    r.len = len;
  }
  return r;
}

/* An mhod as it was read: a string one also remembers its text, so it's written back
 * byte for byte when the text hasn't changed. */
typedef struct mhod_slot {
  uint32_t type;
  int is_string; /* a field of ipod_track (or a playlist's name) */
  raw bytes;
  char *orig;
} mhod_slot;

struct ipod_track_priv {
  raw chunk; /* the whole original mhit, mhods included */
  uint32_t header_len;
  mhod_slot *slots;
  size_t n_slots;
};

typedef struct mhip_raw {
  raw header; /* the mhip header only */
  uint32_t track_id;
  int used;
} mhip_raw;

struct ipod_playlist_priv {
  raw chunk;  /* as read from the type 2 dataset */
  raw chunk3; /* the same playlist (by id) from the type 3 dataset, if there was one */
  uint32_t header_len;
  mhod_slot name_slot; /* type 0 when there was none */
  mhod_slot *slots;    /* other mhods, except the library indexes (52, 53) */
  size_t n_slots;
  mhip_raw *mhips;
  size_t n_mhips;
  char *orig_name;
  uint32_t *orig_items;
  size_t n_orig_items;
};

typedef struct dataset {
  uint32_t type;
  raw mhsd_header;
  raw list_header; /* mhlt / mhlp header, for types 1-3 */
  raw chunk;       /* the whole mhsd, for other types */
} dataset;

struct ipod_db_priv {
  raw mhbd_header;
  dataset *ds;
  size_t n_ds;
  uint32_t *orig_track_ids;
  size_t n_orig_tracks;
  int hashed;
};

/* --- String fields ---------------------------------------------------------------- */

typedef struct str_field {
  uint32_t type;
  size_t offset;
} str_field;

static const str_field track_strings[] = {
    {1, offsetof(ipod_track, title)},
    {2, offsetof(ipod_track, location)},
    {3, offsetof(ipod_track, album)},
    {4, offsetof(ipod_track, artist)},
    {5, offsetof(ipod_track, genre)},
    {6, offsetof(ipod_track, filetype_desc)},
    {8, offsetof(ipod_track, comment)},
    {12, offsetof(ipod_track, composer)},
    {13, offsetof(ipod_track, grouping)},
    {22, offsetof(ipod_track, album_artist)},
    {23, offsetof(ipod_track, sort_artist)},
    {27, offsetof(ipod_track, sort_title)},
    {28, offsetof(ipod_track, sort_album)},
    {29, offsetof(ipod_track, sort_album_artist)},
    {30, offsetof(ipod_track, sort_composer)},
};
#define N_TRACK_STRINGS (sizeof track_strings / sizeof track_strings[0])

static char **track_str(ipod_track *t, size_t i) { return (char **)((char *)t + track_strings[i].offset); }

static int find_string_type(uint32_t type) {
  for (size_t i = 0; i < N_TRACK_STRINGS; i++)
    if (track_strings[i].type == type) return (int)i;
  return -1;
}

int ipod_set_str(char **field, const char *v) {
  char *n = NULL;
  if (v) {
    n = ipod_strdup(v);
    if (!n) return IPOD_ERR_NOMEM;
  }
  free(*field);
  *field = n;
  return 0;
}

static int empty(const char *s) { return !s || !*s; }

/* Decodes a string mhod; NULL if it isn't a well-formed one. */
static char *parse_string_mhod(const uint8_t *p, uint32_t len) {
  if (len < 40) return NULL;
  uint32_t enc = ipod_rd32(p + 24);
  uint32_t slen = ipod_rd32(p + 28);
  if (slen > len - 40) return NULL;
  if (enc == 2) return ipod_strndup((const char *)p + 40, slen);
  return ipod_utf16le_to_utf8(p + 40, slen);
}

static void put_string_mhod(ipod_buf *b, uint32_t type, const char *s) {
  size_t n = 0;
  uint8_t *u = ipod_utf8_to_utf16le(s, &n);
  if (!u) {
    b->oom = 1;
    return;
  }
  ipod_buf_tag(b, "mhod");
  ipod_buf_le32(b, 24);
  ipod_buf_le32(b, (uint32_t)(40 + n));
  ipod_buf_le32(b, type);
  ipod_buf_zero(b, 8);
  ipod_buf_le32(b, 1); /* position / encoding: 1 for UTF-16 */
  ipod_buf_le32(b, (uint32_t)n);
  ipod_buf_le32(b, 1);
  ipod_buf_le32(b, 0);
  ipod_buf_put(b, u, n);
  free(u);
}

static void slot_free(mhod_slot *s) {
  free(s->bytes.p);
  free(s->orig);
}

/* --- Chunk walking ------------------------------------------------------------------- */

typedef struct chunk {
  const uint8_t *p;
  uint32_t header_len, total_len;
} chunk;

/* Reads the chunk at p (avail bytes left) with the given tag. Lists (mhlt, mhlp) have no
 * total length; for those total_len is the header length. */
static int read_chunk(const uint8_t *p, size_t avail, const char *tag, chunk *c) {
  if (avail < 12 || memcmp(p, tag, 4) != 0) return -1;
  c->p = p;
  c->header_len = ipod_rd32(p + 4);
  int is_list = memcmp(tag, "mhlt", 4) == 0 || memcmp(tag, "mhlp", 4) == 0;
  c->total_len = is_list ? c->header_len : ipod_rd32(p + 8);
  if (c->header_len < 12 || c->header_len > avail || c->total_len < c->header_len || c->total_len > avail)
    return -1;
  return 0;
}

/* --- Tracks ------------------------------------------------------------------------- */

ipod_track *ipod_track_new(void) {
  ipod_track *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->has_artwork = 2;
  t->mark_unplayed = 1;
  return t;
}

void ipod_track_free(ipod_track *t) {
  if (!t) return;
  for (size_t i = 0; i < N_TRACK_STRINGS; i++) free(*track_str(t, i));
  if (t->priv) {
    free(t->priv->chunk.p);
    for (size_t i = 0; i < t->priv->n_slots; i++) slot_free(&t->priv->slots[i]);
    free(t->priv->slots);
    free(t->priv);
  }
  free(t);
}

void ipod_track_set_kind(ipod_track *t, const char *codec, int vbr) {
  if (strcmp(codec, "mp3") == 0) {
    t->filetype = IPOD_FILETYPE_MP3;
    t->type1 = vbr ? 1 : 0;
    t->type2 = 1;
    t->unk126 = 0xffff;
    t->unk144 = 0x000c;
    ipod_set_str(&t->filetype_desc, "MPEG audio file");
  } else {
    t->filetype = IPOD_FILETYPE_M4A;
    t->type1 = 0;
    t->type2 = 0;
    t->unk126 = 0xffff;
    t->unk144 = 0x0033;
    ipod_set_str(&t->filetype_desc, strcmp(codec, "alac") == 0 ? "Apple Lossless audio file" : "AAC audio file");
  }
}

/* Header fields: the reader and writer share this list. Each is read and written only
 * when the header is long enough to hold it. */
#define TRACK_FIELDS(X)          \
  X(0x10, 32, id)                \
  X(0x18, 32, filetype)          \
  X(0x1C, 8, type1)              \
  X(0x1D, 8, type2)              \
  X(0x1E, 8, compilation)        \
  X(0x1F, 8, rating)             \
  X(0x20, 32, time_modified)     \
  X(0x24, 32, size)              \
  X(0x28, 32, length_ms)         \
  X(0x2C, 32, track_number)      \
  X(0x30, 32, track_total)       \
  X(0x34, 32, year)              \
  X(0x38, 32, bitrate)           \
  X(0x40, 32, volume)            \
  X(0x44, 32, start_ms)          \
  X(0x48, 32, stop_ms)           \
  X(0x4C, 32, soundcheck)        \
  X(0x50, 32, play_count)        \
  X(0x54, 32, play_count2)       \
  X(0x58, 32, last_played)       \
  X(0x5C, 32, disc_number)       \
  X(0x60, 32, disc_total)        \
  X(0x68, 32, time_added)        \
  X(0x6C, 32, bookmark_ms)       \
  X(0x70, 64, dbid)              \
  X(0x78, 8, checked)            \
  X(0x7A, 16, bpm)               \
  X(0x7C, 16, artwork_count)     \
  X(0x7E, 16, unk126)            \
  X(0x80, 32, artwork_size)      \
  X(0x8C, 32, time_released)     \
  X(0x90, 16, unk144)            \
  X(0x9C, 32, skip_count)        \
  X(0xA0, 32, last_skipped)      \
  X(0xA4, 8, has_artwork)        \
  X(0xB2, 8, mark_unplayed)      \
  X(0xBC, 64, sample_count)      \
  X(0xD0, 32, media_type)

static uint64_t rdn(const uint8_t *p, int bits) {
  switch (bits) {
    case 8: return p[0];
    case 16: return ipod_rd16(p);
    case 32: return ipod_rd32(p);
    default: return ipod_rd64(p);
  }
}

static void wrn(uint8_t *p, int bits, uint64_t v) {
  switch (bits) {
    case 8: p[0] = (uint8_t)v; break;
    case 16: ipod_wr16(p, (uint16_t)v); break;
    case 32: ipod_wr32(p, (uint32_t)v); break;
    default: ipod_wr64(p, v); break;
  }
}

static void read_track_header(ipod_track *t, const uint8_t *h, uint32_t hlen) {
#define X(off, bits, f) \
  if ((off) + (bits) / 8 <= hlen) t->f = (__typeof__(t->f))rdn(h + (off), bits);
  TRACK_FIELDS(X)
#undef X
  if (0x40 <= hlen) t->sample_rate = ipod_rd32(h + 0x3C) >> 16;
}

static float rdf32(const uint8_t *p) {
  uint32_t u = ipod_rd32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

static void wrf32(uint8_t *p, float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  ipod_wr32(p, u);
}

/* Writes the fields into h, a copy of the original header (or a fresh template). Values
 * derived from others (16.16 sample rate, the float sample rate, dbid2) are only replaced
 * when they agreed with the original, so whatever iTunes put there survives. */
static void write_track_header(const ipod_track *t, uint8_t *h, uint32_t hlen) {
  uint32_t old_sr_fixed = ipod_rd32(h + 0x3C);
  uint64_t old_dbid = hlen >= 0x78 ? ipod_rd64(h + 0x70) : 0;
#define X(off, bits, f) \
  if ((off) + (bits) / 8 <= hlen) wrn(h + (off), bits, (uint64_t)t->f);
  TRACK_FIELDS(X)
#undef X
  if ((old_sr_fixed >> 16) != t->sample_rate) ipod_wr32(h + 0x3C, t->sample_rate << 16);
  if (hlen >= 0x8C) {
    float oldf = rdf32(h + 0x88);
    if (oldf == (float)(old_sr_fixed >> 16) || oldf == 0.0f) wrf32(h + 0x88, (float)t->sample_rate);
  }
  if (hlen >= 0xB0) {
    uint64_t d2 = ipod_rd64(h + 0xA8);
    if (d2 == old_dbid || d2 == 0) ipod_wr64(h + 0xA8, t->dbid);
  }
}

#define NEW_MHIT_HEADER 0x184

static int parse_track(const chunk *c, ipod_track **out) {
  ipod_track *t = ipod_track_new();
  if (!t) return IPOD_ERR_NOMEM;
  ipod_track_priv *pv = calloc(1, sizeof *pv);
  if (!pv) {
    ipod_track_free(t);
    return IPOD_ERR_NOMEM;
  }
  t->priv = pv;
  pv->chunk = raw_copy(c->p, c->total_len);
  if (!pv->chunk.p) {
    ipod_track_free(t);
    return IPOD_ERR_NOMEM;
  }
  pv->header_len = c->header_len;
  read_track_header(t, c->p, c->header_len);

  uint32_t n_mhods = ipod_rd32(c->p + 12);
  const uint8_t *p = c->p + c->header_len;
  size_t avail = c->total_len - c->header_len;
  pv->slots = calloc(n_mhods ? n_mhods : 1, sizeof *pv->slots);
  if (!pv->slots) {
    ipod_track_free(t);
    return IPOD_ERR_NOMEM;
  }
  for (uint32_t i = 0; i < n_mhods && avail > 0; i++) {
    chunk m;
    if (read_chunk(p, avail, "mhod", &m) != 0) break;
    mhod_slot *s = &pv->slots[pv->n_slots++];
    s->type = m.total_len >= 16 ? ipod_rd32(m.p + 12) : 0;
    s->bytes = raw_copy(m.p, m.total_len);
    int fi = find_string_type(s->type);
    if (fi >= 0 && *track_str(t, (size_t)fi) == NULL) {
      char *str = parse_string_mhod(m.p, m.total_len);
      if (str) {
        s->is_string = 1;
        s->orig = str;
        *track_str(t, (size_t)fi) = ipod_strdup(str);
      }
    }
    p += m.total_len;
    avail -= m.total_len;
  }
  *out = t;
  return 0;
}

static void emit_track(ipod_buf *b, const ipod_track *t) {
  size_t start = b->len;
  uint32_t hlen;
  if (t->priv) {
    hlen = t->priv->header_len;
    ipod_buf_put(b, t->priv->chunk.p, hlen);
  } else {
    hlen = NEW_MHIT_HEADER;
    uint8_t *h = ipod_buf_grow(b, hlen);
    if (h) {
      memcpy(h, "mhit", 4);
      ipod_wr32(h + 4, hlen);
      ipod_wr32(h + 0x14, 1); /* visible */
    }
  }
  if (b->oom) return;
  write_track_header(t, b->data + start, hlen);

  uint32_t n = 0;
  int written[N_TRACK_STRINGS] = {0};
  if (t->priv) {
    for (size_t i = 0; i < t->priv->n_slots; i++) {
      const mhod_slot *s = &t->priv->slots[i];
      if (s->is_string) {
        int fi = find_string_type(s->type);
        const char *cur = *track_str((ipod_track *)t, (size_t)fi);
        written[fi] = 1;
        if (cur == NULL) continue;
        if (strcmp(cur, s->orig) == 0) ipod_buf_put(b, s->bytes.p, s->bytes.len);
        else put_string_mhod(b, s->type, cur);
      } else {
        ipod_buf_put(b, s->bytes.p, s->bytes.len);
      }
      n++;
    }
  }
  for (size_t i = 0; i < N_TRACK_STRINGS; i++) {
    const char *cur = *track_str((ipod_track *)t, i);
    if (written[i] || empty(cur)) continue;
    put_string_mhod(b, track_strings[i].type, cur);
    n++;
  }
  ipod_buf_set_le32(b, start + 8, (uint32_t)(b->len - start));
  ipod_buf_set_le32(b, start + 12, n);
}

/* --- Playlists ------------------------------------------------------------------------ */

ipod_playlist *ipod_playlist_new(const char *name) {
  ipod_playlist *pl = calloc(1, sizeof *pl);
  if (!pl) return NULL;
  if (name && !(pl->name = ipod_strdup(name))) {
    free(pl);
    return NULL;
  }
  return pl;
}

void ipod_playlist_free(ipod_playlist *pl) {
  if (!pl) return;
  free(pl->name);
  free(pl->items);
  ipod_playlist_priv *pv = pl->priv;
  if (pv) {
    free(pv->chunk.p);
    free(pv->chunk3.p);
    slot_free(&pv->name_slot);
    for (size_t i = 0; i < pv->n_slots; i++) slot_free(&pv->slots[i]);
    free(pv->slots);
    for (size_t i = 0; i < pv->n_mhips; i++) free(pv->mhips[i].header.p);
    free(pv->mhips);
    free(pv->orig_name);
    free(pv->orig_items);
    free(pv);
  }
  free(pl);
}

int ipod_playlist_add(ipod_playlist *pl, uint32_t track_id) {
  if (pl->n_items == pl->cap_items) {
    size_t cap = pl->cap_items ? pl->cap_items * 2 : 16;
    uint32_t *n = realloc(pl->items, cap * sizeof *n);
    if (!n) return IPOD_ERR_NOMEM;
    pl->items = n;
    pl->cap_items = cap;
  }
  pl->items[pl->n_items++] = track_id;
  return 0;
}

void ipod_playlist_clear(ipod_playlist *pl) { pl->n_items = 0; }

static int parse_playlist(const chunk *c, ipod_playlist **out) {
  ipod_playlist *pl = ipod_playlist_new(NULL);
  if (!pl) return IPOD_ERR_NOMEM;
  ipod_playlist_priv *pv = calloc(1, sizeof *pv);
  if (!pv) {
    ipod_playlist_free(pl);
    return IPOD_ERR_NOMEM;
  }
  pl->priv = pv;
  pv->chunk = raw_copy(c->p, c->total_len);
  if (!pv->chunk.p) goto oom;
  pv->header_len = c->header_len;
  const uint8_t *h = c->p;
  uint32_t hlen = c->header_len;
  if (hlen > 0x14) pl->is_master = h[0x14] == 1;
  if (hlen >= 0x1C) pl->timestamp = ipod_rd32(h + 0x18);
  if (hlen >= 0x24) pl->id = ipod_rd64(h + 0x1C);
  if (hlen >= 0x2C) pl->is_podcast = ipod_rd16(h + 0x2A) == 1;

  uint32_t n_mhods = ipod_rd32(h + 12);
  size_t cap_slots = n_mhods ? n_mhods : 1, cap_mhips = 0;
  pv->slots = calloc(cap_slots, sizeof *pv->slots);
  if (!pv->slots) goto oom;

  const uint8_t *p = c->p + hlen;
  size_t avail = c->total_len - hlen;
  mhip_raw *last_mhip = NULL;
  while (avail >= 12) {
    chunk m;
    if (read_chunk(p, avail, "mhod", &m) == 0) {
      uint32_t type = m.total_len >= 16 ? ipod_rd32(m.p + 12) : 0;
      if (type == 100 && last_mhip) {
        /* The position mhod of an old-style mhip; regenerated on write. */
      } else if (type == 1 && pv->name_slot.type == 0 && (pl->name = parse_string_mhod(m.p, m.total_len))) {
        pv->name_slot.type = 1;
        pv->name_slot.is_string = 1;
        pv->name_slot.bytes = raw_copy(m.p, m.total_len);
        pv->name_slot.orig = ipod_strdup(pl->name);
      } else if (type == 52 || type == 53) {
        /* Library indexes: rebuilt on write. */
      } else {
        if (type == 50 || type == 51) pl->is_smart = 1;
        if (pv->n_slots == cap_slots) {
          cap_slots *= 2;
          mhod_slot *ns = realloc(pv->slots, cap_slots * sizeof *ns);
          if (!ns) goto oom;
          pv->slots = ns;
        }
        mhod_slot *s = &pv->slots[pv->n_slots++];
        memset(s, 0, sizeof *s);
        s->type = type;
        s->bytes = raw_copy(m.p, m.total_len);
      }
      p += m.total_len;
      avail -= m.total_len;
    } else if (read_chunk(p, avail, "mhip", &m) == 0) {
      if (pv->n_mhips == cap_mhips) {
        cap_mhips = cap_mhips ? cap_mhips * 2 : 16;
        mhip_raw *nm = realloc(pv->mhips, cap_mhips * sizeof *nm);
        if (!nm) goto oom;
        pv->mhips = nm;
      }
      mhip_raw *r = &pv->mhips[pv->n_mhips++];
      memset(r, 0, sizeof *r);
      r->header = raw_copy(m.p, m.header_len);
      r->track_id = m.header_len >= 0x1C ? ipod_rd32(m.p + 0x18) : 0;
      last_mhip = r;
      /* Podcast group headers (track id 0) aren't items. */
      if (r->track_id && ipod_playlist_add(pl, r->track_id) != 0) goto oom;
      p += m.total_len;
      avail -= m.total_len;
    } else {
      break; /* unknown child: the raw chunk still has it, but the playlist is rebuilt without */
    }
  }
  pv->orig_name = ipod_strdup(pl->name);
  if (pl->n_items) {
    pv->orig_items = malloc(pl->n_items * sizeof *pv->orig_items);
    if (!pv->orig_items) goto oom;
    memcpy(pv->orig_items, pl->items, pl->n_items * sizeof *pv->orig_items);
  }
  pv->n_orig_items = pl->n_items;
  *out = pl;
  return 0;
oom:
  ipod_playlist_free(pl);
  return IPOD_ERR_NOMEM;
}

static int playlist_unchanged(const ipod_playlist *pl) {
  const ipod_playlist_priv *pv = pl->priv;
  if (!pv) return 0;
  if (!ipod_streq(pl->name, pv->orig_name)) return 0;
  if (pl->n_items != pv->n_orig_items) return 0;
  return pl->n_items == 0 || memcmp(pl->items, pv->orig_items, pl->n_items * sizeof *pl->items) == 0;
}

/* --- Library indexes (mhod 52 / 53) ------------------------------------------------------ */

enum { SORT_TITLE = 3, SORT_ALBUM = 4, SORT_ARTIST = 5, SORT_GENRE = 7, SORT_COMPOSER = 0x12 };

typedef struct sort_keys {
  char *title, *album, *artist, *genre, *composer;
  uint32_t disc, track;
} sort_keys;

/* Lower-cased ASCII, other characters as they are. */
static char *fold(const char *a, const char *b) {
  const char *s = !empty(a) ? a : (b ? b : "");
  char *r = ipod_strdup(s);
  if (!r) return NULL;
  for (char *p = r; *p; p++)
    if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
  return r;
}

/* The jump-table letter: the first letter or digit, upper-cased; '0' for digits or none. */
static uint16_t jump_letter(const char *s) {
  uint32_t c;
  while ((c = ipod_utf8_next(&s)) != 0) {
    if (c >= '0' && c <= '9') return '0';
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return (uint16_t)c;
    if (c >= 0xC0 && c < 0x10000 && c != 0xD7 && c != 0xF7) return (uint16_t)c;
  }
  return '0';
}

static int cmp_num(uint32_t a, uint32_t b) { return a < b ? -1 : a > b; }

static int sort_cmp(const sort_keys *a, const sort_keys *b, int type) {
  int r = 0;
  switch (type) {
    case SORT_TITLE: return strcmp(a->title, b->title);
    case SORT_GENRE:
      if ((r = strcmp(a->genre, b->genre))) return r;
      /* fall through */
    case SORT_ARTIST:
      if ((r = strcmp(a->artist, b->artist))) return r;
      goto album;
    case SORT_COMPOSER:
      if ((r = strcmp(a->composer, b->composer))) return r;
      /* fall through */
    case SORT_ALBUM:
    album:
      if ((r = strcmp(a->album, b->album))) return r;
      if ((r = cmp_num(a->disc, b->disc))) return r;
      if ((r = cmp_num(a->track, b->track))) return r;
      return strcmp(a->title, b->title);
    default: return 0;
  }
}

/* Stable merge sort of indexes by sort_cmp. */
static void sort_indexes(uint32_t *idx, uint32_t *tmp, size_t n, const sort_keys *keys, int type) {
  if (n < 2) return;
  size_t mid = n / 2;
  sort_indexes(idx, tmp, mid, keys, type);
  sort_indexes(idx + mid, tmp, n - mid, keys, type);
  size_t i = 0, j = mid, k = 0;
  while (i < mid && j < n) tmp[k++] = sort_cmp(&keys[idx[j]], &keys[idx[i]], type) < 0 ? idx[j++] : idx[i++];
  while (i < mid) tmp[k++] = idx[i++];
  while (j < n) tmp[k++] = idx[j++];
  memcpy(idx, tmp, n * sizeof *idx);
}

static const char *sort_key_of(const sort_keys *k, int type) {
  switch (type) {
    case SORT_TITLE: return k->title;
    case SORT_ALBUM: return k->album;
    case SORT_ARTIST: return k->artist;
    case SORT_GENRE: return k->genre;
    default: return k->composer;
  }
}

static int emit_library_indexes(ipod_buf *b, const ipod_db *db) {
  size_t n = db->n_tracks;
  sort_keys *keys = calloc(n, sizeof *keys);
  uint32_t *idx = malloc(n * sizeof *idx), *tmp = malloc(n * sizeof *tmp);
  int ok = keys && idx && tmp;
  for (size_t i = 0; ok && i < n; i++) {
    const ipod_track *t = db->tracks[i];
    keys[i].title = fold(t->sort_title, t->title);
    keys[i].album = fold(t->sort_album, t->album);
    keys[i].artist = fold(t->sort_artist, t->artist);
    keys[i].genre = fold(NULL, t->genre);
    keys[i].composer = fold(t->sort_composer, t->composer);
    keys[i].disc = t->disc_number;
    keys[i].track = t->track_number;
    if (!keys[i].title || !keys[i].album || !keys[i].artist || !keys[i].genre || !keys[i].composer) ok = 0;
  }
  static const int types[] = {SORT_TITLE, SORT_ARTIST, SORT_ALBUM, SORT_GENRE, SORT_COMPOSER};
  for (size_t ti = 0; ok && ti < 5; ti++) {
    int type = types[ti];
    for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;
    sort_indexes(idx, tmp, n, keys, type);
    ipod_buf_tag(b, "mhod");
    ipod_buf_le32(b, 24);
    ipod_buf_le32(b, (uint32_t)(4 * n + 72));
    ipod_buf_le32(b, 52);
    ipod_buf_zero(b, 8);
    ipod_buf_le32(b, (uint32_t)type);
    ipod_buf_le32(b, (uint32_t)n);
    ipod_buf_zero(b, 40);
    for (size_t i = 0; i < n; i++) ipod_buf_le32(b, idx[i]);

    /* The jump table: one entry per run of the same letter. */
    size_t at = b->len;
    ipod_buf_tag(b, "mhod");
    ipod_buf_le32(b, 24);
    ipod_buf_le32(b, 0); /* below */
    ipod_buf_le32(b, 53);
    ipod_buf_zero(b, 8);
    ipod_buf_le32(b, (uint32_t)type);
    size_t count_at = b->len;
    ipod_buf_le32(b, 0);
    ipod_buf_zero(b, 8);
    uint32_t entries = 0, start = 0;
    uint16_t cur = 0;
    for (size_t i = 0; i <= n; i++) {
      uint16_t l = i < n ? jump_letter(sort_key_of(&keys[idx[i]], type)) : 0;
      if (i == n || (i > 0 && l != cur)) {
        if (i > 0) {
          ipod_buf_le16(b, cur);
          ipod_buf_le16(b, 0);
          ipod_buf_le32(b, start);
          ipod_buf_le32(b, (uint32_t)i - start);
          entries++;
        }
        start = (uint32_t)i;
      }
      cur = l;
    }
    ipod_buf_set_le32(b, count_at, entries);
    ipod_buf_set_le32(b, at + 8, (uint32_t)(b->len - at));
  }
  for (size_t i = 0; keys && i < n; i++) {
    free(keys[i].title);
    free(keys[i].album);
    free(keys[i].artist);
    free(keys[i].genre);
    free(keys[i].composer);
  }
  free(keys);
  free(idx);
  free(tmp);
  return ok ? 10 : -1;
}

/* The per-playlist preferences mhod (type 100, 0x288 bytes) iTunes writes for column
 * settings. iTunes shows an empty playlist without it; the firmware ignores it. */
static void emit_playlist_prefs(ipod_buf *b) {
  static const uint32_t words[] = {0x010084, 0x05, 0x09, 0x03, 0x120001, 0, 0, 0, 0xc80002, 0, 0, 0,
                                   0x3c000d, 0, 0, 0, 0x7d0004, 0, 0, 0, 0x7d0003, 0, 0, 0,
                                   0x640008, 0, 0, 0, 0x640017, 0x01, 0, 0, 0x500014, 0x01, 0, 0,
                                   0x7d0015, 0x01};
  size_t at = b->len;
  ipod_buf_tag(b, "mhod");
  ipod_buf_le32(b, 0x18);
  ipod_buf_le32(b, 0x288);
  ipod_buf_le32(b, 100);
  ipod_buf_zero(b, 24);
  for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) ipod_buf_le32(b, words[i]);
  if (!b->oom && b->len - at < 0x288) ipod_buf_zero(b, 0x288 - (b->len - at));
}

#define NEW_MHYP_HEADER 0x6C
#define NEW_MHIP_HEADER 0x4C

static void emit_playlist(ipod_buf *b, ipod_playlist *pl, const ipod_db *db) {
  ipod_playlist_priv *pv = pl->priv;
  size_t start = b->len;
  uint32_t hlen = pv ? pv->header_len : NEW_MHYP_HEADER;
  if (pv) {
    ipod_buf_put(b, pv->chunk.p, hlen);
  } else {
    uint8_t *h = ipod_buf_grow(b, hlen);
    if (h) {
      memcpy(h, "mhyp", 4);
      ipod_wr32(h + 4, hlen);
      ipod_wr16(h + 0x28, 1);      /* string mhod count */
      ipod_wr32(h + 0x2C, 1);      /* sort order: manual */
      if (!pl->timestamp) pl->timestamp = ipod_unix_to_mac((int64_t)time(NULL), ipod_local_tz_offset());
    }
  }
  if (b->oom) return;
  uint8_t *h = b->data + start;
  if (hlen > 0x14) h[0x14] = pl->is_master ? 1 : 0;
  if (hlen >= 0x1C) ipod_wr32(h + 0x18, pl->timestamp);
  if (hlen >= 0x24) ipod_wr64(h + 0x1C, pl->id);

  uint32_t n_mhods = 0;
  if (!empty(pl->name) || pl->is_master) {
    const char *name = pl->name ? pl->name : "";
    if (pv && pv->name_slot.type && strcmp(pv->name_slot.orig, name) == 0)
      ipod_buf_put(b, pv->name_slot.bytes.p, pv->name_slot.bytes.len);
    else
      put_string_mhod(b, 1, name);
    n_mhods++;
  }
  if (pv) {
    for (size_t i = 0; i < pv->n_slots; i++) {
      ipod_buf_put(b, pv->slots[i].bytes.p, pv->slots[i].bytes.len);
      n_mhods++;
    }
  } else {
    emit_playlist_prefs(b);
    n_mhods++;
  }
  if (pl->is_master && db->n_tracks) {
    int k = emit_library_indexes(b, db);
    if (k < 0) b->oom = 1;
    else n_mhods += (uint32_t)k;
  }

  if (pv)
    for (size_t i = 0; i < pv->n_mhips; i++) pv->mhips[i].used = 0;
  for (size_t i = 0; i < pl->n_items; i++) {
    const mhip_raw *orig = NULL;
    if (pv) {
      for (size_t j = 0; j < pv->n_mhips; j++) {
        if (!pv->mhips[j].used && pv->mhips[j].track_id == pl->items[i]) {
          pv->mhips[j].used = 1;
          orig = &pv->mhips[j];
          break;
        }
      }
    }
    size_t at = b->len;
    if (orig && orig->header.len < 0x20) orig = NULL;
    uint32_t mlen = orig ? orig->header.len : NEW_MHIP_HEADER;
    uint8_t *m = ipod_buf_grow(b, mlen);
    if (!m) return;
    if (orig) {
      memcpy(m, orig->header.p, mlen);
    } else {
      memcpy(m, "mhip", 4);
      ipod_wr32(m + 4, mlen);
    }
    ipod_wr32(m + 12, 1);  /* children: the position mhod */
    ipod_wr32(m + 0x18, pl->items[i]);
    /* The position mhod. */
    ipod_buf_tag(b, "mhod");
    ipod_buf_le32(b, 24);
    ipod_buf_le32(b, 44);
    ipod_buf_le32(b, 100);
    ipod_buf_zero(b, 8);
    ipod_buf_le32(b, (uint32_t)i);
    ipod_buf_zero(b, 16);
    ipod_buf_set_le32(b, at + 8, (uint32_t)(b->len - at));
  }
  ipod_buf_set_le32(b, start + 8, (uint32_t)(b->len - start));
  ipod_buf_set_le32(b, start + 12, n_mhods);
  ipod_buf_set_le32(b, start + 16, (uint32_t)pl->n_items);
}

/* --- Database ------------------------------------------------------------------------------ */

static int db_push_track(ipod_db *db, ipod_track *t) {
  if (db->n_tracks == db->cap_tracks) {
    size_t cap = db->cap_tracks ? db->cap_tracks * 2 : 64;
    ipod_track **n = realloc(db->tracks, cap * sizeof *n);
    if (!n) return IPOD_ERR_NOMEM;
    db->tracks = n;
    db->cap_tracks = cap;
  }
  db->tracks[db->n_tracks++] = t;
  return 0;
}

static int db_push_playlist(ipod_db *db, ipod_playlist *pl) {
  if (db->n_playlists == db->cap_playlists) {
    size_t cap = db->cap_playlists ? db->cap_playlists * 2 : 8;
    ipod_playlist **n = realloc(db->playlists, cap * sizeof *n);
    if (!n) return IPOD_ERR_NOMEM;
    db->playlists = n;
    db->cap_playlists = cap;
  }
  db->playlists[db->n_playlists++] = pl;
  return 0;
}

static uint64_t random_nonzero_u64(void) {
  uint64_t v;
  do v = ipod_random_u64();
  while (v == 0);
  return v;
}

ipod_db *ipod_db_new(const char *name) {
  ipod_db *db = calloc(1, sizeof *db);
  if (!db) return NULL;
  db->priv = calloc(1, sizeof *db->priv);
  ipod_playlist *mpl = ipod_playlist_new(name ? name : "iPod");
  if (!db->priv || !mpl || db_push_playlist(db, mpl) != 0) {
    ipod_playlist_free(mpl);
    ipod_db_free(db);
    return NULL;
  }
  mpl->is_master = 1;
  mpl->id = random_nonzero_u64();
  db->version = 0x13; /* iTunes 7.0: what the click-wheel firmware last saw from iTunes */
  db->id = random_nonzero_u64();
  return db;
}

void ipod_db_free(ipod_db *db) {
  if (!db) return;
  for (size_t i = 0; i < db->n_tracks; i++) ipod_track_free(db->tracks[i]);
  free(db->tracks);
  for (size_t i = 0; i < db->n_playlists; i++) ipod_playlist_free(db->playlists[i]);
  free(db->playlists);
  if (db->priv) {
    free(db->priv->mhbd_header.p);
    for (size_t i = 0; i < db->priv->n_ds; i++) {
      free(db->priv->ds[i].mhsd_header.p);
      free(db->priv->ds[i].list_header.p);
      free(db->priv->ds[i].chunk.p);
    }
    free(db->priv->ds);
    free(db->priv->orig_track_ids);
    free(db->priv);
  }
  free(db);
}

static int parse_playlist_list(const chunk *sd, dataset *ds, ipod_playlist ***out, size_t *n_out,
                               ipod_error *err) {
  const uint8_t *p = sd->p + sd->header_len;
  size_t avail = sd->total_len - sd->header_len;
  chunk lp;
  if (read_chunk(p, avail, "mhlp", &lp) != 0) return ipod_seterr(err, IPOD_ERR_FORMAT, "bad mhlp");
  ds->list_header = raw_copy(lp.p, lp.header_len);
  uint32_t count = ipod_rd32(lp.p + 8);
  p += lp.header_len;
  avail -= lp.header_len;
  ipod_playlist **list = calloc(count ? count : 1, sizeof *list);
  if (!list) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  size_t n = 0;
  for (uint32_t i = 0; i < count; i++) {
    chunk yp;
    if (read_chunk(p, avail, "mhyp", &yp) != 0) {
      for (size_t k = 0; k < n; k++) ipod_playlist_free(list[k]);
      free(list);
      return ipod_seterr(err, IPOD_ERR_FORMAT, "bad mhyp %u", i);
    }
    int r = parse_playlist(&yp, &list[n]);
    if (r != 0) {
      for (size_t k = 0; k < n; k++) ipod_playlist_free(list[k]);
      free(list);
      return ipod_seterr(err, r, "out of memory");
    }
    n++;
    p += yp.total_len;
    avail -= yp.total_len;
  }
  *out = list;
  *n_out = n;
  return 0;
}

ipod_db *ipod_db_parse(const uint8_t *data, size_t len, ipod_error *err) {
  chunk bd;
  if (read_chunk(data, len, "mhbd", &bd) != 0) {
    ipod_seterr(err, IPOD_ERR_FORMAT, "not an iTunesDB (no mhbd)");
    return NULL;
  }
  ipod_db *db = calloc(1, sizeof *db);
  if (!db || !(db->priv = calloc(1, sizeof *db->priv))) {
    free(db);
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  ipod_playlist **pl3 = NULL;
  size_t n_pl3 = 0;
  ipod_db_priv *pv = db->priv;
  pv->mhbd_header = raw_copy(bd.p, bd.header_len);
  if (bd.header_len >= 0x14) db->version = ipod_rd32(bd.p + 0x10);
  if (bd.header_len >= 0x20) db->id = ipod_rd64(bd.p + 0x18);
  if (bd.header_len >= 0x32 && ipod_rd16(bd.p + 0x30) != 0) pv->hashed = 1;
  if (bd.header_len >= 0x72 && ipod_rd16(bd.p + 0x70) != 0) pv->hashed = 1;

  uint32_t n_ds = ipod_rd32(bd.p + 0x14);
  pv->ds = calloc(n_ds ? n_ds : 1, sizeof *pv->ds);
  if (!pv->ds) goto oom;
  const uint8_t *p = bd.p + bd.header_len;
  size_t avail = bd.total_len - bd.header_len;
  int have_type1 = 0, have_type2 = 0;
  for (uint32_t i = 0; i < n_ds; i++) {
    chunk sd;
    if (read_chunk(p, avail, "mhsd", &sd) != 0) {
      ipod_seterr(err, IPOD_ERR_FORMAT, "bad mhsd %u", i);
      goto fail;
    }
    dataset *ds = &pv->ds[pv->n_ds++];
    ds->type = sd.header_len >= 16 ? ipod_rd32(sd.p + 12) : 0;
    ds->mhsd_header = raw_copy(sd.p, sd.header_len);
    if (ds->type == 1 && !have_type1) {
      have_type1 = 1;
      const uint8_t *q = sd.p + sd.header_len;
      size_t qa = sd.total_len - sd.header_len;
      chunk lt;
      if (read_chunk(q, qa, "mhlt", &lt) != 0) {
        ipod_seterr(err, IPOD_ERR_FORMAT, "bad mhlt");
        goto fail;
      }
      ds->list_header = raw_copy(lt.p, lt.header_len);
      uint32_t count = ipod_rd32(lt.p + 8);
      q += lt.header_len;
      qa -= lt.header_len;
      for (uint32_t k = 0; k < count; k++) {
        chunk it;
        if (read_chunk(q, qa, "mhit", &it) != 0) {
          ipod_seterr(err, IPOD_ERR_FORMAT, "bad mhit %u", k);
          goto fail;
        }
        ipod_track *t;
        if (parse_track(&it, &t) != 0 || db_push_track(db, t) != 0) goto oom;
        q += it.total_len;
        qa -= it.total_len;
      }
    } else if (ds->type == 2 && !have_type2) {
      ipod_playlist **list;
      size_t n;
      if (parse_playlist_list(&sd, ds, &list, &n, err) != 0) goto fail;
      for (size_t k = 0; k < n; k++) {
        if (db_push_playlist(db, list[k]) != 0) {
          for (; k < n; k++) ipod_playlist_free(list[k]);
          free(list);
          goto oom;
        }
      }
      free(list);
      have_type2 = 1;
    } else if (ds->type == 3 && !pl3) {
      if (parse_playlist_list(&sd, ds, &pl3, &n_pl3, err) != 0) goto fail;
    } else {
      ds->chunk = raw_copy(sd.p, sd.total_len);
    }
    p += sd.total_len;
    avail -= sd.total_len;
  }
  if (!have_type2 && pl3) {
    /* No flat list: take the playlists from the podcast-style one. */
    for (size_t k = 0; k < n_pl3; k++) {
      if (db_push_playlist(db, pl3[k]) != 0) goto oom;
      pl3[k] = NULL;
    }
  } else if (pl3) {
    /* Keep each type 3 playlist's bytes beside its type 2 twin, for the Podcasts one. */
    for (size_t k = 0; k < n_pl3; k++) {
      for (size_t j = 0; j < db->n_playlists; j++) {
        ipod_playlist *pl = db->playlists[j];
        if (pl->priv && pl->id == pl3[k]->id && !pl->priv->chunk3.p) {
          pl->priv->chunk3 = pl3[k]->priv->chunk;
          pl3[k]->priv->chunk.p = NULL;
          break;
        }
      }
    }
  }
  for (size_t k = 0; k < n_pl3; k++) ipod_playlist_free(pl3[k]);
  free(pl3);
  pl3 = NULL;

  /* Exactly one master playlist, first. */
  size_t mi = db->n_playlists;
  for (size_t j = 0; j < db->n_playlists; j++)
    if (db->playlists[j]->is_master) {
      mi = j;
      break;
    }
  if (mi == db->n_playlists) {
    ipod_playlist *mpl = ipod_playlist_new("iPod");
    if (!mpl || db_push_playlist(db, mpl) != 0) {
      ipod_playlist_free(mpl);
      goto oom;
    }
    mpl->is_master = 1;
    mpl->id = random_nonzero_u64();
    mi = db->n_playlists - 1;
  }
  if (mi != 0) {
    ipod_playlist *m = db->playlists[mi];
    memmove(db->playlists + 1, db->playlists, mi * sizeof *db->playlists);
    db->playlists[0] = m;
  }
  for (size_t j = 1; j < db->n_playlists; j++) db->playlists[j]->is_master = 0;

  pv->n_orig_tracks = db->n_tracks;
  pv->orig_track_ids = malloc((db->n_tracks ? db->n_tracks : 1) * sizeof *pv->orig_track_ids);
  if (!pv->orig_track_ids) goto oom;
  for (size_t k = 0; k < db->n_tracks; k++) pv->orig_track_ids[k] = db->tracks[k]->id;
  return db;

oom:
  ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
fail:
  if (pl3) {
    for (size_t k = 0; k < n_pl3; k++) ipod_playlist_free(pl3[k]);
    free(pl3);
  }
  ipod_db_free(db);
  return NULL;
}

ipod_db *ipod_db_read(const char *path, ipod_error *err) {
  size_t len = 0;
  uint8_t *data = ipod_read_file(path, &len);
  if (!data) {
    ipod_seterr(err, IPOD_ERR_NOT_FOUND, "can't read %s", path);
    return NULL;
  }
  ipod_db *db = ipod_db_parse(data, len, err);
  free(data);
  return db;
}

static void emit_list(ipod_buf *b, uint32_t type, const raw *mhsd_header, const raw *list_header,
                      const char *list_tag, uint32_t count, const ipod_buf *body) {
  size_t start = b->len;
  if (mhsd_header && mhsd_header->p) {
    ipod_buf_put(b, mhsd_header->p, mhsd_header->len);
  } else {
    uint8_t *h = ipod_buf_grow(b, 0x60);
    if (h) {
      memcpy(h, "mhsd", 4);
      ipod_wr32(h + 4, 0x60);
    }
  }
  ipod_buf_set_le32(b, start + 12, type);
  size_t lstart = b->len;
  if (list_header && list_header->p) {
    ipod_buf_put(b, list_header->p, list_header->len);
  } else {
    uint8_t *h = ipod_buf_grow(b, 0x5C);
    if (h) {
      memcpy(h, list_tag, 4);
      ipod_wr32(h + 4, 0x5C);
    }
  }
  ipod_buf_set_le32(b, lstart + 8, count);
  ipod_buf_put(b, body->data, body->len);
  ipod_buf_set_le32(b, start + 8, (uint32_t)(b->len - start));
}

int ipod_db_serialize(ipod_db *db, uint8_t **out, size_t *out_len, ipod_error *err) {
  ipod_db_priv *pv = db->priv;
  if (pv->hashed) return ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "this iTunesDB is hashed; not writing it");

  /* The master playlist lists every track, in database order. */
  ipod_playlist *mpl = db->playlists[0];
  if (mpl->cap_items < db->n_tracks) {
    uint32_t *n = realloc(mpl->items, (db->n_tracks ? db->n_tracks : 1) * sizeof *n);
    if (!n) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    mpl->items = n;
    mpl->cap_items = db->n_tracks;
  }
  for (size_t i = 0; i < db->n_tracks; i++) mpl->items[i] = db->tracks[i]->id;
  mpl->n_items = db->n_tracks;

  ipod_buf tracks = {0};
  int tracks_unchanged = pv->orig_track_ids != NULL && db->n_tracks == pv->n_orig_tracks;
  for (size_t i = 0; i < db->n_tracks; i++) {
    const ipod_track *t = db->tracks[i];
    size_t at = tracks.len;
    emit_track(&tracks, t);
    if (tracks_unchanged) {
      tracks_unchanged = !tracks.oom && t->priv && pv->orig_track_ids[i] == t->id &&
                         tracks.len - at == t->priv->chunk.len &&
                         memcmp(tracks.data + at, t->priv->chunk.p, t->priv->chunk.len) == 0;
    }
  }

  /* Playlists, flat (type 2) and with the Podcasts playlist as iTunes grouped it (type 3). */
  ipod_buf pl2 = {0}, pl3 = {0};
  for (size_t i = 0; i < db->n_playlists; i++) {
    ipod_playlist *pl = db->playlists[i];
    int same = playlist_unchanged(pl) && (!pl->is_master || tracks_unchanged);
    size_t at = pl2.len;
    if (same) ipod_buf_put(&pl2, pl->priv->chunk.p, pl->priv->chunk.len);
    else emit_playlist(&pl2, pl, db);
    if (pl->is_podcast && same && pl->priv->chunk3.p) ipod_buf_put(&pl3, pl->priv->chunk3.p, pl->priv->chunk3.len);
    else if (!pl2.oom) ipod_buf_put(&pl3, pl2.data + at, pl2.len - at);
  }

  ipod_buf b = {0};
  size_t n_children = 0;
  if (pv->mhbd_header.p) {
    ipod_buf_put(&b, pv->mhbd_header.p, pv->mhbd_header.len);
  } else {
    uint8_t *h = ipod_buf_grow(&b, 0xF4);
    if (h) {
      memcpy(h, "mhbd", 4);
      ipod_wr32(h + 4, 0xF4);
      ipod_wr32(h + 0x0C, 1);
      ipod_wr16(h + 0x20, 2); /* platform: Windows */
      h[0x46] = 'e';
      h[0x47] = 'n';
    }
  }
  ipod_buf_set_le32(&b, 0x10, db->version);
  if (!b.oom && b.len >= 0x20) ipod_wr64(b.data + 0x18, db->id);

  const dataset *t1 = NULL, *t2 = NULL, *t3 = NULL;
  for (size_t i = 0; i < pv->n_ds; i++) {
    if (pv->ds[i].type == 1 && pv->ds[i].list_header.p && !t1) t1 = &pv->ds[i];
    if (pv->ds[i].type == 2 && pv->ds[i].list_header.p && !t2) t2 = &pv->ds[i];
    if (pv->ds[i].type == 3 && pv->ds[i].list_header.p && !t3) t3 = &pv->ds[i];
  }
  int fresh = pv->n_ds == 0;
  uint32_t n_pl = (uint32_t)db->n_playlists, n_tr = (uint32_t)db->n_tracks;
  if (fresh) {
    /* iTunes' order: tracks, the podcast-style playlists, then the flat ones. */
    emit_list(&b, 1, NULL, NULL, "mhlt", n_tr, &tracks);
    emit_list(&b, 3, NULL, NULL, "mhlp", n_pl, &pl3);
    emit_list(&b, 2, NULL, NULL, "mhlp", n_pl, &pl2);
    n_children = 3;
  } else {
    for (size_t i = 0; i < pv->n_ds; i++) {
      const dataset *ds = &pv->ds[i];
      if (ds == t1) emit_list(&b, 1, &ds->mhsd_header, &ds->list_header, "mhlt", n_tr, &tracks);
      else if (ds == t2) emit_list(&b, 2, &ds->mhsd_header, &ds->list_header, "mhlp", n_pl, &pl2);
      else if (ds == t3) emit_list(&b, 3, &ds->mhsd_header, &ds->list_header, "mhlp", n_pl, &pl3);
      else if (ds->chunk.p) ipod_buf_put(&b, ds->chunk.p, ds->chunk.len);
      else continue;
      n_children++;
    }
    if (!t1) {
      emit_list(&b, 1, NULL, NULL, "mhlt", n_tr, &tracks);
      n_children++;
    }
    if (!t2) {
      emit_list(&b, 2, NULL, NULL, "mhlp", n_pl, &pl2);
      n_children++;
    }
  }
  ipod_buf_set_le32(&b, 8, (uint32_t)b.len);
  ipod_buf_set_le32(&b, 0x14, (uint32_t)n_children);

  int oom = tracks.oom || pl2.oom || pl3.oom || b.oom;
  ipod_buf_free(&tracks);
  ipod_buf_free(&pl2);
  ipod_buf_free(&pl3);
  if (oom) {
    ipod_buf_free(&b);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  *out = b.data;
  *out_len = b.len;
  return 0;
}

int ipod_db_write(ipod_db *db, const char *path, const char *backup_path, ipod_error *err) {
  uint8_t *data;
  size_t len;
  int r = ipod_db_serialize(db, &data, &len, err);
  if (r != 0) return r;
  if (backup_path && ipod_file_exists(path)) {
    char *tmp = ipod_asprintf("%s.tmp", backup_path);
    r = tmp ? ipod_copy_file(path, tmp, err) : ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    if (r == 0 && ipod_rename_replace(tmp, backup_path) != 0)
      r = ipod_seterr(err, IPOD_ERR_IO, "can't write %s", backup_path);
    free(tmp);
    if (r != 0) {
      free(data);
      return r;
    }
  }
  r = ipod_write_file_atomic(path, data, len, err);
  free(data);
  return r;
}

/* --- Editing ------------------------------------------------------------------------------ */

const char *ipod_db_name(const ipod_db *db) {
  return db->n_playlists && db->playlists[0]->name ? db->playlists[0]->name : "";
}

int ipod_db_set_name(ipod_db *db, const char *name) { return ipod_set_str(&db->playlists[0]->name, name); }

ipod_playlist *ipod_db_master(ipod_db *db) { return db->playlists[0]; }

int ipod_db_is_hashed(const ipod_db *db) { return db->priv->hashed; }

ipod_track *ipod_db_find_track(const ipod_db *db, uint32_t id) {
  for (size_t i = 0; i < db->n_tracks; i++)
    if (db->tracks[i]->id == id) return db->tracks[i];
  return NULL;
}

ipod_track *ipod_db_find_dbid(const ipod_db *db, uint64_t dbid) {
  for (size_t i = 0; i < db->n_tracks; i++)
    if (db->tracks[i]->dbid == dbid) return db->tracks[i];
  return NULL;
}

long ipod_db_track_index(const ipod_db *db, uint32_t id) {
  for (size_t i = 0; i < db->n_tracks; i++)
    if (db->tracks[i]->id == id) return (long)i;
  return -1;
}

int ipod_db_add_track(ipod_db *db, ipod_track *t) {
  if (t->id == 0 || ipod_db_find_track(db, t->id)) {
    uint32_t max = 0;
    for (size_t i = 0; i < db->n_tracks; i++)
      if (db->tracks[i]->id > max) max = db->tracks[i]->id;
    /* iTunes numbers from 52 up after rebuilding; anything unique works. */
    t->id = max < 51 ? 52 : max + 1;
  }
  while (t->dbid == 0 || ipod_db_find_dbid(db, t->dbid)) t->dbid = random_nonzero_u64();
  return db_push_track(db, t);
}

int ipod_db_remove_track(ipod_db *db, uint32_t id) {
  long idx = ipod_db_track_index(db, id);
  if (idx < 0) return IPOD_ERR_NOT_FOUND;
  ipod_track_free(db->tracks[idx]);
  memmove(db->tracks + idx, db->tracks + idx + 1, (db->n_tracks - (size_t)idx - 1) * sizeof *db->tracks);
  db->n_tracks--;
  for (size_t i = 0; i < db->n_playlists; i++) {
    ipod_playlist *pl = db->playlists[i];
    size_t w = 0;
    for (size_t r = 0; r < pl->n_items; r++)
      if (pl->items[r] != id) pl->items[w++] = pl->items[r];
    pl->n_items = w;
  }
  return 0;
}

int ipod_db_add_playlist(ipod_db *db, ipod_playlist *pl) {
  pl->is_master = 0;
  int clash;
  do {
    clash = pl->id == 0;
    for (size_t i = 0; !clash && i < db->n_playlists; i++) clash = db->playlists[i]->id == pl->id;
    if (clash) pl->id = random_nonzero_u64();
  } while (clash);
  return db_push_playlist(db, pl);
}

int ipod_db_remove_playlist(ipod_db *db, ipod_playlist *pl) {
  for (size_t i = 1; i < db->n_playlists; i++) {
    if (db->playlists[i] == pl) {
      ipod_playlist_free(pl);
      memmove(db->playlists + i, db->playlists + i + 1, (db->n_playlists - i - 1) * sizeof *db->playlists);
      db->n_playlists--;
      return 0;
    }
  }
  return IPOD_ERR_NOT_FOUND;
}

ipod_playlist *ipod_db_find_playlist(const ipod_db *db, const char *name) {
  for (size_t i = 1; i < db->n_playlists; i++)
    if (ipod_streq(db->playlists[i]->name, name)) return db->playlists[i];
  return NULL;
}

/* --- Locations -------------------------------------------------------------------------------- */

char *ipod_location_to_path(const char *location) {
  if (!location) return NULL;
  while (*location == ':') location++;
  char *p = ipod_strdup(location);
  if (!p) return NULL;
  for (char *c = p; *c; c++)
    if (*c == ':') *c = '/';
  return p;
}

char *ipod_path_to_location(const char *relpath) {
  if (!relpath) return NULL;
  while (*relpath == '/' || *relpath == '\\') relpath++;
  char *l = ipod_asprintf(":%s", relpath);
  if (!l) return NULL;
  for (char *c = l; *c; c++)
    if (*c == '/' || *c == '\\') *c = ':';
  return l;
}
