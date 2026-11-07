/* Encoders: a synthetic signal to ALAC (boxes checked, packets decoded back bit for bit)
 * and to MP3 (ID3 tag, Xing/LAME frame, frame count against the duration), several at once
 * on different threads. */
#include <math.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <stb_image_write.h>

#include "alac_shim.h"
#include "fixtures.h"
#include "kithipod/encoder.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static ipod_buf g_cover;

/* frames of a two-tone signal with a little noise, as int16_t or 24-bit-in-int32_t. */
static void *make_pcm(uint32_t rate, uint16_t channels, uint16_t bits, size_t frames) {
  size_t bps = bits == 16 ? 2 : 4;
  uint8_t *p = malloc(frames * channels * bps);
  uint32_t seed = 12345;
  for (size_t i = 0; i < frames; i++) {
    for (uint16_t c = 0; c < channels; c++) {
      double f = c == 0 ? 1000.0 : 440.0;
      seed = seed * 1103515245u + 12345u;
      double noise = ((double)(seed >> 16) / 65536.0 - 0.5) * 0.01;
      double v = 0.5 * sin(2 * M_PI * f * (double)i / rate) + noise;
      if (bits == 16) ((int16_t *)p)[i * channels + c] = (int16_t)(v * 32767);
      else ((int32_t *)p)[i * channels + c] = (int32_t)(v * 8388607);
    }
  }
  return p;
}

/* --- MP4 parsing ------------------------------------------------------------------------ */

typedef struct box {
  const uint8_t *p;
  uint32_t size;
} box;

/* Finds a child box of type within [p, p+len). */
static int find_box(const uint8_t *p, size_t len, const char *type, box *out) {
  size_t off = 0;
  while (off + 8 <= len) {
    uint32_t size = ipod_rdbe32(p + off);
    if (size < 8 || off + size > len) return 0;
    if (memcmp(p + off + 4, type, 4) == 0) {
      out->p = p + off;
      out->size = size;
      return 1;
    }
    off += size;
  }
  return 0;
}

/* Follows a path like "moov/trak/mdia/minf/stbl". */
static int find_path(const uint8_t *p, size_t len, const char *path, box *out) {
  char part[5] = {0};
  box cur = {p - 8, (uint32_t)len + 8};
  while (*path) {
    memcpy(part, path, 4);
    path += 4;
    if (*path == '/') path++;
    if (!find_box(cur.p + 8, cur.size - 8, part, &cur)) return 0;
  }
  *out = cur;
  return 1;
}

