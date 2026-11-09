/* Google Cast sender: see kithcore/cast.h and docs/cast.md.
 *
 * Threads. Discovery runs on cast_browse.c's thread. Each cast (a session) has a thread of
 * its own, which holds the TLS connection to the device and does everything on it: it
 * connects, starts the Default Media Receiver (or joins it, if it's running), hands the
 * player over to remote mode (playback/remote.h), and then turns what the player asks
 * (ops, queued by the player's thread) into Cast media messages, and what the device says
 * into the player's remote reports. The music server (cast_http.c) has threads of its own.
 *
 * Locks, in the order they're taken: a session's use (held for every call into the player,
 * so a player being freed can wait for them to finish: op_detached()), the player's, then
 * the handle's lock (everything shared: the device list, the status, a session's ops and
 * requests). Nothing is called holding the handle's lock but quick bookkeeping.
 *
 * What the device has is at most two items: the player's current one (dev_current) and the
 * one after it (dev_queued), which the receiver fetches PRELOAD_S before the first ends and
 * goes on to by itself. Each URL names the player's id for the item ("t<id>.flac"), which is
 * how statuses are tied to items. While requests are unanswered, or the player has asked for
 * something not yet sent, the device's statuses can be from before them, so only its going on
 * to the queued item counts then, as in Android's CastPlayer. */
#include "kithcore/cast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cast/cast_browse.h"
#include "cast/cast_http.h"
#include "cast/cast_test.h"
#include "cast/cast_tls.h"
#include "cast/castv2.h"
#include "kithcore/library.h"
#include "kithcore/tagger.h"
#include "playback/remote.h"
#include "tagger/tag_io.h"
#include "util/platform.h"

enum {
  CONNECT_TIMEOUT_MS = 5000,
  REPLY_TIMEOUT_MS = 10000,
  LAUNCH_TIMEOUT_MS = 20000,  /* starting the receiver takes a few seconds on a TV */
  IO_TIMEOUT_MS = 10000,
  TICK_MS = 50,               /* the session thread looks at what's been asked this often */
  PING_MS = 5000,
  SILENT_MS = 15000,          /* nothing at all heard for this long: the connection is lost */
  RECONNECT_TRIES = 3,
  REQUEST_TIMEOUT_MS = 10000, /* a request unanswered this long is taken as answered */
  PRELOAD_S = 20,
  MAX_REQUESTS = 16,
  MAX_MAP = 8,
  MAX_SERVED = 4,
  NAME_LEN = 32
};

typedef enum op_kind { OP_LOAD, OP_QUEUE, OP_PLAY, OP_PAUSE, OP_SEEK, OP_STOP } op_kind;

/* What the player asked of the remote, queued for the session thread. */
typedef struct op {
  op_kind kind;
  unsigned id;   /* LOAD, QUEUE: 0 for nothing queued */
  char *path;    /* LOAD, QUEUE */
  int64_t duration_ms;
  int64_t ms;    /* LOAD, SEEK */
  int play;      /* LOAD */
} op;

/* The names an item is served under. */
typedef struct served_names {
  unsigned id;
  char track[NAME_LEN];
  char art[NAME_LEN]; /* "" with no cover */
} served_names;

typedef struct session session;

struct mc_cast {
  mc_cast_cb cb;
  void *user;
  mc_mutex lock;
  /* Under lock. */
  mc_cast_device *devices;
  int ndevices;
  cast_browser *browser;
  session *session; /* the cast, or NULL */
  mc_cast_status status;
};

struct session {
  mc_cast *cast;
  mc_thread thread;
  mc_cast_device device;
  mc_mutex use;
  mc_player *player; /* under use; NULL once it's been freed */

  /* Under cast->lock. */
  int stop;          /* end the cast */
  int stop_playback; /* ...and stop playback after handing back */
  mc_cast_end end;   /* why, if it wasn't asked for */
  op *ops;
  int nops;
  int set_level, level, set_muted, muted;
  int finished; /* the thread is done with everything but being joined */

  /* The session thread only. */
  cast_tls *tls;
  cast_server *server;
  char local_addr[MC_CAST_ADDR_LEN];
  char old_base[128]; /* a server replaced on reconnecting: its URLs are ours, but stale */
  unsigned char *rbuf, *sbuf;
  size_t rlen;
  long next_request;
  char transport[64], app_session[64];
  long long msid; /* the media session, 0 for none */
  uint64_t last_rx, last_ping;
  long requests[MAX_REQUESTS]; /* media requests not yet answered */
  uint64_t request_ms[MAX_REQUESTS];
  int nrequests;
  long load_request;           /* the QUEUE_LOAD among them, or 0 */
  long ids_request;            /* a QUEUE_GET_ITEM_IDS among them, or 0 */
  unsigned dev_current, dev_queued;
  int dev_done;                /* the end of dev_current has been reported */
  int current_item;            /* the device's queue item id for dev_current, 0 if unknown */
  int items_known, nitems;
  int items[CAST_MAX_ITEMS];
  int loaded_n;                /* items in the last QUEUE_LOAD, to tell them apart by order */
  int inserted;                /* the queued item was added at the end since the queue was last listed */
  struct { unsigned id; int item; } map[MAX_MAP];
  int nmap;
  int pending_next;            /* a change of the queued item waits: */
  unsigned next_id;
  char *next_path;
  int64_t next_duration_ms;
  served_names served[MAX_SERVED];
  int displaced;
  int ours_seen;               /* the device has said it has our media: others' is someone else's cast */
  uint64_t idle_since;         /* not playing since (mono ms), for MC_CAST_PAUSED_MS; 0 while playing */
};

uint32_t mc_cast_test_paused_ms = MC_CAST_PAUSED_MS;

/* --- Small things --- */

static void notify(mc_cast *c, mc_cast_event event) {
  if (c->cb) c->cb(c->user, event);
}

static void copy_str(char *dst, size_t cap, const char *src) {
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n) memcpy(dst, src, n);
  dst[n] = '\0';
}

static const char *display_name(const mc_cast_device *d) {
  return d->name[0] ? d->name : d->model;
}

/* Ends the session from its own thread, for why (if nothing else said why first). */
static void end_session(session *s, mc_cast_end why, int stop_playback) {
  mc_mutex_lock(&s->cast->lock);
  s->stop = 1;
  if (s->end == MC_CAST_END_NONE) s->end = why;
  s->stop_playback |= stop_playback;
  mc_mutex_unlock(&s->cast->lock);
}

static int stopping(session *s) {
  mc_mutex_lock(&s->cast->lock);
  int stop = s->stop;
  mc_mutex_unlock(&s->cast->lock);
  return stop;
}

