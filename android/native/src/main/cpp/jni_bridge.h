/* Helpers jni_bridge.c shares with the bridge's other jni_*.c files. */
#ifndef KITHSONG_JNI_BRIDGE_H
#define KITHSONG_JNI_BRIDGE_H

#include <jni.h>
#include <stddef.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#define JNI_UNUSED __attribute__((unused))

/* The function behind ng.kithso.core.Class's external fun name, a static one (or a companion's
 * @JvmStatic); JNI_OBJ_FN is for a Kotlin object's, which gets the object as self. env, cls and
 * self may go unused. The bare forms take no arguments after those. */
#define JNI_FN(ret, Class, name, ...) \
  JNIEXPORT ret JNICALL Java_ng_kithso_core_##Class##_##name(JNIEnv *env JNI_UNUSED, jclass cls JNI_UNUSED, __VA_ARGS__)
#define JNI_FN_BARE(ret, Class, name) \
  JNIEXPORT ret JNICALL Java_ng_kithso_core_##Class##_##name(JNIEnv *env JNI_UNUSED, jclass cls JNI_UNUSED)
#define JNI_OBJ_FN(ret, Class, name, ...) \
  JNIEXPORT ret JNICALL Java_ng_kithso_core_##Class##_##name(JNIEnv *env JNI_UNUSED, jobject self JNI_UNUSED, __VA_ARGS__)
#define JNI_OBJ_FN_BARE(ret, Class, name) \
  JNIEXPORT ret JNICALL Java_ng_kithso_core_##Class##_##name(JNIEnv *env JNI_UNUSED, jobject self JNI_UNUSED)

/* A Kotlin class's name for FindClass, and the pieces of signatures. */
#define JNI_CLASS(name) "ng/kithso/core/" name
#define J_STRING "Ljava/lang/String;"
#define J_STRINGS "[Ljava/lang/String;"

#define JNI_BOOL(x) ((x) ? JNI_TRUE : JNI_FALSE)

/* s, or "" for NULL. */
#define JNI_OR_EMPTY(s) ((s) ? (s) : "")

/* Throws KithCoreException(status) and returns status. */
mc_status throw_status(JNIEnv *env, mc_status st);

/* Throws for a failed st; returns whether it was MC_OK. */
static inline int jni_check(JNIEnv *env, mc_status st) {
  if (st == MC_OK) return 1;
  throw_status(env, st);
  return 0;
}

/* Evaluates expr, an mc_status; if it failed, throws and returns what follows (nothing in a
 * void function). */
#define JNI_CHECK(expr, ...)                         \
  do {                                               \
    if (!jni_check(env, (expr))) return __VA_ARGS__; \
  } while (0)

/* Handles: what a native object is to Kotlin, and back. */
#define JNI_PTR(type, handle) ((type *)(intptr_t)(handle))
#define JNI_HANDLE(p) ((jlong)(intptr_t)(p))

/* The mc_library of a CoreLibrary's handle. */
static inline mc_library *jni_lib_of(jlong handle) {
  return JNI_PTR(mc_library, handle);
}

/* The mc_player of a CorePlayer's handle (jni_up_next.c). */
mc_player *jni_player_of(jlong handle);

/* The mc_sync of a CoreSync's handle (jni_sync_here.c). */
mc_sync *jni_sync_of(jlong handle);

/* p's issues as a SyncIssue[], each in words for an app that syncs only while it's open, or
 * NULL with an exception pending (jni_sync_issues.c). */
jobjectArray jni_issue_array(JNIEnv *env, const mc_sync_problems *p);

/* s's UTF-8, or NULL for a null s; *ok is cleared if it couldn't be had (OutOfMemoryError is
 * pending then). Release with jni_release(); JNI_STR does both. */
static inline const char *jni_chars(JNIEnv *env, jstring s, int *ok) {
  if (!s) return NULL;
  const char *c = (*env)->GetStringUTFChars(env, s, NULL);
  if (!c) *ok = 0;
  return c;
}

static inline void jni_release(JNIEnv *env, jstring s, const char *c) {
  if (c) (*env)->ReleaseStringUTFChars(env, s, c);
}

typedef struct jni_str_ {
  JNIEnv *env;
  jstring s;
  const char *c;
} jni_str_;

static inline void jni_str_release_(jni_str_ *h) {
  jni_release(h->env, h->s, h->c);
}

/* Declares const char *var, jstr's UTF-8 (NULL for a null jstr), released when var goes out
 * of scope, so any return may follow. If it couldn't be had, returns what follows (nothing in a
 * void function) with OutOfMemoryError pending. jstr's ref must outlive var. */
#define JNI_STR(var, jstr, ...)                                                              \
  __attribute__((cleanup(jni_str_release_))) jni_str_ var##_held_ = {env, (jstr), NULL};   \
  int var##_ok_ = 1;                                                                        \
  const char *var = var##_held_.c = jni_chars(env, var##_held_.s, &var##_ok_);            \
  if (!var##_ok_) return __VA_ARGS__

/* Deletes the local refs given, skipping nulls. */
static inline void jni_delete_refs(JNIEnv *env, const jobject *refs, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (refs[i]) (*env)->DeleteLocalRef(env, refs[i]);
}
#define JNI_DELETE(...) \
  jni_delete_refs(env, (const jobject[]){__VA_ARGS__}, sizeof((const jobject[]){__VA_ARGS__}) / sizeof(jobject))

/* An int[] of count values, or NULL with OutOfMemoryError pending. */
static inline jintArray jni_ints(JNIEnv *env, const jint *values, jsize count) {
  jintArray out = (*env)->NewIntArray(env, count);
  if (out) (*env)->SetIntArrayRegion(env, out, 0, count, values);
  return out;
}

/* A Java string for s, or NULL for NULL. */
jstring new_string(JNIEnv *env, const char *s);

/* class_name's constructor of signature sig, *cls set to the class; NULL with an exception
 * pending if either can't be found. */
jmethodID jni_ctor(JNIEnv *env, const char *class_name, const char *sig, jclass *cls);

/* Makes a local ref to an element of the array for item, or NULL with an exception pending. */
typedef jobject (*jni_make_fn)(JNIEnv *env, jclass cls, jmethodID ctor, const void *item);

/* A cls[] of the n items, each size bytes, made by make, or NULL with an exception pending. */
jobjectArray jni_object_array(JNIEnv *env, jclass cls, jmethodID ctor, const void *items, size_t size, int n,
                              jni_make_fn make);
#define JNI_OBJECT_ARRAY(cls, ctor, items, n, make) \
  jni_object_array(env, (cls), (ctor), (items), sizeof *(items), (n), (make))

/* The image art makes for uid in cache_dir (mc_playlist_art() and the like), or NULL for a
 * placeholder or with an exception pending. */
typedef mc_status (*jni_art_fn)(mc_library *, const char *, const char *, char **);
jstring jni_art(JNIEnv *env, jlong handle, jstring uid, jstring cache_dir, jni_art_fn art);

/* An AlbumInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list);

/* A TrackInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list);

#endif