static void check_alac(const char *path, const void *pcm, uint32_t rate, uint16_t channels, uint16_t bits,
                       size_t frames, int expect_cover) {
  size_t len;
  uint8_t *d = ipod_read_file(path, &len);
  CHECK(d != NULL);
  if (!d) return;
  box ftyp, moov, mdat, b;
  CHECK(find_box(d, len, "ftyp", &ftyp) && memcmp(ftyp.p + 8, "M4A ", 4) == 0);
  CHECK(find_box(d, len, "moov", &moov));
  CHECK(find_box(d, len, "mdat", &mdat));
  CHECK(moov.p < mdat.p); /* moov first, as iTunes writes */
  CHECK(find_path(d, len, "moov/trak/mdia/mdhd", &b));
  CHECK_EQ(ipod_rdbe32(b.p + 20), rate);
  CHECK_EQ(ipod_rdbe32(b.p + 24), frames);
  CHECK(find_path(d, len, "moov/trak/mdia/hdlr", &b) && memcmp(b.p + 16, "soun", 4) == 0);
  box stbl, stsd, stsz, stco, stts;
  CHECK(find_path(d, len, "moov/trak/mdia/minf/stbl", &stbl));
  CHECK(find_box(stbl.p + 8, stbl.size - 8, "stsd", &stsd));
  const uint8_t *entry = stsd.p + 16;
  CHECK(memcmp(entry + 4, "alac", 4) == 0);
  CHECK_EQ(ipod_rdbe32(entry) >= 36 + 36, 1);
  CHECK_EQ((entry[24] << 8) | entry[25], channels);
  CHECK_EQ((entry[26] << 8) | entry[27], bits);
  CHECK_EQ(ipod_rdbe32(entry + 32), rate << 16);
  const uint8_t *cookie_box = entry + 36;
  CHECK(memcmp(cookie_box + 4, "alac", 4) == 0);
  const uint8_t *cookie = cookie_box + 12;
  uint32_t cookie_len = ipod_rdbe32(cookie_box) - 12;
  CHECK_EQ(cookie_len, 24);
  CHECK_EQ(ipod_rdbe32(cookie), 4096); /* frames per packet */
  CHECK_EQ(cookie[5], bits);
  CHECK_EQ(cookie[9], channels);
  CHECK_EQ(ipod_rdbe32(cookie + 20), rate);
  CHECK(find_box(stbl.p + 8, stbl.size - 8, "stts", &stts));
  CHECK(find_box(stbl.p + 8, stbl.size - 8, "stsz", &stsz));
  CHECK(find_box(stbl.p + 8, stbl.size - 8, "stco", &stco));
  uint32_t n = ipod_rdbe32(stsz.p + 16);
  CHECK_EQ(n, (frames + 4095) / 4096);
  CHECK_EQ(ipod_rdbe32(stco.p + 12), n);
  uint64_t stts_total = 0;
  for (uint32_t i = 0, ne = ipod_rdbe32(stts.p + 12); i < ne; i++)
    stts_total += (uint64_t)ipod_rdbe32(stts.p + 16 + 8 * i) * ipod_rdbe32(stts.p + 20 + 8 * i);
  CHECK_EQ(stts_total, frames);
  CHECK(find_path(d, len, "moov/udta/meta", &b));
  box ilst, item;
  CHECK(find_box(b.p + 12, b.size - 12, "ilst", &ilst));
  CHECK(find_box(ilst.p + 8, ilst.size - 8, "\xa9nam", &item) && memcmp(item.p + 24, "Sine Wave", 9) == 0);
  CHECK(find_box(ilst.p + 8, ilst.size - 8, "trkn", &item) && item.p[27] == 3 && item.p[29] == 10);
  CHECK(find_box(ilst.p + 8, ilst.size - 8, "disk", &item) && item.p[27] == 1);
  CHECK(find_box(ilst.p + 8, ilst.size - 8, "\xa9" "day", &item) && memcmp(item.p + 24, "2024", 4) == 0);
  CHECK_EQ(find_box(ilst.p + 8, ilst.size - 8, "covr", &item), expect_cover);
  if (expect_cover) CHECK_EQ(ipod_rdbe32(item.p + 16), 13); /* JPEG */

  /* Decode every packet and compare with the input. */
  ipod_alac_dec *dec = ipod_alac_dec_new(cookie, cookie_len);
  CHECK(dec != NULL);
  size_t in_bps = bits == 16 ? 2 : 4, out_bps = bits / 8;
  uint8_t *outbuf = malloc(4096 * (size_t)channels * out_bps);
  size_t done = 0;
  int mismatch = 0;
  for (uint32_t i = 0; dec && i < n; i++) {
    uint32_t off = ipod_rdbe32(stco.p + 16 + 4 * i), sz = ipod_rdbe32(stsz.p + 20 + 4 * i);
    CHECK(off + sz <= len && off >= (size_t)(mdat.p - d) + 8);
    int32_t got = ipod_alac_dec_packet(dec, d + off, sz, outbuf, channels);
    CHECK(got > 0);
    if (got <= 0) break;
    for (size_t k = 0; k < (size_t)got * channels && !mismatch; k++) {
      int32_t want, have;
      if (bits == 16) {
        want = ((const int16_t *)pcm)[done * channels + k];
        have = (int16_t)ipod_rd16(outbuf + 2 * k);
      } else {
        want = ((const int32_t *)pcm)[done * channels + k];
        const uint8_t *q = outbuf + 3 * k;
        have = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 8;
      }
      if (want != have) mismatch = 1;
    }
    done += (size_t)got;
  }
  (void)in_bps;
  CHECK(!mismatch);
  CHECK_EQ(done, frames);
  ipod_alac_dec_free(dec);
  free(outbuf);
  free(d);
}