/* Sets the status's state (if this is still the cast) and says so. */
static void set_state(session *s, mc_cast_state state) {
  mc_cast *c = s->cast;
  mc_mutex_lock(&c->lock);
  int ours = c->session == s && c->status.state != state;
  if (ours) c->status.state = state;
  mc_mutex_unlock(&c->lock);
  if (ours) notify(c, MC_CAST_EVENT_STATE);
}

static void free_ops(op *ops, int n) {
  for (int i = 0; i < n; i++) free(ops[i].path);
  free(ops);
}

/* --- The player's ops: on the player's thread, under its lock --- */

static void push_op(session *s, op o) {
  mc_mutex_lock(&s->cast->lock);
  op *grown = realloc(s->ops, (size_t)(s->nops + 1) * sizeof *grown);
  if (grown) {
    s->ops = grown;
    s->ops[s->nops++] = o;
  } else {
    free(o.path);
  }
  mc_mutex_unlock(&s->cast->lock);
}

static void op_load(void *user, unsigned id, const mc_remote_track *t, int64_t ms, int play) {
  op o = {OP_LOAD, id, mc_strdup(t->path), t->duration_ms, ms, play};
  push_op(user, o);
}

static void op_queue(void *user, unsigned id, const mc_remote_track *t) {
  op o = {OP_QUEUE, t ? id : 0, t ? mc_strdup(t->path) : NULL, t ? t->duration_ms : 0, 0, 0};
  push_op(user, o);
}

static void op_play(void *user) {
  push_op(user, (op){OP_PLAY, 0, NULL, 0, 0, 0});
}

static void op_pause(void *user) {
  push_op(user, (op){OP_PAUSE, 0, NULL, 0, 0, 0});
}

static void op_seek(void *user, int64_t ms) {
  push_op(user, (op){OP_SEEK, 0, NULL, 0, ms, 0});
}

static void op_stop(void *user) {
  push_op(user, (op){OP_STOP, 0, NULL, 0, 0, 0});
}

/* The player is being freed: hand it back now, and the cast ends. */
static void op_detached(void *user) {
  session *s = user;
  mc_mutex_lock(&s->use);
  if (s->player) mc_player_remote_end(s->player);
  s->player = NULL;
  mc_mutex_unlock(&s->use);
  end_session(s, MC_CAST_END_PLAYER, 0);
}

static const mc_remote_ops remote_ops = {op_load, op_queue, op_play, op_pause, op_seek, op_stop, op_detached};

/* --- Messages --- */

/* Sends json (which it frees) to dest on ns. */
static mc_status send_msg(session *s, const char *dest, const char *ns, char *json) {
  if (!json) return MC_ERR_NOMEM;
  size_t n = cast_frame_encode(CAST_SENDER_ID, dest, ns, json, s->sbuf, 4 + CAST_MAX_MESSAGE);
  free(json);
  if (!n) return MC_ERR_INVALID_ARG;
  return cast_tls_write(s->tls, s->sbuf, n, IO_TIMEOUT_MS);
}

static long new_request(session *s) {
  if (++s->next_request <= 0) s->next_request = 1;
  return s->next_request;
}

/* Sends a media request, noting it as unanswered. */
static mc_status send_media(session *s, long request, char *json) {
  mc_status st = send_msg(s, s->transport, CAST_NS_MEDIA, json);
  if (st == MC_OK && s->nrequests < MAX_REQUESTS) {
    s->requests[s->nrequests] = request;
    s->request_ms[s->nrequests++] = mc_mono_ms();
  }
  return st;
}

static void request_done(session *s, long request) {
  for (int i = 0; i < s->nrequests; i++) {
    if (s->requests[i] != request) continue;
    s->nrequests--;
    memmove(s->requests + i, s->requests + i + 1, (size_t)(s->nrequests - i) * sizeof *s->requests);
    memmove(s->request_ms + i, s->request_ms + i + 1, (size_t)(s->nrequests - i) * sizeof *s->request_ms);
    break;
  }
  if (request == s->load_request) s->load_request = 0;
  if (request == s->ids_request) s->ids_request = 0;
}

/* Waits up to timeout_ms for a whole message, and parses it into msg and reply: 1, or 0 if
 * none came, -1 if the connection failed. A PING is answered here. */
static int receive(session *s, int timeout_ms, cast_message *msg, cast_reply *reply) {
  for (int round = 0; round < 2; round++) {
    if (s->rlen >= 4) {
      uint32_t len = cast_frame_length(s->rbuf);
      if (len > CAST_MAX_MESSAGE) return -1;
      if (s->rlen >= 4 + (size_t)len) {
        int ok = cast_message_decode(s->rbuf + 4, len, msg) == 0 && !msg->binary;
        if (ok) ok = cast_reply_parse((const char *)msg->payload, msg->payload_len, reply) == 0;
        memmove(s->rbuf, s->rbuf + 4 + len, s->rlen - 4 - len);
        s->rlen -= 4 + len;
        s->last_rx = mc_mono_ms();
        if (!ok) memset(reply, 0, sizeof *reply); /* not ours to read: CAST_T_OTHER */
        if (ok && reply->type == CAST_T_PING && !strcmp(msg->ns, CAST_NS_HEARTBEAT))
          send_msg(s, msg->source, CAST_NS_HEARTBEAT, cast_json_simple("PONG", 0));
        return 1;
      }
    }
    if (round) break;
    long n = cast_tls_read(s->tls, s->rbuf + s->rlen, 4 + CAST_MAX_MESSAGE - s->rlen, timeout_ms);
    if (n < 0) return -1;
    s->rlen += (size_t)n;
    if (!n) return 0;
  }
  return 0;
}

/* Waits up to timeout_ms for a message on ns of type a or b, dropping others: 1 with it, 0
 * if the time ran out or the session is ending, -1 if the connection failed. */
static int wait_for(session *s, const char *ns, cast_type a, cast_type b, int timeout_ms, cast_message *msg,
                    cast_reply *reply) {
  uint64_t end = mc_mono_ms() + (uint64_t)timeout_ms;
  while (mc_mono_ms() < end && !stopping(s)) {
    int r = receive(s, TICK_MS, msg, reply);
    if (r < 0) return -1;
    if (r && !strcmp(msg->ns, ns) && (reply->type == a || reply->type == b)) return 1;
  }
  return 0;
}

/* --- Volume --- */

static void take_volume(session *s, const cast_receiver_status *r) {
  if (!r->has_volume) return;
  mc_cast *c = s->cast;
  int volume = (int)(r->level * 100 + 0.5);
  mc_mutex_lock(&c->lock);
  int changed = c->session == s && (c->status.volume != volume || c->status.muted != r->muted);
  if (changed) {
    c->status.volume = volume;
    c->status.muted = r->muted;
  }
  mc_mutex_unlock(&c->lock);
  if (changed) notify(c, MC_CAST_EVENT_VOLUME);
}

