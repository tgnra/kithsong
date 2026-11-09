/* CASTV2 messages: see castv2.h. */
#include "cast/castv2.h"

#include <stdio.h>
#include <string.h>

#include "yyjson.h"

/* --- Protobuf --- */

enum { WIRE_VARINT = 0, WIRE_64 = 1, WIRE_LEN = 2, WIRE_32 = 5 };

static size_t put_varint(unsigned char *out, uint64_t v) {
  size_t n = 0;
  do {
    unsigned char b = v & 0x7f;
    v >>= 7;
    out[n++] = (unsigned char)(b | (v ? 0x80 : 0));
  } while (v);
  return n;
}

/* Reads a varint at *pos; -1 if it runs off the end or past 64 bits. */
static int get_varint(const unsigned char *in, size_t len, size_t *pos, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (*pos >= len) return -1;
    unsigned char b = in[(*pos)++];
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return 0;
    }
  }
  return -1;
}

static size_t varint_len(uint64_t v) {
  size_t n = 1;
  while (v >>= 7) n++;
  return n;
}

/* A length-delimited field's size on the wire. */
static size_t string_field_len(size_t len) {
  return 1 + varint_len(len) + len;
}

static size_t put_string_field(unsigned char *out, int field, const char *s, size_t len) {
  size_t n = 0;
  out[n++] = (unsigned char)(field << 3 | WIRE_LEN);
  n += put_varint(out + n, len);
  memcpy(out + n, s, len);
  return n + len;
}

size_t cast_frame_encode(const char *source, const char *dest, const char *ns, const char *payload,
                         unsigned char *out, size_t cap) {
  if (!source || !dest || !ns || !payload || !out) return 0;
  size_t ls = strlen(source), ld = strlen(dest), ln = strlen(ns), lp = strlen(payload);
  size_t body = 2 + string_field_len(ls) + string_field_len(ld) + string_field_len(ln) + 2 +
                string_field_len(lp);
  if (body > CAST_MAX_MESSAGE || body + 4 > cap) return 0;
  out[0] = (unsigned char)(body >> 24);
  out[1] = (unsigned char)(body >> 16);
  out[2] = (unsigned char)(body >> 8);
  out[3] = (unsigned char)body;
  size_t n = 4;
  out[n++] = 1 << 3 | WIRE_VARINT; /* protocol_version: CASTV2_1_0 */
  out[n++] = 0;
  n += put_string_field(out + n, 2, source, ls);
  n += put_string_field(out + n, 3, dest, ld);
  n += put_string_field(out + n, 4, ns, ln);
  out[n++] = 5 << 3 | WIRE_VARINT; /* payload_type: STRING */
  out[n++] = 0;
  n += put_string_field(out + n, 6, payload, lp);
  return n;
}

uint32_t cast_frame_length(const unsigned char prefix[4]) {
  return (uint32_t)prefix[0] << 24 | (uint32_t)prefix[1] << 16 | (uint32_t)prefix[2] << 8 | prefix[3];
}

static void copy_id(char out[CAST_ID_LEN], const unsigned char *s, size_t len) {
  if (len >= CAST_ID_LEN) len = CAST_ID_LEN - 1;
  memcpy(out, s, len);
  out[len] = '\0';
}

int cast_message_decode(const unsigned char *body, size_t len, cast_message *out) {
  if (!body || !out) return -1;
  memset(out, 0, sizeof *out);
  unsigned seen = 0;
  size_t pos = 0;
  const unsigned char *utf8 = NULL, *bin = NULL;
  size_t utf8_len = 0, bin_len = 0;
  while (pos < len) {
    uint64_t key, v;
    if (get_varint(body, len, &pos, &key) != 0) return -1;
    unsigned field = (unsigned)(key >> 3), wire = (unsigned)(key & 7);
    switch (wire) {
    case WIRE_VARINT:
      if (get_varint(body, len, &pos, &v) != 0) return -1;
      if (field == 5) out->binary = v == 1;
      break;
    case WIRE_64:
      if (len - pos < 8) return -1;
      pos += 8;
      break;
    case WIRE_32:
      if (len - pos < 4) return -1;
      pos += 4;
      break;
    case WIRE_LEN:
      if (get_varint(body, len, &pos, &v) != 0 || v > len - pos) return -1;
      if (field == 2) copy_id(out->source, body + pos, (size_t)v);
      else if (field == 3) copy_id(out->dest, body + pos, (size_t)v);
      else if (field == 4) copy_id(out->ns, body + pos, (size_t)v);
      else if (field == 6) utf8 = body + pos, utf8_len = (size_t)v;
      else if (field == 7) bin = body + pos, bin_len = (size_t)v;
      pos += (size_t)v;
      break;
    default:
      return -1; /* groups, long gone from protobuf */
    }
    if (field >= 1 && field <= 5) seen |= 1u << field;
  }
  /* The payload of the type it says, which may come before it says. */
  out->payload = out->binary ? bin : utf8;
  out->payload_len = out->binary ? bin_len : utf8_len;
  return (seen & 0x3e) == 0x3e ? 0 : -1; /* fields 1 to 5 are required */
}