/* --- MP3 parsing ------------------------------------------------------------------------------- */

static const int mp3_bitrates[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
static const int mp3_rates[4] = {44100, 48000, 32000, 0};

static void check_mp3(const char *path, uint32_t rate, uint16_t channels, size_t frames, int cbr_kbps) {
  size_t len;
  uint8_t *d = ipod_read_file(path, &len);
  CHECK(d != NULL);
  if (!d) return;
  CHECK(len > 10 && memcmp(d, "ID3", 3) == 0 && d[3] == 3);
  size_t tag = 10 + (((size_t)d[6] << 21) | ((size_t)d[7] << 14) | ((size_t)d[8] << 7) | d[9]);
  CHECK(test_contains(d, tag, (const uint8_t *)"TIT2", 4));
  CHECK(test_contains(d, tag, (const uint8_t *)"Sine Wave", 9));
  CHECK(test_contains(d, tag, (const uint8_t *)"TRCK", 4));
  CHECK(test_contains(d, tag, (const uint8_t *)"3/10", 4));
  CHECK(test_contains(d, tag, (const uint8_t *)"APIC", 4));
  CHECK(test_contains(d, tag, (const uint8_t *)"image/jpeg", 10));
  /* UTF-16 with a BOM for the non-ASCII artist, "Straße". */
  CHECK(test_contains(d, tag, (const uint8_t *)"\xFF\xFES\x00t\x00", 6));
  CHECK(test_contains(d, tag, (const uint8_t *)"\xDF\x00" "e\x00", 4));

  size_t off = tag, n = 0, first = 1;
  int bad = 0, xing = 0;
  while (off + 4 <= len) {
    const uint8_t *h = d + off;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
      bad = 1;
      break;
    }
    int version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3;
    int bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
    if (version != 3 || layer != 1 || !mp3_bitrates[bri] || !mp3_rates[sri]) {
      bad = 1;
      break;
    }
    CHECK_EQ(mp3_rates[sri], rate);
    size_t flen = (size_t)(144 * mp3_bitrates[bri] * 1000 / mp3_rates[sri]) + (size_t)pad;
    if (first) {
      size_t side = channels == 1 ? 17 : 32;
      xing = memcmp(h + 4 + side, "Xing", 4) == 0 || memcmp(h + 4 + side, "Info", 4) == 0;
      if (cbr_kbps) CHECK(memcmp(h + 4 + side, "Info", 4) == 0);
      first = 0;
    } else {
      if (cbr_kbps) CHECK_EQ(mp3_bitrates[bri], cbr_kbps);
      n++;
    }
    off += flen;
  }
  CHECK(!bad);
  CHECK(xing);
  CHECK(off == len);
  /* LAME adds its delay and padding: within two frames of the input's length. */
  long long decoded = (long long)n * 1152;
  CHECK(decoded >= (long long)frames && decoded <= (long long)frames + 3 * 1152);
  free(d);
}

/* --- Jobs, on threads ------------------------------------------------------------------------- */

typedef struct job {
  const char *path;
  ipod_enc_settings settings;
  ipod_pcm_format fmt;
  const void *pcm;
  size_t frames;
  int result;
  ipod_enc_result res;
} job;

