/* The library as one zip, and back (kithcore/library_archive.h). The writer is our own, a
 * small one: it only ever stores, and writes front to back, with each entry's size and
 * checksum before its data. Reading is miniz's, through mc_unzip() for the files. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* localtime_r */
#endif
#include "kithcore/library_archive.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sqlite3.h>
#include <yyjson.h>

#include "kithcore/library.h"
#include "library/library_internal.h"
#include "miniz.h"
#include "tagger/unzip.h"
#include "util/changelog.h"
#include "util/platform.h"

#define MANIFEST_FORMAT 1
#define CHUNK (1024 * 1024)
#define SNAPSHOT_NAME "kithsong-export.db"
#define U32_MAX 0xFFFFFFFFu

/* --- The library's files --- */

typedef struct file_entry {
  char *path;    /* on disk */
  char *name;    /* in the zip: relative, with '/' */
  int64_t size;
  int64_t mtime_ms;
} file_entry;

typedef struct file_list {
  file_entry *items;
  int count, cap;
  size_t root_len;
  int failed;
} file_list;

/* library.db and SQLite's files beside it: the copy goes in instead. */
static int is_database(const char *name) {
  return !strcmp(name, MC_LIBRARY_DB_NAME) || !strcmp(name, MC_LIBRARY_DB_NAME "-wal") ||
         !strcmp(name, MC_LIBRARY_DB_NAME "-shm") || !strcmp(name, MC_LIBRARY_DB_NAME "-journal");
}

static int collect(void *user, const mc_tree_entry *e) {
  file_list *l = user;
  if (e->is_dir) return 0;
  const char *rel = e->path + l->root_len + 1;
  if (is_database(rel)) return 0;
  if (l->count == l->cap) {
    int cap = l->cap ? l->cap * 2 : 256;
    file_entry *grown = realloc(l->items, (size_t)cap * sizeof *grown);
    if (!grown) {
      l->failed = 1;
      return -1;
    }
    l->items = grown;
    l->cap = cap;
  }
  file_entry *f = &l->items[l->count];
  f->path = mc_strdup(e->path);
  f->name = mc_strdup(rel);
  if (!f->path || !f->name || mc_file_info(e->path, &f->size, &f->mtime_ms) != MC_OK) {
    free(f->path);
    free(f->name);
    l->failed = 1;
    return -1;
  }
  for (char *c = f->name; *c; c++)
    if (*c == '\\') *c = '/';
  l->count++;
  return 0;
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const file_entry *)a)->name, ((const file_entry *)b)->name);
}

static void free_list(file_list *l) {
  for (int i = 0; i < l->count; i++) {
    free(l->items[i].path);
    free(l->items[i].name);
  }
  free(l->items);
}

/* --- Writing --- */

typedef struct central {
  char *name;
  uint32_t crc;
  int64_t size, offset;
  uint16_t time, date;
} central;

typedef struct writer {
  mc_archive_write_fn write;
  mc_archive_progress_fn progress;
  void *user;
  int64_t offset; /* bytes written */
  int64_t done, total;
  central *entries;
  int count, cap;
  mc_archive_problem problem;
  mc_status status;
} writer;

static void fail(writer *w, mc_status st, mc_archive_problem problem) {
  if (w->status != MC_OK) return;
  w->status = st;
  w->problem = problem;
}

static void out(writer *w, const void *buf, size_t len) {
  if (w->status != MC_OK || !len) return;
  int err = w->write(w->user, buf, len);
  if (err) {
    fail(w, MC_ERR_IO, err == ENOSPC ? MC_ARCHIVE_NO_SPACE : err == EFBIG ? MC_ARCHIVE_TOO_BIG : MC_ARCHIVE_WRITE);
    return;
  }
  w->offset += (int64_t)len;
}

static void advance(writer *w, int64_t n) {
  w->done += n;
  if (w->status == MC_OK && w->progress && w->progress(w->user, w->done, w->total))
    fail(w, MC_ERR_CANCELLED, MC_ARCHIVE_OK);
}

static uint8_t *put16(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  return p + 2;
}
static uint8_t *put32(uint8_t *p, uint32_t v) {
  return put16(put16(p, v & 0xFFFF), v >> 16);
}
static uint8_t *put64(uint8_t *p, uint64_t v) {
  return put32(put32(p, (uint32_t)v), (uint32_t)(v >> 32));
}

