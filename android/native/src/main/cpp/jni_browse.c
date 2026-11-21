/* JNI bindings for ng.kithso.core.CoreBrowse: browsing the library beyond the album grid
 * (artists' pages, album groups, smart album groups, credits), recents and search. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* mc_library_read_stats() as a long[], in the order CoreLibrary.stats() reads it: a long[]
 * rather than an object, as there are too many numbers for a constructor signature to be
 * worth getting right at run time. */
JNI_FN(jlongArray, CoreBrowse, nativeStats, jlong handle) {
  mc_library_stats s;
  JNI_CHECK(mc_library_read_stats(jni_lib_of(handle), &s), NULL);
  const jlong values[] = {
    s.all.tracks, s.all.albums, s.all.singles, s.all.artists, s.all.genres, s.all.duration_ms,
    s.here.tracks, s.here.albums, s.here.singles, s.here.artists, s.here.genres, s.here.duration_ms,
    s.files, s.size_bytes,
    s.playlists, s.smart_playlists, s.album_groups, s.smart_album_groups,
    s.plays, s.tracks_played, s.listened_ms,
  };
  const jsize count = (jsize)(sizeof values / sizeof *values);
  jlongArray out = (*env)->NewLongArray(env, count);
  if (out) (*env)->SetLongArrayRegion(env, out, 0, count, values);
  return out;
}

/* The albums of an artist (kind 0), an album group (1) or a smart album group (2). */
JNI_FN(jobjectArray, CoreBrowse, nativeListAlbums, jlong handle, jint kind, jstring uid) {
  static mc_status (*const lists[])(mc_library *, const char *, mc_album_list **) = {
    mc_library_list_artist_albums,
    mc_library_list_album_group_albums,
    mc_library_list_smart_albums,
  };
  if (kind < 0 || kind >= (jint)(sizeof lists / sizeof *lists)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  JNI_STR(cuid, uid, NULL);
  mc_album_list *list = NULL;
  mc_status st = lists[kind](jni_lib_of(handle), cuid, &list);
  return album_array(env, st, list);
}

/* Every track credited to an artist. */
JNI_FN(jobjectArray, CoreBrowse, nativeArtistTracks, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_artist_tracks(jni_lib_of(handle), cuid, &list);
  return track_array(env, st, list);
}

/* An artist's singles: their tracks alone on their album, or on none. */
JNI_FN(jobjectArray, CoreBrowse, nativeArtistSingles, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_artist_singles(jni_lib_of(handle), cuid, &list);
  return track_array(env, st, list);
}

/* One track, whether or not it has a file here, as a TrackInfo[] of it, or of nothing if there's
 * no such track. */
JNI_FN(jobjectArray, CoreBrowse, nativeTrack, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_list *list = NULL;
  mc_status st = mc_library_get_track(jni_lib_of(handle), cuid, &list);
  if (st == MC_ERR_NOT_FOUND) {
    jclass info = (*env)->FindClass(env, JNI_CLASS("TrackInfo"));
    return info ? (*env)->NewObjectArray(env, 0, info, NULL) : NULL;
  }
  return track_array(env, st, list);
}

/* What a smart playlist with rule would hold, without saving it. */
JNI_FN(jobjectArray, CoreBrowse, nativePreviewSmartTracks, jlong handle, jstring rule) {
  JNI_STR(crule, rule, NULL);
  mc_track_list *list = NULL;
  mc_status st = mc_smart_preview_tracks(jni_lib_of(handle), crule, &list);
  return track_array(env, st, list);
}

/* The image to show for an album group (see mc_album_group_art()), or NULL for a placeholder. */
JNI_FN(jstring, CoreBrowse, nativeAlbumGroupArt, jlong handle, jstring uid, jstring cache_dir) {
  return jni_art(env, handle, uid, cache_dir, mc_album_group_art);
}

/* The image to show for a smart list (see mc_smart_art()), or NULL for a placeholder. */
JNI_FN(jstring, CoreBrowse, nativeSmartArt, jlong handle, jstring uid, jstring cache_dir) {
  return jni_art(env, handle, uid, cache_dir, mc_smart_art);
}

JNI_FN(void, CoreBrowse, nativeReshuffle, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_smart_reshuffle(jni_lib_of(handle), cuid));
}

/* kind is an mc_recent_kind. */
JNI_FN(void, CoreBrowse, nativeRecordOpened, jlong handle, jint kind, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_library_record_opened(jni_lib_of(handle), (mc_recent_kind)kind, cuid));
}

