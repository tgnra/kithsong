#include "sync/reach.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "net/socket.h"

/* A waits this long for the answer: B's call back, and some slack. */
#define REPLY_MS (MC_REACH_CONNECT_MS + 5000)
/* B waits this long for the request, which follows the preamble at once. */
#define REQUEST_MS 5000

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* Sends doc's root in the clear and frees doc. */
static mc_status send_doc(mc_channel *ch, uint8_t type, yyjson_mut_doc *doc) {
  size_t len = 0;
  char *json = doc ? yyjson_mut_write(doc, 0, &len) : NULL;
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = mc_channel_send_clear(ch, type, json, len);
  free(json);
  return st;
}

/* The next clear message, of type want, as a JSON object; NULL if it isn't one. */
static yyjson_doc *recv_doc(mc_channel *ch, uint8_t want) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  if (mc_channel_recv_clear(ch, &type, &payload, &len) != MC_OK || type != want) return NULL;
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
    yyjson_doc_free(doc);
    return NULL;
  }
  return doc;
}

void mc_reach_host_of(const char *addr, char host[MC_SYNC_ADDR_MAX]) {
  host[0] = '\0';
  if (!addr) return;
  const char *start = addr, *end;
  if (addr[0] == '[') {
    start = addr + 1;
    end = strchr(start, ']');
  } else {
    end = strrchr(addr, ':');
  }
  if (!end || end == start || (size_t)(end - start) >= MC_SYNC_ADDR_MAX) return;
  memcpy(host, start, (size_t)(end - start));
  host[end - start] = '\0';
}

mc_status mc_reach_ask(mc_channel *ch, const char *self_uid, uint16_t self_port,
                       mc_reach_answer *out) {
  memset(out, 0, sizeof *out);
  yyjson_mut_doc *req = yyjson_mut_doc_new(NULL);
  if (!req) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(req);
  yyjson_mut_doc_set_root(req, root);
  yyjson_mut_obj_add_str(req, root, "dev", self_uid);
  yyjson_mut_obj_add_uint(req, root, "port", self_port);
  mc_status st = send_doc(ch, MC_MSG_REACH_REQ, req);
  if (st != MC_OK) return st;
  mc_channel_set_timeout(ch, REPLY_MS);
  yyjson_doc *doc = recv_doc(ch, MC_MSG_REACH_RESP);
  if (!doc) return MC_ERR_FORMAT; /* an older version closed it */
  yyjson_val *obj = yyjson_doc_get_root(doc);
  const char *dev = yyjson_get_str(yyjson_obj_get(obj, "dev"));
  yyjson_val *ok = yyjson_obj_get(obj, "ok");
  if (is_uid(dev) && yyjson_is_bool(ok)) {
    out->answered = 1;
    out->ok = yyjson_get_bool(ok);
    memcpy(out->uid, dev, MC_UID_STRLEN);
  } else {
    st = MC_ERR_FORMAT;
  }
  yyjson_doc_free(doc);
  return st;
}

mc_status mc_reach_serve(mc_channel *ch, const char *self_uid, char claimed[MC_UID_STRLEN],
                         char host[MC_SYNC_ADDR_MAX], int *ok) {
  claimed[0] = '\0';
  *ok = 0;
  mc_reach_host_of(mc_channel_peer_addr(ch), host);
  mc_channel_set_timeout(ch, REQUEST_MS);
  yyjson_doc *doc = recv_doc(ch, MC_MSG_REACH_REQ);
  if (!doc) return MC_ERR_FORMAT;
  yyjson_val *obj = yyjson_doc_get_root(doc);
  const char *dev = yyjson_get_str(yyjson_obj_get(obj, "dev"));
  yyjson_val *port = yyjson_obj_get(obj, "port");
  uint64_t p = yyjson_is_uint(port) ? yyjson_get_uint(port) : 0;
  if (is_uid(dev)) memcpy(claimed, dev, MC_UID_STRLEN);
  yyjson_doc_free(doc);
  /* Only ever back to where the request came from, so this can't be pointed elsewhere. */
  if (!host[0] || p == 0 || p > 65535) return MC_ERR_FORMAT;
  mc_socket *back = NULL;
  *ok = mc_socket_connect(host, (uint16_t)p, MC_REACH_CONNECT_MS, &back) == MC_OK;
  mc_socket_close(back);

  yyjson_mut_doc *resp = yyjson_mut_doc_new(NULL);
  if (!resp) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(resp);
  yyjson_mut_doc_set_root(resp, root);
  yyjson_mut_obj_add_str(resp, root, "dev", self_uid);
  yyjson_mut_obj_add_bool(resp, root, "ok", *ok);
  return send_doc(ch, MC_MSG_REACH_RESP, resp);
}
