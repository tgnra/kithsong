/* JNI bindings for CorePlayer's up next (kithcore/playback.h). Failures throw
 * ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* mc_up_next_add_load(), then mc_player_up_next_add(), so what's loaded needn't cross over. False
 * if it has no track with a file here. */
JNI_FN(jboolean, CorePlayer, nativeUpNextAdd, jlong handle, jlong library, jint kind, jstring uid, jstring name,
       jboolean as_tracks) {
  JNI_STR(cuid, uid, JNI_FALSE);
  JNI_STR(cname, name, JNI_FALSE);
  mc_player *player = jni_player_of(handle);
  mc_up_next_add *add = NULL;
  mc_status st = mc_up_next_add_load(jni_lib_of(library), (mc_up_next_kind)kind, cuid, cname, as_tracks ? 1 : 0, &add);
  if (st == MC_OK) {
    st = mc_player_up_next_add(player, add);
    mc_up_next_add_free(add);
  }
  if (st == MC_ERR_NOT_FOUND) return JNI_FALSE;
  return JNI_BOOL(jni_check(env, st));
}

static jobject new_entry(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_up_next_entry *e = item;
  jstring title = new_string(env, e->title ? e->title : "");
  jstring artist = new_string(env, e->artist);
  jobject out = (*env)->NewObject(env, cls, ctor, (jint)e->id, title, artist, JNI_BOOL(e->block), (jint)e->tracks,
                                  (jlong)e->duration_ms, (jint)e->first_item, (jint)e->context, JNI_BOOL(e->after_list),
                                  JNI_BOOL(e->rest));
  JNI_DELETE(title, artist);
  return out;
}

/* An UpNextEntry[] of up next now. */
JNI_FN(jobjectArray, CorePlayer, nativeUpNext, jlong handle) {
  mc_up_next_list *list = NULL;
  JNI_CHECK(mc_player_up_next(jni_player_of(handle), &list), NULL);
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("UpNextEntry"), "(I" J_STRING J_STRING "ZIJIIZZ)V", &info);
  jobjectArray out = ctor ? JNI_OBJECT_ARRAY(info, ctor, list->items, list->count, new_entry) : NULL;
  mc_up_next_list_free(list);
  return out;
}

/* An entry that has just played finds a move, remove or rename too late, which is no failure. */
static void check(JNIEnv *env, mc_status st) {
  if (st != MC_ERR_NOT_FOUND) jni_check(env, st);
}

JNI_FN(void, CorePlayer, nativeUpNextMove, jlong handle, jint id, jint to) {
  check(env, mc_player_up_next_move(jni_player_of(handle), (uint32_t)id, to));
}

JNI_FN(void, CorePlayer, nativeUpNextRemove, jlong handle, jint id) {
  check(env, mc_player_up_next_remove(jni_player_of(handle), (uint32_t)id));
}

JNI_FN(void, CorePlayer, nativeUpNextClear, jlong handle) {
  check(env, mc_player_up_next_clear(jni_player_of(handle)));
}

JNI_FN(void, CorePlayer, nativeUpNextRename, jlong handle, jint id, jstring title, jstring artist) {
  JNI_STR(ctitle, title);
  JNI_STR(cartist, artist);
  check(env, mc_player_up_next_rename(jni_player_of(handle), (uint32_t)id, ctitle, cartist));
}