static void send_volume(session *s) {
  mc_mutex_lock(&s->cast->lock);
  int set_level = s->set_level, level = s->level, set_muted = s->set_muted, muted = s->muted;
  s->set_level = s->set_muted = 0;
  mc_mutex_unlock(&s->cast->lock);
  /* One at a time: the receiver takes a level or muted in a message, not both. */
  if (set_level)
    send_msg(s, CAST_RECEIVER_ID, CAST_NS_RECEIVER, cast_json_set_volume(new_request(s), 1, level / 100.0, 0, 0));
  if (set_muted)
    send_msg(s, CAST_RECEIVER_ID, CAST_NS_RECEIVER, cast_json_set_volume(new_request(s), 0, 0, 1, muted));
}

/* --- Connecting --- */

/* Connects to the device, starts the Default Media Receiver if it isn't running, and joins
 * it, leaving its media status in *media. */
static mc_status open_session(session *s, cast_media_status *media) {
  cast_message msg;
  cast_reply reply;
  memset(media, 0, sizeof *media);
  s->rlen = 0;
  s->nrequests = 0;
  s->load_request = s->ids_request = 0;
  mc_status st = cast_tls_connect(s->device.host, s->device.port, CONNECT_TIMEOUT_MS, &s->tls);
  if (st != MC_OK) return st;
  s->last_rx = s->last_ping = mc_mono_ms();
  if (send_msg(s, CAST_RECEIVER_ID, CAST_NS_CONNECTION, cast_json_simple("CONNECT", 0)) != MC_OK ||
      send_msg(s, CAST_RECEIVER_ID, CAST_NS_RECEIVER, cast_json_simple("GET_STATUS", new_request(s))) != MC_OK ||
      wait_for(s, CAST_NS_RECEIVER, CAST_T_RECEIVER_STATUS, CAST_T_RECEIVER_STATUS, REPLY_TIMEOUT_MS, &msg, &reply) != 1)
    return MC_ERR_NET;
  take_volume(s, &reply.receiver);
  const cast_app *app = cast_receiver_app(&reply.receiver, CAST_APP_DEFAULT_MEDIA);
  if (!app) {
    if (send_msg(s, CAST_RECEIVER_ID, CAST_NS_RECEIVER, cast_json_launch(new_request(s), CAST_APP_DEFAULT_MEDIA)) != MC_OK)
      return MC_ERR_NET;
    uint64_t end = mc_mono_ms() + LAUNCH_TIMEOUT_MS;
    while (!app && mc_mono_ms() < end) {
      int r = wait_for(s, CAST_NS_RECEIVER, CAST_T_RECEIVER_STATUS, CAST_T_LAUNCH_ERROR, LAUNCH_TIMEOUT_MS, &msg, &reply);
      if (r != 1 || reply.type == CAST_T_LAUNCH_ERROR) return MC_ERR_NET;
      take_volume(s, &reply.receiver);
      app = cast_receiver_app(&reply.receiver, CAST_APP_DEFAULT_MEDIA);
    }
    if (!app || !app->transport_id[0]) return MC_ERR_NET;
  }
  copy_str(s->transport, sizeof s->transport, app->transport_id);
  copy_str(s->app_session, sizeof s->app_session, app->session_id);
  long request = new_request(s);
  if (send_msg(s, s->transport, CAST_NS_CONNECTION, cast_json_simple("CONNECT", 0)) != MC_OK ||
      send_msg(s, s->transport, CAST_NS_MEDIA, cast_json_media(request, "GET_STATUS", 0)) != MC_OK)
    return MC_ERR_NET;
  uint64_t end = mc_mono_ms() + REPLY_TIMEOUT_MS;
  while (mc_mono_ms() < end) {
    int r = wait_for(s, CAST_NS_MEDIA, CAST_T_MEDIA_STATUS, CAST_T_MEDIA_STATUS, REPLY_TIMEOUT_MS, &msg, &reply);
    if (r != 1) return MC_ERR_NET;
    if (reply.request_id == request || !reply.request_id) {
      *media = reply.media;
      s->msid = media->present ? media->media_session_id : 0;
      return MC_OK;
    }
  }
  return MC_ERR_NET;
}

static void close_tls(session *s) {
  cast_tls_close(s->tls);
  s->tls = NULL;
}

/* --- Items --- */

/* The player's id for an item, from a URL of this session's server; 0 if it isn't one. *stale
 * says it's one of an earlier server's. */
static unsigned id_of_url(session *s, const char *url, int *stale) {
  *stale = 0;
  if (!url[0]) return 0;
  const char *base = cast_server_base(s->server);
  size_t n = strlen(base);
  if (strncmp(url, base, n) != 0) {
    *stale = s->old_base[0] && !strncmp(url, s->old_base, strlen(s->old_base));
    return 0;
  }
  const char *name = url + n;
  if (name[0] != 't') return 0;
  char *end;
  unsigned long id = strtoul(name + 1, &end, 10);
  return *end == '.' ? (unsigned)id : 0;
}

static void map_set(session *s, unsigned id, int item) {
  if (!id || !item) return;
  for (int i = 0; i < s->nmap; i++)
    if (s->map[i].id == id || s->map[i].item == item) {
      s->map[i].id = id;
      s->map[i].item = item;
      return;
    }
  if (s->nmap == MAX_MAP) memmove(s->map, s->map + 1, (size_t)--s->nmap * sizeof s->map[0]);
  s->map[s->nmap].id = id;
  s->map[s->nmap++].item = item;
}

static unsigned id_of_item(const session *s, int item) {
  for (int i = 0; i < s->nmap; i++)
    if (s->map[i].item == item) return s->map[i].id;
  return 0;
}

/* Serves only what's on the device: its current item and the one queued. */
static void serve_only(session *s) {
  const char *names[2 * MAX_SERVED];
  int n = 0;
  served_names kept[MAX_SERVED];
  int nkept = 0;
  for (int i = 0; i < MAX_SERVED; i++) {
    served_names *sv = &s->served[i];
    if (!sv->id || (sv->id != s->dev_current && sv->id != s->dev_queued && sv->id != s->next_id)) continue;
    kept[nkept++] = *sv;
  }
  memset(s->served, 0, sizeof s->served);
  for (int i = 0; i < nkept; i++) {
    s->served[i] = kept[i];
    names[n++] = s->served[i].track;
    if (s->served[i].art[0]) names[n++] = s->served[i].art;
  }
  cast_server_keep(s->server, names, n);
}

