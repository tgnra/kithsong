#include "m4a.h"

#include <string.h>
#include <time.h>

#include "util.h"

struct ipod_m4a {
  char *path, *data_path;
  FILE *data;
  uint32_t rate;
  uint16_t channels, bits;
  uint8_t cookie[64];
  uint32_t cookie_len;
  uint32_t *sizes;
  uint32_t last_frames; /* frames in the last packet; all others are full */
  size_t n, cap;
  uint64_t frames, data_bytes;
};

ipod_m4a *ipod_m4a_open(const char *path, uint32_t sample_rate, uint16_t channels, uint16_t bits,
                        const uint8_t *cookie, uint32_t cookie_len, ipod_error *err) {
  if (cookie_len > sizeof ((ipod_m4a *)0)->cookie) {
    ipod_seterr(err, IPOD_ERR_ARG, "magic cookie too long");
    return NULL;
  }
  ipod_m4a *m = calloc(1, sizeof *m);
  if (!m) {
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  m->path = ipod_strdup(path);
  m->data_path = ipod_asprintf("%s.mdat", path);
  if (!m->path || !m->data_path) {
    ipod_m4a_abort(m);
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  m->data = ipod_fopen(m->data_path, "wb");
  if (!m->data) {
    ipod_seterr(err, IPOD_ERR_IO, "can't create %s", m->data_path);
    ipod_m4a_abort(m);
    return NULL;
  }
  m->rate = sample_rate;
  m->channels = channels;
  m->bits = bits;
  memcpy(m->cookie, cookie, cookie_len);
  m->cookie_len = cookie_len;
  return m;
}

int ipod_m4a_packet(ipod_m4a *m, const uint8_t *pkt, uint32_t len, uint32_t frames, ipod_error *err) {
  if (m->n == m->cap) {
    size_t cap = m->cap ? m->cap * 2 : 1024;
    uint32_t *s = realloc(m->sizes, cap * sizeof *s);
    if (!s) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    m->sizes = s;
    m->cap = cap;
  }
  if (fwrite(pkt, 1, len, m->data) != len) return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s", m->data_path);
  m->sizes[m->n++] = len;
  m->last_frames = frames;
  m->frames += frames;
  m->data_bytes += len;
  return 0;
}

void ipod_m4a_abort(ipod_m4a *m) {
  if (!m) return;
  if (m->data) fclose(m->data);
  if (m->data_path) ipod_remove(m->data_path);
  if (m->path) ipod_remove(m->path);
  free(m->path);
  free(m->data_path);
  free(m->sizes);
  free(m);
}

/* --- Boxes ---------------------------------------------------------------------------- */

static size_t box_open(ipod_buf *b, const char *type) {
  size_t at = b->len;
  ipod_buf_be32(b, 0);
  ipod_buf_tag(b, type);
  return at;
}

static void box_close(ipod_buf *b, size_t at) { ipod_buf_set_be32(b, at, (uint32_t)(b->len - at)); }

static size_t full_box_open(ipod_buf *b, const char *type, uint32_t version_flags) {
  size_t at = box_open(b, type);
  ipod_buf_be32(b, version_flags);
  return at;
}

static void put_matrix(ipod_buf *b) {
  static const uint32_t m[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
  for (int i = 0; i < 9; i++) ipod_buf_be32(b, m[i]);
}

/* An ilst item holding one 'data' box. */
static void put_data_item(ipod_buf *b, const char *type, uint32_t data_type, const void *p, size_t n) {
  size_t item = box_open(b, type);
  size_t data = box_open(b, "data");
  ipod_buf_be32(b, data_type);
  ipod_buf_be32(b, 0); /* locale */
  ipod_buf_put(b, p, n);
  box_close(b, data);
  box_close(b, item);
}

static void put_text(ipod_buf *b, const char *type, const char *s) {
  if (s && *s) put_data_item(b, type, 1, s, strlen(s));
}

void ipod_m4a_put_ilst(ipod_buf *b, const ipod_tags *t) {
  size_t ilst = box_open(b, "ilst");
  put_text(b, "\xa9nam", t->title);
  put_text(b, "\xa9" "ART", t->artist);
  put_text(b, "aART", t->album_artist);
  put_text(b, "\xa9" "alb", t->album);
  put_text(b, "\xa9gen", t->genre);
  put_text(b, "\xa9wrt", t->composer);
  put_text(b, "\xa9grp", t->grouping);
  put_text(b, "\xa9" "cmt", t->comment);
  if (t->year > 0) {
    char y[16];
    snprintf(y, sizeof y, "%d", t->year);
    put_text(b, "\xa9" "day", y);
  }
  if (t->track_number > 0 || t->track_total > 0) {
    uint8_t d[8] = {0};
    d[2] = (uint8_t)(t->track_number >> 8);
    d[3] = (uint8_t)t->track_number;
    d[4] = (uint8_t)(t->track_total >> 8);
    d[5] = (uint8_t)t->track_total;
    put_data_item(b, "trkn", 0, d, 8);
  }
  if (t->disc_number > 0 || t->disc_total > 0) {
    uint8_t d[6] = {0};
    d[2] = (uint8_t)(t->disc_number >> 8);
    d[3] = (uint8_t)t->disc_number;
    d[4] = (uint8_t)(t->disc_total >> 8);
    d[5] = (uint8_t)t->disc_total;
    put_data_item(b, "disk", 0, d, 6);
  }
  if (t->compilation) {
    uint8_t one = 1;
    put_data_item(b, "cpil", 21, &one, 1);
  }
  if (t->bpm > 0 && t->bpm < 65536) {
    uint8_t d[2] = {(uint8_t)(t->bpm >> 8), (uint8_t)t->bpm};
    put_data_item(b, "tmpo", 21, d, 2);
  }
  put_text(b, "sonm", t->sort_title);
  put_text(b, "soar", t->sort_artist);
  put_text(b, "soaa", t->sort_album_artist);
  put_text(b, "soal", t->sort_album);
  put_text(b, "soco", t->sort_composer);
  put_text(b, "\xa9too", "Kithsong");
  if (t->cover && t->cover_len > 4) {
    uint32_t type = 0;
    if (t->cover[0] == 0xFF && t->cover[1] == 0xD8) type = 13;
    else if (memcmp(t->cover, "\x89PNG", 4) == 0) type = 14;
    if (type) put_data_item(b, "covr", type, t->cover, t->cover_len);
  }
  box_close(b, ilst);
}

static void put_moov(ipod_buf *b, const ipod_m4a *m, const ipod_tags *tags, uint32_t data_offset) {
  uint32_t now = ipod_unix_to_mac((int64_t)time(NULL), 0);
  uint32_t duration = m->frames > UINT32_MAX ? UINT32_MAX : (uint32_t)m->frames;
  size_t moov = box_open(b, "moov");

  size_t mvhd = full_box_open(b, "mvhd", 0);
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, m->rate); /* timescale */
  ipod_buf_be32(b, duration);
  ipod_buf_be32(b, 0x00010000); /* rate 1.0 */
  ipod_buf_be16(b, 0x0100);     /* volume 1.0 */
  ipod_buf_zero(b, 10);
  put_matrix(b);
  ipod_buf_zero(b, 24);
  ipod_buf_be32(b, 2); /* next track id */
  box_close(b, mvhd);

  size_t trak = box_open(b, "trak");
  size_t tkhd = full_box_open(b, "tkhd", 7); /* enabled, in movie, in preview */
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, 1); /* track id */
  ipod_buf_be32(b, 0);
  ipod_buf_be32(b, duration);
  ipod_buf_zero(b, 8);
  ipod_buf_be16(b, 0);      /* layer */
  ipod_buf_be16(b, 0);      /* alternate group */
  ipod_buf_be16(b, 0x0100); /* volume */
  ipod_buf_be16(b, 0);
  put_matrix(b);
  ipod_buf_be32(b, 0); /* width */
  ipod_buf_be32(b, 0); /* height */
  box_close(b, tkhd);

  size_t mdia = box_open(b, "mdia");
  size_t mdhd = full_box_open(b, "mdhd", 0);
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, now);
  ipod_buf_be32(b, m->rate);
  ipod_buf_be32(b, duration);
  ipod_buf_be16(b, 0x55C4); /* language "und" */
  ipod_buf_be16(b, 0);
  box_close(b, mdhd);
  size_t hdlr = full_box_open(b, "hdlr", 0);
  ipod_buf_be32(b, 0);
  ipod_buf_tag(b, "soun");
  ipod_buf_zero(b, 12);
  ipod_buf_put(b, "SoundHandler", 13);
  box_close(b, hdlr);

  size_t minf = box_open(b, "minf");
  size_t smhd = full_box_open(b, "smhd", 0);
  ipod_buf_zero(b, 4);
  box_close(b, smhd);
  size_t dinf = box_open(b, "dinf");
  size_t dref = full_box_open(b, "dref", 0);
  ipod_buf_be32(b, 1);
  size_t url = full_box_open(b, "url ", 1); /* data in this file */
  box_close(b, url);
  box_close(b, dref);
  box_close(b, dinf);

  size_t stbl = box_open(b, "stbl");
  size_t stsd = full_box_open(b, "stsd", 0);
  ipod_buf_be32(b, 1);
  size_t alac = box_open(b, "alac");
  ipod_buf_zero(b, 6);
  ipod_buf_be16(b, 1); /* data reference index */
  ipod_buf_zero(b, 8);
  ipod_buf_be16(b, m->channels);
  ipod_buf_be16(b, m->bits);
  ipod_buf_be16(b, 0);
  ipod_buf_be16(b, 0);
  ipod_buf_be32(b, m->rate > 65535 ? 0 : m->rate << 16);
  size_t cookie = full_box_open(b, "alac", 0);
  ipod_buf_put(b, m->cookie, m->cookie_len);
  box_close(b, cookie);
  box_close(b, alac);
  box_close(b, stsd);

  /* Every packet holds 4096 frames except perhaps the last. */
  size_t stts = full_box_open(b, "stts", 0);
  uint32_t full = (uint32_t)m->n, last = 0;
  if (m->n && m->last_frames != 4096) {
    full--;
    last = m->last_frames;
  }
  ipod_buf_be32(b, (full ? 1u : 0u) + (last ? 1u : 0u));
  if (full) {
    ipod_buf_be32(b, full);
    ipod_buf_be32(b, 4096);
  }
  if (last) {
    ipod_buf_be32(b, 1);
    ipod_buf_be32(b, last);
  }
  box_close(b, stts);

  /* One packet per chunk. */
  size_t stsc = full_box_open(b, "stsc", 0);
  ipod_buf_be32(b, m->n ? 1 : 0);
  if (m->n) {
    ipod_buf_be32(b, 1);
    ipod_buf_be32(b, 1);
    ipod_buf_be32(b, 1);
  }
  box_close(b, stsc);

  size_t stsz = full_box_open(b, "stsz", 0);
  ipod_buf_be32(b, 0);
  ipod_buf_be32(b, (uint32_t)m->n);
  for (size_t i = 0; i < m->n; i++) ipod_buf_be32(b, m->sizes[i]);
  box_close(b, stsz);

  size_t stco = full_box_open(b, "stco", 0);
  ipod_buf_be32(b, (uint32_t)m->n);
  uint32_t off = data_offset;
  for (size_t i = 0; i < m->n; i++) {
    ipod_buf_be32(b, off);
    off += m->sizes[i];
  }
  box_close(b, stco);
  box_close(b, stbl);
  box_close(b, minf);
  box_close(b, mdia);
  box_close(b, trak);

  if (tags) {
    size_t udta = box_open(b, "udta");
    size_t meta = full_box_open(b, "meta", 0);
    size_t mh = full_box_open(b, "hdlr", 0);
    ipod_buf_be32(b, 0);
    ipod_buf_tag(b, "mdir");
    ipod_buf_tag(b, "appl");
    ipod_buf_zero(b, 9);
    box_close(b, mh);
    ipod_m4a_put_ilst(b, tags);
    box_close(b, meta);
    box_close(b, udta);
  }
  box_close(b, moov);
}

