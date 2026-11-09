/* Google Cast's pieces that need no device: the CastMessage frame (protobuf by hand), parsing
 * what a receiver sends and building what's sent, a device's TXT record, and the music
 * server: parsing requests and ranges, the types served, and a live server answering GET,
 * HEAD and ranges over loopback, with an AIFF served as WAV. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cast/cast_browse.h"
#include "cast/cast_http.h"
#include "cast/castv2.h"
#include "kithcore/kithcore.h"
#include "net/socket.h"
#include "test.h"
#include "util/platform.h"
#include "yyjson.h"

/* --- The frame --- */

static void test_frame(void) {
  unsigned char buf[512];
  const char *json = "{\"type\":\"PING\"}";
  size_t n = cast_frame_encode("sender-0", "receiver-0", CAST_NS_HEARTBEAT, json, buf, sizeof buf);
  CHECK(n > 4 && cast_frame_length(buf) == n - 4);
  /* The protobuf as Google's library writes it: fields in order, version and type 0. */
  static const unsigned char head[] = {0x08, 0x00, 0x12, 8, 's', 'e', 'n', 'd', 'e', 'r', '-', '0', 0x1a, 10};
  CHECK(!memcmp(buf + 4, head, sizeof head));
  cast_message m;
  CHECK(cast_message_decode(buf + 4, n - 4, &m) == 0);
  CHECK_STR(m.source, "sender-0");
  CHECK_STR(m.dest, "receiver-0");
  CHECK_STR(m.ns, CAST_NS_HEARTBEAT);
  CHECK(!m.binary && m.payload_len == strlen(json) && !memcmp(m.payload, json, m.payload_len));

  /* Too small a buffer, or over the device's limit: nothing. */
  CHECK(cast_frame_encode("a", "b", "c", json, buf, 20) == 0);
  char *big = malloc(CAST_MAX_MESSAGE + 1);
  unsigned char *out = malloc(2 * CAST_MAX_MESSAGE);
  if (big && out) {
    memset(big, 'x', CAST_MAX_MESSAGE);
    big[CAST_MAX_MESSAGE] = '\0';
    CHECK(cast_frame_encode("a", "b", "c", big, out, 2 * CAST_MAX_MESSAGE) == 0);
    /* A payload long enough for a two-byte length still round-trips. */
    big[300] = '\0';
    size_t len = cast_frame_encode("a", "b", "c", big, out, 2 * CAST_MAX_MESSAGE);
    CHECK(len > 300 && cast_message_decode(out + 4, len - 4, &m) == 0 && m.payload_len == 300);
  }
  free(big);
  free(out);

  /* Fields out of order, an unknown one of each wire type, and the payload before its type:
   * all still read. */
  static const unsigned char odd[] = {
    0x22, 1, 'n',                              /* namespace */
    0x32, 2, '{', '}',                         /* payload_utf8 */
    0x48, 0x96, 0x01,                          /* field 9, varint 150 */
    0x51, 1, 2, 3, 4, 5, 6, 7, 8,              /* field 10, 64-bit */
    0x5a, 3, 'x', 'y', 'z',                    /* field 11, length-delimited */
    0x65, 1, 2, 3, 4,                          /* field 12, 32-bit */
    0x12, 1, 's', 0x1a, 1, 'd', 0x08, 0x00,    /* source, destination, version */
    0x28, 0x00,                                /* payload_type */
  };
  CHECK(cast_message_decode(odd, sizeof odd, &m) == 0);
  CHECK_STR(m.source, "s");
  CHECK_STR(m.dest, "d");
  CHECK_STR(m.ns, "n");
  CHECK(m.payload_len == 2 && !memcmp(m.payload, "{}", 2));

  /* A binary payload (device authentication) is told apart. */
  static const unsigned char binary[] = {0x08, 0, 0x12, 1, 's', 0x1a, 1, 'd', 0x22, 1, 'n', 0x28, 1, 0x3a, 2, 0xff, 0x00};
  CHECK(cast_message_decode(binary, sizeof binary, &m) == 0 && m.binary && m.payload_len == 2);

  /* Missing a required field, a length running off the end, a bad varint, a group. */
  CHECK(cast_message_decode(odd, sizeof odd - 2, &m) == -1);
  static const unsigned char overrun[] = {0x08, 0, 0x12, 9, 's'};
  CHECK(cast_message_decode(overrun, sizeof overrun, &m) == -1);
  static const unsigned char varint[] = {0x08, 0x80, 0x80};
  CHECK(cast_message_decode(varint, sizeof varint, &m) == -1);
  static const unsigned char group[] = {0x0b, 0x0c};
  CHECK(cast_message_decode(group, sizeof group, &m) == -1);

  /* Ids longer than kept are cut, not overrun. */
  unsigned char longid[400];
  char id[300];
  memset(id, 'i', sizeof id - 1);
  id[sizeof id - 1] = '\0';
  n = cast_frame_encode(id, "d", "n", "{}", longid, sizeof longid);
  CHECK(n && cast_message_decode(longid + 4, n - 4, &m) == 0 && strlen(m.source) == CAST_ID_LEN - 1);
}

