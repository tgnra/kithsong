/* The sockets a test's fake mDNS peer or device sends from: one on each interface address
 * that discovery listens on (mc_msocks_list_addresses()), each sending out of its own
 * interface, as a peer on that link would.
 *
 * Not a socket left on the default multicast interface: Windows doesn't choose that by
 * metric, and once any process has sent to 224.0.0.251 out of loopback, it stays on loopback
 * (seen on GitHub's Windows runners), which discovery doesn't listen on on Windows. The tests
 * used to send from loopback themselves, so they passed only on a runner's first run. */
#ifndef KITH_TEST_FAKE_MDNS_H
#define KITH_TEST_FAKE_MDNS_H

#include <string.h>

#include "discovery/msock.h"
#include "mdns.h"

/* Opens up to max sockets into socks; returns how many. Close each with mdns_socket_close(). */
static int fake_mdns_open(int *socks, int max) {
  struct in_addr v4[MC_MSOCKS_MAX];
  struct in6_addr v6;
  int n4 = 0, has6 = 0, n = 0;
  mc_msocks_list_addresses(v4, MC_MSOCKS_MAX, &n4, &v6, &has6);
  for (int i = 0; i < n4 && n < max; i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr = v4[i];
    int fd = mdns_socket_open_ipv4(&sa); /* port 0: any, as a one-off sender */
    if (fd >= 0) socks[n++] = fd;
  }
  return n;
}

#endif
