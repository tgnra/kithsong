/* Zip extraction for the importer; see unzip.h. Only reading: miniz is built without its
 * writer, and the zip is read through a FILE * from mc_fopen so UTF-8 paths work on Windows. */
#include "tagger/unzip.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"
#include "util/platform.h"

/* Some room left over on the volume after extracting, for the copies into the library. */
#define SPACE_MARGIN ((int64_t)64 * 1024 * 1024)
#define ZIP_FLAG_UTF8 0x0800 /* general purpose bit 11: the name and comment are UTF-8 */

int mc_unzip_is_zip(const char *path) {
  if (!path) return 0;
  size_t n = strlen(path);
  if (n < 5) return 0;
  const char *ext = path + n - 4;
  if (ext[-1] == '/' || ext[-1] == MC_PATH_SEP) return 0; /* just ".zip" */
  return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'z' &&
         tolower((unsigned char)ext[2]) == 'i' && tolower((unsigned char)ext[3]) == 'p';
}

/* --- Names --- */

/* CP437's upper half, the code page zip names are in unless bit 11 says UTF-8. */
static const uint16_t k_cp437[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8,
    0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2,
    0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1,
    0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD,
    0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562,
    0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534,
    0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560,
    0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393,
    0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6,
    0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0,
    0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

/* Whether s[0..len) is well-formed UTF-8: no overlong forms, surrogates or values past
 * U+10FFFF, and no NULs. */
static int valid_utf8(const unsigned char *s, size_t len) {
  size_t i = 0;
  while (i < len) {
    unsigned c = s[i];
    if (c == 0) return 0;
    if (c < 0x80) {
      i++;
      continue;
    }
    int n;
    unsigned min, cp;
    if ((c & 0xE0) == 0xC0) n = 1, min = 0x80, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0) n = 2, min = 0x800, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0) n = 3, min = 0x10000, cp = c & 0x07;
    else return 0;
    for (int k = 1; k <= n; k++) {
      if (i + (size_t)k >= len || (s[i + (size_t)k] & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (s[i + (size_t)k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    i += (size_t)n + 1;
  }
  return 1;
}

static size_t put_utf8(char *out, unsigned cp) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = (char)(0xE0 | (cp >> 12));
  out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[2] = (char)(0x80 | (cp & 0x3F));
  return 3;
}

char *mc_unzip_name(const char *raw, size_t len, int utf8) {
  if (!raw) return NULL;
  const unsigned char *s = (const unsigned char *)raw;
  if (utf8 && valid_utf8(s, len)) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, raw, len);
    out[len] = '\0';
    return out;
  }
  /* Every CP437 character is at most 3 bytes of UTF-8. */
  char *out = malloc(len * 3 + 1);
  if (!out) return NULL;
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == 0) break;
    n += put_utf8(out + n, s[i] < 0x80 ? s[i] : k_cp437[s[i] - 0x80]);
  }
  out[n] = '\0';
  return out;
}

/* Makes one part of a path safe to create on this platform, in place. */
static void clean_part(char *part, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)part[i];
    if (c < 0x20 || c == 0x7F) part[i] = '_';
#ifdef _WIN32
    else if (strchr("<>:\"|?*", c)) part[i] = '_';
#endif
  }
#ifdef _WIN32
  /* Windows drops trailing dots and spaces, which could make two names one. */
  if (len && (part[len - 1] == '.' || part[len - 1] == ' ')) part[len - 1] = '_';
#endif
}

char *mc_unzip_safe_path(const char *name, int *unsafe) {
  if (unsafe) *unsafe = 0;
  if (!name || !*name) return NULL;
  size_t len = strlen(name);
  /* Absolute: "/x", "\x", "C:..." (a drive letter, on any platform). */
  if (name[0] == '/' || name[0] == '\\' || (len >= 2 && name[1] == ':' && isalpha((unsigned char)name[0]))) {
    if (unsafe) *unsafe = 1;
    return NULL;
  }
  char *out = malloc(len + 1);
  if (!out) return NULL;
  size_t n = 0;
  const char *p = name;
  const char *last = NULL; /* the last part, in name */
  size_t last_len = 0;
  while (*p) {
    const char *end = p;
    while (*end && *end != '/' && *end != '\\') end++;
    size_t plen = (size_t)(end - p);
    if (plen == 2 && p[0] == '.' && p[1] == '.') {
      free(out);
      if (unsafe) *unsafe = 1;
      return NULL;
    }
    if (plen && !(plen == 1 && p[0] == '.')) {
      if ((plen == 8 && memcmp(p, "__MACOSX", 8) == 0)) {
        free(out);
        return NULL;
      }
      if (n) out[n++] = '/';
      memcpy(out + n, p, plen);
      clean_part(out + n, plen);
      n += plen;
      last = p;
      last_len = plen;
    }
    p = *end ? end + 1 : end;
  }
  out[n] = '\0';
  if (!n || (last_len == 9 && memcmp(last, ".DS_Store", 9) == 0)) {
    free(out);
    return NULL;
  }
  return out;
}

/* --- Extraction --- */

typedef struct sink {
  FILE *f;
  int64_t written;  /* this entry */
  int64_t limit;    /* the most this entry may write: its stated size */
  int64_t *total;   /* all entries so far */
  int64_t max_total;
  int (*cancel)(void *);
  void *user;
  int failed;       /* 1 write error, 2 too big, 3 cancelled */
} sink;