/* --- Messages --- */

static void test_parse(void) {
  cast_reply r;
  const char *receiver =
    "{\"requestId\":2,\"status\":{\"applications\":[{\"appId\":\"CC1AD845\",\"displayName\":\"Default Media "
    "Receiver\",\"namespaces\":[{\"name\":\"urn:x-cast:com.google.cast.media\"}],\"sessionId\":\"7E2FF513\","
    "\"statusText\":\"Ready To Cast\",\"transportId\":\"web-5\"}],\"isActiveInput\":true,\"volume\":{"
    "\"controlType\":\"attenuation\",\"level\":0.35,\"muted\":false,\"stepInterval\":0.05}},\"type\":\"RECEIVER_"
    "STATUS\"}";
  CHECK(cast_reply_parse(receiver, strlen(receiver), &r) == 0);
  CHECK(r.type == CAST_T_RECEIVER_STATUS && r.request_id == 2);
  CHECK(r.receiver.has_volume && r.receiver.level > 0.34 && r.receiver.level < 0.36 && !r.receiver.muted);
  const cast_app *app = cast_receiver_app(&r.receiver, CAST_APP_DEFAULT_MEDIA);
  CHECK(app && !strcmp(app->session_id, "7E2FF513") && !strcmp(app->transport_id, "web-5"));
  CHECK(cast_receiver_app(&r.receiver, "233637DE") == NULL);

  const char *idle = "{\"type\":\"RECEIVER_STATUS\",\"requestId\":0,\"status\":{\"volume\":{\"level\":1,\"muted\":true}}}";
  CHECK(cast_reply_parse(idle, strlen(idle), &r) == 0 && r.receiver.napps == 0 && r.receiver.muted);

  const char *media =
    "{\"type\":\"MEDIA_STATUS\",\"status\":[{\"mediaSessionId\":3,\"playbackRate\":1,\"playerState\":\"PLAYING\","
    "\"currentTime\":12.5,\"supportedMediaCommands\":12303,\"volume\":{\"level\":1,\"muted\":false},"
    "\"currentItemId\":7,\"loadingItemId\":8,\"media\":{\"contentId\":\"http://10.0.0.2:4000/tok/t5.flac\","
    "\"streamType\":\"BUFFERED\",\"contentType\":\"audio/flac\",\"duration\":200.1},\"items\":[{\"itemId\":7,"
    "\"media\":{\"contentId\":\"http://10.0.0.2:4000/tok/t5.flac\"}},{\"itemId\":8}],\"repeatMode\":\"REPEAT_OFF\"}],"
    "\"requestId\":9}";
  CHECK(cast_reply_parse(media, strlen(media), &r) == 0);
  CHECK(r.type == CAST_T_MEDIA_STATUS && r.request_id == 9 && r.media.present);
  CHECK(r.media.media_session_id == 3 && r.media.state == CAST_PLAYER_PLAYING && r.media.idle_reason == CAST_IDLE_NONE);
  CHECK(r.media.has_time && r.media.current_time == 12.5);
  CHECK(r.media.current_item_id == 7 && r.media.loading_item_id == 8 && r.media.preloaded_item_id == 0);
  CHECK_STR(r.media.content_id, "http://10.0.0.2:4000/tok/t5.flac");
  CHECK(r.media.has_items && r.media.nitems == 2 && r.media.items[0].item_id == 7 && r.media.items[1].item_id == 8);
  CHECK_STR(r.media.items[0].content_id, "http://10.0.0.2:4000/tok/t5.flac");
  CHECK_STR(r.media.items[1].content_id, "");

  /* contentUrl when there's no contentId; an idle reason; no queue listed. */
  const char *ended =
    "{\"type\":\"MEDIA_STATUS\",\"requestId\":0,\"status\":[{\"mediaSessionId\":3,\"playerState\":\"IDLE\","
    "\"idleReason\":\"FINISHED\",\"media\":{\"contentUrl\":\"http://x/y\"}}]}";
  CHECK(cast_reply_parse(ended, strlen(ended), &r) == 0);
  CHECK(r.media.state == CAST_PLAYER_IDLE && r.media.idle_reason == CAST_IDLE_FINISHED && !r.media.has_time);
  CHECK_STR(r.media.content_id, "http://x/y");
  CHECK(!r.media.has_items && r.media.current_item_id == 0);

  const char *none = "{\"type\":\"MEDIA_STATUS\",\"status\":[],\"requestId\":4}";
  CHECK(cast_reply_parse(none, strlen(none), &r) == 0 && !r.media.present && r.request_id == 4);

  const char *ids = "{\"type\":\"QUEUE_ITEM_IDS\",\"requestId\":5,\"itemIds\":[1,2,3]}";
  CHECK(cast_reply_parse(ids, strlen(ids), &r) == 0 && r.type == CAST_T_QUEUE_ITEM_IDS && r.nids == 3 && r.ids[2] == 3);

  const char *types[] = {"{\"type\":\"PING\"}", "{\"type\":\"CLOSE\"}", "{\"type\":\"LOAD_FAILED\",\"requestId\":3}",
                         "{\"type\":\"LAUNCH_ERROR\"}", "{\"type\":\"SOMETHING_NEW\"}"};
  cast_type want[] = {CAST_T_PING, CAST_T_CLOSE, CAST_T_LOAD_FAILED, CAST_T_LAUNCH_ERROR, CAST_T_OTHER};
  for (int i = 0; i < 5; i++) CHECK(cast_reply_parse(types[i], strlen(types[i]), &r) == 0 && r.type == want[i]);

  /* Not JSON, not an object, no type. */
  CHECK(cast_reply_parse("{", 1, &r) == -1);
  CHECK(cast_reply_parse("[1]", 3, &r) == -1);
  CHECK(cast_reply_parse("{\"requestId\":1}", 15, &r) == -1);
}

