/* JNI bindings for ng.kithso.core.RadioNatives: the Home page's stations, and what it takes to
 * start one (musiccore/radio_stations.h, radio.h). As thin as it is; the player's side of a
 * station is in jni_bridge.c with the rest of CorePlayer. */
#include <jni.h>
#include <stdint.h>

#include "jni_bridge.h"
#include "kithcore/radio.h"
#include "kithcore/radio_stations.h"

/* A StationInfo[] as of now_ms, the most played first. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_RadioNatives_nativeStations(JNIEnv *env, jclass cls, jlong handle,
                                                                              jlong now_ms) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/StationInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                                       "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
  if (!ctor) return NULL;

  mc_radio_station_list *list = NULL;
  mc_status st = mc_radio_stations_list(jni_lib_of(handle), (int64_t)now_ms, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_radio_station_info *s = &list->items[i];
    jstring seed = new_string(env, s->seed_track);
    jstring start = new_string(env, s->start_track);
    jstring artist = new_string(env, s->artist_uid[0] ? s->artist_uid : NULL);
    jstring name = new_string(env, s->name);
    jstring sub = new_string(env, s->sub);
    jstring title = new_string(env, s->seed_title);
    jstring cover = new_string(env, s->cover_dir);
    jobject item = (*env)->NewObject(env, info, ctor, seed, start, artist, name, sub, title, cover);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, seed);
    (*env)->DeleteLocalRef(env, start);
    if (artist) (*env)->DeleteLocalRef(env, artist);
    (*env)->DeleteLocalRef(env, name);
    if (sub) (*env)->DeleteLocalRef(env, sub);
    if (title) (*env)->DeleteLocalRef(env, title);
    if (cover) (*env)->DeleteLocalRef(env, cover);
  }
  mc_radio_station_list_free(list);
  return out;
}

/* Whether uid can seed a station here (mc_radio_seed_check()). */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_RadioNatives_nativeSeedUsable(JNIEnv *env, jclass cls, jlong handle,
                                                                            jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return JNI_FALSE;
  mc_radio_options opts = {.seed_track = cuid};
  mc_status st = mc_radio_seed_check(jni_lib_of(handle), &opts);
  jni_release(env, uid, cuid);
  if (st != MC_OK && st != MC_ERR_NOT_FOUND) throw_status(env, st);
  return st == MC_OK ? JNI_TRUE : JNI_FALSE;
}

/* One track, as a TrackInfo[] of it, or of nothing if there's no such track. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_RadioNatives_nativeTrack(JNIEnv *env, jclass cls, jlong handle,
                                                                           jstring uid) {
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_library_get_track(jni_lib_of(handle), cuid, &list);
  jni_release(env, uid, cuid);
  if (st == MC_ERR_NOT_FOUND) {
    jclass info = (*env)->FindClass(env, "ng/kithso/core/TrackInfo");
    return info ? (*env)->NewObjectArray(env, 0, info, NULL) : NULL;
  }
  (void)cls;
  return track_array(env, st, list);
}
