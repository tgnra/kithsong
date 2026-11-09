/* The Cast music server: see cast_http.h. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* fseeko */
#endif

#include "cast/cast_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/socket.h"
#include "playback/decoder.h"
#include "util/platform.h"

enum {
  WORKERS = 6,           /* connections answered at once; the device uses two or three */
  READ_TIMEOUT_MS = 30000,
  SEND_TIMEOUT_MS = 30000,
  BUFFER_BYTES = 64 * 1024,
  TOKEN_BYTES = 16,
  BASE_LEN = 128,
  NAME_LEN = 64
};

/* --- Parsing --- */

static int lower(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int starts_with_nocase(const char *s, const char *prefix) {
  for (; *prefix; s++, prefix++)
    if (lower((unsigned char)*s) != lower((unsigned char)*prefix)) return 0;
  return 1;
}

/* Copies [s, e) into out (cap bytes) with the spaces either side trimmed; 0 if it doesn't fit. */
static int copy_trimmed(char *out, size_t cap, const char *s, const char *e) {
  while (s < e && (*s == ' ' || *s == '\t')) s++;
  while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
  if ((size_t)(e - s) >= cap) return 0;
  memcpy(out, s, (size_t)(e - s));
  out[e - s] = '\0';
  return 1;
}

int cast_http_parse_request(const char *buf, size_t len, cast_http_request *out) {
  memset(out, 0, sizeof *out);
  /* The end of the head: a blank line, with or without CRs. */
  size_t end = 0;
  for (size_t i = 0; i < len && !end; i++) {
    if (buf[i] != '\n') continue;
    if (i >= 1 && buf[i - 1] == '\n') end = i + 1;
    else if (i >= 2 && buf[i - 1] == '\r' && buf[i - 2] == '\n') end = i + 1;
  }
  if (!end) return len > CAST_HTTP_MAX_HEAD ? -1 : 0;
  if (end > CAST_HTTP_MAX_HEAD) return -1;

  const char *p = buf, *stop = buf + end;
  const char *eol = memchr(p, '\n', (size_t)(stop - p));
  const char *line_end = eol > p && eol[-1] == '\r' ? eol - 1 : eol;
  const char *sp1 = memchr(p, ' ', (size_t)(line_end - p));
  if (!sp1) return -1;
  const char *sp2 = memchr(sp1 + 1, ' ', (size_t)(line_end - sp1 - 1));
  if (!sp2) sp2 = line_end; /* HTTP/0.9 style: no version */
  if (sp1 == p || (size_t)(sp1 - p) >= sizeof out->method || sp2 == sp1 + 1 ||
      (size_t)(sp2 - sp1 - 1) >= sizeof out->path)
    return -1;
  for (const char *c = p; c < sp1; c++) out->method[c - p] = (char)toupper((unsigned char)*c);
  memcpy(out->path, sp1 + 1, (size_t)(sp2 - sp1 - 1));

  for (p = eol + 1; p < stop; p = eol + 1) {
    eol = memchr(p, '\n', (size_t)(stop - p));
    line_end = eol > p && eol[-1] == '\r' ? eol - 1 : eol;
    const char *colon = memchr(p, ':', (size_t)(line_end - p));
    if (!colon || colon == p) continue;
    if ((size_t)(colon - p) == 5 && starts_with_nocase(p, "range"))
      out->has_range = copy_trimmed(out->range, sizeof out->range, colon + 1, line_end);
  }
  return 1;
}

/* A whole number of digits only, at most 18 of them, so it can't overflow. */
static int parse_count(const char *s, const char *e, int64_t *out) {
  while (s < e && *s == ' ') s++;
  while (e > s && e[-1] == ' ') e--;
  if (s == e || e - s > 18) return 0;
  int64_t v = 0;
  for (; s < e; s++) {
    if (*s < '0' || *s > '9') return 0;
    v = v * 10 + (*s - '0');
  }
  *out = v;
  return 1;
}

int cast_http_range(const char *header, int64_t length, int64_t *from, int64_t *to) {
  if (!header || !starts_with_nocase(header, "bytes=")) return 0;
  const char *spec = header + 6, *end = strchr(spec, ',');
  if (!end) end = spec + strlen(spec);
  const char *dash = memchr(spec, '-', (size_t)(end - spec));
  if (!dash) return 0;
  const char *s = spec;
  while (s < dash && *s == ' ') s++;
  int64_t first, last;
  if (s == dash) { /* the last n bytes */
    if (length <= 0 || !parse_count(dash + 1, end, &last) || last <= 0) return 0;
    *from = length - last > 0 ? length - last : 0;
    *to = length - 1;
    return 1;
  }
  if (!parse_count(spec, dash, &first)) return 0;
  if (first >= length) return -1;
  const char *t = dash + 1;
  while (t < end && *t == ' ') t++;
  if (t == end) {
    last = length - 1;
  } else {
    if (!parse_count(dash + 1, end, &last)) return 0;
    if (last > length - 1) last = length - 1;
  }
  if (last < first) return 0;
  *from = first;
  *to = last;
  return 1;
}

const char *cast_http_content_type(const char *path, int *as_wav) {
  *as_wav = 0;
  const char *dot = path ? strrchr(path, '.') : NULL;
  if (!dot || strchr(dot, '/') || strchr(dot, '\\')) return NULL;
  char ext[8];
  size_t n = strlen(dot + 1);
  if (n >= sizeof ext) return NULL;
  for (size_t i = 0; i <= n; i++) ext[i] = (char)lower((unsigned char)dot[1 + i]);
  if (!strcmp(ext, "flac")) return "audio/flac";
  if (!strcmp(ext, "mp3")) return "audio/mpeg";
  if (!strcmp(ext, "opus") || !strcmp(ext, "ogg")) return "audio/ogg";
  if (!strcmp(ext, "wav")) return "audio/wav";
  if (!strcmp(ext, "aif") || !strcmp(ext, "aiff")) {
    *as_wav = 1;
    return "audio/wav";
  }
  return NULL;
}

static void put_le(unsigned char *p, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; i++) p[i] = (unsigned char)(v >> (8 * i));
}

