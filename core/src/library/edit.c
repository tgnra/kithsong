/* See musiccore/edit.h. */
#include "musiccore/edit.h"

#include "musiccore/library.h"

mc_status mc_album_tags_get(mc_library *lib, const char *album_uid, mc_album_tags *out) {
  (void)lib, (void)album_uid, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
mc_status mc_album_tags_set(mc_library *lib, const char *album_uid, const mc_album_tags *tags) {
  (void)lib, (void)album_uid, (void)tags;
  return MC_ERR_UNIMPLEMENTED;
}
void mc_album_tags_clear(mc_album_tags *tags) { (void)tags; }

mc_status mc_artist_tags_get(mc_library *lib, const char *artist_uid, mc_artist_tags *out) {
  (void)lib, (void)artist_uid, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
mc_status mc_artist_tags_set(mc_library *lib, const char *artist_uid, const mc_artist_tags *tags) {
  (void)lib, (void)artist_uid, (void)tags;
  return MC_ERR_UNIMPLEMENTED;
}
void mc_artist_tags_clear(mc_artist_tags *tags) { (void)tags; }

mc_status mc_track_tags_get(mc_library *lib, const char *track_uid, mc_track_tags *out) {
  (void)lib, (void)track_uid, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
mc_status mc_track_tags_set(mc_library *lib, const char *track_uid, const mc_track_tags *tags) {
  (void)lib, (void)track_uid, (void)tags;
  return MC_ERR_UNIMPLEMENTED;
}
void mc_track_tags_clear(mc_track_tags *tags) { (void)tags; }

mc_status mc_release_search(mc_library *lib, const char *album_uid, const char *artist,
                            const char *title, mc_release_list **out) {
  (void)lib, (void)album_uid, (void)artist, (void)title, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
void mc_release_list_free(mc_release_list *list) { (void)list; }

mc_status mc_release_tags(mc_library *lib, const char *album_uid, const char *mb_albumid,
                          mc_album_tags *out) {
  (void)lib, (void)album_uid, (void)mb_albumid, (void)out;
  return MC_ERR_UNIMPLEMENTED;
}