/* mtime as MS-DOS's local date and time, which zips keep; 1980 at the earliest. */
static void dos_time(int64_t mtime_ms, uint16_t *time_out, uint16_t *date_out) {
  time_t t = (time_t)(mtime_ms / 1000);
  struct tm tm;
#ifdef _WIN32
  int ok = localtime_s(&tm, &t) == 0;
#else
  int ok = localtime_r(&t, &tm) != NULL;
#endif
  if (!ok || tm.tm_year < 80) {
    *time_out = 0;
    *date_out = (1 << 5) | 1; /* 1980-01-01 */
    return;
  }
  *time_out = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  *date_out = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

/* Starts an entry: notes it for the central directory and writes its local header, with a
 * zip64 extra field only if it's 4 GB or more. */
static void begin_entry(writer *w, const char *name, int64_t size, uint32_t crc, int64_t mtime_ms) {
  if (w->status != MC_OK) return;
  if (w->count == w->cap) {
    int cap = w->cap ? w->cap * 2 : 256;
    central *grown = realloc(w->entries, (size_t)cap * sizeof *grown);
    if (!grown) {
      fail(w, MC_ERR_NOMEM, MC_ARCHIVE_OK);
      return;
    }
    w->entries = grown;
    w->cap = cap;
  }
  central *c = &w->entries[w->count];
  if (!(c->name = mc_strdup(name))) {
    fail(w, MC_ERR_NOMEM, MC_ARCHIVE_OK);
    return;
  }
  w->count++;
  c->crc = crc;
  c->size = size;
  c->offset = w->offset;
  dos_time(mtime_ms, &c->time, &c->date);

  int big = size >= (int64_t)U32_MAX;
  size_t name_len = strlen(name);
  uint8_t h[30 + 20], *p = h;
  p = put32(p, 0x04034b50);
  p = put16(p, big ? 45 : 20); /* version needed */
  p = put16(p, 1 << 11);       /* the name is UTF-8 */
  p = put16(p, 0);             /* stored */
  p = put16(p, c->time);
  p = put16(p, c->date);
  p = put32(p, crc);
  p = put32(p, big ? U32_MAX : (uint32_t)size);
  p = put32(p, big ? U32_MAX : (uint32_t)size);
  p = put16(p, (uint32_t)name_len);
  p = put16(p, big ? 20 : 0);
  out(w, h, 30);
  out(w, name, name_len);
  if (big) {
    p = put16(h, 0x0001);
    p = put16(p, 16);
    p = put64(p, (uint64_t)size);
    put64(p, (uint64_t)size);
    out(w, h, 20);
  }
}

/* What the file at path holds: its CRC-32, read through once; MC_ERR_IO if it can't be read
 * or isn't size bytes. Counts towards the progress. */
static mc_status checksum(writer *w, const char *path, int64_t size, uint8_t *buf, uint32_t *crc_out) {
  FILE *f = mc_fopen_shared(path);
  if (!f) return MC_ERR_IO;
  mz_ulong crc = MZ_CRC32_INIT;
  int64_t read = 0;
  size_t n;
  while (w->status == MC_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
    crc = mz_crc32(crc, buf, n);
    read += (int64_t)n;
    advance(w, (int64_t)n);
  }
  int bad = ferror(f);
  fclose(f);
  if (w->status != MC_OK) return w->status;
  if (bad || read != size) return MC_ERR_IO;
  *crc_out = (uint32_t)crc;
  return MC_OK;
}

/* One file, as an entry: checksummed, then copied in. One that changes between the two
 * readings fails the export, as the zip would hold a header that doesn't match. */
static void add_file(writer *w, const char *path, const char *name, int64_t size, int64_t mtime_ms, uint8_t *buf) {
  uint32_t crc = 0;
  mc_status st = checksum(w, path, size, buf, &crc);
  if (w->status != MC_OK) return;
  if (st != MC_OK) {
    fail(w, st, MC_ARCHIVE_READ);
    return;
  }
  begin_entry(w, name, size, crc, mtime_ms);
  FILE *f = mc_fopen_shared(path);
  if (!f) {
    fail(w, MC_ERR_IO, MC_ARCHIVE_READ);
    return;
  }
  mz_ulong again = MZ_CRC32_INIT;
  int64_t copied = 0;
  size_t n;
  while (w->status == MC_OK && copied < size && (n = fread(buf, 1, CHUNK, f)) > 0) {
    if ((int64_t)n > size - copied) n = (size_t)(size - copied);
    again = mz_crc32(again, buf, n);
    out(w, buf, n);
    copied += (int64_t)n;
    advance(w, (int64_t)n);
  }
  fclose(f);
  if (w->status == MC_OK && (copied != size || (uint32_t)again != crc)) fail(w, MC_ERR_IO, MC_ARCHIVE_READ);
}

static void add_bytes(writer *w, const char *name, const char *data, int64_t mtime_ms) {
  size_t len = strlen(data);
  begin_entry(w, name, (int64_t)len, (uint32_t)mz_crc32(MZ_CRC32_INIT, (const unsigned char *)data, len), mtime_ms);
  out(w, data, len);
}

/* The central directory and its end, with zip64's records where a count, size or offset
 * doesn't fit the old ones. */
static void finish(writer *w) {
  int64_t cd_start = w->offset;
  uint8_t h[46 + 28], *p;
  for (int i = 0; i < w->count && w->status == MC_OK; i++) {
    central *c = &w->entries[i];
    int big = c->size >= (int64_t)U32_MAX, far = c->offset >= (int64_t)U32_MAX;
    uint32_t extra = (big ? 16 : 0) + (far ? 8 : 0);
    size_t name_len = strlen(c->name);
    p = put32(h, 0x02014b50);
    /* Made by Unix, as Info-ZIP's and macOS's zip say: unzippers take the names of zips
     * made by MS-DOS as code page 437, even with the UTF-8 flag (Info-ZIP's unzip, macOS's
     * Archive Utility), and give their files no permissions. */
    p = put16(p, (3 << 8) | 45);
    p = put16(p, big || far ? 45 : 20);
    p = put16(p, 1 << 11);
    p = put16(p, 0);
    p = put16(p, c->time);
    p = put16(p, c->date);
    p = put32(p, c->crc);
    p = put32(p, big ? U32_MAX : (uint32_t)c->size);
    p = put32(p, big ? U32_MAX : (uint32_t)c->size);
    p = put16(p, (uint32_t)name_len);
    p = put16(p, extra ? extra + 4 : 0);
    p = put16(p, 0); /* comment */
    p = put16(p, 0); /* disk */
    p = put16(p, 0); /* internal attributes */
    p = put32(p, 0100644u << 16); /* external attributes: a file, rw-r--r-- */
    p = put32(p, far ? U32_MAX : (uint32_t)c->offset);
    out(w, h, 46);
    out(w, c->name, name_len);
    if (extra) {
      p = put16(h, 0x0001);
      p = put16(p, extra);
      if (big) p = put64(put64(p, (uint64_t)c->size), (uint64_t)c->size);
      if (far) p = put64(p, (uint64_t)c->offset);
      out(w, h, (size_t)(p - h));
    }
  }
  int64_t cd_size = w->offset - cd_start;
  int zip64 = w->count >= 0xFFFF || cd_start >= (int64_t)U32_MAX || cd_size >= (int64_t)U32_MAX;
  if (zip64) {
    int64_t record = w->offset;
    p = put32(h, 0x06064b50);
    p = put64(p, 44);
    p = put16(p, 45);
    p = put16(p, 45);
    p = put32(p, 0);
    p = put32(p, 0);
    p = put64(p, (uint64_t)w->count);
    p = put64(p, (uint64_t)w->count);
    p = put64(p, (uint64_t)cd_size);
    put64(p, (uint64_t)cd_start);
    out(w, h, 56);
    p = put32(h, 0x07064b50);
    p = put32(p, 0);
    p = put64(p, (uint64_t)record);
    put32(p, 1);
    out(w, h, 20);
  }
  p = put32(h, 0x06054b50);
  p = put16(p, 0);
  p = put16(p, 0);
  p = put16(p, zip64 ? 0xFFFF : (uint32_t)w->count);
  p = put16(p, zip64 ? 0xFFFF : (uint32_t)w->count);
  p = put32(p, zip64 ? U32_MAX : (uint32_t)cd_size);
  p = put32(p, zip64 ? U32_MAX : (uint32_t)cd_start);
  put16(p, 0);
  out(w, h, 22);
}

static char *join_path(const char *dir, const char *name) {
  size_t len = strlen(dir) + 1 + strlen(name) + 1;
  char *p = malloc(len);
  if (p) snprintf(p, len, "%s%c%s", dir, MC_PATH_SEP, name);
  return p;
}

/* library.db as it stands, into path: a copy that's consistent whatever else is writing. */
static mc_status snapshot(mc_library *lib, const char *path) {
  mc_remove(path);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "VACUUM INTO ?1", -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

mc_status mc_library_export(mc_library *lib, const char *temp_dir, mc_archive_write_fn write,
                            mc_archive_progress_fn progress, void *user, mc_archive_problem *problem) {
  if (problem) *problem = MC_ARCHIVE_OK;
  if (!lib || !temp_dir || !write) return MC_ERR_INVALID_ARG;
  writer w = {write, progress, user, 0, 0, 0, NULL, 0, 0, MC_ARCHIVE_OK, MC_OK};
  file_list files = {NULL, 0, 0, strlen(lib->dir), 0};
  uint8_t *buf = malloc(CHUNK);
  char *db_copy = join_path(temp_dir, SNAPSHOT_NAME);
  if (!buf || !db_copy) fail(&w, MC_ERR_NOMEM, MC_ARCHIVE_OK);

  if (w.status == MC_OK) {
    mc_status st = mc_tree_walk(lib->dir, collect, &files);
    if (files.failed) fail(&w, MC_ERR_NOMEM, MC_ARCHIVE_OK);
    else if (st != MC_OK) fail(&w, st, MC_ARCHIVE_READ);
  }
  int64_t bytes = 0;
  for (int i = 0; i < files.count; i++) bytes += files.items[i].size;
  w.total = bytes * 2; /* read for the checksum, then again into the zip */
  if (w.status == MC_OK) qsort(files.items, (size_t)files.count, sizeof *files.items, by_name);
  if (w.status == MC_OK && (mc_mkdirs(temp_dir) != MC_OK || snapshot(lib, db_copy) != MC_OK))
    fail(&w, MC_ERR_DB, MC_ARCHIVE_READ);

  int64_t db_size = 0, db_mtime = 0, now = (int64_t)mc_wall_ms();
  if (w.status == MC_OK && mc_file_info(db_copy, &db_size, &db_mtime) != MC_OK) fail(&w, MC_ERR_IO, MC_ARCHIVE_READ);
  w.total += db_size * 2;
  if (w.status == MC_OK) {
    char manifest[256];
    snprintf(manifest, sizeof manifest,
             "{\"format\":%d,\"library_uid\":\"%s\",\"schema\":%d,\"files\":%d,\"bytes\":%lld}\n",
             MANIFEST_FORMAT, mc_library_uid(lib), mc_library_schema_version(lib), files.count,
             (long long)bytes);
    add_bytes(&w, MC_LIBRARY_ARCHIVE_MANIFEST, manifest, now);
  }
  add_file(&w, db_copy, MC_LIBRARY_DB_NAME, db_size, now, buf);
  for (int i = 0; i < files.count && w.status == MC_OK; i++)
    add_file(&w, files.items[i].path, files.items[i].name, files.items[i].size, files.items[i].mtime_ms, buf);
  finish(&w);

  if (db_copy) mc_remove(db_copy);
  free(db_copy);
  free(buf);
  free_list(&files);
  for (int i = 0; i < w.count; i++) free(w.entries[i].name);
  free(w.entries);
  if (problem) *problem = w.problem;
  return w.status;
}

/* --- Reading --- */

static mc_status open_zip(const char *zip_path, mz_zip_archive *z, FILE **f) {
  int64_t size = 0, mtime = 0;
  if (mc_file_info(zip_path, &size, &mtime) != MC_OK || size <= 0) return MC_ERR_FORMAT;
  if (!(*f = mc_fopen(zip_path, "rb"))) return MC_ERR_IO;
  memset(z, 0, sizeof *z);
  if (!mz_zip_reader_init_cfile(z, *f, (mz_uint64)size, 0)) {
    fclose(*f);
    return MC_ERR_FORMAT;
  }
  return MC_OK;
}

static void close_zip(mz_zip_archive *z, FILE *f) {
  mz_zip_reader_end(z);
  fclose(f);
}

mc_status mc_library_archive_info(const char *zip_path, mc_archive_info *out) {
  if (!zip_path || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  mz_zip_archive z;
  FILE *f;
  mc_status st = open_zip(zip_path, &z, &f);
  if (st != MC_OK) return st;
  st = MC_ERR_FORMAT;
  int at = mz_zip_reader_locate_file(&z, MC_LIBRARY_ARCHIVE_MANIFEST, NULL, 0);
  mz_zip_archive_file_stat stat;
  if (at >= 0 && mz_zip_reader_locate_file(&z, MC_LIBRARY_DB_NAME, NULL, 0) >= 0 &&
      mz_zip_reader_file_stat(&z, (mz_uint)at, &stat) && stat.m_uncomp_size < 4096) {
    size_t len = 0;
    char *text = mz_zip_reader_extract_to_heap(&z, (mz_uint)at, &len, 0);
    yyjson_doc *doc = text ? yyjson_read(text, len, 0) : NULL;
    yyjson_val *root = yyjson_doc_get_root(doc);
    const char *uid = yyjson_get_str(yyjson_obj_get(root, "library_uid"));
    if (yyjson_get_int(yyjson_obj_get(root, "format")) == MANIFEST_FORMAT && uid &&
        strlen(uid) == MC_UID_STRLEN - 1) {
      memcpy(out->library_uid, uid, MC_UID_STRLEN);
      out->schema = (int)yyjson_get_int(yyjson_obj_get(root, "schema"));
      out->files = (int)yyjson_get_int(yyjson_obj_get(root, "files"));
      out->bytes = yyjson_get_sint(yyjson_obj_get(root, "bytes"));
      st = MC_OK;
    }
    yyjson_doc_free(doc);
    mz_free(text);
  }
  close_zip(&z, f);
  return st;
}

static size_t to_file(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  (void)ofs;
  return fwrite(buf, 1, n, opaque);
}

/* The zip's library.db, into path. */
static mc_status extract_db(const char *zip_path, const char *path) {
  mz_zip_archive z;
  FILE *f;
  mc_status st = open_zip(zip_path, &z, &f);
  if (st != MC_OK) return st;
  int at = mz_zip_reader_locate_file(&z, MC_LIBRARY_DB_NAME, NULL, 0);
  FILE *o = at >= 0 ? mc_fopen(path, "wb") : NULL;
  st = o && mz_zip_reader_extract_to_callback(&z, (mz_uint)at, to_file, o, 0) ? MC_OK : MC_ERR_FORMAT;
  if (o && fclose(o) != 0) st = MC_ERR_IO;
  close_zip(&z, f);
  return st;
}

/* The names at the top of the library folder, so that a failed restore removes only what it
 * brought. */
typedef struct names {
  char **items;
  int count;
} names;

static void note_name(void *user, const char *name, int is_dir) {
  (void)is_dir;
  names *n = user;
  char **grown = realloc(n->items, (size_t)(n->count + 1) * sizeof *grown);
  if (!grown) return;
  n->items = grown;
  if ((n->items[n->count] = mc_strdup(name))) n->count++;
}

static int has_name(const names *n, const char *name) {
  for (int i = 0; i < n->count; i++)
    if (!strcmp(n->items[i], name)) return 1;
  return 0;
}

typedef struct sweep {
  const char *dir;
  const names *keep;
} sweep;

static void remove_new(void *user, const char *name, int is_dir) {
  sweep *s = user;
  if (has_name(s->keep, name) || is_database(name)) return;
  char *path = join_path(s->dir, name);
  if (!path) return;
  if (is_dir) mc_remove_tree(path);
  else mc_remove(path);
  free(path);
}

typedef struct restore_ctx {
  mc_archive_progress_fn progress;
  void *user;
  mc_unzip_stats stats;
  int64_t total;
  int cancelled;
} restore_ctx;

static int restore_cancel(void *user) {
  restore_ctx *r = user;
  /* The manifest comes out too, a few bytes past what the files hold. */
  int64_t done = r->stats.bytes < r->total ? r->stats.bytes : r->total;
  if (r->progress && r->progress(r->user, done, r->total)) r->cancelled = 1;
  return r->cancelled;
}

/* Replaces lib's database with the one at path, page by page, in one transaction its other
 * connections see as any other write. */
static mc_status replace_db(mc_library *lib, const char *path) {
  sqlite3 *src = NULL;
  mc_status st = MC_ERR_DB;
  if (sqlite3_open_v2(path, &src, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
    sqlite3_backup *b = sqlite3_backup_init(lib->db, "main", src, "main");
    if (b) {
      int rc = sqlite3_backup_step(b, -1);
      if (sqlite3_backup_finish(b) == SQLITE_OK && rc == SQLITE_DONE) st = MC_OK;
    }
  }
  sqlite3_close(src);
  if (st != MC_OK) return st;
  if ((st = mc_library_reload_ids(lib)) != MC_OK) return st;
  mc_hlc_init(&lib->hlc, mc_wall_ms, mc_changelog_max_hlc(lib->db));
  return MC_OK;
}

mc_status mc_library_restore(mc_library *lib, const char *zip_path, const char *temp_dir,
                             mc_archive_progress_fn progress, void *user, mc_archive_problem *problem) {
  mc_archive_problem local;
  if (!problem) problem = &local;
  *problem = MC_ARCHIVE_OK;
  if (!lib || !zip_path || !temp_dir) return MC_ERR_INVALID_ARG;
  int64_t tracks = 0;
  mc_status st = mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  if (st != MC_OK) return st;
  if (tracks > 0) {
    *problem = MC_ARCHIVE_NOT_EMPTY;
    return MC_ERR_LIBRARY_MISMATCH;
  }
  mc_archive_info info;
  if ((st = mc_library_archive_info(zip_path, &info)) != MC_OK) {
    *problem = MC_ARCHIVE_BAD;
    return st;
  }
  if (info.schema > mc_library_schema_latest()) {
    *problem = MC_ARCHIVE_NEWER;
    return MC_ERR_FORMAT;
  }

  /* The database first, in a folder of its own: opened there, it's migrated to this build's
   * schema and given a node id of its own, before anything changes in the library. */
  char *work = join_path(temp_dir, "kithsong-restore");
  char *db = work ? join_path(work, MC_LIBRARY_DB_NAME) : NULL;
  if (!db) {
    free(work);
    return MC_ERR_NOMEM;
  }
  mc_remove_tree(work);
  st = mc_mkdirs(work);
  if (st == MC_OK) st = extract_db(zip_path, db);
  if (st != MC_OK) *problem = st == MC_ERR_FORMAT ? MC_ARCHIVE_BAD : MC_ARCHIVE_NO_SPACE;
  if (st == MC_OK) {
    mc_library *copy = NULL;
    st = mc_library_open(work, &copy);
    if (st == MC_OK) st = mc_library_rotate_node_uid(copy);
    mc_library_close(copy);
    if (st != MC_OK) *problem = MC_ARCHIVE_BAD;
  }

  /* Then the files, into the library folder: library.db is there already, so the zip's is
   * left out, and the manifest is removed after. */
  names before = {NULL, 0};
  mc_list_dir(lib->dir, note_name, &before);
  restore_ctx r = {progress, user, {0}, info.bytes, 0};
  if (st == MC_OK) {
    mc_unzip_result u = mc_unzip(zip_path, lib->dir, INT64_MAX / 2, NULL, restore_cancel, &r, &r.stats);
    if (u != MC_UNZIP_OK) {
      st = u == MC_UNZIP_CANCELLED ? MC_ERR_CANCELLED : MC_ERR_IO;
      *problem = u == MC_UNZIP_CANCELLED ? MC_ARCHIVE_OK
                 : u == MC_UNZIP_NO_SPACE ? MC_ARCHIVE_NO_SPACE
                 : u == MC_UNZIP_WRITE    ? MC_ARCHIVE_WRITE
                                          : MC_ARCHIVE_BAD;
    }
    char *manifest = join_path(lib->dir, MC_LIBRARY_ARCHIVE_MANIFEST);
    if (manifest && !has_name(&before, MC_LIBRARY_ARCHIVE_MANIFEST)) mc_remove(manifest);
    free(manifest);
  }
  if (st == MC_OK && (st = replace_db(lib, db)) != MC_OK) *problem = MC_ARCHIVE_WRITE;
  if (st != MC_OK) {
    sweep s = {lib->dir, &before};
    mc_list_dir(lib->dir, remove_new, &s);
  }
  for (int i = 0; i < before.count; i++) free(before.items[i]);
  free(before.items);
  mc_remove_tree(work);
  free(db);
  free(work);
  return st;
}
