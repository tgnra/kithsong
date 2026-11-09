/* Finding Google Cast devices over mDNS, from a fake device's announcements the test sends
 * itself with mdns.h: it's listed with its id, name, model, port and address (from its A
 * record), renamed, kept while discovery is stopped, and gone once it says goodbye. Skipped
 * (exit 77) when multicast isn't delivered here. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* the socket API mdns.h uses */
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "kithcore/kithcore.h"
#include "mdns.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77
#define INSTANCE "Chromecast-0123abcd._googlecast._tcp.local."
#define HOST "0123abcd-0000-1111.local."

static mc_mutex lock;
static int device_events;

static void on_cast(void *user, mc_cast_event event) {
  (void)user;
  mc_mutex_lock(&lock);
  if (event == MC_CAST_EVENT_DEVICES) device_events++;
  mc_mutex_unlock(&lock);
}

static mdns_string_t str(const char *s) {
  mdns_string_t m;
  m.str = s;
  m.length = strlen(s);
  return m;
}

static mdns_record_t txt(const char *key, const char *value) {
  mdns_record_t r;
  memset(&r, 0, sizeof r);
  r.name = str(INSTANCE);
  r.type = MDNS_RECORDTYPE_TXT;
  r.data.txt.key = str(key);
  r.data.txt.value = str(value);
  return r;
}

/* Announces the fake device as a Chromecast does (or says goodbye), on the default interface
 * and on loopback. 1 if it went out on either. */
static int announce(const char *name, int goodbye) {
  mdns_record_t ptr, extra[5];
  memset(&ptr, 0, sizeof ptr);
  ptr.name = str("_googlecast._tcp.local.");
  ptr.type = MDNS_RECORDTYPE_PTR;
  ptr.data.ptr.name = str(INSTANCE);
  memset(extra, 0, sizeof extra);
  extra[0].name = str(INSTANCE);
  extra[0].type = MDNS_RECORDTYPE_SRV;
  extra[0].data.srv.port = 8009;
  extra[0].data.srv.name = str(HOST);
  extra[1].name = str(HOST);
  extra[1].type = MDNS_RECORDTYPE_A;
  extra[1].data.a.addr.sin_family = AF_INET;
  extra[1].data.a.addr.sin_addr.s_addr = htonl(0x7f000001);
  extra[2] = txt("id", "0123abcdef");
  extra[3] = txt("fn", name);
  extra[4] = txt("md", "Chromecast");
  struct sockaddr_in loopback;
  memset(&loopback, 0, sizeof loopback);
  loopback.sin_family = AF_INET;
  loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int socks[2] = {mdns_socket_open_ipv4(NULL), mdns_socket_open_ipv4(&loopback)}, sent = 0;
  uint32_t buf[512];
  for (int i = 0; i < 2; i++) {
    if (socks[i] < 0) continue;
    int r = goodbye ? mdns_goodbye_multicast(socks[i], buf, sizeof buf, ptr, NULL, 0, extra, 5)
                    : mdns_announce_multicast(socks[i], buf, sizeof buf, ptr, NULL, 0, extra, 5);
    if (r == 0) sent = 1;
    mdns_socket_close(socks[i]);
  }
  return sent;
}

/* Waits up to ms for the list to have count devices, the first named name (if not NULL);
 * copies it to *out. */
static int wait_devices(mc_cast *cast, int count, const char *name, unsigned ms, mc_cast_device *out) {
  uint64_t end = mc_mono_ms() + ms;
  do {
    mc_cast_device_list *list = NULL;
    if (mc_cast_devices(cast, &list) == MC_OK) {
      int ok = list->count == count && (!name || !strcmp(list->items[0].name, name));
      if (ok && count && out) *out = list->items[0];
      mc_cast_device_list_free(list);
      if (ok) return 1;
    }
    mc_sleep_ms(20);
  } while (mc_mono_ms() < end);
  return 0;
}

int main(void) {
  mc_mutex_init(&lock);
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  mc_cast *cast = NULL;
  CHECK(mc_cast_new(on_cast, NULL, &cast) == MC_OK);
  mc_status st = mc_cast_start_discovery(cast);
  if (st == MC_ERR_NET) {
    printf("skipped: can't open mDNS sockets\n");
    mc_cast_free(cast);
    return test_failures ? 1 : SKIP;
  }
  CHECK(st == MC_OK);
  CHECK(mc_cast_start_discovery(cast) == MC_OK); /* again: a no-op */
  mc_sleep_ms(200); /* its sockets are open */

  mc_cast_device d;
  int seen = 0;
  for (int tries = 0; tries < 3 && !seen; tries++) {
    CHECK(announce("Living Room TV", 0));
    seen = wait_devices(cast, 1, "Living Room TV", 1000, &d);
  }
  if (!seen) {
    printf("skipped: the announcements weren't heard; multicast is probably filtered here\n");
    mc_cast_free(cast);
    return test_failures ? 1 : SKIP;
  }
  CHECK_STR(d.id, "0123abcdef");
  CHECK_STR(d.model, "Chromecast");
  CHECK_STR(d.host, "127.0.0.1");
  CHECK(d.port == 8009);
  mc_mutex_lock(&lock);
  CHECK(device_events >= 1);
  mc_mutex_unlock(&lock);

  /* Renamed: the same device, under its new name. */
  CHECK(announce("Kitchen", 0));
  CHECK(wait_devices(cast, 1, "Kitchen", 2000, &d));

  /* Stopped, the list stays, for connecting from a picker that has closed. */
  mc_cast_stop_discovery(cast);
  CHECK(wait_devices(cast, 1, "Kitchen", 0, NULL));

  /* Goodbye: gone. */
  CHECK(mc_cast_start_discovery(cast) == MC_OK);
  mc_sleep_ms(200);
  CHECK(announce("Kitchen", 0));
  CHECK(wait_devices(cast, 1, "Kitchen", 2000, NULL));
  mc_sleep_ms(3000); /* past the first report, which keeps it, having heard it */
  CHECK(wait_devices(cast, 1, "Kitchen", 0, NULL));
  int gone = 0;
  for (int tries = 0; tries < 3 && !gone; tries++) {
    CHECK(announce("Kitchen", 1));
    gone = wait_devices(cast, 0, NULL, 1000, NULL);
  }
  CHECK(gone);

  mc_cast_free(cast);
  return TEST_RESULT();
}
