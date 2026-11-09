/* TLS to a Cast device: see cast_tls.h. */
#include "cast/cast_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> /* its error codes, for the I/O callbacks */
#include <mbedtls/ssl.h>
#include <psa/crypto.h>

#include "net/socket.h"
#include "util/platform.h"

struct cast_tls {
  mc_socket *sock;
  int timeout_ms; /* for the read callback: the read under way's */
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
};

static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  cast_tls *t = ctx;
  /* All of it or nothing, which Mbed TLS takes as all of it. */
  return mc_socket_send_all(t->sock, buf, len, t->timeout_ms) == MC_OK ? (int)len : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms) {
  cast_tls *t = ctx;
  long n = mc_socket_recv_some(t->sock, buf, len, timeout_ms ? (int)timeout_ms : MC_SOCKET_FOREVER);
  if (n > 0) return (int)n;
  return n == 0 ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
}

mc_status cast_tls_connect(const char *host, unsigned port, int timeout_ms, cast_tls **out) {
  if (!host || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  cast_tls *t = calloc(1, sizeof *t);
  if (!t) return MC_ERR_NOMEM;
  uint64_t deadline = mc_mono_ms() + (uint64_t)timeout_ms;
  mbedtls_entropy_init(&t->entropy);
  mbedtls_ctr_drbg_init(&t->drbg);
  mbedtls_ssl_config_init(&t->conf);
  mbedtls_ssl_init(&t->ssl);
  /* Needed by Mbed TLS 3.6 before any handshake, since TLS 1.3 is built in. Repeat calls
   * are no-ops. */
  psa_crypto_init();

  mc_status st = mc_socket_connect(host, (uint16_t)port, timeout_ms, &t->sock);
  static const char pers[] = "kithsong-cast";
  int rc = 0;
  if (st == MC_OK &&
      ((rc = mbedtls_ctr_drbg_seed(&t->drbg, mbedtls_entropy_func, &t->entropy, (const unsigned char *)pers,
                                   sizeof pers - 1)) != 0 ||
       (rc = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0))
    st = MC_ERR_NET;
  if (st == MC_OK) {
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE); /* self-signed: see cast_tls.h */
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->drbg);
    /* As http.c: TLS 1.3 handshakes fail with Mbed TLS 3.6.2 here, and Cast devices all speak
     * 1.2. */
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_read_timeout(&t->conf, (uint32_t)timeout_ms);
    if ((rc = mbedtls_ssl_setup(&t->ssl, &t->conf)) != 0) st = MC_ERR_NET;
  }
  if (st == MC_OK) {
    t->timeout_ms = timeout_ms;
    mbedtls_ssl_set_bio(&t->ssl, t, bio_send, NULL, bio_recv_timeout);
    while ((rc = mbedtls_ssl_handshake(&t->ssl)) != 0) {
      int again = rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE || rc == MBEDTLS_ERR_SSL_TIMEOUT;
      if (!again || mc_mono_ms() >= deadline) {
        st = MC_ERR_NET;
        break;
      }
    }
  }
  if (st != MC_OK) {
    if (rc) {
      char why[128];
      mbedtls_strerror(rc, why, sizeof why);
      fprintf(stderr, "kithcore: TLS to Cast device %s failed: %s\n", host, why);
    }
    cast_tls_close(t);
    return st;
  }
  *out = t;
  return MC_OK;
}

mc_status cast_tls_write(cast_tls *t, const void *buf, size_t len, int timeout_ms) {
  const unsigned char *p = buf;
  t->timeout_ms = timeout_ms;
  while (len) {
    int n = mbedtls_ssl_write(&t->ssl, p, len);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
    } else if (n != MBEDTLS_ERR_SSL_WANT_WRITE && n != MBEDTLS_ERR_SSL_WANT_READ) {
      return MC_ERR_NET;
    }
  }
  return MC_OK;
}

long cast_tls_read(cast_tls *t, void *buf, size_t len, int timeout_ms) {
  t->timeout_ms = timeout_ms;
  mbedtls_ssl_conf_read_timeout(&t->conf, (uint32_t)(timeout_ms > 0 ? timeout_ms : 1));
  for (;;) {
    int n = mbedtls_ssl_read(&t->ssl, buf, len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_TIMEOUT) return 0;
    /* A TLS 1.3 ticket or a renegotiation attempt: nothing for us yet. */
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    return -1; /* 0 is the peer's close_notify */
  }
}

mc_status cast_tls_read_exact(cast_tls *t, void *buf, size_t len, int timeout_ms) {
  unsigned char *p = buf;
  while (len) {
    long n = cast_tls_read(t, p, len, timeout_ms);
    if (n <= 0) return MC_ERR_NET; /* a timeout here is a peer gone quiet mid-message */
    p += n;
    len -= (size_t)n;
  }
  return MC_OK;
}

mc_status cast_tls_local_address(const cast_tls *t, char *out, size_t cap) {
  return mc_socket_local_address(t->sock, out, cap);
}

void cast_tls_shutdown(cast_tls *t) {
  if (t) mc_socket_shutdown(t->sock);
}

void cast_tls_close(cast_tls *t) {
  if (!t) return;
  if (t->sock) {
    mbedtls_ssl_close_notify(&t->ssl);
    mc_socket_close(t->sock);
  }
  mbedtls_ssl_free(&t->ssl);
  mbedtls_ssl_config_free(&t->conf);
  mbedtls_ctr_drbg_free(&t->drbg);
  mbedtls_entropy_free(&t->entropy);
  free(t);
}
