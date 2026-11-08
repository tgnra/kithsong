#include "id3.h"

#include <string.h>

static size_t frame_open(ipod_buf *b, const char id[4]) {
  size_t at = b->len;
  ipod_buf_tag(b, id);
  ipod_buf_be32(b, 0); /* size, below; v2.3 sizes are plain 32-bit */
  ipod_buf_be16(b, 0); /* flags */
  return at;
}

static void frame_close(ipod_buf *b, size_t at) { ipod_buf_set_be32(b, at + 4, (uint32_t)(b->len - at - 10)); }

/* Text in the frame's chosen encoding, with no terminator. */
static void put_enc_text(ipod_buf *b, const char *s, int utf16) {
  if (!utf16) {
    ipod_buf_put(b, s, strlen(s));
    return;
  }
  size_t n;
  uint8_t *u = ipod_utf8_to_utf16le(s, &n);
  if (!u) {
    b->oom = 1;
    return;
  }
  ipod_buf_u8(b, 0xFF);
  ipod_buf_u8(b, 0xFE);
  ipod_buf_put(b, u, n);
  free(u);
}

static void put_text_frame(ipod_buf *b, const char id[4], const char *s) {
  if (!s || !*s) return;
  int utf16 = !ipod_is_ascii(s);
  size_t at = frame_open(b, id);
  ipod_buf_u8(b, utf16 ? 1 : 0);
  put_enc_text(b, s, utf16);
  frame_close(b, at);
}

static void put_pair(ipod_buf *b, const char id[4], int n, int total) {
  if (n <= 0 && total <= 0) return;
  char s[32];
  if (total > 0) snprintf(s, sizeof s, "%d/%d", n > 0 ? n : 0, total);
  else snprintf(s, sizeof s, "%d", n);
  put_text_frame(b, id, s);
}

void ipod_id3_build(ipod_buf *b, const ipod_tags *t) {
  size_t start = b->len;
  ipod_buf_put(b, "ID3", 3);
  ipod_buf_u8(b, 3); /* v2.3.0 */
  ipod_buf_u8(b, 0);
  ipod_buf_u8(b, 0); /* flags */
  ipod_buf_be32(b, 0); /* syncsafe size, below */

  put_text_frame(b, "TIT2", t->title);
  put_text_frame(b, "TPE1", t->artist);
  put_text_frame(b, "TPE2", t->album_artist);
  put_text_frame(b, "TALB", t->album);
  put_text_frame(b, "TCON", t->genre);
  put_text_frame(b, "TCOM", t->composer);
  put_text_frame(b, "TIT1", t->grouping);
  if (t->year > 0) {
    char y[16];
    snprintf(y, sizeof y, "%d", t->year);
    put_text_frame(b, "TYER", y);
  }
  put_pair(b, "TRCK", t->track_number, t->track_total);
  put_pair(b, "TPOS", t->disc_number, t->disc_total);
  if (t->compilation) put_text_frame(b, "TCMP", "1");
  if (t->bpm > 0) {
    char s[16];
    snprintf(s, sizeof s, "%d", t->bpm);
    put_text_frame(b, "TBPM", s);
  }
  put_text_frame(b, "TSOT", t->sort_title);
  put_text_frame(b, "TSOP", t->sort_artist);
  put_text_frame(b, "TSO2", t->sort_album_artist);
  put_text_frame(b, "TSOA", t->sort_album);
  put_text_frame(b, "TSOC", t->sort_composer);
  put_text_frame(b, "TSSE", "Kithsong (LAME 3.100)");
  if (t->comment && *t->comment) {
    int utf16 = !ipod_is_ascii(t->comment);
    size_t at = frame_open(b, "COMM");
    ipod_buf_u8(b, utf16 ? 1 : 0);
    ipod_buf_put(b, "eng", 3);
    if (utf16) {
      /* An empty description: BOM and a two-byte terminator. */
      ipod_buf_u8(b, 0xFF);
      ipod_buf_u8(b, 0xFE);
      ipod_buf_le16(b, 0);
    } else {
      ipod_buf_u8(b, 0);
    }
    put_enc_text(b, t->comment, utf16);
    frame_close(b, at);
  }
  if (t->cover && t->cover_len > 4) {
    const char *mime = NULL;
    if (t->cover[0] == 0xFF && t->cover[1] == 0xD8) mime = "image/jpeg";
    else if (memcmp(t->cover, "\x89PNG", 4) == 0) mime = "image/png";
    if (mime) {
      size_t at = frame_open(b, "APIC");
      ipod_buf_u8(b, 0); /* ISO-8859-1 description */
      ipod_buf_put(b, mime, strlen(mime) + 1);
      ipod_buf_u8(b, 3); /* front cover */
      ipod_buf_u8(b, 0); /* empty description */
      ipod_buf_put(b, t->cover, t->cover_len);
      frame_close(b, at);
    }
  }
  ipod_buf_zero(b, 256); /* padding, for editors that rewrite in place */

  size_t size = b->len - start - 10;
  if (!b->oom) {
    uint8_t *h = b->data + start + 6;
    h[0] = (uint8_t)((size >> 21) & 0x7F);
    h[1] = (uint8_t)((size >> 14) & 0x7F);
    h[2] = (uint8_t)((size >> 7) & 0x7F);
    h[3] = (uint8_t)(size & 0x7F);
  }
}
