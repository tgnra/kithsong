/* Importing .zip files (tagger/unzip.c and "Zips" in tagger.h). The zips are built here, by
 * hand, so their flags and names can be anything: stored or deflated (miniz's deflater),
 * with names in CP437 or flagged UTF-8, with zip-slip entries, Mac junk and a lying size.
 * Checks the name decoding and path checks on their own, extraction, and then a whole import
 * of a flat Bandcamp-style zip with skip_tagging (no network): its tracks and cover reach the
 * library, a broken zip becomes a failed item, and the staging folder is gone afterwards.
 * Last, a dropped folder: its zips are extracted, a zip inside one of them isn't, and the
 * session's limit on all its zips together leaves one out. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "musiccore/i18n.h"
#include "musiccore/musiccore.h"
#include "musiccore/tagger.h"
#include "tagger/unzip.h"
#include "util/platform.h"
#include "test.h"

/* --- Building zips --- */

typedef struct zip_entry {
  const char *name; /* raw bytes, as stored */
  const unsigned char *data;
  size_t len;
  int utf8;    /* set general purpose flag bit 11 */
  int deflate; /* compress, else store */
} zip_entry;

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xFF), f);
  fputc((int)((v >> 8) & 0xFF), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xFFFF);
  put16(f, v >> 16);
}

/* An entry's extra field, in the central directory only. */
typedef struct zip_extra {
  const unsigned char *data;
  size_t len;
} zip_extra;

/* Writes a zip of the n entries to path; extras (may be NULL) holds one per entry. */
static int write_zip_ex(const char *path, const zip_entry *e, int n, const zip_extra *extras) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t *offsets = calloc((size_t)n, sizeof *offsets);
  uint32_t *crcs = calloc((size_t)n, sizeof *crcs);
  uint32_t *sizes = calloc((size_t)n, sizeof *sizes);
  if (!offsets || !crcs || !sizes) return 0;
  for (int i = 0; i < n; i++) {
    void *packed = NULL;
    size_t packed_len = e[i].len;
    const void *body = e[i].data;
    if (e[i].deflate) {
      packed = tdefl_compress_mem_to_heap(e[i].data, e[i].len, &packed_len, TDEFL_DEFAULT_MAX_PROBES);
      body = packed;
    }
    offsets[i] = (uint32_t)ftell(f);
    crcs[i] = (uint32_t)mz_crc32(MZ_CRC32_INIT, e[i].data, e[i].len);
    sizes[i] = (uint32_t)packed_len;
    put32(f, 0x04034b50);
    put16(f, 20);
    put16(f, e[i].utf8 ? 0x0800 : 0);
    put16(f, e[i].deflate ? 8 : 0);
    put16(f, 0);    /* time */
    put16(f, 0x21); /* 1980-01-01 */
    put32(f, crcs[i]);
    put32(f, sizes[i]);
    put32(f, (uint32_t)e[i].len);
    put16(f, (unsigned)strlen(e[i].name));
    put16(f, 0);
    fwrite(e[i].name, 1, strlen(e[i].name), f);
    if (packed_len) fwrite(body, 1, packed_len, f);
    mz_free(packed);
  }
  uint32_t cd = (uint32_t)ftell(f);
  for (int i = 0; i < n; i++) {
    put32(f, 0x02014b50);
    put16(f, 20);
    put16(f, 20);
    put16(f, e[i].utf8 ? 0x0800 : 0);
    put16(f, e[i].deflate ? 8 : 0);
    put16(f, 0);
    put16(f, 0x21);
    put32(f, crcs[i]);
    put32(f, sizes[i]);
    put32(f, (uint32_t)e[i].len);
    put16(f, (unsigned)strlen(e[i].name));
    put16(f, extras ? (unsigned)extras[i].len : 0);
    put16(f, 0); /* comment */
    put16(f, 0); /* disk */
    put16(f, 0); /* internal attributes */
    put32(f, 0); /* external attributes */
    put32(f, offsets[i]);
    fwrite(e[i].name, 1, strlen(e[i].name), f);
    if (extras && extras[i].len) fwrite(extras[i].data, 1, extras[i].len, f);
  }
  uint32_t cd_size = (uint32_t)ftell(f) - cd;
  put32(f, 0x06054b50);
  put16(f, 0);
  put16(f, 0);
  put16(f, (unsigned)n);
  put16(f, (unsigned)n);
  put32(f, cd_size);
  put32(f, cd);
  put16(f, 0);
  free(offsets);
  free(crcs);
  free(sizes);
  return fclose(f) == 0;
}

