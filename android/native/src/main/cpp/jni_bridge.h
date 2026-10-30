/* Helpers jni_bridge.c shares with the bridge's other files (jni_browse.c, jni_settings.c). */
#ifndef KITHSONG_JNI_BRIDGE_H
#define KITHSONG_JNI_BRIDGE_H

#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

/* Throws MusicCoreException(status) and returns status. */
mc_status throw_status(JNIEnv *env, mc_status st);

/* The mc_library of a CoreLibrary's handle. */
static inline mc_library *jni_lib_of(jlong handle) {
  return (mc_library *)(intptr_t)handle;
}

/* s's UTF-8, or NULL for a null s; *ok is cleared if it couldn't be had (OutOfMemoryError is
 * pending then). Release with jni_release(). */
static inline const char *jni_chars(JNIEnv *env, jstring s, int *ok) {
  if (!s) return NULL;
  const char *c = (*env)->GetStringUTFChars(env, s, NULL);
  if (!c) *ok = 0;
  return c;
}

static inline void jni_release(JNIEnv *env, jstring s, const char *c) {
  if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}

/* An int[] of count values, or NULL with OutOfMemoryError pending. */
static inline jintArray jni_ints(JNIEnv *env, const jint *values, jsize count) {
  jintArray out = (*env)->NewIntArray(env, count);
  if (out) (*env)->SetIntArrayRegion(env, out, 0, count, values);
  return out;
}

/* The mc_sync of a CoreSync's handle (jni_sync_here.c). */
mc_sync *jni_sync_of(jlong handle);

/* A Java string for s, or NULL for NULL. */
jstring new_string(JNIEnv *env, const char *s);

/* An AlbumInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list);

/* A TrackInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list);

#endif