JNI_FN(void, CoreBrowse, nativeRemoveRecent, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  mc_status st = mc_library_remove_recent(jni_lib_of(handle), cuid);
  if (st != MC_ERR_NOT_FOUND) jni_check(env, st); /* gone already: sync took it */
}

JNI_FN(void, CoreBrowse, nativeClearRecents, jlong handle) {
  JNI_CHECK(mc_library_clear_recents(jni_lib_of(handle)));
}

static jobject new_recent(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_recent_info *r = item;
  jstring uid = new_string(env, r->uid);
  jstring name = new_string(env, r->name ? r->name : "");
  jstring artist = new_string(env, r->artist);
  jobject out = (*env)->NewObject(env, cls, ctor, (jint)r->kind, uid, (jlong)r->opened_ms, name, artist,
                                  JNI_BOOL(r->is_smart));
  JNI_DELETE(uid, name, artist);
  return out;
}

/* flags are mc_library_list_recents()'s (MC_LIST_PLAYABLE). */
JNI_FN(jobjectArray, CoreBrowse, nativeListRecents, jlong handle, jint limit, jint flags) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("RecentInfo"), "(I" J_STRING "J" J_STRING J_STRING "Z)V", &info);
  if (!ctor) return NULL;
  mc_recent_list *list = NULL;
  JNI_CHECK(mc_library_list_recents(jni_lib_of(handle), (int)limit, (unsigned)flags, &list), NULL);
  /* An empty list may be NULL. */
  jobjectArray out =
    JNI_OBJECT_ARRAY(info, ctor, list ? list->items : NULL, list ? list->count : 0, new_recent);
  mc_recent_list_free(list);
  return out;
}

static jobject new_hit(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_search_hit *h = item;
  jstring uid = new_string(env, h->uid);
  jstring name = new_string(env, h->name ? h->name : "");
  jstring artist = new_string(env, h->artist);
  jstring album = new_string(env, h->album);
  jstring album_uid = h->album_uid[0] ? new_string(env, h->album_uid) : NULL;
  jobject out = (*env)->NewObject(env, cls, ctor, (jint)h->kind, uid, name, artist, album, album_uid,
                                  JNI_BOOL(h->is_smart));
  JNI_DELETE(uid, name, artist, album, album_uid);
  return out;
}

/* What matches query, best first, at most limit of each kind (all if <= 0). */
JNI_FN(jobjectArray, CoreBrowse, nativeSearch, jlong handle, jstring query, jint limit) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SearchHit"),
                            "(I" J_STRING J_STRING J_STRING J_STRING J_STRING "Z)V", &info);
  if (!ctor) return NULL;
  JNI_STR(cquery, query, NULL);
  mc_search_results *res = NULL;
  JNI_CHECK(mc_library_search(jni_lib_of(handle), cquery, (int)limit, &res), NULL);
  jobjectArray out = JNI_OBJECT_ARRAY(info, ctor, res ? res->items : NULL, res ? res->count : 0, new_hit);
  mc_search_results_free(res);
  return out;
}

JNI_FN(void, CoreBrowse, nativeSearchPrepare, jlong handle) {
  JNI_CHECK(mc_library_search_prepare(jni_lib_of(handle)));
}

static jobject new_credit(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_credit_artist *a = item;
  jstring artist_uid = new_string(env, a->uid);
  jstring name = new_string(env, a->name ? a->name : "");
  jstring join = new_string(env, a->join ? a->join : "");
  jobject out = (*env)->NewObject(env, cls, ctor, artist_uid, name, join);
  JNI_DELETE(artist_uid, name, join);
  return out;
}

/* The artists a track or album is credited to, in order (mc_library_list_credit). */
JNI_FN(jobjectArray, CoreBrowse, nativeListCredit, jlong handle, jstring uid) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("CreditArtist"), "(" J_STRING J_STRING J_STRING ")V", &info);
  if (!ctor) return NULL;
  JNI_STR(cuid, uid, NULL);
  mc_credit_list *list = NULL;
  JNI_CHECK(mc_library_list_credit(jni_lib_of(handle), cuid, &list), NULL);
  jobjectArray out = JNI_OBJECT_ARRAY(info, ctor, list ? list->items : NULL, list ? list->count : 0, new_credit);
  mc_credit_list_free(list);
  return out;
}