void cast_http_wav_header(unsigned char out[CAST_WAV_HEADER], unsigned channels, unsigned rate,
                          unsigned bits, uint64_t frames) {
  unsigned block = channels * (bits / 8);
  uint64_t data = frames * block;
  if (data > UINT32_MAX - 36) data = UINT32_MAX - 36;
  memcpy(out, "RIFF", 4);
  put_le(out + 4, (uint32_t)(36 + data), 4);
  memcpy(out + 8, "WAVEfmt ", 8);
  put_le(out + 16, 16, 4);
  put_le(out + 20, 1, 2); /* PCM */
  put_le(out + 22, channels, 2);
  put_le(out + 24, rate, 4);
  put_le(out + 28, rate * block, 4);
  put_le(out + 32, block, 2);
  put_le(out + 34, bits, 2);
  memcpy(out + 36, "data", 4);
  put_le(out + 40, (uint32_t)data, 4);
}

/* --- The server --- */

typedef struct served {
  char name[NAME_LEN];
  char *path;          /* a track, or NULL for bytes */
  unsigned char *data; /* an image */
  size_t len;
  char content_type[32];
  int as_wav;
} served;

typedef struct worker {
  cast_server *server;
  mc_thread thread;
  mc_socket *sock; /* while serving; under the server's lock */
  int running;     /* the thread hasn't been joined */
  int done;        /* ...and has finished */
} worker;

struct cast_server {
  mc_listener *listener;
  mc_thread accept_thread;
  char token[TOKEN_BYTES * 2 + 1];
  char base[BASE_LEN];
  mc_mutex lock;
  /* Under lock. */
  served *items;
  int nitems;
  worker workers[WORKERS];
  int stopping;
};

static void served_free(served *s) {
  free(s->path);
  free(s->data);
  memset(s, 0, sizeof *s);
}

