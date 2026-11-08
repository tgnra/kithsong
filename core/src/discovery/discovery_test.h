/* Test hooks for discovery. Internal to core. */
#ifndef MC_DISCOVERY_TEST_H
#define MC_DISCOVERY_TEST_H

#include "kithcore/discovery.h"

/* Makes every mDNS socket of disc deaf, as a network drop or a sleep can leave them on
 * some systems: they leave the multicast group, so they still send but hear nothing. Done
 * on the discovery thread, soon after the call. */
void mc_discovery_test_deafen(mc_discovery *disc);
/* How many times a socket was reopened because it had gone deaf or failed to send. */
int mc_discovery_test_reopened(mc_discovery *disc);

#endif