/* The value at key of the JSON root, which must parse. */
static yyjson_val *field(yyjson_doc *doc, const char *key) {
  return yyjson_obj_get(yyjson_doc_get_root(doc), key);
}

static void test_build(void) {
  char *json = cast_json_simple("CONNECT", 0);
  CHECK_STR(json, "{\"type\":\"CONNECT\"}");
  free(json);

  json = cast_json_launch(3, CAST_APP_DEFAULT_MEDIA);
  yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && !strcmp(yyjson_get_str(field(doc, "type")), "LAUNCH") && yyjson_get_int(field(doc, "requestId")) == 3);
  CHECK(doc && !strcmp(yyjson_get_str(field(doc, "appId")), "CC1AD845"));
  yyjson_doc_free(doc);
  free(json);

  json = cast_json_set_volume(4, 1, 1.5, 0, 0); /* clamped */
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  yyjson_val *volume = doc ? field(doc, "volume") : NULL;
  CHECK(volume && yyjson_get_num(yyjson_obj_get(volume, "level")) == 1.0 && !yyjson_obj_get(volume, "muted"));
  yyjson_doc_free(doc);
  free(json);

  json = cast_json_seek(5, 9, 61.25);
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && yyjson_get_int(field(doc, "mediaSessionId")) == 9 && yyjson_get_num(field(doc, "currentTime")) == 61.25);
  yyjson_doc_free(doc);
  free(json);

  json = cast_json_media(6, "GET_STATUS", 0);
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && !field(doc, "mediaSessionId"));
  yyjson_doc_free(doc);
  free(json);

  cast_media_item items[2] = {
    {"http://h/t1.flac", "audio/flac", "Title \"quoted\"", "Artist", "Album", 4, "http://h/a1.jpg", 200.5, 30, 0},
    {"http://h/t2.mp3", "audio/mpeg", "Two", NULL, NULL, 0, NULL, 0, 0, 1},
  };
  json = cast_json_queue_load(7, "sess", items, 2, 20);
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && !strcmp(yyjson_get_str(field(doc, "type")), "QUEUE_LOAD"));
  CHECK(doc && !strcmp(yyjson_get_str(field(doc, "sessionId")), "sess") && yyjson_get_num(field(doc, "currentTime")) == 30);
  yyjson_val *list = doc ? field(doc, "items") : NULL;
  CHECK(yyjson_arr_size(list) == 2);
  yyjson_val *first = yyjson_arr_get(list, 0), *second = yyjson_arr_get(list, 1);
  yyjson_val *media = yyjson_obj_get(first, "media"), *meta = yyjson_obj_get(media, "metadata");
  CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(media, "contentId")), "http://h/t1.flac"));
  CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(media, "contentType")), "audio/flac"));
  CHECK(yyjson_get_num(yyjson_obj_get(media, "duration")) == 200.5);
  CHECK(yyjson_get_int(yyjson_obj_get(meta, "metadataType")) == 3);
  CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(meta, "title")), "Title \"quoted\""));
  CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(meta, "albumName")), "Album"));
  CHECK(yyjson_get_int(yyjson_obj_get(meta, "trackNumber")) == 4);
  CHECK(!strcmp(yyjson_get_str(yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(meta, "images"), 0), "url")), "http://h/a1.jpg"));
  CHECK(yyjson_is_false(yyjson_obj_get(first, "autoplay")) && yyjson_get_num(yyjson_obj_get(first, "startTime")) == 30);
  CHECK(yyjson_get_num(yyjson_obj_get(first, "preloadTime")) == 20);
  yyjson_val *meta2 = yyjson_obj_get(yyjson_obj_get(second, "media"), "metadata");
  CHECK(!yyjson_obj_get(meta2, "artist") && !yyjson_obj_get(meta2, "images") && !yyjson_obj_get(meta2, "trackNumber"));
  CHECK(!yyjson_obj_get(yyjson_obj_get(second, "media"), "duration") && yyjson_is_true(yyjson_obj_get(second, "autoplay")));
  yyjson_doc_free(doc);
  free(json);

  int ids[] = {3, 4};
  json = cast_json_queue_remove(8, 2, ids, 2);
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && yyjson_arr_size(field(doc, "itemIds")) == 2 && yyjson_get_int(yyjson_arr_get(field(doc, "itemIds"), 1)) == 4);
  yyjson_doc_free(doc);
  free(json);

  json = cast_json_queue_insert(9, 2, items + 1, 1, 20);
  doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  CHECK(doc && !strcmp(yyjson_get_str(field(doc, "type")), "QUEUE_INSERT") && yyjson_arr_size(field(doc, "items")) == 1);
  CHECK(doc && !field(doc, "insertBefore")); /* at the end */
  yyjson_doc_free(doc);
  free(json);
}

