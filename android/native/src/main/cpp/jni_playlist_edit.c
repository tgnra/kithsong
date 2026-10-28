/* JNI bindings for ng.kithso.core.PlaylistNatives: editing playlists (musiccore/playlist.h)
 * on an open CoreLibrary's handle. Kept apart from jni_bridge.c; as thin as it is: marshal
 * arguments, call musiccore, convert results. Failures throw ng.kithso.core.MusicCoreException. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

/* Throws MusicCoreException(status). */
static void throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/MusicCoreException");
  if (!cls) return; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
}

static mc_library *lib_of(jlong handle) {
  return (mc_library *)(intptr_t)handle;
}

/* s's UTF-8, or NULL for a null s; *ok is cleared if it couldn't be had (OutOfMemoryError is
 * pending then). Release with release(). */
static const char *chars(JNIEnv *env, jstring s, int *ok) {
  if (!s) return NULL;
  const char *c = (*env)->GetStringUTFChars(env, s, NULL);
  if (!c) *ok = 0;
  return c;
}

static void release(JNIEnv *env, jstring s, const char *c) {
  if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}

/* A new playlist called name; returns its uid. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_PlaylistNatives_nativeCreate(JNIEnv *env, jclass cls,
                                                                         jlong handle, jstring name) {
  (void)cls;
  int ok = 1;
  const char *cname = chars(env, name, &ok);
  if (!ok) return NULL;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_playlist_create(lib_of(handle), cname ? cname : "", uid);
  release(env, name, cname);
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
  const char *cuid = chars(env, uid, &ok);
  const char *cname = chars(env, name, &ok);
  mc_status st = ok ? mc_playlist_rename(lib_of(handle), cuid ? cuid : "", cname ? cname : "") : MC_OK;
  release(env, uid, cuid);
  release(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeDelete(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_playlist_delete(lib_of(handle), cuid ? cuid : "");
  release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* Appends each of track_uids to the playlist, stopping at the first that fails. */
JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeAppend(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid,
                                                                      jobjectArray track_uids) {
  (void)cls;
  int ok = 1;
  const char *cuid = chars(env, uid, &ok);
  if (!ok) return;
  mc_library *lib = lib_of(handle);
  mc_status st = MC_OK;
  jsize count = (*env)->GetArrayLength(env, track_uids);
  for (jsize i = 0; st == MC_OK && i < count; i++) {
    jstring track = (*env)->GetObjectArrayElement(env, track_uids, i);
    const char *ctrack = chars(env, track, &ok);
    if (!ok) st = MC_ERR_NOMEM;
    else st = mc_playlist_append(lib, cuid ? cuid : "", ctrack ? ctrack : "", NULL);
    release(env, track, ctrack);
    if (track) (*env)->DeleteLocalRef(env, track);
  }
  release(env, uid, cuid);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

/* Moves entry to after after_entry (null = to the start). */
JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeMove(JNIEnv *env, jclass cls,
                                                                    jlong handle, jstring entry,
                                                                    jstring after_entry) {
  (void)cls;
  int ok = 1;
  const char *centry = chars(env, entry, &ok);
  const char *cafter = chars(env, after_entry, &ok);
  mc_status st = ok ? mc_playlist_move(lib_of(handle), centry ? centry : "", cafter) : MC_OK;
  release(env, entry, centry);
  release(env, after_entry, cafter);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_PlaylistNatives_nativeRemove(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring entry) {
  (void)cls;
  int ok = 1;
  const char *centry = chars(env, entry, &ok);
  if (!ok) return;
  mc_status st = mc_playlist_remove(lib_of(handle), centry ? centry : "");
  release(env, entry, centry);
  if (st != MC_OK) throw_status(env, st);
}

/* The playlist's entry uids in playlist order, one per item of
 * mc_library_list_playlist_tracks() (its items' entry_uid), for moving and removing. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_PlaylistNatives_nativeEntries(JNIEnv *env, jclass cls,
                                                                               jlong handle, jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_playlist_tracks(lib_of(handle), cuid ? cuid : "", &list);
  release(env, uid, cuid);
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
