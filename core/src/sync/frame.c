#include "sync/frame.h"

mc_status mc_frame_send(mc_socket *s, const void *data, size_t len, int timeout_ms) {
  if (!s || len > MC_FRAME_MAX || (!data && len)) return MC_ERR_INVALID_ARG;
  unsigned char hdr[2] = {(unsigned char)(len >> 8), (unsigned char)len};
  mc_status st = mc_socket_send_all(s, hdr, sizeof hdr, timeout_ms);
  if (st == MC_OK && len) st = mc_socket_send_all(s, data, len, timeout_ms);
  return st;
}

mc_status mc_frame_recv(mc_socket *s, void *buf, size_t cap, size_t *len, int timeout_ms) {
  if (!s || !len || (!buf && cap)) return MC_ERR_INVALID_ARG;
  *len = 0;
  unsigned char hdr[2];
  mc_status st = mc_socket_recv_exact(s, hdr, sizeof hdr, timeout_ms);
  if (st != MC_OK) return st;
  size_t n = (size_t)hdr[0] << 8 | hdr[1];
  if (n > cap) return MC_ERR_FORMAT;
  if (n && (st = mc_socket_recv_exact(s, buf, n, timeout_ms)) != MC_OK) return st;
  *len = n;
  return MC_OK;
}
