/* Internals of search.c shared with library.c and tests. */
#ifndef MC_SEARCH_INTERNAL_H
#define MC_SEARCH_INTERNAL_H

#include "musiccore/search.h"

typedef struct mc_search mc_search;

/* Stops a library handle's background build, if one is running, and frees its index; NULL
 * is fine. */
void mc_search_free(mc_search *search);

/* For tests: waits for a background build to finish and puts its index to use. */
void mc_search_wait(mc_library *lib);

/* For tests: how many indexes this handle has put to use. */
int mc_search_builds(const mc_library *lib);

#endif
