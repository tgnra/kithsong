/* Test hooks for Google Cast. Internal to core. */
#ifndef MC_CAST_CAST_TEST_H
#define MC_CAST_CAST_TEST_H

#include <stdint.h>

#include "kithcore/cast.h"

/* Makes the device list devices, as if discovery had found them (and fires
 * MC_CAST_EVENT_DEVICES), so a test can connect to a fake device without mDNS. */
void mc_cast_test_set_devices(mc_cast *cast, const mc_cast_device *devices, int count);
/* How long a cast may stay paused before it ends; MC_CAST_PAUSED_MS unless a test changes it
 * before connecting. */
extern uint32_t mc_cast_test_paused_ms;

#endif
