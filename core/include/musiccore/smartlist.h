/* Smart lists: playlists (of tracks) and album groups (of albums) defined by a rule instead
 * of by hand, e.g. "the 10 least played tracks" or "albums added in the last 30 days". The
 * rule is JSON, stored as one value so an edit syncs whole, and is evaluated each time the
 * list is listed, so it always reflects current play counts and this device's files. The
 * format, fields and operators are in docs/schema.md (Smart lists); mc_smart_fields() lists
 * them for building an editor.
 *
 * MC_ERR_FORMAT means a rule isn't valid: bad JSON, an unknown field or operator, a value of
 * the wrong type, or a newer rule version than this build understands. MC_ERR_NOT_FOUND means
 * the smart list named doesn't exist. */
#ifndef MUSICCORE_SMARTLIST_H
#define MUSICCORE_SMARTLIST_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defined in library.h, which includes this through musiccore.h before it defines them. */
typedef struct mc_library mc_library;
typedef struct mc_track_list mc_track_list;
typedef struct mc_album_list mc_album_list;

typedef enum mc_smart_subject {
  MC_SMART_TRACKS, /* a smart playlist */
  MC_SMART_ALBUMS  /* a smart album group */
} mc_smart_subject;

/* Checks rule_json against subject without saving anything, e.g. while it's being edited. */
mc_status mc_smart_validate(mc_smart_subject subject, const char *rule_json);

/* name must not be empty; names needn't be unique. The rule must be valid. */
mc_status mc_smart_create(mc_library *lib, const char *name, mc_smart_subject subject,
                          const char *rule_json, char out_uid[MC_UID_STRLEN]);
mc_status mc_smart_rename(mc_library *lib, const char *smart_uid, const char *name);
mc_status mc_smart_set_rule(mc_library *lib, const char *smart_uid, const char *rule_json);
mc_status mc_smart_delete(mc_library *lib, const char *smart_uid);
/* Picks a new seed for a list ordered by random, so it comes out in a new order (on every
 * device, once it syncs). MC_ERR_INVALID_ARG if the list isn't ordered by random. */
mc_status mc_smart_reshuffle(mc_library *lib, const char *smart_uid);

typedef struct mc_smart_info {
  char uid[MC_UID_STRLEN];
  char *name;
  mc_smart_subject subject;
  char *rule; /* the rule's JSON */
} mc_smart_info;

typedef struct mc_smart_list {
  mc_smart_info *items;
  int count;
  int hidden; /* smart lists MC_LIST_PLAYABLE left out */
} mc_smart_list;

/* Every smart list of either subject, by name. With MC_LIST_PLAYABLE (library.h) in flags,
 * only those that hold something to play here: each rule is evaluated, and a list is left out
 * if it holds no track with a file here (a smart playlist), no album with a file here (a smart
 * album group), or its rule can't be evaluated (MC_ERR_FORMAT, e.g. from a newer app). Free
 * with mc_smart_list_free(). */
mc_status mc_library_list_smart(mc_library *lib, unsigned flags, mc_smart_list **out);
void mc_smart_list_free(mc_smart_list *list);

/* What a saved smart list holds now. MC_ERR_INVALID_ARG if it's the other subject. Free with
 * mc_track_list_free() / mc_album_list_free(). */
mc_status mc_library_list_smart_tracks(mc_library *lib, const char *smart_uid, mc_track_list **out);
mc_status mc_library_list_smart_albums(mc_library *lib, const char *smart_uid, mc_album_list **out);
/* What a rule would hold, without saving it: a live preview while editing. */
mc_status mc_smart_preview_tracks(mc_library *lib, const char *rule_json, mc_track_list **out);
mc_status mc_smart_preview_albums(mc_library *lib, const char *rule_json, mc_album_list **out);

/* A field a rule can test or order by. type says what a value looks like: "text", "int",
 * "duration" (seconds), "date" (Unix ms, or {"days": n} for within/not_within), "bool", or
 * "playlist" / "group" / "format" (a playlist uid, an album group uid, a format such as
 * "flac"). ops is NULL-terminated. A list can also be ordered by "random", which isn't a
 * field. Every string is static. */
typedef struct mc_smart_field {
  const char *name;
  const char *type;
  const char *const *ops;
  int sortable;
} mc_smart_field;

/* The fields subject supports, in a sensible order for a picker: field i, for i from 0 to
 * mc_smart_field_count() - 1. mc_smart_field_get() returns 0 if i is out of range. */
int mc_smart_field_count(mc_smart_subject subject);
int mc_smart_field_get(mc_smart_subject subject, int i, mc_smart_field *out);

#ifdef __cplusplus
}
#endif

#endif