int ipod_m4a_finish(ipod_m4a *m, const ipod_tags *tags, uint64_t *file_size, ipod_error *err) {
  int bad = fclose(m->data) != 0;
  m->data = NULL;
  if (bad) {
    ipod_m4a_abort(m);
    return ipod_seterr(err, IPOD_ERR_FULL, "can't write the audio data");
  }
  if (m->data_bytes > 0xF0000000u) {
    ipod_m4a_abort(m);
    return ipod_seterr(err, IPOD_ERR_UNSUPPORTED, "file too large for 32-bit MP4 offsets");
  }

  ipod_buf head = {0};
  size_t ftyp = box_open(&head, "ftyp");
  ipod_buf_tag(&head, "M4A ");
  ipod_buf_be32(&head, 0);
  ipod_buf_tag(&head, "M4A ");
  ipod_buf_tag(&head, "mp42");
  ipod_buf_tag(&head, "isom");
  ipod_buf_be32(&head, 0);
  box_close(&head, ftyp);
  size_t ftyp_len = head.len;

  /* moov's size doesn't depend on the offsets in it: measure, then write for real. */
  ipod_buf probe = {0};
  put_moov(&probe, m, tags, 0);
  size_t moov_len = probe.len;
  ipod_buf_free(&probe);
  uint32_t data_offset = (uint32_t)(ftyp_len + moov_len + 8);
  put_moov(&head, m, tags, data_offset);
  ipod_buf_be32(&head, (uint32_t)(8 + m->data_bytes));
  ipod_buf_tag(&head, "mdat");
  if (head.oom || head.len != data_offset) {
    ipod_buf_free(&head);
    ipod_m4a_abort(m);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }

  FILE *out = ipod_fopen(m->path, "wb");
  FILE *in = ipod_fopen(m->data_path, "rb");
  int ok = out && in && fwrite(head.data, 1, head.len, out) == head.len;
  ipod_buf_free(&head);
  uint8_t *buf = ok ? malloc(1 << 20) : NULL;
  ok = ok && buf;
  size_t n;
  while (ok && (n = fread(buf, 1, 1 << 20, in)) > 0)
    if (fwrite(buf, 1, n, out) != n) ok = 0;
  free(buf);
  if (in) {
    if (ferror(in)) ok = 0;
    fclose(in);
  }
  if (out && fclose(out) != 0) ok = 0;
  if (!ok) {
    ipod_m4a_abort(m);
    return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s", m->path ? m->path : "");
  }
  ipod_remove(m->data_path);
  if (file_size) *file_size = data_offset + m->data_bytes;
  free(m->path);
  free(m->data_path);
  free(m->sizes);
  free(m);
  return 0;
}
