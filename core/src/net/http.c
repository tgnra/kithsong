#include "net/http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

#include "config.h"

extern const unsigned char mc_cacert_pem[];
extern const size_t mc_cacert_pem_len;

void mc_buffer_free(mc_buffer *buf) {
  free(buf->data);
  buf->data = NULL;
  buf->len = 0;
}

char *mc_url_escape(const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  char *out = malloc(strlen(s) * 3 + 1);
  if (!out) return NULL;
  char *p = out;
  for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
    if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
        *c == '-' || *c == '_' || *c == '.' || *c == '~') {
      *p++ = (char)*c;
    } else {
      *p++ = '%';
      *p++ = hex[*c >> 4];
      *p++ = hex[*c & 15];
    }
  }
  *p = '\0';
  return out;
}

static size_t on_body(char *ptr, size_t size, size_t nmemb, void *user) {
  mc_buffer *buf = user;
  size_t n = size * nmemb;
  char *grown = realloc(buf->data, buf->len + n + 1);
  if (!grown) return 0; /* makes curl abort with CURLE_WRITE_ERROR */
  memcpy(grown + buf->len, ptr, n);
  buf->data = grown;
  buf->len += n;
  buf->data[buf->len] = '\0';
  return n;
}

/* Curl's progress callback: non-zero makes it stop with CURLE_ABORTED_BY_CALLBACK. */
static int on_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow) {
  (void)dltotal, (void)dlnow, (void)ultotal, (void)ulnow;
  const mc_http_cancel *cancel = user;
  return cancel->cancelled(cancel->user) ? 1 : 0;
}

/* GET url, or POST form when it isn't NULL. */
static mc_status perform(const char *url, const char *form, mc_buffer *body, long *http_status,
                         const mc_http_cancel *cancel) {
  if (!url || !body) return MC_ERR_INVALID_ARG;
  body->data = NULL;
  body->len = 0;

  CURL *curl = curl_easy_init();
  if (!curl) return MC_ERR_NOMEM;

  struct curl_blob ca = {(void *)mc_cacert_pem, mc_cacert_pem_len, CURL_BLOB_NOCOPY};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Kithsong/" MC_VERSION_STRING " ( " MC_CONTACT " )");
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  /* TLS 1.3 handshakes fail with curl 8.11.1 + Mbed TLS 3.6.2 ("SSL - Internal error",
   * -0x6C00) against every host tried, with or without psa_crypto_init(). Cap at 1.2 until
   * the pinned versions move on, then remove this and re-run manual_mb_ping. */
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, (long)(CURL_SSLVERSION_TLSv1_2 | CURL_SSLVERSION_MAX_TLSv1_2));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  /* A name lookup still running when the call times out or is cancelled is left to finish
   * on curl's resolver thread rather than waited for: otherwise a network with broken DNS
   * holds every failed call until the lookup itself gives up, whatever the timeouts say. */
  curl_easy_setopt(curl, CURLOPT_QUICK_EXIT, 1L);
  if (cancel && cancel->cancelled) {
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  }
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  if (form) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form); /* form-urlencoded by default */

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK && http_status) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
  curl_easy_cleanup(curl);

  if (rc == CURLE_ABORTED_BY_CALLBACK) { /* cancelled */
    mc_buffer_free(body);
    return MC_ERR_NET;
  }
  if (rc != CURLE_OK) {
    fprintf(stderr, "musiccore: %s %s failed: %s\n", form ? "POST" : "GET", url,
            curl_easy_strerror(rc));
    mc_buffer_free(body);
    return rc == CURLE_WRITE_ERROR ? MC_ERR_NOMEM : MC_ERR_NET;
  }
  return MC_OK;
}

mc_status mc_http_get(const char *url, mc_buffer *body, long *http_status) {
  return perform(url, NULL, body, http_status, NULL);
}

mc_status mc_http_get_cancel(const char *url, mc_buffer *body, long *http_status,
                             const mc_http_cancel *cancel) {
  return perform(url, NULL, body, http_status, cancel);
}

mc_status mc_http_post_form(const char *url, const char *form, mc_buffer *body, long *http_status,
                            const mc_http_cancel *cancel) {
  if (!form) return MC_ERR_INVALID_ARG;
  return perform(url, form, body, http_status, cancel);
}

void mc_ratelimit_init(mc_ratelimit *rl, uint32_t interval_ms) {
  mc_mutex_init(&rl->lock);
  rl->next_ms = 0;
  rl->interval_ms = interval_ms;
}

/* Callers reserve consecutive slots under the lock, then sleep outside it. */
void mc_ratelimit_wait(mc_ratelimit *rl) {
  mc_mutex_lock(&rl->lock);
  uint64_t now = mc_mono_ms();
  uint64_t slot = rl->next_ms > now ? rl->next_ms : now;
  rl->next_ms = slot + rl->interval_ms;
  mc_mutex_unlock(&rl->lock);

  if (slot > now) mc_sleep_ms((uint32_t)(slot - now));
}

int mc_ratelimit_wait_cancel(mc_ratelimit *rl, const mc_http_cancel *cancel) {
  if (!cancel || !cancel->cancelled) {
    mc_ratelimit_wait(rl);
    return 0;
  }
  mc_mutex_lock(&rl->lock);
  uint64_t now = mc_mono_ms();
  uint64_t slot = rl->next_ms > now ? rl->next_ms : now;
  rl->next_ms = slot + rl->interval_ms;
  mc_mutex_unlock(&rl->lock);

  for (;;) {
    if (cancel->cancelled(cancel->user)) return 1;
    now = mc_mono_ms();
    if (now >= slot) return 0;
    mc_sleep_ms(slot - now < 100 ? (uint32_t)(slot - now) : 100);
  }
}