static void run_job(job *j) {
  ipod_tags tags = {0};
  tags.title = "Sine Wave";
  tags.artist = "Straße";
  tags.album = "Test Tones";
  tags.year = 2024;
  tags.track_number = 3;
  tags.track_total = 10;
  tags.disc_number = 1;
  tags.disc_total = 1;
  tags.compilation = 1;
  tags.comment = "made by test_ipod_encode";
  tags.cover = g_cover.data;
  tags.cover_len = g_cover.len;
  ipod_error err = {0};
  ipod_encoder *e = ipod_encoder_open(j->path, &j->settings, &j->fmt, &tags, &err);
  if (!e) {
    fprintf(stderr, "open %s: %s\n", j->path, err.message);
    j->result = -1;
    return;
  }
  /* Odd chunk sizes, to cross packet boundaries. */
  size_t bpf = (size_t)j->fmt.channels * (j->fmt.bits == 16 ? 2 : 4), off = 0, step = 1000;
  while (off < j->frames) {
    size_t n = j->frames - off < step ? j->frames - off : step;
    if (ipod_encoder_write(e, (const uint8_t *)j->pcm + off * bpf, n, &err) != 0) {
      ipod_encoder_abort(e);
      j->result = -1;
      return;
    }
    off += n;
    step = step == 1000 ? 7777 : 1000;
  }
  j->result = ipod_encoder_finish(e, &j->res, &err);
  if (j->result) fprintf(stderr, "finish %s: %s\n", j->path, err.message);
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
  run_job(p);
  return 0;
}
#else
static void *thread_main(void *p) {
  run_job(p);
  return NULL;
}
#endif

static void jpg_sink(void *ctx, void *data, int size) { ipod_buf_put((ipod_buf *)ctx, data, (size_t)size); }

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  char *dir = test_scratch(root, "enc");
  uint8_t px[32 * 32 * 3];
  for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i * 7);
  stbi_write_jpg_to_func(jpg_sink, &g_cover, 32, 32, 3, px, 80);

  /* Settings. */
  ipod_enc_settings s;
  CHECK_EQ(ipod_enc_settings_parse("alac", NULL, &s), 0);
  CHECK_EQ(s.codec, IPOD_CODEC_ALAC);
  CHECK_EQ(ipod_enc_settings_parse("mp3", "v2", &s), 0);
  CHECK(s.codec == IPOD_CODEC_MP3 && s.mp3_vbr && s.mp3_vbr_quality == 2);
  CHECK_EQ(ipod_enc_settings_parse("mp3", "256", &s), 0);
  CHECK(!s.mp3_vbr && s.mp3_bitrate == 256);
  CHECK_EQ(ipod_enc_settings_parse("mp3", "100", &s), IPOD_ERR_ARG);
  CHECK_EQ(ipod_enc_settings_parse("ogg", NULL, &s), IPOD_ERR_ARG);
  size_t nf;
  const ipod_format_desc *fd = ipod_encoder_formats(&nf);
  CHECK(nf == 2 && fd[0].lossless && !fd[1].lossless && fd[1].n_settings == 6);

  const size_t frames44 = 44100 * 3 + 123, frames48 = 48000 + 77;
  void *pcm16 = make_pcm(44100, 2, 16, frames44);
  void *pcm24 = make_pcm(48000, 1, 24, frames48);
  void *pcm16m = make_pcm(48000, 1, 16, frames48);

  job jobs[5];
  memset(jobs, 0, sizeof jobs);
  char *paths[5];
  const char *names[5] = {"a16.m4a", "a24.m4a", "v0.mp3", "cbr128.mp3", "mono.mp3"};
  for (int i = 0; i < 5; i++) paths[i] = ipod_path_join(dir, names[i]);
  jobs[0] = (job){paths[0], {IPOD_CODEC_ALAC, 0, 0, 0}, {44100, 2, 16}, pcm16, frames44, 0, {0}};
  jobs[1] = (job){paths[1], {IPOD_CODEC_ALAC, 0, 0, 0}, {48000, 1, 24}, pcm24, frames48, 0, {0}};
  jobs[2] = (job){paths[2], {IPOD_CODEC_MP3, 1, 0, 0}, {44100, 2, 16}, pcm16, frames44, 0, {0}};
  jobs[3] = (job){paths[3], {IPOD_CODEC_MP3, 0, 0, 128}, {44100, 2, 16}, pcm16, frames44, 0, {0}};
  jobs[4] = (job){paths[4], {IPOD_CODEC_MP3, 1, 2, 0}, {48000, 1, 16}, pcm16m, frames48, 0, {0}};

  /* All five at once. */
