/* This device's own LAN addresses, from the same interface list discovery announces.
 * Internal to core. */
#ifndef MC_DISCOVERY_LOCAL_ADDR_H
#define MC_DISCOVERY_LOCAL_ADDR_H

/* Room for a numeric IPv6 address and its NUL. */
#define MC_LOCAL_ADDR_STRLEN 46

/* Writes up to max numeric addresses: each IPv4 interface address except loopback, then
 * one global IPv6 address if there is one. Returns how many. */
int mc_local_addresses(char (*out)[MC_LOCAL_ADDR_STRLEN], int max);

#endif