static int write_zip(const char *path, const zip_entry *e, int n) {
  return write_zip_ex(path, e, n, NULL);
}

/* --- Files --- */

static void put_u32(unsigned char *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* Not a whole JPEG, but what the cover code checks: the magic bytes. */
static const unsigned char k_jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 'c', 'o', 'v', 'e', 'r', 0xFF, 0xD9};

/* A tenth of a second of silent 8 kHz mono WAV with tags, read back into *out (malloc'd). */
static size_t make_track(const char *scratch, const char *artist, const char *album, const char *title,
                         int track, unsigned char **out) {
  enum { DATA_LEN = 1600 };
  unsigned char hdr[44] = "RIFF....WAVEfmt ";
  put_u32(hdr + 4, 36 + DATA_LEN);
  put_u32(hdr + 16, 16);
  hdr[20] = 1;
  hdr[22] = 1;
  put_u32(hdr + 24, 8000);
  put_u32(hdr + 28, 16000);
  hdr[32] = 2;
  hdr[34] = 16;
  memcpy(hdr + 36, "data", 4);
  put_u32(hdr + 40, DATA_LEN);
  FILE *f = mc_fopen(scratch, "wb");
  if (!f) return 0;
  static const unsigned char silence[DATA_LEN];
  fwrite(hdr, sizeof hdr, 1, f);
  fwrite(silence, sizeof silence, 1, f);
  fclose(f);
  mc_tags tags = {0};
  tags.artist = (char *)artist;
  tags.album = (char *)album;
  tags.title = (char *)title;
  tags.track = track;
  if (mc_tags_write(scratch, &tags) != MC_OK) return 0;
  int64_t size = 0, mtime = 0;
  if (mc_file_info(scratch, &size, &mtime) != MC_OK || size <= 0) return 0;
  *out = malloc((size_t)size);
  f = mc_fopen(scratch, "rb");
  size_t n = f && *out ? fread(*out, 1, (size_t)size, f) : 0;
  if (f) fclose(f);
  mc_remove(scratch);
  return n;
}

static void count_cb(void *user, const char *name, int is_dir) {
  (void)name;
  (void)is_dir;
  (*(int *)user)++;
}

static int count_entries(const char *dir) {
  int n = 0;
  mc_list_dir(dir, count_cb, &n);
  return n;
}

static void path_join(char *out, size_t cap, const char *a, const char *b) {
  snprintf(out, cap, "%s%c%s", a, MC_PATH_SEP, b);
}

/* --- Names and paths --- */

/* Writes a Unicode Path extra field (0x7075) for the stored name raw into out, with the
 * CRC-32 of raw, or of something else if stale. Returns its length. */
static size_t unicode_path(unsigned char *out, const char *raw, const char *utf8, int stale) {
  size_t n = strlen(utf8);
  uint32_t crc = (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)raw, strlen(raw));
  out[0] = 0x75;
  out[1] = 0x70;
  out[2] = (unsigned char)(5 + n);
  out[3] = 0;
  out[4] = 1; /* version */
  put_u32(out + 5, stale ? crc ^ 1 : crc);
  memcpy(out + 9, utf8, n);
  return 9 + n;
}

static void test_unicode_path(void) {
  static const char raw[] = "Caf\x82.txt";
  unsigned char f[64];
  size_t n = unicode_path(f, raw, "Caf\xC3\xA9.txt", 0);
  char *s = mc_unzip_unicode_path(f, n, raw, strlen(raw));
  CHECK_STR(s, "Caf\xC3\xA9.txt");
  free(s);
  CHECK(mc_unzip_unicode_path(f, n, "Other.txt", 9) == NULL); /* the CRC-32 is of another name */
  CHECK(mc_unzip_unicode_path(f, n - 1, raw, strlen(raw)) == NULL); /* cut short */
  f[4] = 2;
  CHECK(mc_unzip_unicode_path(f, n, raw, strlen(raw)) == NULL); /* an unknown version */
  n = unicode_path(f, raw, "Caf\xC3", 0);
  CHECK(mc_unzip_unicode_path(f, n, raw, strlen(raw)) == NULL); /* not UTF-8 */
  CHECK(mc_unzip_unicode_path(NULL, 0, raw, strlen(raw)) == NULL);
}

