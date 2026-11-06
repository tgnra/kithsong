/* JNI bindings for CorePlayer's up next (kithcore/playback.h). Kept apart from jni_bridge.c; as
 * thin as it is: marshal arguments, call kithcore, convert results. Failures throw
 * ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* mc_up_next_add_load(), then mc_player_play_next() or with front false mc_player_play_last(), so
 * what's loaded needn't cross over. False if it has no track with a file here. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNextAdd(JNIEnv *env, jclass cls, jlong handle,
                                                                         jlong library, jint kind, jstring uid,
                                                                         jstring name, jboolean as_tracks,
                                                                         jboolean front) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *cname = jni_chars(env, name, &ok);
  mc_status st = MC_OK;
  if (ok) {
    mc_player *player = jni_player_of(handle);
    mc_up_next_add *add = NULL;
    st = mc_up_next_add_load(jni_lib_of(library), (mc_up_next_kind)kind, cuid, cname, as_tracks ? 1 : 0, &add);
    if (st == MC_OK) {
      st = front ? mc_player_play_next(player, add) : mc_player_play_last(player, add);
      mc_up_next_add_free(add);
    }
  }
  jni_release(env, uid, cuid);
  jni_release(env, name, cname);
  if (!ok || st == MC_ERR_NOT_FOUND) return JNI_FALSE; /* !ok has OutOfMemoryError pending */
  if (st != MC_OK) throw_status(env, st);
  return st == MC_OK ? JNI_TRUE : JNI_FALSE;
}

/* An UpNextEntry[] of up next now. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNext(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  mc_up_next_list *list = NULL;
  mc_status st = mc_player_up_next(jni_player_of(handle), &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/UpNextEntry");
  jmethodID ctor = info ? (*env)->GetMethodID(env, info, "<init>", "(ILjava/lang/String;Ljava/lang/String;ZIJII)V") : NULL;
  if (ctor) out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_up_next_entry *e = &list->items[i];
    jstring title = new_string(env, e->title ? e->title : "");
    jstring artist = new_string(env, e->artist);
    jobject item = (*env)->NewObject(env, info, ctor, (jint)e->id, title, artist, e->block ? JNI_TRUE : JNI_FALSE,
                                     (jint)e->tracks, (jlong)e->duration_ms, (jint)e->first_item, (jint)e->context);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, title);
    if (artist) (*env)->DeleteLocalRef(env, artist);
  }
  mc_up_next_list_free(list);
  return out;
}

/* An entry that has just played finds a move, remove or rename too late, which is no failure. */
static void check(JNIEnv *env, mc_status st) {
  if (st != MC_OK && st != MC_ERR_NOT_FOUND) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNextMove(JNIEnv *env, jclass cls, jlong handle, jint id,
                                                                      jint to) {
  (void)cls;
  check(env, mc_player_up_next_move(jni_player_of(handle), (uint32_t)id, to));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNextRemove(JNIEnv *env, jclass cls, jlong handle,
                                                                        jint id) {
  (void)cls;
  check(env, mc_player_up_next_remove(jni_player_of(handle), (uint32_t)id));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNextClear(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  check(env, mc_player_up_next_clear(jni_player_of(handle)));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeUpNextRename(JNIEnv *env, jclass cls, jlong handle, jint id,
                                                                        jstring title, jstring artist) {
  (void)cls;
  int ok = 1;
  const char *ctitle = jni_chars(env, title, &ok);
  const char *cartist = jni_chars(env, artist, &ok);
  if (ok) check(env, mc_player_up_next_rename(jni_player_of(handle), (uint32_t)id, ctitle, cartist));
  jni_release(env, title, ctitle);
  jni_release(env, artist, cartist);
}
