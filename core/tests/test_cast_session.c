/* A whole cast, against a fake Cast device on loopback: a TLS server (with a self-signed
 * certificate made here) that speaks enough of the protocol, as the Default Media Receiver
 * does. Connecting starts the receiver and hands the player over; playing loads the current
 * track with the next one queued, which the device fetches from the music server; pausing on
 * the device shows in the player and resuming in the player reaches the device; the volume;
 * the device going on to the queued track moves the player on and the queue is put right;
 * seeking; the connection dropping and being got back; disconnecting stops the receiver and
 * hands back. Then another app casting over it, which
 * ends the cast and leaves the receiver alone; a cast left paused, which ends and stops
 * (with the timeout cut short); and freeing the player. No audio device is needed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include "cast/cast_test.h"
#include "cast/castv2.h"
#include "kithcore/kithcore.h"
#include "net/socket.h"
#include "test.h"
#include "util/platform.h"
#include "yyjson.h"

/* --- The fake device --- */

typedef struct fake {
  mc_thread thread;
  mc_listener *listener;
  uint16_t port;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_pk_context key;
  mbedtls_x509_crt cert;
  mbedtls_ssl_config conf;

  mc_mutex lock;
  /* Under lock. */
  int quit;
  int launched;          /* the Default Media Receiver runs */
  char log[64][64];      /* the type of each message received, "ns-tail:TYPE" */
  int nlog;
  char load[8192];       /* the last QUEUE_LOAD */
  int items[8];          /* the queue: item ids */
  char urls[8][256];
  int nitems, next_item_id, current;
  const char *state;     /* "PLAYING", "PAUSED", "IDLE" */
  double level;
  int push;              /* 1: send a status nobody asked for; 2: go on to the next item; 3: someone else's
                          * media; 4: drop the connection */
  /* The connection's thread only. */
  mc_socket *sock;
  mbedtls_ssl_context ssl;
  unsigned char buf[4 + CAST_MAX_MESSAGE];
  size_t len;
} fake;

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  fake *f = ctx;
  return mc_socket_send_all(f->sock, buf, len, 5000) == MC_OK ? (int)len : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len, uint32_t timeout) {
  fake *f = ctx;
  long n = mc_socket_recv_some(f->sock, buf, len, timeout ? (int)timeout : 5000);
  return n > 0 ? (int)n : n == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

static void fake_send(fake *f, const char *source, const char *ns, const char *json) {
  unsigned char out[4 + 16384];
  size_t n = cast_frame_encode(source, "sender-0", ns, json, out, sizeof out);
  for (size_t done = 0; n && done < n;) {
    int w = mbedtls_ssl_write(&f->ssl, out + done, n - done);
    if (w <= 0) return;
    done += (size_t)w;
  }
}

static void receiver_status(fake *f, long request) {
  char json[1024];
  mc_mutex_lock(&f->lock);
  if (f->launched)
    snprintf(json, sizeof json,
             "{\"type\":\"RECEIVER_STATUS\",\"requestId\":%ld,\"status\":{\"applications\":[{\"appId\":\"CC1AD845\","
             "\"sessionId\":\"sess-1\",\"transportId\":\"web-1\"}],\"volume\":{\"level\":%.2f,\"muted\":false}}}",
             request, f->level);
  else
    snprintf(json, sizeof json, "{\"type\":\"RECEIVER_STATUS\",\"requestId\":%ld,\"status\":{\"volume\":{\"level\":%.2f,\"muted\":false}}}",
             request, f->level);
  mc_mutex_unlock(&f->lock);
  fake_send(f, "receiver-0", CAST_NS_RECEIVER, json);
}

/* A MEDIA_STATUS: with the queue's item ids if with_items, someone else's media if foreign,
 * and without the current item's media if bare, as receivers leave it out once they've
 * said it. */
static void media_status_as(fake *f, long request, int with_items, const char *foreign, int bare) {
  char json[4096], items[2048] = "";
  mc_mutex_lock(&f->lock);
  if (!f->nitems && !foreign) {
    mc_mutex_unlock(&f->lock);
    snprintf(json, sizeof json, "{\"type\":\"MEDIA_STATUS\",\"requestId\":%ld,\"status\":[]}", request);
    fake_send(f, "web-1", CAST_NS_MEDIA, json);
    return;
  }
  size_t n = 0;
  for (int i = 0; with_items && i < f->nitems; i++)
    n += (size_t)snprintf(items + n, sizeof items - n, "%s{\"itemId\":%d}", i ? "," : "", f->items[i]);
  const char *url = foreign ? foreign : f->urls[f->current];
  snprintf(json, sizeof json,
           "{\"type\":\"MEDIA_STATUS\",\"requestId\":%ld,\"status\":[{\"mediaSessionId\":1,\"playerState\":\"%s\","
           "\"currentTime\":0.5,\"currentItemId\":%d%s%s%s%s%s%s}]}",
           request, f->state, foreign ? 99 : f->items[f->current], bare ? "" : ",\"media\":{\"contentId\":\"",
           bare ? "" : url, bare ? "" : "\"}", with_items ? ",\"items\":[" : "", items, with_items ? "]" : "");
  mc_mutex_unlock(&f->lock);
  fake_send(f, "web-1", CAST_NS_MEDIA, json);
}

static void media_status(fake *f, long request, int with_items, const char *foreign) {
  media_status_as(f, request, with_items, foreign, 0);
}

static void note(fake *f, const char *ns, const char *type) {
  mc_mutex_lock(&f->lock);
  if (f->nlog < 64) snprintf(f->log[f->nlog++], 64, "%s:%s", strrchr(ns, '.') + 1, type);
  mc_mutex_unlock(&f->lock);
}

/* Adds the items of a QUEUE_LOAD or QUEUE_INSERT to the queue. */
static void add_items(fake *f, yyjson_val *items) {
  size_t i, max;
  yyjson_val *item;
  yyjson_arr_foreach(items, i, max, item) {
    if (f->nitems == 8) break;
    const char *url = yyjson_get_str(yyjson_obj_get(yyjson_obj_get(item, "media"), "contentId"));
    snprintf(f->urls[f->nitems], sizeof f->urls[0], "%s", url ? url : "");
    f->items[f->nitems++] = ++f->next_item_id;
  }
}

static void handle(fake *f, const cast_message *m) {
  yyjson_doc *doc = yyjson_read((const char *)m->payload, m->payload_len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  const char *type = yyjson_get_str(yyjson_obj_get(root, "type"));
  long request = (long)yyjson_get_int(yyjson_obj_get(root, "requestId"));
  if (!type) type = "?";
  note(f, m->ns, type);
  if (!strcmp(m->ns, CAST_NS_HEARTBEAT) && !strcmp(type, "PING")) {
    fake_send(f, "receiver-0", CAST_NS_HEARTBEAT, "{\"type\":\"PONG\"}");
  } else if (!strcmp(m->ns, CAST_NS_RECEIVER)) {
    mc_mutex_lock(&f->lock);
    if (!strcmp(type, "LAUNCH")) f->launched = 1;
    if (!strcmp(type, "STOP")) f->launched = 0, f->nitems = 0;
    if (!strcmp(type, "SET_VOLUME")) {
      yyjson_val *level = yyjson_obj_get(yyjson_obj_get(root, "volume"), "level");
      if (level) f->level = yyjson_get_num(level);
    }
    mc_mutex_unlock(&f->lock);
    receiver_status(f, request);
  } else if (!strcmp(m->ns, CAST_NS_MEDIA)) {
    int with_items = 0;
    mc_mutex_lock(&f->lock);
    if (!strcmp(type, "QUEUE_LOAD")) {
      snprintf(f->load, sizeof f->load, "%.*s", (int)m->payload_len, (const char *)m->payload);
      f->nitems = f->current = 0;
      add_items(f, yyjson_obj_get(root, "items"));
      f->state = yyjson_is_true(yyjson_obj_get(yyjson_arr_get_first(yyjson_obj_get(root, "items")), "autoplay")) ? "PLAYING" : "PAUSED";
      with_items = 1;
    } else if (!strcmp(type, "QUEUE_INSERT")) {
      add_items(f, yyjson_obj_get(root, "items"));
      with_items = 1;
    } else if (!strcmp(type, "QUEUE_REMOVE")) {
      size_t i, max;
      yyjson_val *id;
      yyjson_arr_foreach(yyjson_obj_get(root, "itemIds"), i, max, id) {
        for (int k = 0; k < f->nitems; k++) {
          if (f->items[k] != (int)yyjson_get_int(id)) continue;
          memmove(f->items + k, f->items + k + 1, (size_t)(f->nitems - k - 1) * sizeof f->items[0]);
          memmove(f->urls + k, f->urls + k + 1, (size_t)(f->nitems - k - 1) * sizeof f->urls[0]);
          if (f->current > k) f->current--;
          f->nitems--;
          break;
        }
      }
      with_items = 1;
    } else if (!strcmp(type, "PLAY")) {
      f->state = "PLAYING";
    } else if (!strcmp(type, "PAUSE")) {
      f->state = "PAUSED";
    }
    mc_mutex_unlock(&f->lock);
    media_status(f, request, with_items, NULL);
  }
  yyjson_doc_free(doc);
}

/* One connection: the handshake, then messages until it closes or the fake is told to quit. */
static void serve(fake *f) {
  mbedtls_ssl_init(&f->ssl);
  f->len = 0;
  if (mbedtls_ssl_setup(&f->ssl, &f->conf) != 0) return;
  mbedtls_ssl_set_bio(&f->ssl, f, bio_send, NULL, bio_recv);
  int rc;
  while ((rc = mbedtls_ssl_handshake(&f->ssl)) != 0)
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE && rc != MBEDTLS_ERR_SSL_TIMEOUT) goto done;
  for (;;) {
    mc_mutex_lock(&f->lock);
    int quit = f->quit, push = f->push;
    f->push = 0;
    if (push == 2 && f->current + 1 < f->nitems) f->current++;
    mc_mutex_unlock(&f->lock);
    if (quit || push == 4) break; /* 4: drop the connection */
    if (push == 1) media_status(f, 0, 0, NULL);
    if (push == 2) media_status_as(f, 0, 0, NULL, 1); /* the item id alone says it moved on */
    if (push == 3) media_status(f, 0, 0, "http://elsewhere/video.mp4");
    int n = mbedtls_ssl_read(&f->ssl, f->buf + f->len, sizeof f->buf - f->len);
    if (n == MBEDTLS_ERR_SSL_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ) continue;
    if (n <= 0) break;
    f->len += (size_t)n;
    while (f->len >= 4 && f->len >= 4 + cast_frame_length(f->buf)) {
      uint32_t len = cast_frame_length(f->buf);
      cast_message m;
      if (cast_message_decode(f->buf + 4, len, &m) == 0) handle(f, &m);
      memmove(f->buf, f->buf + 4 + len, f->len - 4 - len);
      f->len -= 4 + len;
    }
  }
done:
  mbedtls_ssl_free(&f->ssl);
}

static void fake_run(void *arg) {
  fake *f = arg;
  for (;;) {
    mc_socket *s = NULL;
    mc_listener_accept(f->listener, MC_SOCKET_FOREVER, &s);
    if (!s) break;
    f->sock = s;
    serve(f);
    mc_socket_close(s);
    f->sock = NULL;
  }
}

static int fake_start(fake *f) {
  memset(f, 0, sizeof *f);
  mc_mutex_init(&f->lock);
  f->state = "IDLE";
  f->level = 0.5;
  psa_crypto_init();
  mbedtls_entropy_init(&f->entropy);
  mbedtls_ctr_drbg_init(&f->drbg);
  mbedtls_pk_init(&f->key);
  mbedtls_x509_crt_init(&f->cert);
  mbedtls_ssl_config_init(&f->conf);
  if (mbedtls_ctr_drbg_seed(&f->drbg, mbedtls_entropy_func, &f->entropy, NULL, 0) != 0) return 0;
  /* A self-signed certificate, as a Cast device has. */
  if (mbedtls_pk_setup(&f->key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(f->key), mbedtls_ctr_drbg_random, &f->drbg) != 0)
    return 0;
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  mbedtls_x509write_crt_set_subject_key(&crt, &f->key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &f->key);
  mbedtls_x509write_crt_set_subject_name(&crt, "CN=Fake Cast");
  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=Fake Cast");
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char *)"\x01", 1);
  mbedtls_x509write_crt_set_validity(&crt, "20200101000000", "20400101000000");
  unsigned char der[2048];
  int len = mbedtls_x509write_crt_der(&crt, der, sizeof der, mbedtls_ctr_drbg_random, &f->drbg);
  mbedtls_x509write_crt_free(&crt);
  if (len <= 0 || mbedtls_x509_crt_parse_der(&f->cert, der + sizeof der - len, (size_t)len) != 0) return 0;
  if (mbedtls_ssl_config_defaults(&f->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    return 0;
  mbedtls_ssl_conf_rng(&f->conf, mbedtls_ctr_drbg_random, &f->drbg);
  mbedtls_ssl_conf_max_tls_version(&f->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_conf_read_timeout(&f->conf, 20);
  if (mbedtls_ssl_conf_own_cert(&f->conf, &f->cert, &f->key) != 0) return 0;
  if (mc_listener_open_on("127.0.0.1", 0, &f->listener) != MC_OK) return 0;
  f->port = mc_listener_port(f->listener);
  return mc_thread_start(&f->thread, fake_run, f) == MC_OK;
}

static void fake_stop(fake *f) {
  mc_mutex_lock(&f->lock);
  f->quit = 1;
  mc_mutex_unlock(&f->lock);
  mc_listener_stop(f->listener);
  mc_thread_join(&f->thread);
  mc_listener_close(f->listener);
  mbedtls_ssl_config_free(&f->conf);
  mbedtls_x509_crt_free(&f->cert);
  mbedtls_pk_free(&f->key);
  mbedtls_ctr_drbg_free(&f->drbg);
  mbedtls_entropy_free(&f->entropy);
}

/* Whether the fake has received a message "ns-tail:TYPE" (e.g. "media:PLAY") since mark;
 * waits up to two seconds. */
static int received(fake *f, int mark, const char *what) {
  uint64_t end = mc_mono_ms() + 2000;
  do {
    mc_mutex_lock(&f->lock);
    for (int i = mark; i < f->nlog; i++)
      if (!strcmp(f->log[i], what)) {
        mc_mutex_unlock(&f->lock);
        return 1;
      }
    mc_mutex_unlock(&f->lock);
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  fprintf(stderr, "the device never got %s\n", what);
  return 0;
}

static int log_mark(fake *f) {
  mc_mutex_lock(&f->lock);
  int n = f->nlog;
  mc_mutex_unlock(&f->lock);
  return n;
}

static void push(fake *f, int what) {
  mc_mutex_lock(&f->lock);
  f->push = what;
  mc_mutex_unlock(&f->lock);
}

/* --- The test --- */

static const char *str_of(yyjson_val *v) {
  const char *s = yyjson_get_str(v);
  return s ? s : "";
}

static int wait_cast(mc_cast *cast, mc_cast_state state, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  mc_cast_status st;
  do {
    mc_cast_status_get(cast, &st);
    if (st.state == state) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  fprintf(stderr, "cast state %d, wanted %d\n", (int)st.state, (int)state);
  return 0;
}

static int wait_player(mc_player *player, mc_player_state state, int index) {
  uint64_t end = mc_mono_ms() + 2000;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    if (st.state == state && st.index == index) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  fprintf(stderr, "player state %d index %d, wanted %d %d\n", (int)st.state, st.index, (int)state, index);
  return 0;
}

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

static void write_wav(const char *path, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  uint32_t bytes = 8000 * ms / 1000 * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, 8000);
  put32(f, 16000);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < bytes; i++) fputc(0, f);
  fclose(f);
}

/* The status line and length of GETting url. */
static int http_get(const char *url, char *status, size_t cap, long *length) {
  char host[64];
  unsigned port;
  char path[256];
  if (sscanf(url, "http://%63[^:]:%u%255s", host, &port, path) != 3) return 0;
  mc_socket *s;
  if (mc_socket_connect(host, (uint16_t)port, 5000, &s) != MC_OK) return 0;
  char req[512];
  snprintf(req, sizeof req, "HEAD %s HTTP/1.1\r\nHost: %s\r\n\r\n", path, host);
  char buf[2048];
  long n = 0;
  if (mc_socket_send_all(s, req, strlen(req), 5000) == MC_OK)
    for (long got; n < (long)sizeof buf - 1 && (got = mc_socket_recv_some(s, buf + n, sizeof buf - 1 - (size_t)n, 5000)) > 0;)
      n += got;
  mc_socket_close(s);
  buf[n] = '\0';
  const char *eol = strstr(buf, "\r\n");
  if (!eol) return 0;
  snprintf(status, cap, "%.*s", (int)(eol - buf), buf);
  const char *cl = strstr(buf, "Content-Length: ");
  *length = cl ? atol(cl + 16) : -1;
  return 1;
}

static mc_mutex events_lock;
static int state_events, volume_events;

static void on_cast(void *user, mc_cast_event event) {
  (void)user;
  mc_mutex_lock(&events_lock);
  if (event == MC_CAST_EVENT_STATE) state_events++;
  if (event == MC_CAST_EVENT_VOLUME) volume_events++;
  mc_mutex_unlock(&events_lock);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  mc_mutex_init(&events_lock);
  char a[1100], b[1100], c[1100];
  snprintf(a, sizeof a, "%s/cast_a.wav", dir);
  snprintf(b, sizeof b, "%s/cast_b.wav", dir);
  snprintf(c, sizeof c, "%s/cast_c.wav", dir);
  write_wav(a, 4000);
  write_wav(b, 4000);
  write_wav(c, 4000);
  const char *paths[] = {a, b, c};

  fake f;
  if (!fake_start(&f)) {
    fprintf(stderr, "couldn't start the fake device\n");
    return 1;
  }
  mc_cast *cast = NULL;
  CHECK(mc_cast_new(on_cast, NULL, &cast) == MC_OK);
  mc_cast_device device;
  memset(&device, 0, sizeof device);
  strcpy(device.id, "fake-id");
  strcpy(device.name, "Fake TV");
  strcpy(device.host, "127.0.0.1");
  device.port = f.port;
  mc_cast_test_set_devices(cast, &device, 1);
  mc_cast_device_list *list = NULL;
  CHECK(mc_cast_devices(cast, &list) == MC_OK && list && list->count == 1 && !strcmp(list->items[0].name, "Fake TV"));
  mc_cast_device_list_free(list);

  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);
  CHECK(mc_player_load_queue(player, paths, 3, 0, NULL) == MC_OK);
  CHECK(mc_cast_connect(cast, "nope", player) == MC_ERR_NOT_FOUND);

  /* Connecting starts the receiver and joins it. */
  CHECK(mc_cast_connect(cast, "fake-id", player) == MC_OK);
  mc_cast_status st;
  mc_cast_status_get(cast, &st);
  CHECK(st.state == MC_CAST_CONNECTING && !strcmp(st.device_name, "Fake TV") && !strcmp(st.device_id, "fake-id"));
  CHECK(wait_cast(cast, MC_CAST_CASTING, 5000));
  CHECK(received(&f, 0, "connection:CONNECT") && received(&f, 0, "receiver:GET_STATUS"));
  CHECK(received(&f, 0, "receiver:LAUNCH") && received(&f, 0, "media:GET_STATUS"));
  mc_cast_status_get(cast, &st);
  CHECK(st.volume == 50 && !st.muted && mc_cast_is_casting(cast));

  /* Playing loads the current track and queues the next, served by the music server. */
  int mark = log_mark(&f);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(received(&f, mark, "media:QUEUE_LOAD"));
  mc_mutex_lock(&f.lock);
  yyjson_doc *doc = yyjson_read(f.load, strlen(f.load), 0);
  int nitems = f.nitems;
  char url0[256];
  snprintf(url0, sizeof url0, "%s", f.urls[0]);
  mc_mutex_unlock(&f.lock);
  yyjson_val *items = yyjson_obj_get(yyjson_doc_get_root(doc), "items");
  yyjson_val *media0 = yyjson_obj_get(yyjson_arr_get(items, 0), "media");
  CHECK(nitems == 2 && yyjson_arr_size(items) == 2);
  CHECK(!strcmp(str_of(yyjson_obj_get(media0, "contentType")), "audio/wav"));
  CHECK(!strcmp(str_of(yyjson_obj_get(yyjson_obj_get(media0, "metadata"), "title")), "cast_a"));
  CHECK(yyjson_get_num(yyjson_obj_get(media0, "duration")) == 4.0);
  CHECK(!strcmp(str_of(yyjson_obj_get(yyjson_doc_get_root(doc), "sessionId")), "sess-1"));
  yyjson_doc_free(doc);
  char status[128];
  long length = 0;
  CHECK(http_get(url0, status, sizeof status, &length) && !strcmp(status, "HTTP/1.1 200 OK") && length == 44 + 64000);
  CHECK(wait_player(player, MC_PLAYER_PLAYING, 0));

  /* Paused on the device (another phone, the TV's remote): the player follows. */
  mc_mutex_lock(&f.lock);
  f.state = "PAUSED";
  mc_mutex_unlock(&f.lock);
  push(&f, 1);
  CHECK(wait_player(player, MC_PLAYER_PAUSED, 0));
  mark = log_mark(&f);
  CHECK(mc_player_resume(player) == MC_OK);
  CHECK(received(&f, mark, "media:PLAY"));
  CHECK(wait_player(player, MC_PLAYER_PLAYING, 0));

  /* The volume. */
  mark = log_mark(&f);
  CHECK(mc_cast_set_volume(cast, 40) == MC_OK);
  mc_cast_status_get(cast, &st);
  CHECK(st.volume == 40);
  CHECK(received(&f, mark, "receiver:SET_VOLUME"));
  mc_sleep_ms(100);
  mc_mutex_lock(&f.lock);
  CHECK(f.level > 0.39 && f.level < 0.41);
  mc_mutex_unlock(&f.lock);

  /* The device goes on to the queued track: so does the player, and the device's queue becomes
   * that track and the one after it, the one played going. */
  mark = log_mark(&f);
  push(&f, 2);
  CHECK(wait_player(player, MC_PLAYER_PLAYING, 1));
  CHECK(received(&f, mark, "media:QUEUE_REMOVE") && received(&f, mark, "media:QUEUE_INSERT"));
  mc_sleep_ms(100);
  mc_mutex_lock(&f.lock);
  CHECK(f.nitems == 2 && f.current == 0 && strstr(f.urls[1], "/t3.wav"));
  mc_mutex_unlock(&f.lock);

  /* Seeking in the player seeks on the device. */
  mark = log_mark(&f);
  CHECK(mc_player_seek_ms(player, 1000) == MC_OK);
  CHECK(received(&f, mark, "media:SEEK"));

  /* The connection drops: it's got back, joining the receiver rather than starting it again,
   * and the cast goes on where it was. */
  mark = log_mark(&f);
  push(&f, 4);
  CHECK(wait_cast(cast, MC_CAST_CONNECTING, 3000));
  CHECK(wait_cast(cast, MC_CAST_CASTING, 5000));
  CHECK(received(&f, mark, "media:GET_STATUS"));
  mc_mutex_lock(&f.lock);
  for (int i = mark; i < f.nlog; i++) CHECK(strcmp(f.log[i], "receiver:LAUNCH") != 0 && strcmp(f.log[i], "media:QUEUE_LOAD") != 0);
  mc_mutex_unlock(&f.lock);
  CHECK(wait_player(player, MC_PLAYER_PLAYING, 1));

  /* Disconnecting stops the receiver and hands back, paused, on the track the device was on. */
  mark = log_mark(&f);
  mc_cast_disconnect(cast);
  CHECK(wait_cast(cast, MC_CAST_IDLE, 5000));
  CHECK(received(&f, mark, "receiver:STOP") && received(&f, mark, "connection:CLOSE"));
  mc_cast_status_get(cast, &st);
  CHECK(st.ended == MC_CAST_END_NONE && !st.device_name[0]);
  mc_player_status pst;
  mc_player_status_get(player, &pst);
  CHECK(pst.index == 1 && pst.state != MC_PLAYER_PLAYING);
  mc_mutex_lock(&events_lock);
  CHECK(state_events >= 3 && volume_events >= 1);
  mc_mutex_unlock(&events_lock);

  /* Another app casts over it: the cast ends and leaves its receiver alone. */
  CHECK(mc_player_set_queue(player, paths, 3, 0, NULL) == MC_OK); /* playing here, if there's a device */
  mark = log_mark(&f);
  CHECK(mc_cast_connect(cast, "fake-id", player) == MC_OK);
  CHECK(wait_cast(cast, MC_CAST_CASTING, 5000));
  mc_player_resume(player);
  CHECK(wait_player(player, MC_PLAYER_PLAYING, 0));
  CHECK(received(&f, mark, "media:QUEUE_LOAD"));
  mc_sleep_ms(100); /* its answer */
  mark = log_mark(&f);
  push(&f, 3);
  CHECK(wait_cast(cast, MC_CAST_IDLE, 5000));
  mc_cast_status_get(cast, &st);
  CHECK(st.ended == MC_CAST_END_DISPLACED);
  mc_sleep_ms(100);
  mc_mutex_lock(&f.lock);
  for (int i = mark; i < f.nlog; i++) CHECK(strcmp(f.log[i], "receiver:STOP") != 0);
  mc_mutex_unlock(&f.lock);
  mc_player_status_get(player, &pst);
  CHECK(pst.state != MC_PLAYER_PLAYING);

  /* Left paused, the cast ends by itself, and playback stops. */
  mc_cast_test_paused_ms = 300;
  CHECK(mc_cast_connect(cast, "fake-id", player) == MC_OK);
  CHECK(wait_cast(cast, MC_CAST_CASTING, 5000));
  CHECK(wait_cast(cast, MC_CAST_IDLE, 3000));
  mc_cast_status_get(cast, &st);
  CHECK(st.ended == MC_CAST_END_PAUSED);
  CHECK(wait_player(player, MC_PLAYER_STOPPED, 0));

  /* Freeing the player while casting ends the cast. */
  mc_cast_test_paused_ms = MC_CAST_PAUSED_MS;
  CHECK(mc_cast_connect(cast, "fake-id", player) == MC_OK);
  CHECK(wait_cast(cast, MC_CAST_CASTING, 5000));
  mc_player_free(player);
  CHECK(wait_cast(cast, MC_CAST_IDLE, 3000));
  mc_cast_status_get(cast, &st);
  CHECK(st.ended == MC_CAST_END_PLAYER);

  mc_cast_free(cast);
  fake_stop(&f);
  return TEST_RESULT();
}