static void test_names(void) {
  char *s;
  /* CP437: 0x82 is é, 0x81 ü, 0xE1 ß. */
  s = mc_unzip_name("Caf\x82 \x81\xE1", 7, 0);
  CHECK_STR(s, "Caf\xC3\xA9 \xC3\xBC\xC3\x9F");
  free(s);
  /* Flagged UTF-8 stays as it is. */
  s = mc_unzip_name("Caf\xC3\xA9", 5, 1);
  CHECK_STR(s, "Caf\xC3\xA9");
  free(s);
  /* The same bytes without the flag are CP437: two characters. */
  s = mc_unzip_name("Caf\xC3\xA9", 5, 0);
  CHECK_STR(s, "Caf\xE2\x94\x9C\xE2\x8C\x90");
  free(s);
  /* Flagged but not valid UTF-8: read as CP437 rather than kept broken. */
  s = mc_unzip_name("Caf\x82", 4, 1);
  CHECK_STR(s, "Caf\xC3\xA9");
  free(s);
  /* Overlong and surrogate forms aren't valid UTF-8. */
  s = mc_unzip_name("\xC0\xAF", 2, 1);
  CHECK(s && strcmp(s, "\xC0\xAF") != 0);
  free(s);
  s = mc_unzip_name("\xED\xA0\x80", 3, 1);
  CHECK(s && strcmp(s, "\xED\xA0\x80") != 0);
  free(s);

  int unsafe = 0;
  CHECK(!mc_unzip_safe_path("../evil.txt", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("Album/../../evil.txt", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("Album\\..\\..\\evil.txt", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("/etc/passwd", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("\\Windows\\evil.dll", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("C:\\evil.txt", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("C:evil.txt", &unsafe) && unsafe);
  CHECK(!mc_unzip_safe_path("__MACOSX/Album/._01.flac", &unsafe) && !unsafe);
  CHECK(!mc_unzip_safe_path("Album/.DS_Store", &unsafe) && !unsafe);
  s = mc_unzip_safe_path("./Album//Disc 1\\01 Song.flac", &unsafe);
  CHECK_STR(s, "Album/Disc 1/01 Song.flac");
  CHECK(!unsafe);
  free(s);
  /* ".." inside a name is fine; only a whole ".." part climbs. */
  s = mc_unzip_safe_path("Album..Live/..01.flac", &unsafe);
  CHECK_STR(s, "Album..Live/..01.flac");
  free(s);
  s = mc_unzip_safe_path("bad\x01name.flac", &unsafe);
  CHECK_STR(s, "bad_name.flac");
  free(s);

  CHECK(mc_unzip_is_zip("a/Album.zip"));
  CHECK(mc_unzip_is_zip("Album.ZIP"));
  CHECK(!mc_unzip_is_zip("Album.flac"));
  CHECK(!mc_unzip_is_zip(".zip"));
  CHECK(!mc_unzip_is_zip("zip"));
}

/* --- Extraction --- */

static void test_extract(const char *root) {
  static const unsigned char text[] = "hello";
  char zip[1200], dest[1200], path[1400];

  /* Zip slip: only the safe entry is written, and nothing outside dest. */
  zip_entry slip[] = {
      {"../evil.txt", text, 5, 0, 0},
      {"Album/../../evil2.txt", text, 5, 0, 0},
      {"/abs.txt", text, 5, 0, 0},
      {"Album/", NULL, 0, 0, 0},
      {"Album/good.txt", text, 5, 0, 1},
  };
  path_join(zip, sizeof zip, root, "slip.zip");
  path_join(dest, sizeof dest, root, "slip");
  CHECK(write_zip(zip, slip, 5));
  mc_unzip_stats stats;
  CHECK(mc_unzip(zip, dest, 0, NULL, NULL, NULL, &stats) == MC_UNZIP_OK);
  CHECK(stats.files == 1);
  CHECK(stats.unsafe == 3);
  snprintf(path, sizeof path, "%s%cAlbum%cgood.txt", dest, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(mc_file_exists(path));
  path_join(path, sizeof path, root, "evil.txt");
  CHECK(!mc_file_exists(path));
  path_join(path, sizeof path, root, "evil2.txt");
  CHECK(!mc_file_exists(path));
  path_join(path, sizeof path, dest, "evil.txt");
  CHECK(!mc_file_exists(path));
  mc_remove_tree(dest);

  /* Names: CP437 without the flag, UTF-8 with it; Mac junk left out. */
  zip_entry names[] = {
      {"Caf\x82/01.txt", text, 5, 0, 0},
      {"\xC3\x9C" "ber/02.txt", text, 5, 1, 1},
      {"__MACOSX/Caf\x82/._01.txt", text, 5, 0, 0},
      {"Caf\x82/.DS_Store", text, 5, 0, 0},
  };
  path_join(zip, sizeof zip, root, "names.zip");
  path_join(dest, sizeof dest, root, "names");
  CHECK(write_zip(zip, names, 4));
  CHECK(mc_unzip(zip, dest, 0, NULL, NULL, NULL, &stats) == MC_UNZIP_OK);
  CHECK(stats.files == 2 && stats.unsafe == 0);
  snprintf(path, sizeof path, "%s%cCaf\xC3\xA9%c01.txt", dest, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(mc_file_exists(path));
  snprintf(path, sizeof path, "%s%c\xC3\x9C" "ber%c02.txt", dest, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(mc_file_exists(path));
  path_join(path, sizeof path, dest, "__MACOSX");
  CHECK(!mc_file_exists(path));
  snprintf(path, sizeof path, "%s%cCaf\xC3\xA9", dest, MC_PATH_SEP);
  CHECK(count_entries(path) == 1);
  mc_remove_tree(dest);

  /* Unicode Path extra fields: an unflagged entry's UTF-8 name is taken from one whose CRC-32
   * matches the stored name (here Shift-JIS, which would come out as CP437 junk; the field
   * follows an extended timestamp field), and not from one left naming another (a stale
   * CRC-32). */
  unsigned char good[64], stale[64];
  size_t good_len = 9; /* the timestamp: 0x5455, 5 bytes, flags and mtime */
  memcpy(good, "\x55\x54\x05\x00\x01\x00\x00\x00\x00", good_len);
  good_len += unicode_path(good + good_len, "\x93\xFA\x96{/03.txt", "\xE6\x97\xA5\xE6\x9C\xAC/03.txt", 0);
  size_t stale_len = unicode_path(stale, "Caf\x82/04.txt", "Other/04.txt", 1);
  zip_entry upaths[] = {
      {"\x93\xFA\x96{/03.txt", text, 5, 0, 0},
      {"Caf\x82/04.txt", text, 5, 0, 0},
  };
  zip_extra extras[] = {{good, good_len}, {stale, stale_len}};
  path_join(zip, sizeof zip, root, "upath.zip");
  path_join(dest, sizeof dest, root, "upath");
  CHECK(write_zip_ex(zip, upaths, 2, extras));
  CHECK(mc_unzip(zip, dest, 0, NULL, NULL, NULL, &stats) == MC_UNZIP_OK);
  CHECK(stats.files == 2);
  snprintf(path, sizeof path, "%s%c\xE6\x97\xA5\xE6\x9C\xAC%c03.txt", dest, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(mc_file_exists(path));
  snprintf(path, sizeof path, "%s%cCaf\xC3\xA9%c04.txt", dest, MC_PATH_SEP, MC_PATH_SEP);
  CHECK(mc_file_exists(path));
  mc_remove_tree(dest);

  /* Over the size limit: nothing is written. */
  path_join(zip, sizeof zip, root, "slip.zip");
  path_join(dest, sizeof dest, root, "big");
  CHECK(mc_unzip(zip, dest, 4, NULL, NULL, NULL, &stats) == MC_UNZIP_TOO_BIG);
  CHECK(stats.files == 0);
  CHECK(!mc_file_exists(dest) || count_entries(dest) == 0);
  mc_remove_tree(dest);

  /* A session's budget: what's extracted counts towards it, and a zip that would take the
   * session past it isn't extracted, though it fits on its own (slip.zip claims
   * 20 bytes, of which 5 are written). */
  mc_unzip_budget budget = {40, 0};
  CHECK(mc_unzip(zip, dest, 0, &budget, NULL, NULL, &stats) == MC_UNZIP_OK);
  CHECK(budget.used == 5 && stats.bytes == 5);
  mc_remove_tree(dest);
  budget.used = 25;
  CHECK(mc_unzip(zip, dest, 0, &budget, NULL, NULL, &stats) == MC_UNZIP_SESSION_FULL);
  CHECK(stats.files == 0 && budget.used == 25);
  CHECK(!mc_file_exists(dest) || count_entries(dest) == 0);
  mc_remove_tree(dest);
  /* The free space must also hold what the session extracted before, to copy it. */
  budget.max = INT64_MAX;
  budget.used = INT64_MAX / 2;
  CHECK(mc_unzip(zip, dest, 0, &budget, NULL, NULL, &stats) == MC_UNZIP_NO_SPACE);
  CHECK(stats.files == 0 && budget.used == INT64_MAX / 2);
  mc_remove_tree(dest);

  /* Not a zip at all. */
  path_join(zip, sizeof zip, root, "not.zip");
  FILE *f = mc_fopen(zip, "wb");
  if (f) {
    fputs("this is not a zip file, just some text that is long enough to look for one", f);
    fclose(f);
  }
  path_join(dest, sizeof dest, root, "not");
  CHECK(mc_unzip(zip, dest, 0, NULL, NULL, NULL, &stats) == MC_UNZIP_BAD);
  mc_remove_tree(dest);
}

/* --- Importing --- */

/* Waits until the session has items and none is still being worked on. */
static int wait_settled(mc_import *imp, uint32_t timeout_ms) {
  uint64_t deadline = mc_mono_ms() + timeout_ms;
  while (mc_mono_ms() < deadline) {
    int n = mc_import_item_count(imp), settled = 0;
    for (int i = 0; i < n; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      settled += it->state == MC_IMPORT_DONE || it->state == MC_IMPORT_FAILED || it->state == MC_IMPORT_SKIPPED;
      mc_import_item_free(it);
    }
    if (n > 0 && settled == n) return n;
    mc_sleep_ms(20);
  }
  return 0;
}

static int file_equals(const char *path, const unsigned char *want, size_t len) {
  unsigned char got[64];
  FILE *f = mc_fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(got, 1, sizeof got, f);
  fclose(f);
  return n == len && memcmp(got, want, len) == 0;
}

static void test_import(const char *root) {
  char scratch[1200], zip[1200], bad[1200], lib_dir[1200], staging[1200], path[1500];
  path_join(scratch, sizeof scratch, root, "track.wav");
  path_join(lib_dir, sizeof lib_dir, root, "lib");
  path_join(staging, sizeof staging, root, "staging");
  CHECK(mc_mkdirs(lib_dir) == MC_OK && mc_mkdirs(staging) == MC_OK);

  /* Bandcamp's layout: flat, "Artist - Album - NN Title.ext", and cover.jpg. */
  unsigned char *t1 = NULL, *t2 = NULL;
  size_t n1 = make_track(scratch, "Zip Artist", "Zip Album", "First", 1, &t1);
  size_t n2 = make_track(scratch, "Zip Artist", "Zip Album", "Second", 2, &t2);
  CHECK(n1 > 0 && n2 > 0);
  zip_entry album[] = {
      {"Zip Artist - Zip Album - 01 First.wav", t1, n1, 0, 1},
      {"Zip Artist - Zip Album - 02 Second.wav", t2, n2, 0, 1},
      {"cover.jpg", k_jpeg, sizeof k_jpeg, 0, 0},
      {"__MACOSX/._cover.jpg", k_jpeg, sizeof k_jpeg, 0, 0},
      {".DS_Store", k_jpeg, sizeof k_jpeg, 0, 0},
  };
  path_join(zip, sizeof zip, root, "Zip Artist - Zip Album.zip");
  CHECK(write_zip(zip, album, 5));
  free(t1);
  free(t2);
  path_join(bad, sizeof bad, root, "Broken.zip");
  FILE *f = mc_fopen(bad, "wb");
  if (f) {
    fputs("PK\x03\x04 but not really a zip, and no central directory either", f);
    fclose(f);
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  const char *paths[] = {zip, bad};
  mc_import_options opts = {.fetch_art = 0, .skip_tagging = 1, .staging_dir = staging};
  mc_import *imp = NULL;
  CHECK(lib && mc_import_start(lib, paths, 2, &opts, NULL, NULL, &imp) == MC_OK);
  if (imp) {
    CHECK(wait_settled(imp, 20000) == 2);
    int done = 0, failed = 0;
    for (int i = 0; i < mc_import_item_count(imp); i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      if (it->state == MC_IMPORT_DONE) {
        done++;
        CHECK(it->file_count == 2);
        CHECK_STR(it->album, "Zip Album");
        /* The staging folder is named after the zip. */
        const char *base = it->folder ? strrchr(it->folder, MC_PATH_SEP) : NULL;
        CHECK(base && strcmp(base + 1, "Zip Artist - Zip Album") == 0);
      } else if (it->state == MC_IMPORT_FAILED) {
        failed++;
        CHECK(it->file_count == 0);
        CHECK_STR(it->album, "Broken.zip");
        CHECK(it->folder && strcmp(it->folder, bad) == 0);
        CHECK_STR(it->error, mc_str(MC_STR_IMPORT_ERROR_ZIP_BAD));
      }
      mc_import_item_free(it);
    }
    CHECK(done == 1 && failed == 1);
    /* Extracted until the session is freed... */
    CHECK(count_entries(staging) == 1);
    mc_import_free(imp);
  }
  /* ...and then gone. */
  CHECK(count_entries(staging) == 0);

  int64_t tracks = -1;
  mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  CHECK(tracks == 2);
  /* The zip's cover.jpg came along, as it does from a folder. */
  snprintf(path, sizeof path, "%s/Zip Artist/Zip Album/cover.jpg", lib_dir);
  CHECK(file_equals(path, k_jpeg, sizeof k_jpeg));
  mc_library_close(lib);
  /* The zip itself is untouched. */
  CHECK(mc_file_exists(zip));
}

/* Writes an album of count tracks to the zip at path, flat, and inner (inner_len bytes, may be
 * NULL) as "Nested.zip" beside them. *bytes is what it holds unzipped. */
static int write_album_zip(const char *scratch, const char *path, const char *album, int count,
                           const unsigned char *inner, size_t inner_len, int64_t *bytes) {
  zip_entry e[4];
  unsigned char *data[3] = {0};
  char names[3][64];
  int n = 0, ok = 1;
  *bytes = 0;
  for (int i = 0; i < count && i < 3; i++) {
    snprintf(names[i], sizeof names[i], "%02d Track %d.wav", i + 1, i + 1);
    size_t len = make_track(scratch, "Zip Folder Artist", album, names[i] + 3, i + 1, &data[i]);
    ok &= len > 0;
    e[n++] = (zip_entry){names[i], data[i], len, 0, 1};
    *bytes += (int64_t)len;
  }
  if (inner) {
    e[n++] = (zip_entry){"Nested.zip", inner, inner_len, 0, 0};
    *bytes += (int64_t)inner_len;
  }
  ok &= write_zip(path, e, n);
  for (int i = 0; i < 3; i++) free(data[i]);
  return ok;
}

static unsigned char *read_file(const char *path, size_t *len) {
  int64_t size = 0, mtime = 0;
  if (mc_file_info(path, &size, &mtime) != MC_OK || size <= 0) return NULL;
  unsigned char *out = malloc((size_t)size);
  FILE *f = mc_fopen(path, "rb");
  *len = f && out ? fread(out, 1, (size_t)size, f) : 0;
  if (f) fclose(f);
  return out;
}

/* Imports the folder at dir into lib and counts the items done and failed; *error gets the
 * first failed item's error (malloc'd). */
static void import_folder(mc_library *lib, const char *dir, const char *staging, int *done, int *failed,
                          char **error) {
  *done = *failed = 0;
  *error = NULL;
  const char *paths[] = {dir};
  mc_import_options opts = {.fetch_art = 0, .skip_tagging = 1, .staging_dir = staging};
  mc_import *imp = NULL;
  CHECK(mc_import_start(lib, paths, 1, &opts, NULL, NULL, &imp) == MC_OK);
  if (!imp) return;
  CHECK(wait_settled(imp, 20000) > 0);
  for (int i = 0; i < mc_import_item_count(imp); i++) {
    mc_import_item *it;
    if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
    if (it->state == MC_IMPORT_DONE) (*done)++;
    if (it->state == MC_IMPORT_FAILED) {
      if (!*failed && it->error) *error = mc_strdup(it->error);
      (*failed)++;
      CHECK(it->file_count == 0);
    }
    mc_import_item_free(it);
  }
  mc_import_free(imp);
}

/* Zips inside a dropped folder are extracted too, one level deep, within the session's
 * limit on all its zips together. */
static void test_import_folder(const char *root) {
  char scratch[1200], drop[1200], sub[1300], lib_dir[1200], staging[1200], path[1500];
  path_join(scratch, sizeof scratch, root, "folder-track.wav");
  path_join(drop, sizeof drop, root, "drop");
  path_join(sub, sizeof sub, drop, "Downloads");
  path_join(lib_dir, sizeof lib_dir, root, "folder-lib");
  path_join(staging, sizeof staging, root, "folder-staging");
  CHECK(mc_mkdirs(sub) == MC_OK && mc_mkdirs(lib_dir) == MC_OK && mc_mkdirs(staging) == MC_OK);

  /* A zip in the folder, and one in a folder of it that holds another zip. */
  int64_t bytes;
  path_join(path, sizeof path, root, "Nested.zip");
  CHECK(write_album_zip(scratch, path, "Nested Album", 1, NULL, 0, &bytes));
  size_t inner_len = 0;
  unsigned char *inner = read_file(path, &inner_len);
  CHECK(inner != NULL);
  mc_remove(path);
  path_join(path, sizeof path, drop, "Folder A.zip");
  CHECK(write_album_zip(scratch, path, "Folder A", 2, NULL, 0, &bytes));
  path_join(path, sizeof path, sub, "Folder B.zip");
  CHECK(write_album_zip(scratch, path, "Folder B", 1, inner, inner_len, &bytes));
  free(inner);
  path_join(path, sizeof path, sub, "Broken.zip");
  FILE *f = mc_fopen(path, "wb");
  if (f) {
    fputs("PK\x03\x04 but not a zip", f);
    fclose(f);
  }

  mc_library *lib = NULL;
  CHECK(mc_library_open(lib_dir, &lib) == MC_OK);
  if (!lib) return;
  int done, failed;
  char *error;
  import_folder(lib, drop, staging, &done, &failed, &error);
  CHECK(done == 2 && failed == 1);
  CHECK_STR(error, mc_str(MC_STR_IMPORT_ERROR_ZIP_BAD));
  free(error);
  /* Nested.zip came out of Folder B.zip, so it wasn't extracted. */
  int64_t tracks = -1;
  mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  CHECK(tracks == 3);
  CHECK(count_entries(staging) == 0);
  /* The zips are untouched. */
  path_join(path, sizeof path, drop, "Folder A.zip");
  CHECK(mc_file_exists(path));
  mc_remove_tree(drop);

  /* The session's limit: room for C, then D (same size) is over it, though it'd fit alone. */
  CHECK(mc_mkdirs(drop) == MC_OK);
  path_join(path, sizeof path, drop, "Folder C.zip");
  CHECK(write_album_zip(scratch, path, "Folder C", 2, NULL, 0, &bytes));
  path_join(path, sizeof path, drop, "Folder D.zip");
  CHECK(write_album_zip(scratch, path, "Folder D", 2, NULL, 0, &bytes));
  int64_t was = mc_unzip_session_max_bytes;
  mc_unzip_session_max_bytes = bytes + bytes / 2;
  import_folder(lib, drop, staging, &done, &failed, &error);
  mc_unzip_session_max_bytes = was;
  CHECK(done == 1 && failed == 1);
  char *want = mc_format_alloc(mc_str(MC_STR_IMPORT_ERROR_ZIP_SESSION_TOO_BIG), MC_D(0));
  CHECK(error && want && strcmp(error, want) == 0);
  mc_free(want);
  free(error);
  mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  CHECK(tracks == 5);
  mc_library_close(lib);
  mc_remove_tree(drop);
}

int main(int argc, char **argv) {
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_import_zip.d", argc > 1 ? argv[1] : ".");
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  test_names();
  test_unicode_path();
  test_extract(root);
  test_import(root);
  test_import_folder(root);

  mc_remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
