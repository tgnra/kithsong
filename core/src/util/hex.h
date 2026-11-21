/* Hex and percent encoding. */
#ifndef MC_UTIL_HEX_H
#define MC_UTIL_HEX_H

#include <stddef.h>

/* Writes n bytes as 2n lowercase hex digits plus a NUL to out. */
void mc_hex(const void *bytes, size_t n, char *out);

/* s with every byte but A-Z a-z 0-9 - _ . ~ and those in keep (NULL for none) as %XX.
 * malloc'd, or NULL if out of memory. */
char *mc_percent_encode(const char *s, const char *keep);

#endif