static void note_served(session *s, const served_names *names) {
  for (int i = 0; i < MAX_SERVED; i++)
    if (!s->served[i].id || s->served[i].id == names->id) {
      s->served[i] = *names;
      return;
    }
  memmove(s->served, s->served + 1, (MAX_SERVED - 1) * sizeof s->served[0]);
  s->served[MAX_SERVED - 1] = *names;
}

static const char *image_type(const unsigned char *data, size_t len) {
  if (len >= 8 && !memcmp(data, "\x89PNG", 4)) return "image/png";
  return "image/jpeg";
}

/* What an item puts on the device: its media and metadata, with the strings it needs. */
typedef struct item_info {
  cast_media_item item;
  char url[256], image_url[256];
  mc_tags tags;
  int has_tags;
  char *title; /* the file's name, without a title tag */
} item_info;

/* Puts the item with id and path up on the server, with its cover, and describes it. */
static int make_item(session *s, unsigned id, const char *path, int64_t duration_ms, int64_t start_ms, int autoplay,
                     item_info *out) {
  memset(out, 0, sizeof *out);
  int as_wav;
  const char *type = cast_http_content_type(path, &as_wav);
  if (!type) type = "application/octet-stream"; /* the device says it can't play it */
  served_names names;
  memset(&names, 0, sizeof names);
  names.id = id;
  const char *dot = strrchr(path, '.');
  char ext[8] = "bin";
  if (as_wav) {
    strcpy(ext, "wav");
  } else if (dot && strlen(dot + 1) < sizeof ext && !strchr(dot, '/') && !strchr(dot, '\\')) {
    for (size_t i = 0; dot[1 + i]; i++) {
      char ch = dot[1 + i];
      ext[i] = ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
      ext[i + 1] = '\0';
    }
  }
  for (char *e = ext; *e; e++)
    if (!((*e >= 'a' && *e <= 'z') || (*e >= '0' && *e <= '9'))) strcpy(ext, "bin");
  snprintf(names.track, sizeof names.track, "t%u.%s", id, ext);
  if (cast_server_add_file(s->server, names.track, path, type, as_wav) != MC_OK) return 0;
  snprintf(out->url, sizeof out->url, "%s%s", cast_server_base(s->server), names.track);

  /* The cover: the one in the track's folder, as the library has it, else the file's own. */
  char *dir = mc_strdup(path);
  char *slash = dir ? strrchr(dir, '/') : NULL;
#ifdef _WIN32
  char *bslash = dir ? strrchr(dir, '\\') : NULL;
  if (bslash && (!slash || bslash > slash)) slash = bslash;
  /* A file at a drive's root keeps its folder's backslash: "C:" alone is the drive's current folder. */
  if (slash == dir + 2 && dir[1] == ':') slash++;
#endif
  if (slash) *slash = '\0';
  char *cover = slash ? mc_coverart_find(dir) : NULL;
  free(dir);
  if (cover) {
    const char *cdot = strrchr(cover, '.');
    int png = cdot && (!strcmp(cdot, ".png") || !strcmp(cdot, ".PNG"));
    snprintf(names.art, sizeof names.art, "a%u.%s", id, png ? "png" : "jpg");
    if (cast_server_add_file(s->server, names.art, cover, png ? "image/png" : "image/jpeg", 0) != MC_OK)
      names.art[0] = '\0';
    mc_free(cover);
  } else {
    unsigned char *data = NULL;
    size_t len = 0;
    if (mc_tags_read_picture(path, &data, &len) == MC_OK) {
      const char *itype = image_type(data, len);
      snprintf(names.art, sizeof names.art, "a%u.%s", id, strcmp(itype, "image/png") ? "jpg" : "png");
      if (cast_server_add_bytes(s->server, names.art, data, len, itype) != MC_OK) names.art[0] = '\0';
      free(data);
    }
  }
  if (names.art[0]) snprintf(out->image_url, sizeof out->image_url, "%s%s", cast_server_base(s->server), names.art);
  note_served(s, &names);

  out->has_tags = mc_tags_read(path, &out->tags) == MC_OK;
  cast_media_item *it = &out->item;
  it->url = out->url;
  it->content_type = type;
  if (out->has_tags && out->tags.title && *out->tags.title) {
    it->title = out->tags.title;
  } else { /* the file's name, as the library would call it */
    const char *base = strrchr(path, '/');
#ifdef _WIN32
    const char *bbase = strrchr(path, '\\');
    if (bbase && (!base || bbase > base)) base = bbase;
#endif
    out->title = mc_strdup(base ? base + 1 : path);
    char *tdot = out->title ? strrchr(out->title, '.') : NULL;
    if (tdot && tdot != out->title) *tdot = '\0';
    it->title = out->title;
  }
  if (out->has_tags && out->tags.artist && *out->tags.artist) it->artist = out->tags.artist;
  if (out->has_tags && out->tags.album && *out->tags.album) it->album = out->tags.album;
  if (out->has_tags) it->track_no = out->tags.track;
  it->image_url = names.art[0] ? out->image_url : NULL;
  if (!duration_ms && out->has_tags) duration_ms = out->tags.duration_ms;
  it->duration_s = (double)duration_ms / 1000.0;
  it->start_s = (double)start_ms / 1000.0;
  it->autoplay = autoplay;
  return 1;
}

static void item_free(item_info *info) {
  if (info->has_tags) mc_tags_clear(&info->tags);
  free(info->title);
}

/* --- Talking to the player --- */

static void report_status(session *s, unsigned id, const cast_media_status *m) {
  mc_remote_state state = m->state == CAST_PLAYER_PLAYING  ? MC_REMOTE_PLAYING
                          : m->state == CAST_PLAYER_PAUSED ? MC_REMOTE_PAUSED
                                                           : MC_REMOTE_LOADING;
  int64_t ms = m->has_time ? (int64_t)(m->current_time * 1000 + 0.5) : -1;
  mc_mutex_lock(&s->use);
  if (s->player) mc_player_remote_status(s->player, id, state, ms);
  mc_mutex_unlock(&s->use);
}

typedef enum report_kind { REPORT_FINISHED, REPORT_FAILED, REPORT_DROPPED, REPORT_RELOAD } report_kind;

static void report(session *s, report_kind kind, unsigned id) {
  mc_mutex_lock(&s->use);
  if (s->player) {
    if (kind == REPORT_FINISHED) mc_player_remote_finished(s->player, id);
    else if (kind == REPORT_FAILED) mc_player_remote_failed(s->player, id);
    else if (kind == REPORT_DROPPED) mc_player_remote_dropped(s->player);
    else mc_player_remote_reload(s->player);
  }
  mc_mutex_unlock(&s->use);
}

/* --- What the device says --- */

