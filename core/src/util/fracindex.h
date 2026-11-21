/* Fractional index keys, for ordered lists that sync (playlist_entries, album_group_entries).
 * A key always exists strictly between any two others, so inserting or moving an entry
 * writes only that entry's row.
 *
 * Keys are base-62 strings ("0-9A-Za-z") that sort bytewise, in the format of
 * rocicorp/fractional-indexing: an integer part whose first character gives its length
 * ("a0", "a1", ... "az", "b100"; "Zz" and below for keys before "a0"), then an optional
 * fraction that never ends in '0' ("a0V"). Appending one after another stays short. */
#ifndef MC_FRACINDEX_H
#define MC_FRACINDEX_H

/* A new key strictly between a and b; NULL a is the start of the list and NULL b the end.
 * a must sort before b. Returns a malloc'd string, or NULL if a or b isn't a valid key, a
 * isn't before b, or out of memory. */
char *mc_frac_key_between(const char *a, const char *b);

#endif