static size_t write_out(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  (void)ofs;
  sink *s = opaque;
  if (s->cancel && s->cancel(s->user)) {
    s->failed = 3;
    return 0;
  }
  if (s->written + (int64_t)n > s->limit || *s->total + (int64_t)n > s->max_total) {
    s->failed = 2;
    return 0;
  }
  if (fwrite(buf, 1, n, s->f) != n) {
    s->failed = 1;
    return 0;
  }
  s->written += (int64_t)n;
  *s->total += (int64_t)n;
  return n;
}

/* The entry's safe path, or NULL to leave it out (*unsafe set if for zip slip). */
static char *entry_path(mz_zip_archive *z, mz_uint i, const mz_zip_archive_file_stat *st, int *unsafe) {
  *unsafe = 0;
  mz_uint len = mz_zip_reader_get_filename(z, i, NULL, 0);
  if (len <= 1) return NULL;
  char *raw = malloc(len);
  if (!raw) return NULL;
  mz_zip_reader_get_filename(z, i, raw, len);
  char *name = mc_unzip_name(raw, len - 1, (st->m_bit_flag & ZIP_FLAG_UTF8) != 0);
  free(raw);
  char *safe = name ? mc_unzip_safe_path(name, unsafe) : NULL;
  free(name);
  return safe;
}

static char *join(const char *dir, const char *rel) {
  size_t n = strlen(dir) + strlen(rel) + 2;
  char *out = malloc(n);
  if (!out) return NULL;
  snprintf(out, n, "%s%c%s", dir, MC_PATH_SEP, rel);
  for (char *c = out + strlen(dir) + 1; *c; c++)
    if (*c == '/') *c = MC_PATH_SEP;
  return out;
}

mc_unzip_result mc_unzip(const char *zip_path, const char *dest, int64_t max_bytes,
                         int (*cancel)(void *user), void *user, mc_unzip_stats *stats) {
  mc_unzip_stats local = {0};
  if (!stats) stats = &local;
  memset(stats, 0, sizeof *stats);
  if (!zip_path || !dest) return MC_UNZIP_BAD;
  if (max_bytes <= 0) max_bytes = MC_UNZIP_MAX_BYTES;

  int64_t size = 0, mtime = 0;
  if (mc_file_info(zip_path, &size, &mtime) != MC_OK || size <= 0) return MC_UNZIP_BAD;
  FILE *f = mc_fopen(zip_path, "rb");
  if (!f) return MC_UNZIP_BAD;
  mz_zip_archive z;
  memset(&z, 0, sizeof z);
  if (!mz_zip_reader_init_cfile(&z, f, (mz_uint64)size, 0)) {
    fclose(f);
    return MC_UNZIP_BAD;
  }

  /* First the sizes, so nothing is written for a zip that can't fit. */
  mc_unzip_result result = MC_UNZIP_OK;
  mz_uint count = mz_zip_reader_get_num_files(&z);
  int64_t total = 0;
  for (mz_uint i = 0; i < count && result == MC_UNZIP_OK; i++) {
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, i, &st)) result = MC_UNZIP_BAD;
    else if (!st.m_is_directory && st.m_is_encrypted) result = MC_UNZIP_BAD;
    else if (st.m_uncomp_size > (mz_uint64)max_bytes) result = MC_UNZIP_TOO_BIG;
    else if (!st.m_is_directory && (total += (int64_t)st.m_uncomp_size) > max_bytes) result = MC_UNZIP_TOO_BIG;
  }
  if (result == MC_UNZIP_OK && mc_mkdirs(dest) != MC_OK) result = MC_UNZIP_WRITE;
  int64_t space = 0;
  if (result == MC_UNZIP_OK && mc_free_space(dest, &space) == MC_OK && space < total + SPACE_MARGIN)
    result = MC_UNZIP_NO_SPACE;

  int64_t written = 0;
  for (mz_uint i = 0; i < count && result == MC_UNZIP_OK; i++) {
    if (cancel && cancel(user)) {
      result = MC_UNZIP_CANCELLED;
      break;
    }
    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&z, i, &st)) {
      result = MC_UNZIP_BAD;
      break;
    }
    int unsafe = 0;
    char *rel = entry_path(&z, i, &st, &unsafe);
    if (unsafe) stats->unsafe++;
    char *path = rel ? join(dest, rel) : NULL;
    free(rel);
    if (!path) continue;
    if (st.m_is_directory) {
      if (mc_mkdirs(path) != MC_OK) result = MC_UNZIP_WRITE;
      free(path);
      continue;
    }
    if (mc_file_exists(path)) { /* the same name twice */
      free(path);
      continue;
    }
    char *sep = strrchr(path, MC_PATH_SEP);
    if (sep) {
      *sep = '\0';
      mc_status made = mc_mkdirs(path);
      *sep = MC_PATH_SEP;
      if (made != MC_OK) {
        free(path);
        result = MC_UNZIP_WRITE;
        break;
      }
    }
    sink s = {mc_fopen(path, "wb"), 0, (int64_t)st.m_uncomp_size, &written, max_bytes, cancel, user, 0};
    if (!s.f) {
      free(path);
      result = MC_UNZIP_WRITE;
      break;
    }
    mz_bool ok = mz_zip_reader_extract_to_callback(&z, i, write_out, &s, 0);
    if (fclose(s.f) != 0 && ok) ok = 0, s.failed = 1;
    if (!ok) {
      mc_remove(path);
      result = s.failed == 3 ? MC_UNZIP_CANCELLED : s.failed == 2 ? MC_UNZIP_TOO_BIG
               : s.failed == 1 ? MC_UNZIP_WRITE : MC_UNZIP_BAD;
    } else {
      stats->files++;
    }
    free(path);
  }
  stats->bytes = written;
  mz_zip_reader_end(&z);
  fclose(f);
  return result;
}
