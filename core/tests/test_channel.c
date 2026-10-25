#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "net/socket.h"
#include "sync/channel.h"
#include "sync/frame.h"
#include "sync/pair.h"
#include "sync/sync_db.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 10000

static void remove_db(const char *dir) {
  const char *const names[] = {MC_SYNC_DB_NAME, MC_SYNC_DB_NAME "-wal", MC_SYNC_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

/* --- The server side, on its own thread --- */

typedef enum job { JOB_KK, JOB_PAIR, JOB_ACCEPT_ONLY } job;

typedef struct server {
  mc_listener *l;
  mc_sync_db *sdb;
  job job;
  const unsigned char (*peers)[MC_NOISE_KEY_LEN];
  size_t n_peers;
  int refuse; /* the accept callback says no */
  mc_pair_responder *pair;
  /* results */
  mc_status accept_st, st;
  int got_channel, mode, new_code, echoed;
  unsigned char peer[MC_NOISE_KEY_LEN];
  mc_thread thread;
} server;

static int accept_key(void *user, const unsigned char key[MC_NOISE_KEY_LEN]) {
  server *s = user;
  (void)key;
  return !s->refuse;
}

/* Echoes every message back until BYE or an error. */
static void echo(server *s, mc_channel *ch) {
  for (;;) {
    uint8_t type;
    const unsigned char *payload;
    size_t len;
    if (mc_channel_recv(ch, &type, &payload, &len) != MC_OK || type == MC_MSG_BYE) return;
    if (mc_channel_send(ch, type, payload, len) != MC_OK) return;
    s->echoed++;
  }
}

static void serve(void *arg) {
  server *s = arg;
  mc_channel *ch = NULL;
  s->accept_st = mc_channel_accept(s->l, WAIT_MS, &ch);
  s->got_channel = ch != NULL;
  if (!ch) return;
  s->mode = mc_channel_mode(ch);
  const unsigned char *priv = mc_sync_db_private_key(s->sdb);
  const unsigned char *pub = mc_sync_db_public_key(s->sdb);
  if (s->job == JOB_KK) {
    s->st = mc_channel_handshake_kk(ch, priv, pub, s->peers, s->n_peers, accept_key, s);
    if (s->st == MC_OK) memcpy(s->peer, mc_channel_peer_key(ch), MC_NOISE_KEY_LEN);
  } else if (s->job == JOB_PAIR) {
    s->st = mc_pair_respond(s->pair, ch, priv, pub, s->peer, &s->new_code);
  }
  if (s->st == MC_OK && s->job != JOB_ACCEPT_ONLY) echo(s, ch);
  mc_channel_close(ch);
}

static void start(server *s, job j) {
  s->job = j;
  s->st = MC_ERR_UNIMPLEMENTED;
  s->accept_st = MC_ERR_UNIMPLEMENTED;
  s->got_channel = s->mode = s->new_code = s->echoed = 0;
  memset(s->peer, 0, sizeof s->peer);
  CHECK(mc_thread_start(&s->thread, serve, s) == MC_OK);
}

/* --- Client helpers --- */

static mc_channel *dial(server *s, int mode) {
  mc_channel *ch = NULL;
  CHECK(mc_channel_connect("127.0.0.1", mc_listener_port(s->l), mode, WAIT_MS, &ch) == MC_OK);
  return ch;
}

static mc_status kk(mc_channel *ch, mc_sync_db *me, const unsigned char peer[MC_NOISE_KEY_LEN]) {
  return mc_channel_handshake_kk(ch, mc_sync_db_private_key(me), mc_sync_db_public_key(me),
                                 (const unsigned char (*)[MC_NOISE_KEY_LEN])peer, 1, NULL, NULL);
}

/* Sends a message and checks it comes back unchanged. */
static int round_trip(mc_channel *ch, uint8_t type, const unsigned char *data, size_t len) {
  uint8_t got_type = 0;
  const unsigned char *got;
  size_t got_len = 0;
  if (mc_channel_send(ch, type, data, len) != MC_OK) return 0;
  if (mc_channel_recv(ch, &got_type, &got, &got_len) != MC_OK) return 0;
  return got_type == type && got_len == len && (len == 0 || memcmp(got, data, len) == 0);
}

static void bye(mc_channel *ch) {
  mc_channel_send(ch, MC_MSG_BYE, "{}", 2);
}

/* A code that isn't this one. */
static void wrong_code(const char *code, char out[MC_PAIR_CODE_DIGITS + 1]) {
  memcpy(out, code, MC_PAIR_CODE_DIGITS + 1);
  out[0] = (char)('0' + (out[0] - '0' + 1) % 10);
}

/* --- Tests --- */

static void test_listener_stop(server *s) {
  /* A blocked accept returns as soon as the listener is stopped from another thread. */
  mc_listener *l = NULL;
  CHECK(mc_listener_open(0, &l) == MC_OK);
  if (!l) return;
  server t = *s;
  t.l = l;
  t.job = JOB_ACCEPT_ONLY;
  t.accept_st = MC_ERR_UNIMPLEMENTED;
  t.got_channel = 1;
  mc_thread th;
  CHECK(mc_thread_start(&th, serve, &t) == MC_OK);
  mc_sleep_ms(100);
  uint64_t t0 = mc_mono_ms();
  mc_listener_stop(l);
  mc_thread_join(&th);
  CHECK(mc_mono_ms() - t0 < WAIT_MS / 2);
  CHECK(t.accept_st == MC_OK && !t.got_channel);
  CHECK(mc_listener_stopped(l));
  mc_listener_close(l);
}

static void test_frames(server *s) {
  /* A raw frame over the wire, and a bad preamble refused by accept. */
  start(s, JOB_ACCEPT_ONLY);
  mc_socket *sock = NULL;
  CHECK(mc_socket_connect("127.0.0.1", mc_listener_port(s->l), WAIT_MS, &sock) == MC_OK);
  if (sock) {
    CHECK(mc_socket_send_all(sock, "MA\x07\x02", 4, WAIT_MS) == MC_OK); /* unknown version */
    mc_thread_join(&s->thread);
    CHECK(s->accept_st == MC_ERR_FORMAT && !s->got_channel);
    unsigned char b;
    CHECK(mc_socket_recv_exact(sock, &b, 1, WAIT_MS) == MC_ERR_NET); /* closed on us */
    mc_socket_close(sock);
  } else {
    mc_thread_join(&s->thread);
  }
  CHECK(mc_frame_send(NULL, "", 0, 0) == MC_ERR_INVALID_ARG);

  /* Nobody listening: connect fails within its timeout. */
  mc_listener *l = NULL;
  CHECK(mc_listener_open(0, &l) == MC_OK);
  uint16_t port = l ? mc_listener_port(l) : 1;
  mc_listener_close(l);
  CHECK(mc_socket_connect("127.0.0.1", port, 2000, &sock) == MC_ERR_NET && sock == NULL);
}

static void test_kk(server *s, mc_sync_db *a, mc_sync_db *b, mc_sync_db *c) {
  unsigned char known[2][MC_NOISE_KEY_LEN];
  memcpy(known[0], mc_sync_db_public_key(c), MC_NOISE_KEY_LEN); /* tried first, doesn't match */
  memcpy(known[1], mc_sync_db_public_key(a), MC_NOISE_KEY_LEN);
  s->peers = (const unsigned char (*)[MC_NOISE_KEY_LEN])known;
  s->n_peers = 2;
  s->refuse = 0;

  /* A calls B: both learn the other's key; messages up to the largest go both ways. */
  start(s, JOB_KK);
  mc_channel *ch = dial(s, MC_MODE_SESSION);
  if (ch) {
    CHECK(kk(ch, a, mc_sync_db_public_key(b)) == MC_OK);
    CHECK(mc_channel_is_encrypted(ch));
    CHECK(memcmp(mc_channel_peer_key(ch), mc_sync_db_public_key(b), MC_NOISE_KEY_LEN) == 0);
    unsigned char *big = malloc(MC_CHANNEL_MAX_PAYLOAD + 1);
    if (big) {
      for (size_t i = 0; i <= MC_CHANNEL_MAX_PAYLOAD; i++) big[i] = (unsigned char)(i * 7);
      CHECK(round_trip(ch, MC_MSG_ROWS, big, MC_CHANNEL_MAX_PAYLOAD));
      CHECK(round_trip(ch, MC_MSG_FILE_DATA, big, MC_CHANNEL_MAX_PAYLOAD - 1));
      CHECK(mc_channel_send(ch, MC_MSG_ROWS, big, MC_CHANNEL_MAX_PAYLOAD + 1) ==
            MC_ERR_INVALID_ARG);
      free(big);
    }
    CHECK(round_trip(ch, MC_MSG_HELLO, (const unsigned char *)"{\"v\":1}", 7));
    CHECK(round_trip(ch, MC_MSG_VV, NULL, 0));
    /* Clear text is refused once encrypted. */
    CHECK(mc_channel_send_clear(ch, MC_MSG_PAIR_HELLO, "{}", 2) == MC_ERR_INVALID_ARG);
    bye(ch);
    mc_channel_close(ch);
  }
  mc_thread_join(&s->thread);
  CHECK(s->mode == MC_MODE_SESSION);
  CHECK(s->st == MC_OK && s->echoed == 4);
  CHECK(memcmp(s->peer, mc_sync_db_public_key(a), MC_NOISE_KEY_LEN) == 0);

  /* Over IPv6 too, where the host has it. */
  start(s, JOB_KK);
  ch = NULL;
  if (mc_channel_connect("::1", mc_listener_port(s->l), MC_MODE_SESSION, 2000, &ch) == MC_OK) {
    CHECK(kk(ch, a, mc_sync_db_public_key(b)) == MC_OK);
    CHECK(round_trip(ch, MC_MSG_HELLO, (const unsigned char *)"v6", 2));
    bye(ch);
    mc_channel_close(ch);
  } else {
    fprintf(stderr, "test_channel: no IPv6 loopback, skipping that case\n");
    /* Release the server, which is waiting in accept. */
    ch = dial(s, MC_MODE_SESSION);
    if (ch) {
      kk(ch, a, mc_sync_db_public_key(b));
      bye(ch);
      mc_channel_close(ch);
    }
  }
  mc_thread_join(&s->thread);
  CHECK(s->st == MC_OK);

  /* C isn't among B's keys: B refuses, and C's handshake fails too. */
  start(s, JOB_KK);
  s->n_peers = 1;
  s->peers = (const unsigned char (*)[MC_NOISE_KEY_LEN])known[1];
  ch = dial(s, MC_MODE_SESSION);
  if (ch) {
    CHECK(kk(ch, c, mc_sync_db_public_key(b)) != MC_OK);
    CHECK(mc_channel_send(ch, MC_MSG_HELLO, "{}", 2) == MC_ERR_INVALID_ARG);
    mc_channel_close(ch);
  }
  mc_thread_join(&s->thread);
  CHECK(s->st == MC_ERR_NOT_FOUND && s->echoed == 0);

  /* A is known, but the accept callback says no (say, A was removed from the group). */
  start(s, JOB_KK);
  s->refuse = 1;
  ch = dial(s, MC_MODE_SESSION);
  if (ch) {
    CHECK(kk(ch, a, mc_sync_db_public_key(b)) != MC_OK);
    mc_channel_close(ch);
  }
  mc_thread_join(&s->thread);
  CHECK(s->st == MC_ERR_NOT_FOUND);
  s->refuse = 0;

  /* A expects someone else's key at this address: A's handshake fails. */
  start(s, JOB_KK);
  ch = dial(s, MC_MODE_SESSION);
  if (ch) {
    CHECK(kk(ch, a, mc_sync_db_public_key(c)) != MC_OK);
    mc_channel_close(ch);
  }
  mc_thread_join(&s->thread);
  CHECK(s->st != MC_OK);
}

/* One pairing attempt: A dials B and enters code. Returns A's status, after B is done. */
static mc_status pair_once(server *s, mc_sync_db *a, const char *code, int *confirmed) {
  start(s, JOB_PAIR);
  mc_channel *ch = dial(s, MC_MODE_PAIRING);
  mc_status st = MC_ERR_NET;
  unsigned char peer[MC_NOISE_KEY_LEN];
  *confirmed = 0;
  if (ch) {
    st = mc_pair_initiate(ch, code, mc_sync_db_private_key(a), mc_sync_db_public_key(a), peer);
    /* B's first encrypted message confirms the code; here, its echo. */
    if (st == MC_OK) {
      *confirmed = round_trip(ch, MC_MSG_PAIR_ACCEPT, (const unsigned char *)"{}", 2);
      if (*confirmed) CHECK(memcmp(peer, mc_channel_peer_key(ch), MC_NOISE_KEY_LEN) == 0);
      bye(ch);
    }
    mc_channel_close(ch);
  }
  mc_thread_join(&s->thread);
  CHECK(s->mode == MC_MODE_PAIRING);
  return st;
}

static void test_pairing(server *s, mc_sync_db *a, mc_sync_db *b) {
  mc_pair_responder r;
  CHECK(mc_pair_responder_init(&r) == MC_OK);
  s->pair = &r;
  int confirmed;

  /* The right code: both learn each other's key. */
  CHECK(pair_once(s, a, r.code, &confirmed) == MC_OK);
  CHECK(confirmed);
  CHECK(s->st == MC_OK && !s->new_code && r.failures == 0);
  CHECK(memcmp(s->peer, mc_sync_db_public_key(a), MC_NOISE_KEY_LEN) == 0);
  (void)b;

  /* A wrong code: B fails, and A's next receive does too. The code stays for two more
   * tries; the third failure draws a new one. */
  char code[MC_PAIR_CODE_DIGITS + 1], wrong[MC_PAIR_CODE_DIGITS + 1];
  memcpy(code, r.code, sizeof code);
  wrong_code(code, wrong);
  for (int attempt = 1; attempt <= MC_PAIR_MAX_FAILURES; attempt++) {
    pair_once(s, a, wrong, &confirmed);
    CHECK(!confirmed);
    CHECK(s->st == MC_ERR_FORMAT);
    if (attempt < MC_PAIR_MAX_FAILURES) {
      CHECK(!s->new_code && r.failures == attempt && strcmp(r.code, code) == 0);
    } else {
      CHECK(s->new_code && r.failures == 0);
    }
  }
  /* The new code works. (It equals the old one with probability 10^-6; that's fine.) */
  CHECK(pair_once(s, a, r.code, &confirmed) == MC_OK && confirmed && s->st == MC_OK);

  /* A success resets the count. */
  wrong_code(r.code, wrong);
  pair_once(s, a, wrong, &confirmed);
  CHECK(r.failures == 1);
  CHECK(pair_once(s, a, r.code, &confirmed) == MC_OK && confirmed && r.failures == 0);

  /* Codes are six digits; malformed ones are refused before touching the wire. */
  for (int i = 0; i < 1000; i++) {
    char c[MC_PAIR_CODE_DIGITS + 1];
    CHECK(mc_pair_new_code(c) == MC_OK);
    int ok = strlen(c) == MC_PAIR_CODE_DIGITS;
    for (int k = 0; k < MC_PAIR_CODE_DIGITS; k++) ok &= c[k] >= '0' && c[k] <= '9';
    CHECK(ok);
  }
  mc_channel *ch = dial(s, MC_MODE_PAIRING); /* nobody accepts; refused before any I/O */
  unsigned char peer[MC_NOISE_KEY_LEN];
  if (ch) {
    CHECK(mc_pair_initiate(ch, "12345", mc_sync_db_private_key(a), mc_sync_db_public_key(a),
                           peer) == MC_ERR_INVALID_ARG);
    CHECK(mc_pair_initiate(ch, "12345x", mc_sync_db_private_key(a), mc_sync_db_public_key(a),
                           peer) == MC_ERR_INVALID_ARG);
    mc_channel_close(ch);
  }
  /* Drain that connection from the listener's backlog. */
  start(s, JOB_ACCEPT_ONLY);
  mc_thread_join(&s->thread);
  mc_pair_responder_wipe(&r);
  s->pair = NULL;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  char dir_a[1024], dir_b[1024], dir_c[1024];
  snprintf(dir_a, sizeof dir_a, "%s/test_channel_a.d", scratch);
  snprintf(dir_b, sizeof dir_b, "%s/test_channel_b.d", scratch);
  snprintf(dir_c, sizeof dir_c, "%s/test_channel_c.d", scratch);
  remove_db(dir_a);
  remove_db(dir_b);
  remove_db(dir_c);

  mc_sync_db *a = NULL, *b = NULL, *c = NULL;
  CHECK(mc_sync_db_open(dir_a, &a) == MC_OK);
  CHECK(mc_sync_db_open(dir_b, &b) == MC_OK);
  CHECK(mc_sync_db_open(dir_c, &c) == MC_OK);
  if (!a || !b || !c) return TEST_RESULT();

  server s;
  memset(&s, 0, sizeof s);
  s.sdb = b;
  CHECK(mc_listener_open(0, &s.l) == MC_OK);
  if (s.l) {
    CHECK(mc_listener_port(s.l) != 0);
    test_listener_stop(&s);
    test_frames(&s);
    test_kk(&s, a, b, c);
    test_pairing(&s, a, b);
    mc_listener_close(s.l);
  }

  mc_sync_db_close(a);
  mc_sync_db_close(b);
  mc_sync_db_close(c);
  return TEST_RESULT();
}
