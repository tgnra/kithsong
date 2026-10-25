#include "net/http.h"

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

mc_status mc_http_get(const char *url, mc_buffer *body, long *http_status) {
  if (!url || !body) return MC_ERR_INVALID_ARG;
  body->data = NULL;
  body->len = 0;

  CURL *curl = curl_easy_init();
  if (!curl) return MC_ERR_NOMEM;

  struct curl_blob ca = {(void *)mc_cacert_pem, mc_cacert_pem_len, CURL_BLOB_NOCOPY};
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "musicapp/" MC_VERSION_STRING " ( " MC_CONTACT " )");
  curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK && http_status) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    mc_buffer_free(body);
    return rc == CURLE_WRITE_ERROR ? MC_ERR_NOMEM : MC_ERR_NET;
  }
  return MC_OK;
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
