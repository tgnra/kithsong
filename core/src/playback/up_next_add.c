/* What a frontend adds to up next, read from the library: mc_up_next_add_load(). It's here rather
 * than in the player, which knows files and not the library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/i18n.h"
#include "kithcore/library.h"
#include "kithcore/playback.h"
#include "kithcore/playlist.h"
#include "kithcore/smartlist.h"
#include "util/platform.h"

/* An mc_up_next_add, with all it points to. */
typedef struct loaded {
  mc_up_next_add add; /* first: it's what the caller holds */
  mc_track_list *list;
  mc_up_next_track *tracks;
  char *title, *artist;
  char uid[MC_UID_STRLEN];
} loaded;

/* An album's artist credit as shown, for its row. Malloc'd, or NULL. */
static char *album_credit(mc_library *lib, const char *album_uid) {
  mc_credit_list *credit = NULL;
  if (mc_library_list_credit(lib, album_uid, &credit) != MC_OK) return NULL;
  size_t n = 1;
  for (int i = 0; i < credit->count; i++) n += strlen(credit->items[i].name) + strlen(credit->items[i].join);
  char *text = credit->count ? calloc(n, 1) : NULL;
  for (int i = 0; text && i < credit->count; i++) {
    strcat(text, credit->items[i].name);
    strcat(text, credit->items[i].join);
  }
  mc_credit_list_free(credit);
  return text;
}

/* Every album of the group, in its order: their tracks with a file here (see
 * mc_library_list_albums_tracks()). NULL if it can't be listed. */
static mc_track_list *group_tracks(mc_library *lib, const char *uid, int smart) {
  mc_album_list *albums = NULL;
  mc_status st = smart ? mc_library_list_smart_albums(lib, uid, &albums) : mc_library_list_album_group_albums(lib, uid, &albums);
  if (st != MC_OK) return NULL;
  const char **uids = albums->count ? calloc((size_t)albums->count, sizeof *uids) : NULL;
  for (int i = 0; uids && i < albums->count; i++) uids[i] = albums->items[i].uid;
  mc_track_list *list = NULL;
  if (uids && mc_library_list_albums_tracks(lib, uids, albums->count, &list) != MC_OK) list = NULL;
  free(uids);
  mc_album_list_free(albums);
  return list;
}

static mc_track_list *tracks_of(mc_library *lib, mc_up_next_kind kind, const char *uid) {
  mc_track_list *list = NULL;
  mc_status st = MC_ERR_NOT_FOUND;
  switch (kind) {
    case MC_UP_NEXT_TRACK: st = mc_library_get_track(lib, uid, &list); break;
    case MC_UP_NEXT_ALBUM: st = mc_library_list_album_tracks(lib, uid, &list); break;
    case MC_UP_NEXT_ARTIST: st = mc_library_list_artist_tracks(lib, uid, &list); break;
    case MC_UP_NEXT_PLAYLIST: st = mc_library_list_playlist_tracks(lib, uid, &list); break;
    case MC_UP_NEXT_SMART_PLAYLIST: st = mc_library_list_smart_tracks(lib, uid, &list); break;
    case MC_UP_NEXT_ALBUM_GROUP: return group_tracks(lib, uid, 0);
    case MC_UP_NEXT_SMART_ALBUM_GROUP: return group_tracks(lib, uid, 1);
  }
  return st == MC_OK ? list : NULL;
}

void mc_up_next_add_free(mc_up_next_add *add) {
  loaded *l = (loaded *)add;
  if (!l) return;
  mc_track_list_free(l->list);
  free(l->tracks);
  free(l->title);
  free(l->artist);
  free(l);
}

mc_status mc_up_next_add_load(mc_library *lib, mc_up_next_kind kind, const char *uid, const char *name, int as_tracks,
                              mc_up_next_add **out) {
  /* What's played as, for the play log, and the second line of a block's row: what it is, or with
   * none given, its tracks left. */
  static const struct {
    mc_play_context context;
    int has_line;
    enum mc_str_id line;
  } k_kinds[] = {[MC_UP_NEXT_TRACK] = {MC_PLAY_CONTEXT_MANUAL, 0, 0},
                 [MC_UP_NEXT_ALBUM] = {MC_PLAY_CONTEXT_ALBUM, 0, 0},
                 [MC_UP_NEXT_ARTIST] = {MC_PLAY_CONTEXT_ARTIST, 0, 0},
                 [MC_UP_NEXT_PLAYLIST] = {MC_PLAY_CONTEXT_PLAYLIST, 1, MC_STR_PLAYLIST},
                 [MC_UP_NEXT_SMART_PLAYLIST] = {MC_PLAY_CONTEXT_PLAYLIST, 1, MC_STR_SMART_PLAYLIST},
                 [MC_UP_NEXT_ALBUM_GROUP] = {MC_PLAY_CONTEXT_ALBUM_GROUP, 1, MC_STR_ALBUM_GROUP},
                 [MC_UP_NEXT_SMART_ALBUM_GROUP] = {MC_PLAY_CONTEXT_ALBUM_GROUP, 1, MC_STR_ALBUM_GROUP}};
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  if (!lib || !uid || (unsigned)kind > MC_UP_NEXT_SMART_ALBUM_GROUP) return MC_ERR_INVALID_ARG;

  loaded *l = calloc(1, sizeof *l);
  if (!l) return MC_ERR_NOMEM;
  l->list = tracks_of(lib, kind, uid);
  l->tracks = l->list && l->list->count ? calloc((size_t)l->list->count, sizeof *l->tracks) : NULL;
  int count = 0;
  for (int i = 0; l->tracks && i < l->list->count; i++) {
    const mc_track_info *t = &l->list->items[i];
    if (t->path) l->tracks[count++] = (mc_up_next_track){t->path, t->uid, t->title, t->artist, t->duration_ms};
  }
  if (!count) {
    mc_up_next_add_free(&l->add);
    return MC_ERR_NOT_FOUND;
  }
  if (kind != MC_UP_NEXT_TRACK) {
    snprintf(l->uid, sizeof l->uid, "%s", uid);
    l->add.context_uid = l->uid;
    if (!as_tracks && name) {
      l->title = mc_strdup(name);
      if (kind == MC_UP_NEXT_ALBUM) l->artist = album_credit(lib, uid);
      else if (k_kinds[kind].has_line) l->artist = mc_strdup(mc_str(k_kinds[kind].line));
    }
  }
  l->add.tracks = l->tracks;
  l->add.count = count;
  l->add.title = l->title;
  l->add.artist = l->artist;
  l->add.context = k_kinds[kind].context;
  *out = &l->add;
  return MC_OK;
}