#ifdef _WIN32
  HANDLE th[5];
  for (int i = 0; i < 5; i++) th[i] = CreateThread(NULL, 0, thread_main, &jobs[i], 0, NULL);
  WaitForMultipleObjects(5, th, TRUE, INFINITE);
  for (int i = 0; i < 5; i++) CloseHandle(th[i]);
#else
  pthread_t th[5];
  for (int i = 0; i < 5; i++) pthread_create(&th[i], NULL, thread_main, &jobs[i]);
  for (int i = 0; i < 5; i++) pthread_join(th[i], NULL);
#endif
  for (int i = 0; i < 5; i++) CHECK_EQ(jobs[i].result, 0);

  check_alac(paths[0], pcm16, 44100, 2, 16, frames44, 1);
  check_alac(paths[1], pcm24, 48000, 1, 24, frames48, 1);
  CHECK_EQ(jobs[0].res.frames, frames44);
  CHECK_EQ(jobs[0].res.length_ms, frames44 * 1000 / 44100);
  CHECK(jobs[0].res.bitrate_kbps > 100 && jobs[0].res.bitrate_kbps < 1411);
  CHECK_EQ(jobs[0].res.file_size, ipod_file_size(paths[0]));
  check_mp3(paths[2], 44100, 2, frames44, 0);
  check_mp3(paths[3], 44100, 2, frames44, 128);
  check_mp3(paths[4], 48000, 1, frames48, 0);
  CHECK(jobs[3].res.bitrate_kbps >= 120 && jobs[3].res.bitrate_kbps <= 140);
  CHECK(jobs[2].res.bitrate_kbps > jobs[3].res.bitrate_kbps); /* V0 on a busy signal */
  CHECK_EQ(jobs[2].res.file_size, ipod_file_size(paths[2]));
  char *side = ipod_asprintf("%s.mdat", paths[0]);
  CHECK(!ipod_file_exists(side));
  free(side);

  /* The same MP3 encode again gives the same bytes: no state leaks between encoders. */
  char *again = ipod_path_join(dir, "v0-again.mp3");
  job j2 = jobs[2];
  j2.path = again;
  run_job(&j2);
  size_t la, lb;
  uint8_t *a = ipod_read_file(paths[2], &la), *b = ipod_read_file(again, &lb);
  CHECK(a && b && la == lb && memcmp(a, b, la) == 0);
  free(a);
  free(b);
  free(again);

  /* Abort leaves nothing behind; bad formats are refused. */
  char *ab = ipod_path_join(dir, "aborted.m4a");
  ipod_encoder *e = ipod_encoder_open(ab, &jobs[0].settings, &jobs[0].fmt, NULL, NULL);
  CHECK(e != NULL);
  if (e) {
    ipod_encoder_write(e, pcm16, 5000, NULL);
    ipod_encoder_abort(e);
  }
  CHECK(!ipod_file_exists(ab));
  ipod_pcm_format bad = {44100, 6, 16};
  CHECK(ipod_encoder_open(ab, &jobs[0].settings, &bad, NULL, NULL) == NULL);
  ipod_pcm_format hi = {96000, 2, 16};
  CHECK(ipod_encoder_open(ab, &jobs[2].settings, &hi, NULL, NULL) == NULL);
  free(ab);

  for (int i = 0; i < 5; i++) free(paths[i]);
  free(pcm16);
  free(pcm24);
  free(pcm16m);
  ipod_buf_free(&g_cover);
  free(dir);
  return TEST_RESULT();
}