static void test_txt(void) {
  mc_cast_device d;
  memset(&d, 0, sizeof d);
  cast_browse_txt(&d, "id", 2, "a1b2c3", 6);
  cast_browse_txt(&d, "fn", 2, "Living Room TV", 14);
  cast_browse_txt(&d, "md", 2, "Chromecast", 10);
  cast_browse_txt(&d, "ve", 2, "05", 2);
  cast_browse_txt(&d, "fnx", 3, "no", 2);
  CHECK_STR(d.id, "a1b2c3");
  CHECK_STR(d.name, "Living Room TV");
  CHECK_STR(d.model, "Chromecast");
  char longname[300];
  memset(longname, 'n', sizeof longname);
  cast_browse_txt(&d, "fn", 2, longname, sizeof longname);
  CHECK(strlen(d.name) == MC_CAST_NAME_LEN - 1);
  cast_browse_txt(&d, "md", 2, NULL, 0); /* a key with no value */
  CHECK_STR(d.model, "");
}

/* --- HTTP --- */

static void test_request(void) {
  cast_http_request req;
  const char *get = "GET /tok/t1.flac HTTP/1.1\r\nHost: 10.0.0.2:4000\r\nrange:  bytes=100-\r\nUser-Agent: x\r\n\r\n";
  CHECK(cast_http_parse_request(get, strlen(get), &req) == 1);
  CHECK_STR(req.method, "GET");
  CHECK_STR(req.path, "/tok/t1.flac");
  CHECK(req.has_range);
  CHECK_STR(req.range, "bytes=100-");
  /* Not there yet, until the blank line. */
  CHECK(cast_http_parse_request(get, strlen(get) - 2, &req) == 0);
  CHECK(cast_http_parse_request("GET / HTTP/1.1\r\n", 16, &req) == 0);
  /* Bare newlines, lower-case method, no range. */
  const char *bare = "head /x HTTP/1.0\nHost: h\n\n";
  CHECK(cast_http_parse_request(bare, strlen(bare), &req) == 1 && !strcmp(req.method, "HEAD") && !req.has_range);
  /* Malformed request lines. */
  CHECK(cast_http_parse_request("GARBAGE\r\n\r\n", 11, &req) == -1);
  CHECK(cast_http_parse_request("VERYLONGMETHOD / HTTP/1.1\r\n\r\n", 29, &req) == -1);
  /* Too long without ending. */
  char *flood = malloc(CAST_HTTP_MAX_HEAD + 10);
  if (flood) {
    memset(flood, 'a', CAST_HTTP_MAX_HEAD + 10);
    CHECK(cast_http_parse_request(flood, CAST_HTTP_MAX_HEAD + 10, &req) == -1);
  }
  free(flood);
}

