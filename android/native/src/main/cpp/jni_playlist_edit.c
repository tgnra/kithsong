/* JNI bindings for ng.kithso.core.PlaylistNatives and AlbumGroupNatives: editing playlists and
 * album groups (kithcore/playlist.h) on an open CoreLibrary's handle. Failures throw
 * ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* A new playlist called name; returns its uid. */
JNI_FN(jstring, PlaylistNatives, nativeCreate, jlong handle, jstring name) {
  JNI_STR(cname, name, NULL);
  char uid[MC_UID_STRLEN];
  JNI_CHECK(mc_playlist_create(jni_lib_of(handle), JNI_OR_EMPTY(cname), uid), NULL);
  return (*env)->NewStringUTF(env, uid);
}

JNI_FN(void, PlaylistNatives, nativeRename, jlong handle, jstring uid, jstring name) {
  JNI_STR(cuid, uid);
  JNI_STR(cname, name);
  JNI_CHECK(mc_playlist_rename(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(cname)));
}

JNI_FN(void, PlaylistNatives, nativeDelete, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_playlist_delete(jni_lib_of(handle), JNI_OR_EMPTY(cuid)));
}

/* Appends each of track_uids to the playlist, stopping at the first that fails. */
JNI_FN(void, PlaylistNatives, nativeAppend, jlong handle, jstring uid, jobjectArray track_uids) {
  JNI_STR(cuid, uid);
  mc_library *lib = jni_lib_of(handle);
  mc_status st = MC_OK;
  jsize count = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; st == MC_OK && i < count; i++) {
    jstring track = (*env)->GetObjectArrayElement(env, track_uids, i);
    int ok = 1;
    const char *ctrack = jni_chars(env, track, &ok);
    st = ok ? mc_playlist_append(lib, JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(ctrack), NULL) : MC_ERR_NOMEM;
    jni_release(env, track, ctrack);
    JNI_DELETE(track);
  }
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

/* Moves entry to after after_entry (null = to the start). */
JNI_FN(void, PlaylistNatives, nativeMove, jlong handle, jstring entry, jstring after_entry) {
  JNI_STR(centry, entry);
  JNI_STR(cafter, after_entry);
  JNI_CHECK(mc_playlist_move(jni_lib_of(handle), JNI_OR_EMPTY(centry), cafter));
}

JNI_FN(void, PlaylistNatives, nativeRemove, jlong handle, jstring entry) {
  JNI_STR(centry, entry);
  JNI_CHECK(mc_playlist_remove(jni_lib_of(handle), JNI_OR_EMPTY(centry)));
}

static jobject entry_string(JNIEnv *env, jclass cls JNI_UNUSED, jmethodID ctor JNI_UNUSED, const void *item) {
  return (*env)->NewStringUTF(env, ((const mc_track_info *)item)->entry_uid);
}

/* The playlist's entry uids in playlist order, one per item of
 * mc_library_list_playlist_tracks() (its items' entry_uid), for moving and removing. */
JNI_FN(jobjectArray, PlaylistNatives, nativeEntries, jlong handle, jstring uid) {
  JNI_STR(cuid, uid, NULL);
  mc_track_list *list = NULL;
  JNI_CHECK(mc_library_list_playlist_tracks(jni_lib_of(handle), JNI_OR_EMPTY(cuid), &list), NULL);
  jclass string = (*env)->FindClass(env, "java/lang/String");
  jobjectArray out =
    string ? JNI_OBJECT_ARRAY(string, NULL, list ? list->items : NULL, list ? list->count : 0, entry_string) : NULL;
  mc_track_list_free(list);
  return out;
}

/* --- AlbumGroupNatives: album groups, the playlists of albums --- */

/* A new album group called name; returns its uid. */
JNI_FN(jstring, AlbumGroupNatives, nativeCreate, jlong handle, jstring name) {
  JNI_STR(cname, name, NULL);
  char uid[MC_UID_STRLEN];
  JNI_CHECK(mc_album_group_create(jni_lib_of(handle), JNI_OR_EMPTY(cname), uid), NULL);
  return (*env)->NewStringUTF(env, uid);
}

JNI_FN(void, AlbumGroupNatives, nativeRename, jlong handle, jstring uid, jstring name) {
  JNI_STR(cuid, uid);
  JNI_STR(cname, name);
  JNI_CHECK(mc_album_group_rename(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(cname)));
}

JNI_FN(void, AlbumGroupNatives, nativeDelete, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_album_group_delete(jni_lib_of(handle), JNI_OR_EMPTY(cuid)));
}

/* Adds album to the end of the group; nothing if it's in it already. */
JNI_FN(void, AlbumGroupNatives, nativeAdd, jlong handle, jstring uid, jstring album) {
  JNI_STR(cuid, uid);
  JNI_STR(calbum, album);
  JNI_CHECK(mc_album_group_add(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(calbum)));
}

JNI_FN(void, AlbumGroupNatives, nativeRemove, jlong handle, jstring uid, jstring album) {
  JNI_STR(cuid, uid);
  JNI_STR(calbum, album);
  JNI_CHECK(mc_album_group_remove(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(calbum)));
}

/* Moves album to after after_album (null = to the start) within the group. */
JNI_FN(void, AlbumGroupNatives, nativeMove, jlong handle, jstring uid, jstring album, jstring after_album) {
  JNI_STR(cuid, uid);
  JNI_STR(calbum, album);
  JNI_STR(cafter, after_album);
  JNI_CHECK(mc_album_group_move(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(calbum), cafter));
}
