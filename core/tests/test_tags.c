/* Round-trips tags through TagLib. The fixture is generated (1 s of silent 16-bit mono
 * WAV) rather than checked in, so the repo carries no binary test files. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "test.h"

static void put_u32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static void put_u16(unsigned char *p, uint16_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
}

static int write_silent_wav(const char *path) {
  enum { RATE = 8000, SECONDS = 1, DATA_LEN = RATE * 2 * SECONDS };
  unsigned char hdr[44];
  memcpy(hdr, "RIFF", 4);
  put_u32(hdr + 4, 36 + DATA_LEN);
  memcpy(hdr + 8, "WAVEfmt ", 8);
  put_u32(hdr + 16, 16);       /* fmt chunk size */
  put_u16(hdr + 20, 1);        /* PCM */
  put_u16(hdr + 22, 1);        /* mono */
  put_u32(hdr + 24, RATE);
  put_u32(hdr + 28, RATE * 2); /* byte rate */
  put_u16(hdr + 32, 2);        /* block align */
  put_u16(hdr + 34, 16);       /* bits per sample */
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, DATA_LEN);

  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char silence[DATA_LEN];
  int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(silence, sizeof silence, 1, f) == 1;
  return fclose(f) == 0 && ok;
}

int main(int argc, char **argv) {
  char path[1024];
  snprintf(path, sizeof path, "%s/test_tags.wav", argc > 1 ? argv[1] : ".");
  CHECK(write_silent_wav(path));

  mc_tags in = {0};
  in.title = "Sunflower";
  in.artist = "Low";
  in.album = "Things We Lost in the Fire";
  in.mb_trackid = "8f8e3b9c-2b4e-4f6a-9d2a-1c3b5e7f9a0b";
  in.year = 2001;
  in.track = 1;
  in.track_total = 13;
  CHECK(mc_tags_write(path, &in) == MC_OK);

  mc_tags out;
  CHECK(mc_tags_read(path, &out) == MC_OK);
  CHECK_STR(out.title, in.title);
  CHECK_STR(out.artist, in.artist);
  CHECK_STR(out.album, in.album);
  CHECK_STR(out.mb_trackid, in.mb_trackid);
  CHECK(out.album_artist == NULL);
  CHECK(out.year == 2001);
  CHECK(out.track == 1 && out.track_total == 13);
  CHECK(out.sample_rate == 8000 && out.channels == 1);
  CHECK(out.duration_ms >= 990 && out.duration_ms <= 1010);
  mc_tags_clear(&out);

  /* NULL leaves a tag alone, "" removes it. */
  mc_tags edit = {0};
  edit.mb_trackid = "";
  edit.album_artist = "Low";
  CHECK(mc_tags_write(path, &edit) == MC_OK);
  CHECK(mc_tags_read(path, &out) == MC_OK);
  CHECK(out.mb_trackid == NULL);
  CHECK_STR(out.album_artist, "Low");
  CHECK_STR(out.title, in.title);
  mc_tags_clear(&out);

  CHECK(mc_tags_read("does-not-exist.flac", &out) == MC_ERR_FORMAT);

  return TEST_RESULT();
}