/* Learns which device item is which of ours, from the status's queue. */
static void learn_items(session *s, const cast_media_status *m) {
  if (!m->has_items) return;
  s->items_known = 1;
  s->nitems = m->nitems;
  int named = 0;
  for (int i = 0; i < m->nitems; i++) {
    s->items[i] = m->items[i].item_id;
    int stale;
    unsigned id = id_of_url(s, m->items[i].content_id, &stale);
    if (id) {
      map_set(s, id, m->items[i].item_id);
      named = 1;
    }
  }
  /* Without media in the list, a queue as loaded is in the order it was loaded, and an item
   * inserted is at the end. */
  if (!named && s->loaded_n && m->nitems == s->loaded_n) {
    map_set(s, s->dev_current, m->items[0].item_id);
    if (s->loaded_n > 1) map_set(s, s->dev_queued, m->items[1].item_id);
  }
  if (s->inserted && s->dev_queued && m->nitems && !id_of_item(s, m->items[m->nitems - 1].item_id))
    map_set(s, s->dev_queued, m->items[m->nitems - 1].item_id);
  s->loaded_n = 0;
  s->inserted = 0;
}

static int has_item_after(const session *s, int item) {
  if (!s->items_known) return 0;
  for (int i = 0; i < s->nitems; i++)
    if (s->items[i] == item) return i + 1 < s->nitems;
  return 0;
}

/* Whether the player has asked for something not yet sent: a status meanwhile would undo it. */
static int ops_waiting(session *s) {
  mc_mutex_lock(&s->cast->lock);
  int n = s->nops;
  mc_mutex_unlock(&s->cast->lock);
  return n > 0;
}

static void media_status(session *s, const cast_media_status *m) {
  if (!m->present) { /* nothing loaded: what we had was stopped, or played out and was cleared */
    s->msid = 0;
    return;
  }
  s->msid = m->media_session_id;
  learn_items(s, m);
  int stale;
  unsigned on = id_of_url(s, m->content_id, &stale);
  if (on) s->ours_seen = 1;
  if (m->content_id[0] && !on && !stale && !s->ours_seen) return; /* what was there before ours */
  if (m->content_id[0] && !on && !stale) {
    /* Someone else's media: another app cast to the device. Paused here, and the cast ends,
     * leaving the device to them. */
    s->displaced = 1;
    report(s, REPORT_DROPPED, 0);
    end_session(s, MC_CAST_END_DISPLACED, 0);
    return;
  }
  if (!on && m->current_item_id) on = id_of_item(s, m->current_item_id);
  /* An item it's on that we can't name, with the current one known: the device has only ever
   * the current item and the queued one (and those played, which are named), so it's that. */
  if (!on && m->current_item_id && s->current_item && m->current_item_id != s->current_item && s->dev_queued)
    on = s->dev_queued;
  if (on && m->current_item_id) map_set(s, on, m->current_item_id);
  if (on && on == s->dev_queued && on != s->dev_current) {
    /* It went on to the queued item by itself. */
    s->dev_current = on;
    s->dev_queued = 0;
    s->dev_done = 0;
    s->current_item = m->current_item_id;
    serve_only(s);
    report_status(s, on, m);
    return;
  }
  if (s->nrequests || ops_waiting(s)) return;
  if (!on || on != s->dev_current || s->dev_done) {
    /* A queue that played out can leave a status with no media at all. */
    if (m->state == CAST_PLAYER_IDLE && !m->content_id[0] && !m->current_item_id && s->dev_current &&
        !s->dev_done && m->idle_reason == CAST_IDLE_FINISHED) {
      s->dev_done = 1;
      report(s, REPORT_FINISHED, s->dev_current);
    }
    return;
  }
  if (m->current_item_id) s->current_item = m->current_item_id;
  if (m->state != CAST_PLAYER_IDLE) {
    report_status(s, on, m);
    return;
  }
  switch (m->idle_reason) {
  case CAST_IDLE_FINISHED:
    if (m->loading_item_id || has_item_after(s, m->current_item_id)) break; /* going on by itself */
    s->dev_done = 1;
    report(s, REPORT_FINISHED, on);
    break;
  case CAST_IDLE_ERROR:
    s->dev_done = 1;
    report(s, REPORT_FAILED, on);
    break;
  case CAST_IDLE_CANCELLED:
  case CAST_IDLE_INTERRUPTED: /* stopped from elsewhere */
    s->dev_done = 1;
    report(s, REPORT_DROPPED, on);
    break;
  case CAST_IDLE_NONE:
    break;
  }
}

/* Handles a message from the device, while casting. */
static void handle(session *s, const cast_message *msg, const cast_reply *reply) {
  if (!strcmp(msg->ns, CAST_NS_CONNECTION)) {
    if (reply->type == CAST_T_CLOSE) /* the receiver closed us, or the app went */
      end_session(s, strcmp(msg->source, s->transport) ? MC_CAST_END_LOST : MC_CAST_END_REMOTE, 0);
  } else if (!strcmp(msg->ns, CAST_NS_RECEIVER)) {
    if (reply->type != CAST_T_RECEIVER_STATUS) return;
    take_volume(s, &reply->receiver);
    const cast_app *app = cast_receiver_app(&reply->receiver, CAST_APP_DEFAULT_MEDIA);
    if (app && !strcmp(app->session_id, s->app_session)) return;
    /* Our receiver has gone: replaced by another app, or stopped. */
    s->displaced = reply->receiver.napps > 0;
    report(s, REPORT_DROPPED, 0);
    end_session(s, s->displaced ? MC_CAST_END_DISPLACED : MC_CAST_END_REMOTE, 0);
  } else if (!strcmp(msg->ns, CAST_NS_MEDIA)) {
    if (reply->request_id) request_done(s, reply->request_id);
    switch (reply->type) {
    case CAST_T_MEDIA_STATUS:
      media_status(s, &reply->media);
      break;
    case CAST_T_LOAD_FAILED:
      if (s->dev_current && !s->dev_done) {
        s->dev_done = 1;
        report(s, REPORT_FAILED, s->dev_current);
      }
      break;
    case CAST_T_QUEUE_ITEM_IDS:
      s->items_known = 1;
      s->nitems = reply->nids;
      memcpy(s->items, reply->ids, (size_t)reply->nids * sizeof *s->items);
      break;
    default:
      break;
    }
  }
}

/* --- What the player asks --- */

