/* JNI bindings for ng.kithso.core.CoreBrowse: browsing the library beyond the album grid
 * (artists' pages, album groups, smart album groups), recents and search. Thin like
 * jni_bridge.c, whose helpers it shares. */
#include <jni.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#include "jni_bridge.h"

/* The albums of an artist (kind 0), an album group (1) or a smart album group (2). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreBrowse_nativeListAlbums(JNIEnv *env, jclass cls,
                                                                              jlong handle, jint kind,
                                                                              jstring uid) {
  (void)cls;
  static mc_status (*const lists[])(mc_library *, const char *, mc_album_list **) = {
    mc_library_list_artist_albums,
    mc_library_list_album_group_albums,
    mc_library_list_smart_albums,
  };
  if (kind < 0 || kind >= (jint)(sizeof lists / sizeof *lists)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  mc_album_list *list = NULL;
  mc_status st = lists[kind](jni_lib_of(handle), cuid, &list);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  return album_array(env, st, list);
}

/* Every track credited to an artist. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreBrowse_nativeArtistTracks(JNIEnv *env, jclass cls,
                                                                                jlong handle, jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_artist_tracks(jni_lib_of(handle), cuid, &list);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  return track_array(env, st, list);
}

/* What a smart playlist with rule would hold, without saving it. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreBrowse_nativePreviewSmartTracks(JNIEnv *env, jclass cls,
                                                                                      jlong handle, jstring rule) {
  (void)cls;
  const char *crule = (*env)->GetStringUTFChars(env, rule, NULL);
  if (!crule) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_smart_preview_tracks(jni_lib_of(handle), crule, &list);
  (*env)->ReleaseStringUTFChars(env, rule, crule);
  return track_array(env, st, list);
}

typedef mc_status (*art_fn)(mc_library *, const char *, const char *, char **);

/* The image art makes for uid in cache_dir, or NULL for a placeholder. */
static jstring art_of(JNIEnv *env, jlong handle, jstring uid, jstring cache_dir, art_fn art) {
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  const char *cdir = (*env)->GetStringUTFChars(env, cache_dir, NULL);
  if (!cdir) {
    (*env)->ReleaseStringUTFChars(env, uid, cuid);
    return NULL;
  }
  char *path = NULL;
  mc_status st = art(jni_lib_of(handle), cuid, cdir, &path);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  (*env)->ReleaseStringUTFChars(env, cache_dir, cdir);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring out = new_string(env, path);
  mc_free(path);
  return out;
}

/* The image to show for an album group (see mc_album_group_art()), or NULL for a placeholder. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreBrowse_nativeAlbumGroupArt(JNIEnv *env, jclass cls,
                                                                            jlong handle, jstring uid,
                                                                            jstring cache_dir) {
  (void)cls;
  return art_of(env, handle, uid, cache_dir, mc_album_group_art);
}

/* The image to show for a smart list (see mc_smart_art()), or NULL for a placeholder. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreBrowse_nativeSmartArt(JNIEnv *env, jclass cls, jlong handle,
                                                                       jstring uid, jstring cache_dir) {
  (void)cls;
  return art_of(env, handle, uid, cache_dir, mc_smart_art);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreBrowse_nativeReshuffle(JNIEnv *env, jclass cls, jlong handle,
                                                                     jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_smart_reshuffle(jni_lib_of(handle), cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* kind is an mc_recent_kind. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreBrowse_nativeRecordOpened(JNIEnv *env, jclass cls, jlong handle,
                                                                        jint kind, jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_library_record_opened(jni_lib_of(handle), (mc_recent_kind)kind, cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreBrowse_nativeRemoveRecent(JNIEnv *env, jclass cls, jlong handle,
                                                                        jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_library_remove_recent(jni_lib_of(handle), cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK && st != MC_ERR_NOT_FOUND) throw_status(env, st); /* gone already: sync took it */
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreBrowse_nativeClearRecents(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  mc_status st = mc_library_clear_recents(jni_lib_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

/* flags are mc_library_list_recents()'s (MC_LIST_PLAYABLE). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreBrowse_nativeListRecents(JNIEnv *env, jclass cls,
                                                                               jlong handle, jint limit,
                                                                               jint flags) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/RecentInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(ILjava/lang/String;JLjava/lang/String;Ljava/lang/String;Z)V");
  if (!ctor) return NULL;

  mc_recent_list *list = NULL;
  mc_status st = mc_library_list_recents(jni_lib_of(handle), (int)limit, (unsigned)flags, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  int n = list ? list->count : 0; /* an empty list may be NULL */
  jobjectArray out = (*env)->NewObjectArray(env, n, info, NULL);
  for (int i = 0; out && i < n; i++) {
    const mc_recent_info *r = &list->items[i];
    jstring uid = new_string(env, r->uid);
    jstring name = new_string(env, r->name ? r->name : "");
    jstring artist = new_string(env, r->artist);
    jobject item = (*env)->NewObject(env, info, ctor, (jint)r->kind, uid, (jlong)r->opened_ms, name, artist,
                                     (jboolean)(r->is_smart != 0));
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
    if (artist) (*env)->DeleteLocalRef(env, artist);
  }
  mc_recent_list_free(list);
  return out;
}

/* What matches query, best first, at most limit of each kind (all if <= 0). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreBrowse_nativeSearch(JNIEnv *env, jclass cls, jlong handle,
                                                                          jstring query, jint limit) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/SearchHit");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(
    env, info, "<init>",
    "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Z)V");
  if (!ctor) return NULL;

  const char *cquery = (*env)->GetStringUTFChars(env, query, NULL);
  if (!cquery) return NULL;
  mc_search_results *res = NULL;
  mc_status st = mc_library_search(jni_lib_of(handle), cquery, (int)limit, &res);
  (*env)->ReleaseStringUTFChars(env, query, cquery);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  int n = res ? res->count : 0;
  jobjectArray out = (*env)->NewObjectArray(env, n, info, NULL);
  for (int i = 0; out && i < n; i++) {
    const mc_search_hit *h = &res->items[i];
    jstring uid = new_string(env, h->uid);
    jstring name = new_string(env, h->name ? h->name : "");
    jstring artist = new_string(env, h->artist);
    jstring album = new_string(env, h->album);
    jstring album_uid = h->album_uid[0] ? new_string(env, h->album_uid) : NULL;
    jobject item = (*env)->NewObject(env, info, ctor, (jint)h->kind, uid, name, artist, album, album_uid,
                                     (jboolean)(h->is_smart != 0));
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
    if (artist) (*env)->DeleteLocalRef(env, artist);
    if (album) (*env)->DeleteLocalRef(env, album);
    if (album_uid) (*env)->DeleteLocalRef(env, album_uid);
  }
  mc_search_results_free(res);
  return out;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreBrowse_nativeSearchPrepare(JNIEnv *env, jclass cls, jlong handle) {
  (void)cls;
  mc_status st = mc_library_search_prepare(jni_lib_of(handle));
  if (st != MC_OK) throw_status(env, st);
}
