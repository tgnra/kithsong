/* The track to start a radio from when what's asked for is a whole album or artist. A station
 * starts from one track (musiccore/radio.h), so this picks the one that stands for the rest.
 *
 * That's the track nearest the middle of those that can seed one: the one with the least
 * distance to the others, as mc_analysis_distance() has it. What's been played decides only
 * between tracks that are level, which mostly means an album with two, then the uid does, so the
 * same library gives the same seed.
 *
 * An artist's tracks are mc_library_list_artist_tracks()'s. Of a very long list only some are
 * compared, spread over it. Either call reads no more than its own tracks, so a UI thread can
 * ask as a menu opens. */
#ifndef MUSICCORE_RADIO_SEEDS_H
#define MUSICCORE_RADIO_SEEDS_H

#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fills out with the seed, or "" on failure. MC_ERR_NOT_FOUND if no track of it passes
 * mc_radio_seed_check() with the default length limit, as playing a station has it. */
mc_status mc_radio_seed_for_album(mc_library *lib, const char *album_uid, char out[MC_UID_STRLEN]);
mc_status mc_radio_seed_for_artist(mc_library *lib, const char *artist_uid, char out[MC_UID_STRLEN]);

#ifdef __cplusplus
}
#endif

#endif