/* Loads the item of load on the device, with the item of queue (may be NULL) after it. */
static void do_load(session *s, const op *load, const op *queue) {
  item_info info[2];
  int n = 0;
  if (make_item(s, load->id, load->path, load->duration_ms, load->ms, load->play, &info[0])) n = 1;
  if (n && queue && queue->id && make_item(s, queue->id, queue->path, queue->duration_ms, 0, 1, &info[1])) n = 2;
  s->dev_current = load->id;
  s->dev_queued = n == 2 ? queue->id : 0;
  s->dev_done = 0;
  s->current_item = 0;
  s->items_known = 0;
  s->nmap = 0;
  s->loaded_n = n;
  s->inserted = 0;
  s->pending_next = 0;
  free(s->next_path);
  s->next_path = NULL;
  s->next_id = 0;
  if (!n) { /* couldn't even put it up */
    s->dev_done = 1;
    report(s, REPORT_FAILED, load->id);
    return;
  }
  cast_media_item items[2] = {info[0].item, n == 2 ? info[1].item : info[0].item};
  long request = new_request(s);
  if (send_media(s, request, cast_json_queue_load(request, s->app_session, items, n, PRELOAD_S)) == MC_OK)
    s->load_request = request;
  for (int i = 0; i < n; i++) item_free(&info[i]);
  serve_only(s);
}

/* Makes the item queued on the device what the player last asked for: everything after the
 * current item goes (and before it, played already), and the new one is added. Once the
 * device is on the current item and has answered everything, a change at a time. */
static void apply_next(session *s) {
  if (!s->pending_next || !s->msid || s->nrequests || !s->dev_current || s->dev_done || !s->current_item) return;
  if (!s->items_known) {
    long request = new_request(s);
    if (send_media(s, request, cast_json_media(request, "QUEUE_GET_ITEM_IDS", s->msid)) == MC_OK)
      s->ids_request = request;
    return;
  }
  int remove[CAST_MAX_ITEMS], n = 0;
  for (int i = 0; i < s->nitems; i++)
    if (s->items[i] != s->current_item) remove[n++] = s->items[i];
  if (n) {
    long request = new_request(s);
    send_media(s, request, cast_json_queue_remove(request, s->msid, remove, n));
  }
  s->dev_queued = s->next_id;
  item_info info;
  int added = 0;
  if (s->next_id && make_item(s, s->next_id, s->next_path, s->next_duration_ms, 0, 1, &info)) {
    long request = new_request(s);
    added = send_media(s, request, cast_json_queue_insert(request, s->msid, &info.item, 1, PRELOAD_S)) == MC_OK;
    item_free(&info);
  }
  s->inserted = added;
  if (!added) s->dev_queued = 0;
  if (n || added) s->items_known = 0; /* the next status lists them */
  s->pending_next = 0;
  free(s->next_path);
  s->next_path = NULL;
  s->next_id = 0;
  serve_only(s);
}

/* Carries out what the player has asked since last time, in order. Media commands wait for a
 * load to be answered, as they need its media session. */
static void run_ops(session *s) {
  mc_mutex_lock(&s->cast->lock);
  op *ops = s->ops;
  int n = s->nops;
  s->ops = NULL;
  s->nops = 0;
  mc_mutex_unlock(&s->cast->lock);
  int done = 0;
  for (; done < n; done++) {
    op *o = &ops[done];
    int needs_media = o->kind == OP_PLAY || o->kind == OP_PAUSE || o->kind == OP_SEEK || o->kind == OP_STOP;
    if (needs_media && s->load_request) break; /* after the load's answer */
    long request;
    switch (o->kind) {
    case OP_LOAD: {
      const op *queue = done + 1 < n && ops[done + 1].kind == OP_QUEUE ? &ops[done + 1] : NULL;
      do_load(s, o, queue);
      if (queue) done++;
      break;
    }
    case OP_QUEUE:
      s->pending_next = 1;
      free(s->next_path);
      s->next_path = o->path;
      o->path = NULL;
      s->next_id = o->id;
      s->next_duration_ms = o->duration_ms;
      break;
    case OP_PLAY:
    case OP_PAUSE:
      if (!s->msid) break; /* nothing loaded: the player loads it */
      request = new_request(s);
      send_media(s, request, cast_json_media(request, o->kind == OP_PLAY ? "PLAY" : "PAUSE", s->msid));
      break;
    case OP_SEEK:
      if (!s->msid) break;
      request = new_request(s);
      send_media(s, request, cast_json_seek(request, s->msid, (double)o->ms / 1000.0));
      break;
    case OP_STOP:
      if (s->msid) {
        request = new_request(s);
        send_media(s, request, cast_json_media(request, "STOP", s->msid));
      }
      s->dev_current = s->dev_queued = 0;
      s->pending_next = 0;
      serve_only(s);
      break;
    }
  }
  /* What waits goes back in front of anything asked meanwhile. */
  if (done < n) {
    mc_mutex_lock(&s->cast->lock);
    op *merged = malloc((size_t)(n - done + s->nops) * sizeof *merged);
    if (merged) {
      memcpy(merged, ops + done, (size_t)(n - done) * sizeof *merged);
      if (s->nops) memcpy(merged + n - done, s->ops, (size_t)s->nops * sizeof *merged);
      free(s->ops);
      s->ops = merged;
      s->nops += n - done;
      n = done; /* the rest are merged's now */
    }
    mc_mutex_unlock(&s->cast->lock);
  }
  free_ops(ops, n);
}

/* --- The connection dropping --- */

/* Gets a lost connection back, as Cast's own senders do: a few tries, a second or more apart.
 * Meanwhile the player holds its position. 0 if it couldn't. */
static int reconnect(session *s) {
  if (s->dev_current) {
    mc_mutex_lock(&s->use);
    if (s->player) mc_player_remote_status(s->player, s->dev_current, MC_REMOTE_LOADING, -1);
    mc_mutex_unlock(&s->use);
  }
  close_tls(s);
  set_state(s, MC_CAST_CONNECTING);
  for (int attempt = 0; attempt < RECONNECT_TRIES; attempt++) {
    for (uint64_t end = mc_mono_ms() + (1000u << attempt); mc_mono_ms() < end;) {
      if (stopping(s)) return 0;
      mc_sleep_ms(TICK_MS);
    }
    /* At its new address, should discovery have seen it move. */
    mc_mutex_lock(&s->cast->lock);
    for (int i = 0; i < s->cast->ndevices; i++)
      if (!strcmp(s->cast->devices[i].id, s->device.id)) s->device = s->cast->devices[i];
    mc_mutex_unlock(&s->cast->lock);
    cast_media_status media;
    if (open_session(s, &media) != MC_OK) {
      close_tls(s);
      continue;
    }
    char local[MC_CAST_ADDR_LEN];
    int lost = 0;
    if (cast_tls_local_address(s->tls, local, sizeof local) == MC_OK && strcmp(local, s->local_addr) != 0) {
      cast_server *server;
      if (cast_server_start(local, &server) != MC_OK) {
        close_tls(s);
        continue;
      }
      copy_str(s->old_base, sizeof s->old_base, cast_server_base(s->server));
      cast_server_stop(s->server);
      s->server = server;
      copy_str(s->local_addr, sizeof s->local_addr, local);
      lost = 1;
    }
    int stale;
    if (!lost && media.present && id_of_url(s, media.content_id, &stale) == s->dev_current && s->dev_current) {
      media_status(s, &media); /* still ours, where it had got to */
    } else if (s->dev_current) {
      s->dev_current = s->dev_queued = 0;
      s->pending_next = 0;
      report(s, REPORT_RELOAD, 0);
    }
    set_state(s, MC_CAST_CASTING);
    return 1;
  }
  return 0;
}