/* --- Parsing --- */

static void copy_str(char *out, size_t cap, yyjson_val *v) {
  const char *s = yyjson_get_str(v);
  size_t n = s ? yyjson_get_len(v) : 0;
  if (n >= cap) n = cap - 1;
  if (n) memcpy(out, s, n);
  out[n] = '\0';
}

static int get_int(yyjson_val *obj, const char *key) {
  yyjson_val *v = yyjson_obj_get(obj, key);
  return yyjson_is_num(v) ? (int)yyjson_get_num(v) : 0;
}

/* The media's content id: contentId, else contentUrl (which newer senders set). */
static void content_id_of(yyjson_val *media, char out[CAST_URL_LEN]) {
  yyjson_val *id = yyjson_obj_get(media, "contentId");
  if (!yyjson_is_str(id) || !yyjson_get_len(id)) id = yyjson_obj_get(media, "contentUrl");
  copy_str(out, CAST_URL_LEN, id);
}

static void parse_receiver(yyjson_val *status, cast_receiver_status *out) {
  yyjson_val *volume = yyjson_obj_get(status, "volume");
  yyjson_val *level = yyjson_obj_get(volume, "level"), *muted = yyjson_obj_get(volume, "muted");
  if (yyjson_is_num(level)) {
    out->has_volume = 1;
    out->level = yyjson_get_num(level);
    out->muted = yyjson_is_true(muted);
  }
  yyjson_val *apps = yyjson_obj_get(status, "applications");
  size_t i, max;
  yyjson_val *app;
  yyjson_arr_foreach(apps, i, max, app) {
    if (out->napps == CAST_MAX_APPS) break;
    cast_app *a = &out->apps[out->napps++];
    copy_str(a->app_id, sizeof a->app_id, yyjson_obj_get(app, "appId"));
    copy_str(a->session_id, sizeof a->session_id, yyjson_obj_get(app, "sessionId"));
    copy_str(a->transport_id, sizeof a->transport_id, yyjson_obj_get(app, "transportId"));
  }
}

static cast_player_state player_state(const char *s) {
  if (!s) return CAST_PLAYER_IDLE;
  if (!strcmp(s, "PLAYING")) return CAST_PLAYER_PLAYING;
  if (!strcmp(s, "PAUSED")) return CAST_PLAYER_PAUSED;
  if (!strcmp(s, "BUFFERING")) return CAST_PLAYER_BUFFERING;
  if (!strcmp(s, "LOADING")) return CAST_PLAYER_LOADING;
  return CAST_PLAYER_IDLE;
}

static cast_idle_reason idle_reason(const char *s) {
  if (!s) return CAST_IDLE_NONE;
  if (!strcmp(s, "FINISHED")) return CAST_IDLE_FINISHED;
  if (!strcmp(s, "CANCELLED")) return CAST_IDLE_CANCELLED;
  if (!strcmp(s, "INTERRUPTED")) return CAST_IDLE_INTERRUPTED;
  if (!strcmp(s, "ERROR")) return CAST_IDLE_ERROR;
  return CAST_IDLE_NONE;
}

static void parse_media(yyjson_val *root, cast_media_status *out) {
  yyjson_val *list = yyjson_obj_get(root, "status");
  yyjson_val *s = yyjson_is_arr(list) ? yyjson_arr_get_first(list) : NULL;
  if (!yyjson_is_obj(s)) return;
  out->present = 1;
  yyjson_val *msid = yyjson_obj_get(s, "mediaSessionId");
  out->media_session_id = yyjson_is_num(msid) ? (long long)yyjson_get_num(msid) : 0;
  out->state = player_state(yyjson_get_str(yyjson_obj_get(s, "playerState")));
  out->idle_reason = idle_reason(yyjson_get_str(yyjson_obj_get(s, "idleReason")));
  yyjson_val *t = yyjson_obj_get(s, "currentTime");
  if (yyjson_is_num(t)) {
    out->has_time = 1;
    out->current_time = yyjson_get_num(t);
  }
  out->current_item_id = get_int(s, "currentItemId");
  out->loading_item_id = get_int(s, "loadingItemId");
  out->preloaded_item_id = get_int(s, "preloadedItemId");
  content_id_of(yyjson_obj_get(s, "media"), out->content_id);
  yyjson_val *items = yyjson_obj_get(s, "items");
  if (yyjson_is_arr(items)) {
    out->has_items = 1;
    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(items, i, max, item) {
      if (out->nitems == CAST_MAX_ITEMS) break;
      cast_queue_item *q = &out->items[out->nitems++];
      q->item_id = get_int(item, "itemId");
      content_id_of(yyjson_obj_get(item, "media"), q->content_id);
    }
  }
}