/* A copy of what name serves (under lock), or 0 if it isn't up. */
static int look_up(cast_server *server, const char *name, served *out) {
  int found = 0;
  mc_mutex_lock(&server->lock);
  for (int i = 0; i < server->nitems && !found; i++) {
    const served *s = &server->items[i];
    if (strcmp(s->name, name) != 0) continue;
    *out = *s;
    out->path = s->path ? mc_strdup(s->path) : NULL;
    out->data = NULL;
    if (s->data && (out->data = malloc(s->len ? s->len : 1))) memcpy(out->data, s->data, s->len);
    found = (s->path && out->path) || (s->data && out->data);
    if (!found) served_free(out);
  }
  mc_mutex_unlock(&server->lock);
  return found;
}

/* Whether a and b are the same, in a time that doesn't depend on where they differ. */
static int same_token(const char *a, const char *b, size_t len) {
  unsigned char diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

/* The name in "/<token>/<name>", or NULL if it isn't that with this server's token. */
static const char *name_in(const cast_server *server, char *path) {
  char *q = strchr(path, '?');
  if (q) *q = '\0';
  size_t tlen = strlen(server->token);
  if (path[0] != '/' || strlen(path) < tlen + 3 || path[tlen + 1] != '/') return NULL;
  if (!same_token(path + 1, server->token, tlen)) return NULL;
  const char *name = path + tlen + 2;
  return strchr(name, '/') ? NULL : name;
}

static mc_status send_str(mc_socket *s, const char *str) {
  return mc_socket_send_all(s, str, strlen(str), SEND_TIMEOUT_MS);
}

/* The status line and headers, with what every answer has: CORS, for the receiver is a web
 * page (a media element needs none of it, but fetching does), and the connection closing. */
static mc_status send_head(mc_socket *s, int code, const char *reason, const char *headers) {
  char head[1024];
  snprintf(head, sizeof head,
           "HTTP/1.1 %d %s\r\n%s"
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Range\r\n"
           "Access-Control-Expose-Headers: Content-Length, Content-Range, Accept-Ranges\r\n"
           "Connection: close\r\n\r\n",
           code, reason, headers ? headers : "");
  return send_str(s, head);
}

static void send_empty(mc_socket *s, int code, const char *reason) {
  send_head(s, code, reason, "Content-Length: 0\r\n");
}

static int seek64(FILE *f, int64_t off) {
#ifdef _WIN32
  return _fseeki64(f, off, SEEK_SET);
#else
  return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* The head for length bytes of type, of which from..to are sent: 206 with ranged, else 200. */
static mc_status send_content_head(mc_socket *s, const char *type, int64_t length, int ranged, int64_t from,
                                   int64_t to) {
  char headers[512];
  int n = snprintf(headers, sizeof headers, "Content-Type: %s\r\nContent-Length: %lld\r\nAccept-Ranges: bytes\r\n",
                   type, (long long)(to - from + 1));
  if (ranged)
    snprintf(headers + n, sizeof headers - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)from,
             (long long)to, (long long)length);
  return ranged ? send_head(s, 206, "Partial Content", headers) : send_head(s, 200, "OK", headers);
}

/* Works out the range to send of length bytes: 1 to send from..to (ranged says whether it's a
 * range), 0 if a 416 was sent instead. */
static int pick_range(mc_socket *s, const cast_http_request *req, int64_t length, int64_t *from, int64_t *to,
                      int *ranged) {
  *ranged = 0;
  *from = 0;
  *to = length - 1;
  if (!req->has_range) return 1;
  int r = cast_http_range(req->range, length, from, to);
  if (r < 0) {
    char headers[96];
    snprintf(headers, sizeof headers, "Content-Range: bytes */%lld\r\nContent-Length: 0\r\n", (long long)length);
    send_head(s, 416, "Range Not Satisfiable", headers);
    return 0;
  }
  if (r == 0) {
    *from = 0;
    *to = length - 1;
  }
  *ranged = r == 1;
  return 1;
}

static void serve_file(mc_socket *s, const cast_http_request *req, const served *what) {
  int64_t length = 0, mtime;
  FILE *f = mc_file_info(what->path, &length, &mtime) == MC_OK ? mc_fopen_shared(what->path) : NULL;
  if (!f) {
    send_empty(s, 404, "Not Found");
    return;
  }
  int64_t from, to;
  int ranged;
  if (!pick_range(s, req, length, &from, &to, &ranged)) {
    fclose(f);
    return;
  }
  if (send_content_head(s, what->content_type, length, ranged, from, to) != MC_OK || !strcmp(req->method, "HEAD") ||
      length == 0 || seek64(f, from) != 0) {
    fclose(f);
    return;
  }
  char *buf = malloc(BUFFER_BYTES);
  int64_t left = to - from + 1;
  while (buf && left > 0) {
    size_t want = left < BUFFER_BYTES ? (size_t)left : BUFFER_BYTES;
    size_t got = fread(buf, 1, want, f);
    if (!got || mc_socket_send_all(s, buf, got, SEND_TIMEOUT_MS) != MC_OK) break;
    left -= (int64_t)got;
  }
  free(buf);
  fclose(f);
}

/* Writes a float sample as 24-bit little-endian PCM. */
static void put_s24(unsigned char *p, float v) {
  double x = (double)v * 8388608.0;
  long i = x >= 0 ? (long)(x + 0.5) : -(long)(-x + 0.5);
  if (i > 8388607) i = 8388607;
  if (i < -8388608) i = -8388608;
  uint32_t u = (uint32_t)i;
  p[0] = (unsigned char)u;
  p[1] = (unsigned char)(u >> 8);
  p[2] = (unsigned char)(u >> 16);
}

/* A file the player decodes, as 24-bit WAV: the first two channels of it, if it has more. */
static void serve_wav(mc_socket *s, const cast_http_request *req, const served *what) {
  mc_decoder *dec = NULL;
  if (mc_decoder_open(what->path, &dec) != MC_OK) {
    send_empty(s, 404, "Not Found");
    return;
  }
  unsigned in_ch = mc_decoder_channels(dec), ch = in_ch > 2 ? 2 : in_ch, rate = mc_decoder_rate(dec);
  uint64_t frames = mc_decoder_length(dec);
  unsigned block = ch * 3;
  unsigned char header[CAST_WAV_HEADER];
  cast_http_wav_header(header, ch, rate, 24, frames);
  int64_t length = CAST_WAV_HEADER + (int64_t)(frames * block);
  int64_t from, to;
  int ranged;
  if (!frames || !pick_range(s, req, length, &from, &to, &ranged) ||
      send_content_head(s, what->content_type, length, ranged, from, to) != MC_OK || !strcmp(req->method, "HEAD")) {
    if (!frames) send_empty(s, 404, "Not Found"); /* no length: the header couldn't say */
    mc_decoder_close(dec);
    return;
  }
  int64_t pos = from, end = to + 1;
  if (pos < CAST_WAV_HEADER) {
    int64_t n = (end < CAST_WAV_HEADER ? end : CAST_WAV_HEADER) - pos;
    if (mc_socket_send_all(s, header + pos, (size_t)n, SEND_TIMEOUT_MS) != MC_OK) {
      mc_decoder_close(dec);
      return;
    }
    pos += n;
  }
  enum { CHUNK = 4096 };
  float *in = malloc((size_t)CHUNK * in_ch * sizeof *in);
  unsigned char *out = malloc((size_t)CHUNK * block);
  if (pos < end && in && out) {
    uint64_t frame = (uint64_t)(pos - CAST_WAV_HEADER) / block;
    size_t skip = (size_t)((uint64_t)(pos - CAST_WAV_HEADER) % block);
    mc_decoder_seek(dec, frame);
    while (pos < end) {
      uint64_t got = mc_decoder_read(dec, in, CHUNK);
      if (!got) break;
      for (uint64_t f = 0; f < got; f++)
        for (unsigned c = 0; c < ch; c++) put_s24(out + f * block + c * 3, in[f * in_ch + c]);
      size_t bytes = (size_t)got * block - skip;
      if ((int64_t)bytes > end - pos) bytes = (size_t)(end - pos);
      if (mc_socket_send_all(s, out + skip, bytes, SEND_TIMEOUT_MS) != MC_OK) break;
      pos += (int64_t)bytes;
      skip = 0;
    }
  }
  free(in);
  free(out);
  mc_decoder_close(dec);
}

/* Images are small, so sent whole, ranges or not. */
static void serve_bytes(mc_socket *s, const cast_http_request *req, const served *what) {
  char headers[128];
  snprintf(headers, sizeof headers, "Content-Type: %s\r\nContent-Length: %zu\r\n", what->content_type, what->len);
  if (send_head(s, 200, "OK", headers) == MC_OK && strcmp(req->method, "HEAD") != 0)
    mc_socket_send_all(s, what->data, what->len, SEND_TIMEOUT_MS);
}

static void respond(cast_server *server, mc_socket *s, cast_http_request *req) {
  if (!strcmp(req->method, "OPTIONS")) {
    send_head(s, 204, "No Content", NULL);
    return;
  }
  if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
    send_head(s, 405, "Method Not Allowed", "Allow: GET, HEAD, OPTIONS\r\nContent-Length: 0\r\n");
    return;
  }
  const char *name = name_in(server, req->path);
  served what;
  if (!name || !look_up(server, name, &what)) {
    send_empty(s, 404, "Not Found");
    return;
  }
  if (what.data) serve_bytes(s, req, &what);
  else if (what.as_wav) serve_wav(s, req, &what);
  else serve_file(s, req, &what);
  served_free(&what);
}