/* --- The session --- */

static void finish(session *s, mc_cast_end why) {
  mc_cast *c = s->cast;
  mc_mutex_lock(&c->lock);
  int ours = c->session == s;
  if (ours) {
    memset(&c->status, 0, sizeof c->status);
    c->status.state = MC_CAST_IDLE;
    c->status.ended = why;
  }
  s->finished = 1;
  mc_mutex_unlock(&c->lock);
  if (ours) notify(c, MC_CAST_EVENT_STATE);
}

/* Plays on the device until the cast ends. 0 if it never started. */
static int cast_loop(session *s) {
  cast_message msg;
  cast_reply reply;
  for (;;) {
    if (stopping(s)) return 1;
    int r = receive(s, TICK_MS, &msg, &reply);
    while (r == 1) {
      handle(s, &msg, &reply);
      r = receive(s, 0, &msg, &reply);
    }
    uint64_t now = mc_mono_ms();
    if (r < 0 || now - s->last_rx > SILENT_MS) {
      if (stopping(s)) return 1;
      if (!reconnect(s)) {
        if (stopping(s)) return 1; /* asked to end meanwhile */
        report(s, REPORT_DROPPED, 0);
        end_session(s, MC_CAST_END_LOST, 0);
        return 1;
      }
      continue;
    }
    if (now - s->last_ping >= PING_MS) {
      s->last_ping = now;
      if (send_msg(s, CAST_RECEIVER_ID, CAST_NS_HEARTBEAT, cast_json_simple("PING", 0)) != MC_OK) continue;
    }
    for (int i = s->nrequests - 1; i >= 0; i--)
      if (now - s->request_ms[i] > REQUEST_TIMEOUT_MS) request_done(s, s->requests[i]);
    run_ops(s);
    apply_next(s);
    send_volume(s);

    /* Three minutes paused (or stopped), by whoever paused it, ends it, and playback stops. */
    mc_player_status st;
    int have = 0;
    mc_mutex_lock(&s->use);
    if (s->player) {
      mc_player_status_get(s->player, &st);
      have = 1;
    }
    mc_mutex_unlock(&s->use);
    if (have && st.state == MC_PLAYER_PLAYING) s->idle_since = 0;
    else if (have && !s->idle_since) s->idle_since = now;
    else if (have && now - s->idle_since >= mc_cast_test_paused_ms) end_session(s, MC_CAST_END_PAUSED, 1);
  }
}

static void session_main(void *arg) {
  session *s = arg;
  mc_mutex_lock(&s->cast->lock);
  int ours = s->cast->session == s;
  mc_mutex_unlock(&s->cast->lock);
  if (ours) notify(s->cast, MC_CAST_EVENT_STATE); /* connecting, as mc_cast_connect() set it */
  cast_media_status media;
  mc_cast_end why = MC_CAST_END_FAILED;
  int began = 0;
  if (open_session(s, &media) == MC_OK && cast_tls_local_address(s->tls, s->local_addr, sizeof s->local_addr) == MC_OK &&
      cast_server_start(s->local_addr, &s->server) == MC_OK) {
    mc_mutex_lock(&s->use);
    mc_status st = s->player ? mc_player_remote_begin(s->player, &remote_ops, s) : MC_ERR_INVALID_ARG;
    mc_mutex_unlock(&s->use);
    if (st == MC_OK) {
      began = 1;
      s->idle_since = 0;
      set_state(s, MC_CAST_CASTING);
      cast_loop(s);
    } else if (!s->player) {
      why = MC_CAST_END_PLAYER;
    }
  }

  mc_mutex_lock(&s->cast->lock);
  int stop_playback = s->stop_playback;
  if (began) why = s->end;
  else if (s->stop && s->end == MC_CAST_END_NONE) why = MC_CAST_END_NONE; /* given up on, as asked */
  else if (s->end != MC_CAST_END_NONE) why = s->end;
  mc_mutex_unlock(&s->cast->lock);

  /* Back to the computer, at the place the device had got to, paused; then, if the pause
   * ran out, stopped. */
  mc_mutex_lock(&s->use);
  if (s->player && began) {
    mc_player_remote_end(s->player);
    if (stop_playback) mc_player_stop(s->player);
  }
  mc_mutex_unlock(&s->use);

  /* The device stops, unless another app has it, or it went by itself. */
  if (s->tls && s->transport[0]) {
    if (began && !s->displaced && why != MC_CAST_END_REMOTE && why != MC_CAST_END_LOST)
      send_msg(s, CAST_RECEIVER_ID, CAST_NS_RECEIVER, cast_json_stop_app(new_request(s), s->app_session));
    send_msg(s, s->transport, CAST_NS_CONNECTION, cast_json_simple("CLOSE", 0));
  }
  if (s->tls) send_msg(s, CAST_RECEIVER_ID, CAST_NS_CONNECTION, cast_json_simple("CLOSE", 0));
  close_tls(s);
  cast_server_stop(s->server);
  s->server = NULL;
  finish(s, why);
}

static void session_free(session *s) {
  if (!s) return;
  free_ops(s->ops, s->nops);
  free(s->next_path);
  free(s->rbuf);
  free(s->sbuf);
  mc_mutex_destroy(&s->use);
  free(s);
}

/* Asks s to end, and waits for it. Without the handle's lock. */
static void session_end_and_free(mc_cast *c, session *s) {
  if (!s) return;
  mc_mutex_lock(&c->lock);
  s->stop = 1;
  mc_mutex_unlock(&c->lock);
  mc_thread_join(&s->thread);
  session_free(s);
}

/* --- API --- */

