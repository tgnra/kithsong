/* JNI bindings for ng.kithso.core.PlaylistNatives and AlbumGroupNatives: editing playlists and
 * album groups (musiccore/playlist.h) on an open CoreLibrary's handle. Kept apart from
 * jni_bridge.c; as thin as it is: marshal arguments, call musiccore, convert results.
 * Failures throw ng.kithso.core.MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#include "jni_bridge.h"

/* A new playlist called name; returns its uid. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_PlaylistNatives_nativeCreate(JNIEnv *env, jclass cls,
                                                                         jlong handle, jstring name) {
  (void)cls;
  int ok = 1;
  const char *cname = jni_chars(env, name, &ok);
  if (!ok) return NULL;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_playlist_create(jni_lib_of(handle), cname ? cname : "", uid);
  jni_release(env, name, cname);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return (*env)->NewStringUTF(env, uid);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeRename(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid,
                                                                      jstring name) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *cname = jni_chars(env, name, &ok);
  mc_status st = ok ? mc_playlist_rename(jni_lib_of(handle), cuid ? cuid : "", cname ? cname : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeDelete(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_playlist_delete(jni_lib_of(handle), cuid ? cuid : "");
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* Appends each of track_uids to the playlist, stopping at the first that fails. */
JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeAppend(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid,
                                                                      jobjectArray track_uids) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_library *lib = jni_lib_of(handle);
  mc_status st = MC_OK;
  jsize count = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; st == MC_OK && i < count; i++) {
    jstring track = (*env)->GetObjectArrayElement(env, track_uids, i);
    const char *ctrack = jni_chars(env, track, &ok);
    if (!ok) st = MC_ERR_NOMEM;
    else st = mc_playlist_append(lib, cuid ? cuid : "", ctrack ? ctrack : "", NULL);
    jni_release(env, track, ctrack);
    if (track) (*env)->DeleteLocalRef(env, track);
  }
  jni_release(env, uid, cuid);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

/* Moves entry to after after_entry (null = to the start). */
JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeMove(JNIEnv *env, jclass cls,
                                                                    jlong handle, jstring entry,
                                                                    jstring after_entry) {
  (void)cls;
  int ok = 1;
  const char *centry = jni_chars(env, entry, &ok);
  const char *cafter = jni_chars(env, after_entry, &ok);
  mc_status st = ok ? mc_playlist_move(jni_lib_of(handle), centry ? centry : "", cafter) : MC_OK;
  jni_release(env, entry, centry);
  jni_release(env, after_entry, cafter);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeRemove(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring entry) {
  (void)cls;
  int ok = 1;
  const char *centry = jni_chars(env, entry, &ok);
  if (!ok) return;
  mc_status st = mc_playlist_remove(jni_lib_of(handle), centry ? centry : "");
  jni_release(env, entry, centry);
  if (st != MC_OK) throw_status(env, st);
}

/* The playlist's entry uids in playlist order, one per item of
 * mc_library_list_playlist_tracks() (its items' entry_uid), for moving and removing. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_PlaylistNatives_nativeEntries(JNIEnv *env, jclass cls,
                                                                               jlong handle, jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_playlist_tracks(jni_lib_of(handle), cuid ? cuid : "", &list);
  jni_release(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jclass string = (*env)->FindClass(env, "java/lang/String");
  int count = list ? list->count : 0;
  jobjectArray out = string ? (*env)->NewObjectArray(env, count, string, NULL) : NULL;
  for (int i = 0; out && i < count; i++) {
    jstring entry = (*env)->NewStringUTF(env, list->items[i].entry_uid);
    if (!entry) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, entry);
    (*env)->DeleteLocalRef(env, entry);
  }
  mc_track_list_free(list);
  return out;
}

/* --- AlbumGroupNatives: album groups, the playlists of albums --- */

/* A new album group called name; returns its uid. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeCreate(JNIEnv *env, jclass cls,
                                                                           jlong handle, jstring name) {
  (void)cls;
  int ok = 1;
  const char *cname = jni_chars(env, name, &ok);
  if (!ok) return NULL;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_album_group_create(jni_lib_of(handle), cname ? cname : "", uid);
  jni_release(env, name, cname);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return (*env)->NewStringUTF(env, uid);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeRename(JNIEnv *env, jclass cls,
                                                                        jlong handle, jstring uid,
                                                                        jstring name) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *cname = jni_chars(env, name, &ok);
  mc_status st = ok ? mc_album_group_rename(jni_lib_of(handle), cuid ? cuid : "", cname ? cname : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeDelete(JNIEnv *env, jclass cls,
                                                                        jlong handle, jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_album_group_delete(jni_lib_of(handle), cuid ? cuid : "");
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* Adds album to the end of the group; nothing if it's in it already. */
JNIEXPORT void JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeAdd(JNIEnv *env, jclass cls,
                                                                     jlong handle, jstring uid,
                                                                     jstring album) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *calbum = jni_chars(env, album, &ok);
  mc_status st = ok ? mc_album_group_add(jni_lib_of(handle), cuid ? cuid : "", calbum ? calbum : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, album, calbum);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeRemove(JNIEnv *env, jclass cls,
                                                                        jlong handle, jstring uid,
                                                                        jstring album) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *calbum = jni_chars(env, album, &ok);
  mc_status st = ok ? mc_album_group_remove(jni_lib_of(handle), cuid ? cuid : "", calbum ? calbum : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, album, calbum);
  if (st != MC_OK) throw_status(env, st);
}

/* Moves album to after after_album (null = to the start) within the group. */
JNIEXPORT void JNICALL Java_ng_kithso_core_AlbumGroupNatives_nativeMove(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid,
                                                                      jstring album, jstring after_album) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *calbum = jni_chars(env, album, &ok);
  const char *cafter = jni_chars(env, after_album, &ok);
  mc_status st = ok ? mc_album_group_move(jni_lib_of(handle), cuid ? cuid : "", calbum ? calbum : "", cafter) : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, album, calbum);
  jni_release(env, after_album, cafter);
  if (st != MC_OK) throw_status(env, st);
}