static void serve(void *arg) {
  worker *w = arg;
  cast_server *server = w->server;
  mc_socket *s = w->sock;
  char *head = malloc(CAST_HTTP_MAX_HEAD + 1);
  size_t len = 0;
  cast_http_request req;
  int parsed = 0;
  while (head && parsed == 0 && len < CAST_HTTP_MAX_HEAD + 1) {
    long n = mc_socket_recv_some(s, head + len, CAST_HTTP_MAX_HEAD + 1 - len, READ_TIMEOUT_MS);
    if (n <= 0) break; /* the device gave up on it, as it does when it seeks */
    len += (size_t)n;
    parsed = cast_http_parse_request(head, len, &req);
  }
  free(head);
  if (parsed == 1) respond(server, s, &req);
  else if (parsed < 0) send_empty(s, 400, "Bad Request");
  mc_socket_shutdown_send(s);
  mc_socket_drain(s, 1000);
  mc_mutex_lock(&server->lock);
  w->sock = NULL;
  w->done = 1;
  mc_mutex_unlock(&server->lock);
  mc_socket_close(s);
}

static void accept_loop(void *arg) {
  cast_server *server = arg;
  for (;;) {
    mc_socket *s = NULL;
    mc_status st = mc_listener_accept(server->listener, MC_SOCKET_FOREVER, &s);
    if (st != MC_OK && !mc_listener_stopped(server->listener)) {
      mc_sleep_ms(100); /* out of descriptors, say: try again in a moment */
      continue;
    }
    if (!s) break; /* stopped */
    mc_mutex_lock(&server->lock);
    worker *free_slot = NULL;
    for (int i = 0; i < WORKERS; i++) {
      worker *w = &server->workers[i];
      if (w->running && w->done) { /* finished: join it, which returns at once */
        mc_mutex_unlock(&server->lock);
        mc_thread_join(&w->thread);
        mc_mutex_lock(&server->lock);
        w->running = w->done = 0;
      }
      if (!w->running && !free_slot) free_slot = w;
    }
    if (free_slot && !server->stopping) {
      free_slot->server = server;
      free_slot->sock = s;
      free_slot->done = 0;
      free_slot->running = mc_thread_start(&free_slot->thread, serve, free_slot) == MC_OK;
      if (!free_slot->running) free_slot->sock = NULL;
    }
    int taken = free_slot && free_slot->running;
    mc_mutex_unlock(&server->lock);
    if (!taken) { /* all busy: the device asks again */
      send_head(s, 503, "Service Unavailable", "Content-Length: 0\r\nRetry-After: 1\r\n");
      mc_socket_close(s);
    }
  }
}