static void on_devices(void *user, const mc_cast_device *devices, int count) {
  mc_cast *c = user;
  mc_cast_device *copy = count ? malloc((size_t)count * sizeof *copy) : NULL;
  if (count && !copy) return;
  if (count) memcpy(copy, devices, (size_t)count * sizeof *copy);
  mc_mutex_lock(&c->lock);
  free(c->devices);
  c->devices = copy;
  c->ndevices = count;
  /* The device being cast to was renamed. */
  int renamed = 0;
  for (int i = 0; c->session && i < count; i++) {
    if (strcmp(devices[i].id, c->status.device_id) != 0) continue;
    const char *name = display_name(&devices[i]);
    if (c->status.state != MC_CAST_IDLE && strcmp(name, c->status.device_name) != 0) {
      copy_str(c->status.device_name, sizeof c->status.device_name, name);
      renamed = 1;
    }
  }
  mc_mutex_unlock(&c->lock);
  notify(c, MC_CAST_EVENT_DEVICES);
  if (renamed) notify(c, MC_CAST_EVENT_STATE);
}

void mc_cast_test_set_devices(mc_cast *c, const mc_cast_device *devices, int count) {
  on_devices(c, devices, count);
}

mc_status mc_cast_new(mc_cast_cb cb, void *user, mc_cast **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  mc_cast *c = calloc(1, sizeof *c);
  if (!c) return MC_ERR_NOMEM;
  c->cb = cb;
  c->user = user;
  mc_mutex_init(&c->lock);
  *out = c;
  return MC_OK;
}

void mc_cast_free(mc_cast *c) {
  if (!c) return;
  mc_cast_stop_discovery(c);
  mc_mutex_lock(&c->lock);
  session *s = c->session;
  c->session = NULL; /* so its ending reports nothing */
  mc_mutex_unlock(&c->lock);
  session_end_and_free(c, s);
  free(c->devices);
  mc_mutex_destroy(&c->lock);
  free(c);
}

mc_status mc_cast_start_discovery(mc_cast *c) {
  if (!c) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&c->lock);
  int running = c->browser != NULL;
  mc_mutex_unlock(&c->lock);
  if (running) return MC_OK;
  cast_browser *b;
  mc_status st = cast_browser_start(on_devices, c, &b);
  if (st != MC_OK) return st;
  mc_mutex_lock(&c->lock);
  cast_browser *extra = c->browser ? b : NULL; /* started twice at once */
  if (!c->browser) c->browser = b;
  mc_mutex_unlock(&c->lock);
  cast_browser_stop(extra);
  return MC_OK;
}

void mc_cast_stop_discovery(mc_cast *c) {
  if (!c) return;
  mc_mutex_lock(&c->lock);
  cast_browser *b = c->browser;
  c->browser = NULL;
  mc_mutex_unlock(&c->lock);
  cast_browser_stop(b);
}

mc_status mc_cast_devices(mc_cast *c, mc_cast_device_list **out) {
  if (!c || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_cast_device_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  mc_mutex_lock(&c->lock);
  if (c->ndevices && (list->items = malloc((size_t)c->ndevices * sizeof *list->items))) {
    memcpy(list->items, c->devices, (size_t)c->ndevices * sizeof *list->items);
    list->count = c->ndevices;
  }
  int failed = c->ndevices && !list->items;
  mc_mutex_unlock(&c->lock);
  if (failed) {
    free(list);
    return MC_ERR_NOMEM;
  }
  *out = list;
  return MC_OK;
}

void mc_cast_device_list_free(mc_cast_device_list *list) {
  if (!list) return;
  free(list->items);
  free(list);
}

mc_status mc_cast_connect(mc_cast *c, const char *device_id, mc_player *player) {
  if (!c || !device_id || !player) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&c->lock);
  const mc_cast_device *found = NULL;
  for (int i = 0; i < c->ndevices && !found; i++)
    if (!strcmp(c->devices[i].id, device_id)) found = &c->devices[i];
  if (!found) {
    mc_mutex_unlock(&c->lock);
    return MC_ERR_NOT_FOUND;
  }
  session *old = c->session;
  if (old && !old->stop && !old->finished && !strcmp(old->device.id, device_id) && old->player == player) {
    mc_mutex_unlock(&c->lock);
    return MC_OK; /* casting there already */
  }
  mc_cast_device device = *found;
  c->session = NULL;
  mc_mutex_unlock(&c->lock);
  session_end_and_free(c, old);

  session *s = calloc(1, sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  s->cast = c;
  s->device = device;
  s->player = player;
  mc_mutex_init(&s->use);
  s->rbuf = malloc(4 + CAST_MAX_MESSAGE);
  s->sbuf = malloc(4 + CAST_MAX_MESSAGE);
  if (!s->rbuf || !s->sbuf) {
    session_free(s);
    return MC_ERR_NOMEM;
  }
  mc_mutex_lock(&c->lock);
  c->session = s;
  memset(&c->status, 0, sizeof c->status);
  c->status.state = MC_CAST_CONNECTING;
  copy_str(c->status.device_id, sizeof c->status.device_id, device.id);
  copy_str(c->status.device_name, sizeof c->status.device_name, display_name(&device));
  mc_mutex_unlock(&c->lock);
  mc_status st = mc_thread_start(&s->thread, session_main, s);
  if (st != MC_OK) {
    mc_mutex_lock(&c->lock);
    c->session = NULL;
    memset(&c->status, 0, sizeof c->status);
    c->status.ended = MC_CAST_END_FAILED;
    mc_mutex_unlock(&c->lock);
    session_free(s);
  }
  return st;
}

void mc_cast_disconnect(mc_cast *c) {
  if (!c) return;
  mc_mutex_lock(&c->lock);
  if (c->session) c->session->stop = 1;
  mc_mutex_unlock(&c->lock);
}

void mc_cast_status_get(mc_cast *c, mc_cast_status *out) {
  if (!out) return;
  memset(out, 0, sizeof *out);
  if (!c) return;
  mc_mutex_lock(&c->lock);
  *out = c->status;
  mc_mutex_unlock(&c->lock);
}

int mc_cast_is_casting(mc_cast *c) {
  mc_cast_status st;
  mc_cast_status_get(c, &st);
  return st.state == MC_CAST_CASTING;
}

mc_status mc_cast_set_volume(mc_cast *c, int volume) {
  if (!c) return MC_ERR_INVALID_ARG;
  volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
  mc_mutex_lock(&c->lock);
  session *s = c->session;
  if (s && !s->stop && !s->finished) {
    s->set_level = 1;
    s->level = volume;
    if (c->status.muted) {
      s->set_muted = 1;
      s->muted = 0;
    }
    c->status.volume = volume;
    c->status.muted = 0;
  }
  mc_mutex_unlock(&c->lock);
  return MC_OK;
}

mc_status mc_cast_set_muted(mc_cast *c, int muted) {
  if (!c) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&c->lock);
  session *s = c->session;
  if (s && !s->stop && !s->finished) {
    s->set_muted = 1;
    s->muted = muted != 0;
    c->status.muted = muted != 0;
  }
  mc_mutex_unlock(&c->lock);
  return MC_OK;
}