static void test_range(void) {
  int64_t from, to;
  CHECK(cast_http_range("bytes=0-99", 1000, &from, &to) == 1 && from == 0 && to == 99);
  CHECK(cast_http_range("bytes=500-", 1000, &from, &to) == 1 && from == 500 && to == 999);
  CHECK(cast_http_range("bytes=-100", 1000, &from, &to) == 1 && from == 900 && to == 999);
  CHECK(cast_http_range("bytes=-5000", 1000, &from, &to) == 1 && from == 0 && to == 999);
  CHECK(cast_http_range("bytes=900-5000", 1000, &from, &to) == 1 && from == 900 && to == 999); /* cut to the end */
  CHECK(cast_http_range("BYTES=1-2, 5-6", 1000, &from, &to) == 1 && from == 1 && to == 2); /* the first only */
  CHECK(cast_http_range("bytes= 3 - 4 ", 1000, &from, &to) == 1 && from == 3 && to == 4);
  CHECK(cast_http_range("bytes=999-999", 1000, &from, &to) == 1 && from == 999 && to == 999);
  /* Past the end: 416. */
  CHECK(cast_http_range("bytes=1000-", 1000, &from, &to) == -1);
  CHECK(cast_http_range("bytes=2000-3000", 1000, &from, &to) == -1);
  /* Ignored, and the whole file sent. */
  CHECK(cast_http_range("bytes=5-2", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=-0", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=abc-", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=1-x", 1000, &from, &to) == 0);
  CHECK(cast_http_range("items=0-1", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=12", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=-", 1000, &from, &to) == 0);
  CHECK(cast_http_range("bytes=99999999999999999999-", 1000, &from, &to) == 0); /* too many digits */
  CHECK(cast_http_range(NULL, 1000, &from, &to) == 0);
}

static void test_types(void) {
  int wav;
  CHECK_STR(cast_http_content_type("/m/a.flac", &wav), "audio/flac");
  CHECK(!wav);
  CHECK_STR(cast_http_content_type("/m/a.MP3", &wav), "audio/mpeg");
  CHECK_STR(cast_http_content_type("/m/a.opus", &wav), "audio/ogg");
  CHECK_STR(cast_http_content_type("/m/a.wav", &wav), "audio/wav");
  CHECK(!wav);
  CHECK_STR(cast_http_content_type("/m/a.aiff", &wav), "audio/wav");
  CHECK(wav);
  CHECK_STR(cast_http_content_type("C:\\m\\a.Aif", &wav), "audio/wav");
  CHECK(wav);
  CHECK(cast_http_content_type("/m/a.m4a", &wav) == NULL);
  CHECK(cast_http_content_type("/m.flac/noext", &wav) == NULL);

  unsigned char h[CAST_WAV_HEADER];
  cast_http_wav_header(h, 2, 44100, 24, 1000);
  CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
  uint32_t data = (uint32_t)h[40] | (uint32_t)h[41] << 8 | (uint32_t)h[42] << 16 | (uint32_t)h[43] << 24;
  uint32_t riff = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
  CHECK(data == 6000 && riff == 6036);
  CHECK(h[22] == 2 && h[32] == 6 && h[34] == 24);
  CHECK(((uint32_t)h[28] | (uint32_t)h[29] << 8 | (uint32_t)h[30] << 16) == 44100 * 6);
}

/* An AIFF of 16-bit mono at 8 kHz whose sample i is i * 16 (a ramp), so what comes out as
 * 24-bit WAV can be checked: sample i is i * 16 * 256. */
static void write_aiff(const char *path, unsigned frames) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  uint32_t ssnd = 8 + frames * 2, form = 4 + 8 + 18 + 8 + ssnd;
  unsigned char b[64];
  memcpy(b, "FORM", 4);
  b[4] = (unsigned char)(form >> 24), b[5] = (unsigned char)(form >> 16), b[6] = (unsigned char)(form >> 8), b[7] = (unsigned char)form;
  memcpy(b + 8, "AIFFCOMM", 8);
  b[16] = 0, b[17] = 0, b[18] = 0, b[19] = 18;
  b[20] = 0, b[21] = 1;                                           /* channels */
  b[22] = (unsigned char)(frames >> 24), b[23] = (unsigned char)(frames >> 16), b[24] = (unsigned char)(frames >> 8),
  b[25] = (unsigned char)frames;
  b[26] = 0, b[27] = 16;                                          /* bits */
  static const unsigned char rate[10] = {0x40, 0x0b, 0xfa, 0, 0, 0, 0, 0, 0, 0}; /* 8000 as an 80-bit float */
  memcpy(b + 28, rate, 10);
  memcpy(b + 38, "SSND", 4);
  b[42] = (unsigned char)(ssnd >> 24), b[43] = (unsigned char)(ssnd >> 16), b[44] = (unsigned char)(ssnd >> 8), b[45] = (unsigned char)ssnd;
  memset(b + 46, 0, 8); /* offset, block size */
  fwrite(b, 1, 54, f);
  for (unsigned i = 0; i < frames; i++) {
    unsigned v = (i * 16) & 0x7fff;
    fputc((int)(v >> 8), f);
    fputc((int)(v & 0xff), f);
  }
  fclose(f);
}

/* GETs path from the server at base with extra headers; the whole answer, head and body, is
 * put in *out (malloc'd, NUL-terminated) and its length returned, or -1. */
static long fetch(const char *base, const char *method, const char *path, const char *extra, char **out) {
  *out = NULL;
  char host[64];
  unsigned port;
  if (sscanf(base, "http://%63[^:]:%u/", host, &port) != 2) return -1;
  mc_socket *s;
  if (mc_socket_connect(host, (uint16_t)port, 5000, &s) != MC_OK) return -1;
  char req[512];
  snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: %s\r\n%s\r\n", method, path, host, extra ? extra : "");
  long total = 0;
  size_t cap = 1 << 16;
  char *buf = malloc(cap + 1);
  if (buf && mc_socket_send_all(s, req, strlen(req), 5000) == MC_OK) {
    for (;;) {
      if ((size_t)total == cap) {
        char *grown = realloc(buf, cap * 2 + 1);
        if (!grown) break;
        buf = grown;
        cap *= 2;
      }
      long n = mc_socket_recv_some(s, buf + total, cap - (size_t)total, 5000);
      if (n <= 0) break;
      total += n;
    }
    buf[total] = '\0';
  }
  mc_socket_close(s);
  *out = buf;
  return buf ? total : -1;
}

/* The body of an answer: after the blank line. */
static const char *body_of(const char *answer, long len, long *body_len) {
  const char *b = answer ? strstr(answer, "\r\n\r\n") : NULL;
  if (!b) {
    *body_len = 0;
    return "";
  }
  b += 4;
  *body_len = len - (b - answer);
  return b;
}

static void test_server(const char *dir) {
  char path[1100], aiff[1100];
  snprintf(path, sizeof path, "%s/cast_track.flac", dir);
  snprintf(aiff, sizeof aiff, "%s/cast_track.aiff", dir);
  FILE *f = mc_fopen(path, "wb");
  for (int i = 0; f && i < 10000; i++) fputc(i % 251, f);
  if (f) fclose(f);
  write_aiff(aiff, 800);

  cast_server *server = NULL;
  CHECK(cast_server_start("127.0.0.1", &server) == MC_OK);
  if (!server) return;
  const char *base = cast_server_base(server);
  CHECK(!strncmp(base, "http://127.0.0.1:", 17) && strlen(base) > 17 + 32);
  const char *token_path = strchr(base + 7, '/'); /* "/<token>/" */
  char url[256];
  CHECK(cast_server_add_file(server, "t1.flac", path, "audio/flac", 0) == MC_OK);
  CHECK(cast_server_add_file(server, "t/1", path, "audio/flac", 0) == MC_ERR_INVALID_ARG);
  static const unsigned char png[] = {0x89, 'P', 'N', 'G', 1, 2, 3};
  CHECK(cast_server_add_bytes(server, "a1.png", png, sizeof png, "image/png") == MC_OK);
  CHECK(cast_server_add_file(server, "t2.wav", aiff, "audio/wav", 1) == MC_OK);

  char *answer;
  long len, blen;
  const char *body;

  /* The whole file. */
  snprintf(url, sizeof url, "%st1.flac", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  body = body_of(answer, len, &blen);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 200", 12) && strstr(answer, "Content-Length: 10000\r\n"));
  CHECK(answer && strstr(answer, "Content-Type: audio/flac\r\n") && strstr(answer, "Accept-Ranges: bytes\r\n"));
  CHECK(answer && strstr(answer, "Access-Control-Allow-Origin: *\r\n"));
  CHECK(blen == 10000 && (unsigned char)body[300] == 300 % 251);
  free(answer);

  /* A range, as the device seeks. */
  len = fetch(base, "GET", url, "Range: bytes=5000-5009\r\n", &answer);
  body = body_of(answer, len, &blen);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 206", 12) && strstr(answer, "Content-Range: bytes 5000-5009/10000\r\n"));
  CHECK(blen == 10 && (unsigned char)body[0] == 5000 % 251 && (unsigned char)body[9] == 5009 % 251);
  free(answer);
  len = fetch(base, "GET", url, "Range: bytes=-3\r\n", &answer);
  body = body_of(answer, len, &blen);
  CHECK(blen == 3 && (unsigned char)body[2] == 9999 % 251);
  free(answer);

  /* Past the end; HEAD; a query string, which is ignored. */
  len = fetch(base, "GET", url, "Range: bytes=10000-\r\n", &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 416", 12) && strstr(answer, "Content-Range: bytes */10000\r\n"));
  free(answer);
  len = fetch(base, "HEAD", url, NULL, &answer);
  body_of(answer, len, &blen);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 200", 12) && strstr(answer, "Content-Length: 10000\r\n") && blen == 0);
  free(answer);
  snprintf(url, sizeof url, "%st1.flac?x=1", token_path);
  len = fetch(base, "GET", url, "Range: bytes=0-0\r\n", &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 206", 12));
  free(answer);

  /* Only what's put up, and only with the token. */
  len = fetch(base, "GET", "/0123456789abcdef0123456789abcdef/t1.flac", NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 404", 12));
  free(answer);
  snprintf(url, sizeof url, "%st9.flac", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 404", 12));
  free(answer);
  snprintf(url, sizeof url, "%s../t1.flac", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 404", 12));
  free(answer);
  len = fetch(base, "POST", token_path, NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 405", 12));
  free(answer);
  len = fetch(base, "OPTIONS", token_path, NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 204", 12) && strstr(answer, "Access-Control-Allow-Headers: Range\r\n"));
  free(answer);

  /* The cover. */
  snprintf(url, sizeof url, "%sa1.png", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  body = body_of(answer, len, &blen);
  CHECK(answer && strstr(answer, "Content-Type: image/png\r\n") && blen == (long)sizeof png && !memcmp(body, png, sizeof png));
  free(answer);

  /* The AIFF, as 24-bit WAV: 800 frames, 3 bytes each, after the 44-byte header. */
  snprintf(url, sizeof url, "%st2.wav", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  body = body_of(answer, len, &blen);
  CHECK(answer && strstr(answer, "Content-Type: audio/wav\r\n") && strstr(answer, "Content-Length: 2444\r\n"));
  CHECK(blen == 2444 && !memcmp(body, "RIFF", 4));
  if (blen == 2444) {
    const unsigned char *pcm = (const unsigned char *)body + 44;
    uint32_t s100 = pcm[300] | (uint32_t)pcm[301] << 8 | (uint32_t)pcm[302] << 16;
    CHECK(s100 == 100 * 16 * 256);
  }
  free(answer);
  /* ...and a range of it starting mid-sample, which is cut as from the whole. */
  len = fetch(base, "GET", url, "Range: bytes=345-350\r\n", &answer);
  body = body_of(answer, len, &blen);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 206", 12) && strstr(answer, "Content-Range: bytes 345-350/2444\r\n"));
  if (blen == 6) {
    /* Byte 345 is sample 100's second byte (44 + 300 + 1). */
    uint32_t s = 100 * 16 * 256;
    const unsigned char *b = (const unsigned char *)body;
    CHECK(b[0] == (unsigned char)(s >> 8) && b[1] == (unsigned char)(s >> 16));
    uint32_t s101 = b[2] | (uint32_t)b[3] << 8 | (uint32_t)b[4] << 16;
    CHECK(s101 == 101 * 16 * 256);
  } else {
    CHECK(blen == 6);
  }
  free(answer);
  len = fetch(base, "GET", url, "Range: bytes=0-9\r\n", &answer);
  body = body_of(answer, len, &blen);
  CHECK(blen == 10 && !memcmp(body, "RIFF", 4));
  free(answer);

  /* Taken down. */
  const char *keep[] = {"a1.png"};
  cast_server_keep(server, keep, 1);
  snprintf(url, sizeof url, "%st1.flac", token_path);
  len = fetch(base, "GET", url, NULL, &answer);
  CHECK(answer && !strncmp(answer, "HTTP/1.1 404", 12));
  free(answer);

  /* Stopping with a connection open that hasn't sent anything doesn't wait for it. */
  char host[64];
  unsigned port;
  mc_socket *idle = NULL;
  if (sscanf(base, "http://%63[^:]:%u/", host, &port) == 2) mc_socket_connect(host, (uint16_t)port, 5000, &idle);
  mc_sleep_ms(50);
  uint64_t t0 = mc_mono_ms();
  cast_server_stop(server);
  CHECK(mc_mono_ms() - t0 < 2000);
  mc_socket_close(idle);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  test_frame();
  test_parse();
  test_build();
  test_txt();
  test_request();
  test_range();
  test_types();
  test_server(dir);
  return TEST_RESULT();
}