mc_status cast_server_start(const char *address, cast_server **out) {
  if (!address || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  cast_server *server = calloc(1, sizeof *server);
  if (!server) return MC_ERR_NOMEM;
  unsigned char raw[TOKEN_BYTES];
  mc_status st = mc_random_bytes(raw, sizeof raw);
  if (st != MC_OK) {
    free(server);
    return st;
  }
  for (int i = 0; i < TOKEN_BYTES; i++) snprintf(server->token + 2 * i, 3, "%02x", raw[i]);
  st = mc_listener_open_on(address, 0, &server->listener);
  if (st != MC_OK) {
    free(server);
    return st;
  }
  int v6 = strchr(address, ':') != NULL;
  snprintf(server->base, sizeof server->base, "http://%s%s%s:%u/%s/", v6 ? "[" : "", address, v6 ? "]" : "",
           (unsigned)mc_listener_port(server->listener), server->token);
  mc_mutex_init(&server->lock);
  st = mc_thread_start(&server->accept_thread, accept_loop, server);
  if (st != MC_OK) {
    mc_listener_close(server->listener);
    mc_mutex_destroy(&server->lock);
    free(server);
    return st;
  }
  *out = server;
  return MC_OK;
}

const char *cast_server_base(const cast_server *server) {
  return server->base;
}

/* Puts item up as its name, replacing what had it; takes item's memory either way. */
static mc_status put(cast_server *server, served *item) {
  mc_mutex_lock(&server->lock);
  for (int i = 0; i < server->nitems; i++) {
    if (strcmp(server->items[i].name, item->name) != 0) continue;
    served_free(&server->items[i]);
    server->items[i] = *item;
    mc_mutex_unlock(&server->lock);
    return MC_OK;
  }
  served *grown = realloc(server->items, (size_t)(server->nitems + 1) * sizeof *grown);
  if (grown) {
    server->items = grown;
    server->items[server->nitems++] = *item;
  }
  mc_mutex_unlock(&server->lock);
  if (!grown) served_free(item);
  return grown ? MC_OK : MC_ERR_NOMEM;
}

static int valid_name(const char *name) {
  if (!name || !*name || strlen(name) >= NAME_LEN) return 0;
  for (const char *c = name; *c; c++)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.')) return 0;
  return 1;
}

mc_status cast_server_add_file(cast_server *server, const char *name, const char *path, const char *content_type,
                               int as_wav) {
  if (!server || !valid_name(name) || !path || !content_type || strlen(content_type) >= 32) return MC_ERR_INVALID_ARG;
  served item;
  memset(&item, 0, sizeof item);
  strcpy(item.name, name);
  strcpy(item.content_type, content_type);
  item.as_wav = as_wav;
  if (!(item.path = mc_strdup(path))) return MC_ERR_NOMEM;
  return put(server, &item);
}

mc_status cast_server_add_bytes(cast_server *server, const char *name, const unsigned char *data, size_t len,
                                const char *content_type) {
  if (!server || !valid_name(name) || !data || !content_type || strlen(content_type) >= 32) return MC_ERR_INVALID_ARG;
  served item;
  memset(&item, 0, sizeof item);
  strcpy(item.name, name);
  strcpy(item.content_type, content_type);
  if (!(item.data = malloc(len ? len : 1))) return MC_ERR_NOMEM;
  memcpy(item.data, data, len);
  item.len = len;
  return put(server, &item);
}

void cast_server_keep(cast_server *server, const char *const *names, int n) {
  if (!server) return;
  mc_mutex_lock(&server->lock);
  int kept = 0;
  for (int i = 0; i < server->nitems; i++) {
    int keep = 0;
    for (int k = 0; k < n && !keep; k++) keep = names[k] && !strcmp(names[k], server->items[i].name);
    if (keep) server->items[kept++] = server->items[i];
    else served_free(&server->items[i]);
  }
  server->nitems = kept;
  mc_mutex_unlock(&server->lock);
}

void cast_server_stop(cast_server *server) {
  if (!server) return;
  mc_listener_stop(server->listener);
  mc_thread_join(&server->accept_thread);
  mc_mutex_lock(&server->lock);
  server->stopping = 1;
  for (int i = 0; i < WORKERS; i++)
    if (server->workers[i].sock) mc_socket_shutdown(server->workers[i].sock);
  mc_mutex_unlock(&server->lock);
  for (int i = 0; i < WORKERS; i++)
    if (server->workers[i].running) mc_thread_join(&server->workers[i].thread);
  mc_listener_close(server->listener);
  for (int i = 0; i < server->nitems; i++) served_free(&server->items[i]);
  free(server->items);
  mc_mutex_destroy(&server->lock);
  free(server);
}
