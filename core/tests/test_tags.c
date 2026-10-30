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

/* A FLAC file with a STREAMINFO block and no audio frames, which TagLib can tag. */
static int write_empty_flac(const char *path) {
  unsigned char b[4 + 4 + 34] = "fLaC";
  b[4] = 0x80; /* last block, STREAMINFO */
  b[7] = 34;
  b[8] = 0x10; /* min block size 4096 */
  b[10] = 0x10; /* max block size 4096 */
  /* 8000 Hz (20 bits), mono, 16 bits per sample, 0 samples. */
  b[18] = 0x01;
  b[19] = 0xF4;
  b[20] = 0x00 | (0 << 1) | 0;
  b[21] = 0xF0;
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(b, sizeof b, 1, f) == 1;
  return fclose(f) == 0 && ok;
}

/* Twenty silent MPEG-1 layer III frames, 128 kb/s at 44.1 kHz. */
static int write_silent_mp3(const char *path) {
  enum { FRAME = 417 };
  static unsigned char frame[FRAME];
  frame[0] = 0xFF;
  frame[1] = 0xFB;
  frame[2] = 0x90;
  frame[3] = 0xC0; /* mono */
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  int ok = 1;
  for (int i = 0; i < 20; i++) ok &= fwrite(frame, sizeof frame, 1, f) == 1;
  return fclose(f) == 0 && ok;
}

#define ID_A "11111111-1111-4111-8111-111111111111"
#define ID_B "22222222-2222-4222-8222-222222222222"

/* ARTISTS, ALBUMARTISTS and several artist ids survive a write and a read, and lists are
 * replaced and removed as tagger.h says. */
static void check_multi(const char *path) {
  mc_tags in = {0};
  in.title = "Run This Town";
  in.artist = "Jay-Z feat. Rihanna";
  in.album_artist = "Jay-Z & Kanye West";
  in.artists = "Jay-Z\nRihanna";
  in.album_artists = "Jay-Z\nKanye West";
  in.mb_artistid = ID_B; /* the lists win */
  in.mb_artistids = ID_A "\n" ID_B;
  in.mb_albumartistids = ID_A "\n" ID_B;
  if (mc_tags_write(path, &in) != MC_OK) {
    fprintf(stderr, "can't tag %s\n", path);
    CHECK(0);
    return;
  }
  mc_tags out;
  CHECK(mc_tags_read(path, &out) == MC_OK);
  CHECK_STR(out.artist, "Jay-Z feat. Rihanna");
  CHECK_STR(out.album_artist, "Jay-Z & Kanye West");
  CHECK_STR(out.artists, "Jay-Z\nRihanna");
  CHECK_STR(out.album_artists, "Jay-Z\nKanye West");
  CHECK_STR(out.mb_artistids, ID_A "\n" ID_B);
  CHECK_STR(out.mb_albumartistids, ID_A "\n" ID_B);
  CHECK_STR(out.mb_artistid, ID_A); /* the first value */
  CHECK_STR(out.mb_albumartistid, ID_A);
  if (test_failures) fprintf(stderr, "multi-valued tags failed for %s\n", path);
  mc_tags_clear(&out);

  /* One id without the list replaces them all; "" removes a list. */
  mc_tags edit = {0};
  edit.mb_artistid = ID_B;
  edit.artists = "";
  edit.mb_albumartistids = ID_B;
  CHECK(mc_tags_write(path, &edit) == MC_OK);
  CHECK(mc_tags_read(path, &out) == MC_OK);
  CHECK(out.artists == NULL);
  CHECK_STR(out.album_artists, "Jay-Z\nKanye West");
  CHECK_STR(out.mb_artistids, ID_B);
  CHECK_STR(out.mb_artistid, ID_B);
  CHECK_STR(out.mb_albumartistids, ID_B);
  mc_tags_clear(&out);
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

  /* Multi-valued tags in each container: ID3v2 in WAV and MP3 (TXXX frames), Vorbis
   * comments in FLAC and Opus. */
  const char *dir = argc > 1 ? argv[1] : ".";
  check_multi(path);
  char other[1100];
  snprintf(other, sizeof other, "%s/test_tags.flac", dir);
  CHECK(write_empty_flac(other));
  check_multi(other);
  snprintf(other, sizeof other, "%s/test_tags.mp3", dir);
  CHECK(write_silent_mp3(other));
  check_multi(other);
  char cache[1100];
  snprintf(cache, sizeof cache, "%s/test_tags_opus", dir);
  mc_transcode_opts opts = {MC_FORMAT_OPUS, 64};
  char *opus = NULL;
  CHECK(mc_transcode_cached(cache, "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6f", path, &opts, &opus) == MC_OK);
  if (opus) check_multi(opus);
  mc_free(opus);
  mc_transcode_cache_trim(cache, 0);

  return TEST_RESULT();
}