static const struct {
  const char *name;
  cast_type type;
} types[] = {
  {"PING", CAST_T_PING},
  {"PONG", CAST_T_PONG},
  {"CLOSE", CAST_T_CLOSE},
  {"RECEIVER_STATUS", CAST_T_RECEIVER_STATUS},
  {"LAUNCH_ERROR", CAST_T_LAUNCH_ERROR},
  {"MEDIA_STATUS", CAST_T_MEDIA_STATUS},
  {"LOAD_FAILED", CAST_T_LOAD_FAILED},
  {"LOAD_CANCELLED", CAST_T_LOAD_CANCELLED},
  {"INVALID_REQUEST", CAST_T_INVALID_REQUEST},
  {"INVALID_PLAYER_STATE", CAST_T_INVALID_PLAYER_STATE},
  {"QUEUE_ITEM_IDS", CAST_T_QUEUE_ITEM_IDS},
};

int cast_reply_parse(const char *json, size_t len, cast_reply *out) {
  if (!json || !out) return -1;
  memset(out, 0, sizeof *out);
  yyjson_doc *doc = yyjson_read(json, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  const char *type = yyjson_get_str(yyjson_obj_get(root, "type"));
  if (!yyjson_is_obj(root) || !type) {
    yyjson_doc_free(doc);
    return -1;
  }
  out->type = CAST_T_OTHER;
  for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
    if (!strcmp(type, types[i].name)) out->type = types[i].type;
  yyjson_val *id = yyjson_obj_get(root, "requestId");
  out->request_id = yyjson_is_num(id) ? (long)yyjson_get_num(id) : 0;
  if (out->type == CAST_T_RECEIVER_STATUS) {
    parse_receiver(yyjson_obj_get(root, "status"), &out->receiver);
  } else if (out->type == CAST_T_MEDIA_STATUS) {
    parse_media(root, &out->media);
  } else if (out->type == CAST_T_QUEUE_ITEM_IDS) {
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(yyjson_obj_get(root, "itemIds"), i, max, v) {
      if (out->nids == CAST_MAX_ITEMS) break;
      out->ids[out->nids++] = yyjson_is_num(v) ? (int)yyjson_get_num(v) : 0;
    }
  }
  yyjson_doc_free(doc);
  return 0;
}

const cast_app *cast_receiver_app(const cast_receiver_status *status, const char *app_id) {
  for (int i = 0; i < status->napps; i++)
    if (!strcmp(status->apps[i].app_id, app_id)) return &status->apps[i];
  return NULL;
}

/* --- Building --- */

/* A document whose root is {"type": type} with requestId if it isn't 0. NULL if out of
 * memory. */
static yyjson_mut_doc *start(const char *type, long request_id, yyjson_mut_val **root) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return NULL;
  *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, *root);
  yyjson_mut_obj_add_str(doc, *root, "type", type);
  if (request_id) yyjson_mut_obj_add_int(doc, *root, "requestId", request_id);
  return doc;
}

static char *finish(yyjson_mut_doc *doc) {
  if (!doc) return NULL;
  char *json = yyjson_mut_write(doc, 0, NULL);
  yyjson_mut_doc_free(doc);
  return json;
}

char *cast_json_simple(const char *type, long request_id) {
  yyjson_mut_val *root;
  return finish(start(type, request_id, &root));
}

char *cast_json_launch(long request_id, const char *app_id) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("LAUNCH", request_id, &root);
  if (doc) yyjson_mut_obj_add_str(doc, root, "appId", app_id);
  return finish(doc);
}

char *cast_json_stop_app(long request_id, const char *session_id) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("STOP", request_id, &root);
  if (doc) yyjson_mut_obj_add_str(doc, root, "sessionId", session_id);
  return finish(doc);
}

char *cast_json_set_volume(long request_id, int set_level, double level, int set_muted, int muted) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("SET_VOLUME", request_id, &root);
  if (!doc) return NULL;
  yyjson_mut_val *volume = yyjson_mut_obj_add_obj(doc, root, "volume");
  if (set_level) yyjson_mut_obj_add_real(doc, volume, "level", level < 0 ? 0 : level > 1 ? 1 : level);
  if (set_muted) yyjson_mut_obj_add_bool(doc, volume, "muted", muted != 0);
  return finish(doc);
}

char *cast_json_media(long request_id, const char *type, long long media_session_id) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start(type, request_id, &root);
  if (doc && media_session_id) yyjson_mut_obj_add_int(doc, root, "mediaSessionId", media_session_id);
  return finish(doc);
}

char *cast_json_seek(long request_id, long long media_session_id, double seconds) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("SEEK", request_id, &root);
  if (!doc) return NULL;
  yyjson_mut_obj_add_int(doc, root, "mediaSessionId", media_session_id);
  yyjson_mut_obj_add_real(doc, root, "currentTime", seconds < 0 ? 0 : seconds);
  return finish(doc);
}

/* A queue item: the media, with its metadata, and how it plays. */
static yyjson_mut_val *queue_item(yyjson_mut_doc *doc, const cast_media_item *it, double preload_s) {
  yyjson_mut_val *item = yyjson_mut_obj(doc);
  yyjson_mut_val *media = yyjson_mut_obj_add_obj(doc, item, "media");
  yyjson_mut_obj_add_str(doc, media, "contentId", it->url);
  yyjson_mut_obj_add_str(doc, media, "contentUrl", it->url);
  yyjson_mut_obj_add_str(doc, media, "streamType", "BUFFERED");
  yyjson_mut_obj_add_str(doc, media, "contentType", it->content_type);
  if (it->duration_s > 0) yyjson_mut_obj_add_real(doc, media, "duration", it->duration_s);
  yyjson_mut_val *meta = yyjson_mut_obj_add_obj(doc, media, "metadata");
  yyjson_mut_obj_add_int(doc, meta, "metadataType", 3); /* MusicTrackMediaMetadata */
  if (it->title) yyjson_mut_obj_add_str(doc, meta, "title", it->title);
  if (it->artist) yyjson_mut_obj_add_str(doc, meta, "artist", it->artist);
  if (it->album) yyjson_mut_obj_add_str(doc, meta, "albumName", it->album);
  if (it->track_no > 0) yyjson_mut_obj_add_int(doc, meta, "trackNumber", it->track_no);
  if (it->image_url) {
    yyjson_mut_val *images = yyjson_mut_obj_add_arr(doc, meta, "images");
    yyjson_mut_val *image = yyjson_mut_arr_add_obj(doc, images);
    yyjson_mut_obj_add_str(doc, image, "url", it->image_url);
  }
  yyjson_mut_obj_add_bool(doc, item, "autoplay", it->autoplay != 0);
  yyjson_mut_obj_add_real(doc, item, "startTime", it->start_s > 0 ? it->start_s : 0);
  yyjson_mut_obj_add_real(doc, item, "preloadTime", preload_s);
  return item;
}

static void add_items(yyjson_mut_doc *doc, yyjson_mut_val *root, const cast_media_item *items, int n,
                      double preload_s) {
  yyjson_mut_val *arr = yyjson_mut_obj_add_arr(doc, root, "items");
  for (int i = 0; i < n; i++) yyjson_mut_arr_append(arr, queue_item(doc, &items[i], preload_s));
}

char *cast_json_queue_load(long request_id, const char *session_id, const cast_media_item *items, int n,
                           double preload_s) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("QUEUE_LOAD", request_id, &root);
  if (!doc) return NULL;
  if (session_id) yyjson_mut_obj_add_str(doc, root, "sessionId", session_id);
  add_items(doc, root, items, n, preload_s);
  yyjson_mut_obj_add_int(doc, root, "startIndex", 0);
  yyjson_mut_obj_add_str(doc, root, "repeatMode", "REPEAT_OFF");
  if (n && items[0].start_s > 0) yyjson_mut_obj_add_real(doc, root, "currentTime", items[0].start_s);
  return finish(doc);
}

char *cast_json_queue_insert(long request_id, long long media_session_id, const cast_media_item *items,
                             int n, double preload_s) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("QUEUE_INSERT", request_id, &root);
  if (!doc) return NULL;
  yyjson_mut_obj_add_int(doc, root, "mediaSessionId", media_session_id);
  add_items(doc, root, items, n, preload_s);
  return finish(doc);
}

char *cast_json_queue_remove(long request_id, long long media_session_id, const int *item_ids, int n) {
  yyjson_mut_val *root;
  yyjson_mut_doc *doc = start("QUEUE_REMOVE", request_id, &root);
  if (!doc) return NULL;
  yyjson_mut_obj_add_int(doc, root, "mediaSessionId", media_session_id);
  yyjson_mut_val *ids = yyjson_mut_obj_add_arr(doc, root, "itemIds");
  for (int i = 0; i < n; i++) yyjson_mut_arr_add_int(doc, ids, item_ids[i]);
  return finish(doc);
}
