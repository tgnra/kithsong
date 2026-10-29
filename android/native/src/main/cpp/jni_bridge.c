/* JNI bindings for ng.kithso.core.NativeCore, CoreLibrary, CoreImport, CorePlayer and CoreSync
 * (CoreScrobbler's are in jni_scrobbler.c). Keep this thin: marshal arguments, call musiccore,
 * convert results. Failures throw ng.kithso.core.MusicCoreException. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"

#include "jni_bridge.h"

static JavaVM *g_vm;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  g_vm = vm;
  return JNI_VERSION_1_6;
}

mc_status throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "ng/kithso/core/MusicCoreException");
  if (!cls) return st; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
  return st;
}

jstring new_string(JNIEnv *env, const char *s) {
  return s ? (*env)->NewStringUTF(env, s) : NULL;
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_NativeCore_version(JNIEnv *env, jclass cls) {
  (void)cls;
  return (*env)->NewStringUTF(env, mc_version());
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_NativeCore_init(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return mc_init();
}

/* The core's language for its own text, from a BCP 47 tag such as "fr-CA". */
JNIEXPORT void JNICALL Java_ng_kithso_core_NativeCore_setLocale(JNIEnv *env, jclass cls,
                                                               jstring tag) {
  (void)cls;
  const char *ctag = (*env)->GetStringUTFChars(env, tag, NULL);
  if (!ctag) return; /* OutOfMemoryError is pending */
  mc_strings_set_locale(ctag);
  (*env)->ReleaseStringUTFChars(env, tag, ctag);
}

/* Returns the number of albums, or a negative mc_status on failure. */
JNIEXPORT jlong JNICALL Java_ng_kithso_core_NativeCore_albumCount(JNIEnv *env, jclass cls,
                                                                 jstring path) {
  (void)cls;
  const char *cpath = (*env)->GetStringUTFChars(env, path, NULL);
  if (!cpath) return MC_ERR_NOMEM;

  mc_library *lib = NULL;
  int64_t count = 0;
  mc_status st = mc_library_open(cpath, &lib);
  (*env)->ReleaseStringUTFChars(env, path, cpath);
  if (st == MC_OK) st = mc_library_count(lib, MC_ENTITY_ALBUMS, &count);
  mc_library_close(lib);
  return st == MC_OK ? (jlong)count : (jlong)st;
}

/* The cover file in an album folder (mc_coverart_find()), or null. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_NativeCore_coverFile(JNIEnv *env, jclass cls, jstring dir) {
  (void)cls;
  const char *cdir = (*env)->GetStringUTFChars(env, dir, NULL);
  if (!cdir) return NULL;
  char *path = mc_coverart_find(cdir);
  (*env)->ReleaseStringUTFChars(env, dir, cdir);
  jstring out = new_string(env, path);
  mc_free(path);
  return out;
}

/* --- CoreLibrary ------------------------------------------------------------------- */

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreLibrary_nativeOpen(JNIEnv *env, jclass cls,
                                                                  jstring dir) {
  (void)cls;
  const char *cdir = (*env)->GetStringUTFChars(env, dir, NULL);
  if (!cdir) return 0;
  mc_library *lib = NULL;
  mc_status st = mc_library_open(cdir, &lib);
  (*env)->ReleaseStringUTFChars(env, dir, cdir);
  if (st != MC_OK) throw_status(env, st);
  return (jlong)(intptr_t)lib;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreLibrary_nativeClose(JNIEnv *env, jclass cls,
                                                                  jlong handle) {
  (void)env;
  (void)cls;
  mc_library_close((mc_library *)(intptr_t)handle);
}

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreLibrary_nativeCount(JNIEnv *env, jclass cls,
                                                                   jlong handle, jint entity) {
  (void)cls;
  int64_t count = 0;
  mc_status st = mc_library_count((mc_library *)(intptr_t)handle, (mc_entity)entity, &count);
  if (st != MC_OK) throw_status(env, st);
  return (jlong)count;
}

jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list) {
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/AlbumInfo");
  jmethodID ctor = info ? (*env)->GetMethodID(
                            env, info, "<init>",
                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZZLjava/lang/String;Ljava/lang/String;)V")
                        : NULL;
  if (ctor) out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_album_info *a = &list->items[i];
    jstring uid = new_string(env, a->uid);
    jstring title = new_string(env, a->title ? a->title : "");
    jstring artist = new_string(env, a->artist);
    jstring folder = new_string(env, a->folder);
    jstring artist_uid = new_string(env, a->artist_uid[0] ? a->artist_uid : NULL);
    jobject item = (*env)->NewObject(env, info, ctor, uid, title, artist, (jint)a->year,
                                     (jboolean)(a->is_singles != 0), (jboolean)(a->has_art != 0),
                                     folder, artist_uid);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, title);
    if (artist) (*env)->DeleteLocalRef(env, artist);
    if (folder) (*env)->DeleteLocalRef(env, folder);
    if (artist_uid) (*env)->DeleteLocalRef(env, artist_uid);
  }
  mc_album_list_free(list);
  return out;
}

JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListAlbums(JNIEnv *env,
                                                                               jclass cls,
                                                                               jlong handle) {
  (void)cls;
  mc_album_list *list = NULL;
  mc_status st = mc_library_list_albums((mc_library *)(intptr_t)handle, &list);
  return album_array(env, st, list);
}

/* What a smart album group with rule would hold, without saving it. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativePreviewSmartAlbums(JNIEnv *env,
                                                                                       jclass cls,
                                                                                       jlong handle,
                                                                                       jstring rule) {
  (void)cls;
  const char *crule = (*env)->GetStringUTFChars(env, rule, NULL);
  if (!crule) return NULL;
  mc_album_list *list = NULL;
  mc_status st = mc_smart_preview_albums((mc_library *)(intptr_t)handle, crule, &list);
  (*env)->ReleaseStringUTFChars(env, rule, crule);
  return album_array(env, st, list);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreLibrary_nativeRecordPlay(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_library_record_play((mc_library *)(intptr_t)handle, cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* The image to show for a playlist (see mc_playlist_art()), or NULL for a placeholder. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreLibrary_nativePlaylistArt(JNIEnv *env, jclass cls,
                                                                         jlong handle, jstring uid,
                                                                         jstring cache_dir) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  const char *cdir = (*env)->GetStringUTFChars(env, cache_dir, NULL);
  if (!cdir) {
    (*env)->ReleaseStringUTFChars(env, uid, cuid);
    return NULL;
  }
  char *path = NULL;
  mc_status st = mc_playlist_art((mc_library *)(intptr_t)handle, cuid, cdir, &path);
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

/* Deletes cached pictures of playlists that have gone (see mc_playlist_art_prune()). */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreLibrary_nativePlaylistArtPrune(JNIEnv *env, jclass cls, jlong handle,
                                                                           jstring cache_dir) {
  (void)cls;
  const char *cdir = (*env)->GetStringUTFChars(env, cache_dir, NULL);
  if (!cdir) return;
  mc_status st = mc_playlist_art_prune((mc_library *)(intptr_t)handle, cdir);
  (*env)->ReleaseStringUTFChars(env, cache_dir, cdir);
  if (st != MC_OK) throw_status(env, st);
}

jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list) {
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/TrackInfo");
  jmethodID ctor = info ? (*env)->GetMethodID(env, info, "<init>",
                                              "(Ljava/lang/String;Ljava/lang/String;IIILjava/lang/String;"
                                              "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
                        : NULL;
  if (ctor) out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_track_info *t = &list->items[i];
    jstring uid = new_string(env, t->uid);
    jstring title = new_string(env, t->title ? t->title : "");
    jstring path = new_string(env, t->path);
    jstring artist = new_string(env, t->artist);
    jstring album = new_string(env, t->album);
    jstring cover_dir = new_string(env, t->cover_dir);
    jstring artist_uid = new_string(env, t->artist_uid[0] ? t->artist_uid : NULL);
    jobject item = (*env)->NewObject(env, info, ctor, uid, title, (jint)t->track_no, (jint)t->disc_no,
                                     (jint)t->duration_ms, path, artist, album, cover_dir, artist_uid);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, title);
    if (path) (*env)->DeleteLocalRef(env, path);
    if (artist) (*env)->DeleteLocalRef(env, artist);
    if (album) (*env)->DeleteLocalRef(env, album);
    if (cover_dir) (*env)->DeleteLocalRef(env, cover_dir);
    if (artist_uid) (*env)->DeleteLocalRef(env, artist_uid);
  }
  mc_track_list_free(list);
  return out;
}

/* The tracks of an album (kind 0), a playlist (1) or a smart playlist (2). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListTracks(JNIEnv *env, jclass cls,
                                                                               jlong handle, jint kind,
                                                                               jstring uid) {
  (void)cls;
  static mc_status (*const lists[])(mc_library *, const char *, mc_track_list **) = {
    mc_library_list_album_tracks,
    mc_library_list_playlist_tracks,
    mc_library_list_smart_tracks,
  };
  if (kind < 0 || kind >= (jint)(sizeof lists / sizeof *lists)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  mc_track_list *list = NULL;
  mc_status st = lists[kind]((mc_library *)(intptr_t)handle, cuid, &list);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  return track_array(env, st, list);
}

/* flags are mc_library_list_playlists()'s (MC_LIST_PLAYABLE). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListPlaylists(JNIEnv *env,
                                                                                  jclass cls,
                                                                                  jlong handle,
                                                                                  jint flags) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/PlaylistInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V");
  if (!ctor) return NULL;

  mc_playlist_list *list = NULL;
  mc_status st = mc_library_list_playlists((mc_library *)(intptr_t)handle, (unsigned)flags, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_playlist_info *p = &list->items[i];
    jstring uid = new_string(env, p->uid);
    jstring name = new_string(env, p->name ? p->name : "");
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)p->track_count, (jint)p->playable_count);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
  }
  mc_playlist_list_free(list);
  return out;
}

/* flags are mc_library_list_smart()'s (MC_LIST_PLAYABLE). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListSmart(JNIEnv *env, jclass cls,
                                                                              jlong handle, jint flags) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/SmartInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;Z)V");
  if (!ctor) return NULL;

  mc_smart_list *list = NULL;
  mc_status st = mc_library_list_smart((mc_library *)(intptr_t)handle, (unsigned)flags, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_smart_info *s = &list->items[i];
    jstring uid = new_string(env, s->uid);
    jstring name = new_string(env, s->name ? s->name : "");
    jstring rule = new_string(env, s->rule ? s->rule : "");
    jobject item = (*env)->NewObject(env, info, ctor, uid, name,
                                     (jboolean)(s->subject == MC_SMART_ALBUMS), rule,
                                     (jboolean)(s->random != 0));
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
    (*env)->DeleteLocalRef(env, rule);
  }
  mc_smart_list_free(list);
  return out;
}

/* A NamedInfo[] of n items, item i's uid, name and album count from get(items, i), or NULL
 * with an exception pending. */
static jobjectArray named_array(JNIEnv *env, const void *items, int n,
                                void (*get)(const void *, int, const char **, const char **, int *)) {
  jclass info = (*env)->FindClass(env, "ng/kithso/core/NamedInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V");
  if (!ctor) return NULL;
  jobjectArray out = (*env)->NewObjectArray(env, n, info, NULL);
  for (int i = 0; out && i < n; i++) {
    const char *cuid, *cname;
    int count;
    get(items, i, &cuid, &cname, &count);
    jstring uid = new_string(env, cuid);
    jstring name = new_string(env, cname ? cname : "");
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)count);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
  }
  return out;
}

static void artist_at(const void *items, int i, const char **uid, const char **name, int *count) {
  const mc_artist_info *a = &((const mc_artist_info *)items)[i];
  *uid = a->uid;
  *name = a->name;
  *count = a->album_count;
}

static void album_group_at(const void *items, int i, const char **uid, const char **name,
                           int *count) {
  const mc_album_group_info *g = &((const mc_album_group_info *)items)[i];
  *uid = g->uid;
  *name = g->name;
  *count = g->album_count;
}

JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListArtists(JNIEnv *env,
                                                                                jclass cls,
                                                                                jlong handle) {
  (void)cls;
  mc_artist_list *list = NULL;
  mc_status st = mc_library_list_artists((mc_library *)(intptr_t)handle, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  /* An empty list may be NULL. */
  jobjectArray out =
    named_array(env, list ? list->items : NULL, list ? list->count : 0, artist_at);
  mc_artist_list_free(list);
  return out;
}

/* The artists with something here (mc_library_list_artists_here()), as ArtistInfo[]. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListArtistsHere(JNIEnv *env,
                                                                                    jclass cls,
                                                                                    jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/ArtistInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>", "(Ljava/lang/String;Ljava/lang/String;II)V");
  if (!ctor) return NULL;

  mc_artist_list *list = NULL;
  mc_status st = mc_library_list_artists_here((mc_library *)(intptr_t)handle, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_artist_info *a = &list->items[i];
    jstring uid = new_string(env, a->uid);
    jstring name = new_string(env, a->name ? a->name : "");
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)a->album_count, (jint)a->track_count);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
  }
  mc_artist_list_free(list);
  return out;
}

JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreLibrary_nativeListAlbumGroups(JNIEnv *env,
                                                                                    jclass cls,
                                                                                    jlong handle) {
  (void)cls;
  mc_album_group_list *list = NULL;
  mc_status st = mc_library_list_album_groups((mc_library *)(intptr_t)handle, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  /* An empty list may be NULL. */
  jobjectArray out =
    named_array(env, list ? list->items : NULL, list ? list->count : 0, album_group_at);
  mc_album_group_list_free(list);
  return out;
}

/* --- CoreImport -------------------------------------------------------------------- */

typedef struct jni_import {
  mc_import *imp;
  jobject listener; /* global ref to the CoreImport, whose onEvent(int, int) gets the events */
  jmethodID on_event;
} jni_import;

/* On the import thread, which the JVM doesn't know: attach it for the call. */
static void on_import_event(void *user, mc_import_event event, int arg) {
  jni_import *p = user;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    attached = 1;
  }
  (*env)->CallVoidMethod(env, p->listener, p->on_event, (jint)event, (jint)arg);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static mc_import *import_of(jlong handle) {
  return ((jni_import *)(intptr_t)handle)->imp;
}

/* staging_dir may be null: then zips are extracted in the system's temporary folder. */
JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreImport_nativeStart(JNIEnv *env, jclass cls,
                                                                  jlong library, jobjectArray paths,
                                                                  jboolean fetch_art,
                                                                  jboolean skip_tagging,
                                                                  jstring staging_dir,
                                                                  jobject listener) {
  (void)cls;
  jni_import *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  p->on_event = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, listener), "onEvent", "(II)V");
  p->listener = p->on_event ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }

  jsize count = (*env)->GetArrayLength(env, paths);
  const char **cpaths = calloc(count ? (size_t)count : 1, sizeof *cpaths);
  jstring *jpaths = calloc(count ? (size_t)count : 1, sizeof *jpaths);
  mc_status st = cpaths && jpaths ? MC_OK : MC_ERR_NOMEM;
  jsize got = 0;
  for (; st == MC_OK && got < count; got++) {
    jpaths[got] = (*env)->GetObjectArrayElement(env, paths, got);
    cpaths[got] = jpaths[got] ? (*env)->GetStringUTFChars(env, jpaths[got], NULL) : NULL;
    if (!cpaths[got]) st = MC_ERR_INVALID_ARG;
  }
  const char *cstaging = staging_dir ? (*env)->GetStringUTFChars(env, staging_dir, NULL) : NULL;
  if (staging_dir && !cstaging) st = MC_ERR_NOMEM;
  /* mc_import_start copies the paths and the staging folder. */
  mc_import_options opts = {
      .threshold = 0,
      .fetch_art = fetch_art ? 1 : 0,
      .skip_tagging = skip_tagging ? 1 : 0,
      .staging_dir = cstaging,
  };
  if (st == MC_OK) {
    st = mc_import_start((mc_library *)(intptr_t)library, cpaths, count, &opts, on_import_event, p,
                         &p->imp);
  }
  if (cstaging) (*env)->ReleaseStringUTFChars(env, staging_dir, cstaging);
  for (jsize i = 0; i < got; i++) {
    if (cpaths[i]) (*env)->ReleaseStringUTFChars(env, jpaths[i], cpaths[i]);
    if (jpaths[i]) (*env)->DeleteLocalRef(env, jpaths[i]);
  }
  free(cpaths);
  free(jpaths);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    if (!(*env)->ExceptionCheck(env)) throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)p;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreImport_nativeFree(JNIEnv *env, jclass cls,
                                                                jlong handle) {
  (void)cls;
  jni_import *p = (jni_import *)(intptr_t)handle;
  if (!p) return;
  mc_import_free(p->imp); /* waits for the import thread, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreImport_nativeItemCount(JNIEnv *env, jclass cls,
                                                                     jlong handle) {
  (void)env;
  (void)cls;
  return mc_import_item_count(import_of(handle));
}

/* The item's mc_import_state. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreImport_nativeItemState(JNIEnv *env, jclass cls,
                                                                     jlong handle, jint item) {
  (void)cls;
  mc_import_item *it = NULL;
  mc_status st = mc_import_item_get(import_of(handle), item, &it);
  if (st != MC_OK) {
    throw_status(env, st);
    return 0;
  }
  jint state = (jint)it->state;
  mc_import_item_free(it);
  return state;
}

/* What to call the item: "Artist – Album" (the core's track_byline) from its tags, else its
 * folder. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreImport_nativeItemName(JNIEnv *env, jclass cls,
                                                                      jlong handle, jint item) {
  (void)cls;
  mc_import_item *it = NULL;
  mc_status st = mc_import_item_get(import_of(handle), item, &it);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring name;
  if (it->artist && it->album) {
    char *text = mc_format_alloc(mc_str(MC_STR_TRACK_BYLINE), MC_S(it->artist), MC_S(it->album));
    name = new_string(env, text ? text : it->album);
    mc_free(text);
  } else {
    name = new_string(env, it->album ? it->album : it->folder ? it->folder : "");
  }
  mc_import_item_free(it);
  return name;
}

/* Why the item failed or needs review, or null. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreImport_nativeItemError(JNIEnv *env, jclass cls,
                                                                       jlong handle, jint item) {
  (void)cls;
  mc_import_item *it = NULL;
  mc_status st = mc_import_item_get(import_of(handle), item, &it);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring error = new_string(env, it->error);
  mc_import_item_free(it);
  return error;
}

/* Fills out[i] with s, or "" for NULL. Returns 0 if out of memory. */
static int put_string(JNIEnv *env, jobjectArray out, jsize i, const char *s) {
  jstring j = (*env)->NewStringUTF(env, s ? s : "");
  if (!j) return 0;
  (*env)->SetObjectArrayElement(env, out, i, j);
  (*env)->DeleteLocalRef(env, j);
  return 1;
}

static int put_int(JNIEnv *env, jobjectArray out, jsize i, long long v) {
  char num[32];
  snprintf(num, sizeof num, "%lld", v);
  return put_string(env, out, i, num);
}

/* A snapshot of the item as strings, in CoreImport.ItemInfo's order: kind, state, file count,
 * artist, album, folder, error, searched wider, outcome, chosen, match count. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreImport_nativeItemInfo(JNIEnv *env, jclass cls,
                                                                           jlong handle, jint item) {
  (void)cls;
  mc_import_item *it = NULL;
  mc_status st = mc_import_item_get(import_of(handle), item, &it);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  jobjectArray out = string_class ? (*env)->NewObjectArray(env, 11, string_class, NULL) : NULL;
  if (out && !(put_int(env, out, 0, it->kind) && put_int(env, out, 1, it->state) &&
               put_int(env, out, 2, it->file_count) && put_string(env, out, 3, it->artist) &&
               put_string(env, out, 4, it->album) && put_string(env, out, 5, it->folder) &&
               put_string(env, out, 6, it->error) && put_int(env, out, 7, it->searched_wider) &&
               put_int(env, out, 8, it->outcome) && put_int(env, out, 9, it->chosen) &&
               put_int(env, out, 10, it->match_count)))
    out = NULL;
  mc_import_item_free(it);
  return out;
}

#define MATCH_FIELDS 10

/* The item's candidates, best first, flattened MATCH_FIELDS strings each, in
 * CoreImport.Match's order: album, album artist, release, year, confidence in thousandths,
 * track count, format, country, label, whether it's a single. */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreImport_nativeItemMatches(JNIEnv *env, jclass cls,
                                                                              jlong handle, jint item) {
  (void)cls;
  mc_import_item *it = NULL;
  mc_status st = mc_import_item_get(import_of(handle), item, &it);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jclass string_class = (*env)->FindClass(env, "java/lang/String");
  jobjectArray out =
      string_class ? (*env)->NewObjectArray(env, it->match_count * MATCH_FIELDS, string_class, NULL) : NULL;
  for (int i = 0; out && i < it->match_count; i++) {
    const mc_import_match *m = &it->matches[i];
    jsize at = i * MATCH_FIELDS;
    if (!(put_string(env, out, at, m->album) && put_string(env, out, at + 1, m->album_artist) &&
          put_string(env, out, at + 2, m->release) && put_int(env, out, at + 3, m->year) &&
          put_int(env, out, at + 4, (long long)(m->confidence * 1000 + 0.5)) &&
          put_int(env, out, at + 5, m->track_count) && put_string(env, out, at + 6, m->format) &&
          put_string(env, out, at + 7, m->country) && put_string(env, out, at + 8, m->label) &&
          put_int(env, out, at + 9, m->is_single)))
      out = NULL;
  }
  mc_import_item_free(it);
  return out;
}

/* The calls that resolve an item needing review: 0 accept as is, 1 skip, 2 search more widely. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreImport_nativeResolve(JNIEnv *env, jclass cls,
                                                                   jlong handle, jint item,
                                                                   jint how) {
  (void)cls;
  static mc_status (*const calls[])(mc_import *, int) = {mc_import_accept_as_is, mc_import_skip,
                                                         mc_import_search_wider};
  if (how < 0 || how >= (jint)(sizeof calls / sizeof *calls)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  mc_status st = calls[how](import_of(handle), item);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreImport_nativeAccept(JNIEnv *env, jclass cls,
                                                                  jlong handle, jint item,
                                                                  jint match) {
  (void)cls;
  mc_status st = mc_import_accept(import_of(handle), item, match);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreImport_nativeSearch(JNIEnv *env, jclass cls,
                                                                  jlong handle, jint item,
                                                                  jstring artist, jstring title) {
  (void)cls;
  const char *a = artist ? (*env)->GetStringUTFChars(env, artist, NULL) : NULL;
  const char *t = title ? (*env)->GetStringUTFChars(env, title, NULL) : NULL;
  mc_status st = (artist && !a) || (title && !t) ? MC_ERR_NOMEM : mc_import_search(import_of(handle), item, a, t);
  if (a) (*env)->ReleaseStringUTFChars(env, artist, a);
  if (t) (*env)->ReleaseStringUTFChars(env, title, t);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreImport_nativeSkipTagging(JNIEnv *env, jclass cls,
                                                                       jlong handle) {
  (void)cls;
  mc_status st = mc_import_skip_tagging(import_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreImport_nativeOffline(JNIEnv *env, jclass cls,
                                                                       jlong handle) {
  (void)env;
  (void)cls;
  return mc_import_offline(import_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

/* --- CorePlayer -------------------------------------------------------------------- */

typedef struct jni_player {
  mc_player *player;
  jobject listener; /* global ref to the CorePlayer, whose onEvent(int) gets the events */
  jmethodID on_event;
} jni_player;

/* On the player's thread, which the JVM doesn't know: attach it for the call. */
static void on_player_event(void *user, mc_player_event event) {
  jni_player *p = user;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    attached = 1;
  }
  (*env)->CallVoidMethod(env, p->listener, p->on_event, (jint)event);
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static mc_player *player_of(jlong handle) {
  return ((jni_player *)(intptr_t)handle)->player;
}

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CorePlayer_nativeNew(JNIEnv *env, jclass cls,
                                                                jobject listener) {
  (void)cls;
  jni_player *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  p->on_event = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, listener), "onEvent", "(I)V");
  p->listener = p->on_event ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }
  mc_status st = mc_player_new(on_player_event, p, &p->player);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)p;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeFree(JNIEnv *env, jclass cls,
                                                                jlong handle) {
  (void)cls;
  jni_player *p = (jni_player *)(intptr_t)handle;
  if (!p) return;
  mc_player_free(p->player); /* first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

/* mc_player_set_queue(), or if load, mc_player_load_queue(). */
static void set_queue(JNIEnv *env, jlong handle, jobjectArray paths, jint start, int load) {
  jsize count = (*env)->GetArrayLength(env, paths);
  const char **cpaths = calloc(count ? (size_t)count : 1, sizeof *cpaths);
  jstring *jpaths = calloc(count ? (size_t)count : 1, sizeof *jpaths);
  mc_status st = cpaths && jpaths ? MC_OK : MC_ERR_NOMEM;
  jsize got = 0;
  for (; st == MC_OK && got < count; got++) {
    jpaths[got] = (*env)->GetObjectArrayElement(env, paths, got);
    cpaths[got] = jpaths[got] ? (*env)->GetStringUTFChars(env, jpaths[got], NULL) : NULL;
    if (!cpaths[got]) st = MC_ERR_INVALID_ARG;
  }
  /* Both copy the paths. */
  if (st == MC_OK) {
    st = load ? mc_player_load_queue(player_of(handle), cpaths, count, start)
              : mc_player_set_queue(player_of(handle), cpaths, count, start);
  }
  for (jsize i = 0; i < got; i++) {
    if (cpaths[i]) (*env)->ReleaseStringUTFChars(env, jpaths[i], cpaths[i]);
    if (jpaths[i]) (*env)->DeleteLocalRef(env, jpaths[i]);
  }
  free(cpaths);
  free(jpaths);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSetQueue(JNIEnv *env, jclass cls,
                                                                    jlong handle,
                                                                    jobjectArray paths,
                                                                    jint start) {
  (void)cls;
  set_queue(env, handle, paths, start, 0);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeLoadQueue(JNIEnv *env, jclass cls,
                                                                     jlong handle,
                                                                     jobjectArray paths,
                                                                     jint start) {
  (void)cls;
  set_queue(env, handle, paths, start, 1);
}

/* The player calls that take no arguments: 0 toggle, 1 pause, 2 resume, 3 stop, 4 next,
 * 5 previous. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeCommand(JNIEnv *env, jclass cls,
                                                                   jlong handle, jint command) {
  (void)cls;
  static mc_status (*const commands[])(mc_player *) = {
    mc_player_toggle, mc_player_pause, mc_player_resume,
    mc_player_stop,   mc_player_next,  mc_player_previous,
  };
  if (command < 0 || command >= (jint)(sizeof commands / sizeof *commands)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  mc_status st = commands[command](player_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSkipTo(JNIEnv *env, jclass cls,
                                                                  jlong handle, jint index) {
  (void)cls;
  mc_status st = mc_player_skip_to(player_of(handle), index);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSeekMs(JNIEnv *env, jclass cls,
                                                                  jlong handle, jlong ms) {
  (void)cls;
  mc_status st = mc_player_seek_ms(player_of(handle), ms);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSetShuffle(JNIEnv *env, jclass cls,
                                                                      jlong handle, jboolean on) {
  (void)cls;
  mc_status st = mc_player_set_shuffle(player_of(handle), on ? 1 : 0);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSetVolume(JNIEnv *env, jclass cls,
                                                                     jlong handle, jint volume) {
  (void)cls;
  mc_status st = mc_player_set_volume(player_of(handle), volume);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeSetMuted(JNIEnv *env, jclass cls,
                                                                    jlong handle, jboolean muted) {
  (void)cls;
  mc_status st = mc_player_set_muted(player_of(handle), muted ? 1 : 0);
  if (st != MC_OK) throw_status(env, st);
}

/* The queue items at places from to from + count - 1 around the current one (mc_player_order). */
JNIEXPORT jintArray JNICALL Java_ng_kithso_core_CorePlayer_nativeOrder(JNIEnv *env, jclass cls,
                                                                     jlong handle, jint from,
                                                                     jint count) {
  (void)cls;
  if (count < 0) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return NULL;
  }
  _Static_assert(sizeof(jint) == sizeof(int), "jint is int");
  int *order = malloc(count ? (size_t)count * sizeof *order : 1);
  if (!order) {
    throw_status(env, MC_ERR_NOMEM);
    return NULL;
  }
  mc_status st = mc_player_order(player_of(handle), from, count, order);
  jintArray out = st == MC_OK ? (*env)->NewIntArray(env, count) : NULL;
  if (out) (*env)->SetIntArrayRegion(env, out, 0, count, (const jint *)order);
  free(order);
  if (st != MC_OK) throw_status(env, st);
  return out;
}

/* Fills out with state, index, count, position_ms, duration_ms, shuffle, volume, muted. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CorePlayer_nativeStatus(JNIEnv *env, jclass cls,
                                                                  jlong handle, jlongArray out) {
  (void)cls;
  mc_player_status st;
  mc_player_status_get(player_of(handle), &st);
  jlong values[8] = {st.state, st.index, st.count, st.position_ms, st.duration_ms, st.shuffle, st.volume, st.muted};
  (*env)->SetLongArrayRegion(env, out, 0, 8, values);
}

/* Fills out with the item and start (wall clock ms) of the oldest play that counts; false if
 * there's none. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CorePlayer_nativeTakePlayed(JNIEnv *env, jclass cls,
                                                                         jlong handle,
                                                                         jlongArray out) {
  (void)cls;
  int index;
  int64_t started_ms;
  if (mc_player_take_played(player_of(handle), &index, &started_ms) != MC_OK) return JNI_FALSE;
  jlong values[2] = {index, started_ms};
  (*env)->SetLongArrayRegion(env, out, 0, 2, values);
  return JNI_TRUE;
}

/* --- CoreSync ---------------------------------------------------------------------- */


typedef struct jni_sync {
  mc_sync *sync;
  jobject listener; /* global ref to the CoreSync, whose on* methods get the events */
  jmethodID on_peer, on_pair_code, on_pair_result, on_progress, on_sync_done;
  jmethodID on_pair_merge, on_merge_progress, on_merge_done;
} jni_sync;

/* The current core thread's JNIEnv, attaching the thread if the JVM doesn't know it, or NULL.
 * Pass *attached to detach() afterwards. */
static JNIEnv *attach(int *attached) {
  JNIEnv *env = NULL;
  *attached = 0;
  if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    *attached = 1;
  }
  return env;
}

static void detach(JNIEnv *env, int attached) {
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
  if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static void on_sync_peer(void *user, const char *device_uid, int online, int member) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, device_uid ? device_uid : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_peer, uid, (jboolean)(online != 0),
                         (jboolean)(member != 0));
  if (uid) (*env)->DeleteLocalRef(env, uid);
  detach(env, attached);
}

static void on_sync_pair_code(void *user, const char *code, const char *requester_name,
                              int attempts_left) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring jcode = new_string(env, code ? code : "");
  jstring name = new_string(env, requester_name ? requester_name : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_pair_code, jcode, name, (jint)attempts_left);
  if (jcode) (*env)->DeleteLocalRef(env, jcode);
  if (name) (*env)->DeleteLocalRef(env, name);
  detach(env, attached);
}

static void on_sync_pair_result(void *user, const mc_pair_result *r) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, r->device_uid ? r->device_uid : "");
  jstring name = new_string(env, r->name ? r->name : "");
  jstring archive = new_string(env, r->archive_name ? r->archive_name : "");
  jstring scrobble = new_string(env, r->scrobble_signed_out ? r->scrobble_signed_out : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_pair_result, uid, name,
                         (jboolean)(r->initiator != 0), (jint)r->reason, (jint)r->attempts_left,
                         (jint)r->status, (jboolean)(r->joined != 0), archive, (jint)r->archives,
                         (jboolean)(r->merged != 0), (jboolean)(r->kept_here != 0), scrobble);
  if (uid) (*env)->DeleteLocalRef(env, uid);
  if (name) (*env)->DeleteLocalRef(env, name);
  if (archive) (*env)->DeleteLocalRef(env, archive);
  if (scrobble) (*env)->DeleteLocalRef(env, scrobble);
  detach(env, attached);
}

/* Merging libraries: the proposal, with forget_names as a String[]. */
static void on_sync_pair_merge(void *user, const mc_pair_merge *m) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, m->device_uid ? m->device_uid : "");
  jstring name = new_string(env, m->name ? m->name : "");
  jstring scrobble = new_string(env, m->scrobble_user ? m->scrobble_user : "");
  int count = m->forget_names && m->forget_count > 0 ? m->forget_count : 0;
  jclass string_cls = (*env)->FindClass(env, "java/lang/String");
  jobjectArray forget = string_cls ? (*env)->NewObjectArray(env, count, string_cls, NULL) : NULL;
  for (int i = 0; forget && i < count; i++) {
    jstring n = new_string(env, m->forget_names[i] ? m->forget_names[i] : "");
    if (!n) break;
    (*env)->SetObjectArrayElement(env, forget, i, n);
    (*env)->DeleteLocalRef(env, n);
  }
  if (uid && name && scrobble && forget && !(*env)->ExceptionCheck(env)) {
    (*env)->CallVoidMethod(env, p->listener, p->on_pair_merge, uid, name,
                           (jboolean)(m->initiator != 0), (jboolean)(m->kept_here != 0),
                           (jboolean)(m->needs_confirm != 0), (jlong)m->tracks_here,
                           (jlong)m->tracks_there, (jlong)m->bytes_here, (jlong)m->bytes_there,
                           (jboolean)(m->scrobble_sign_out != 0), scrobble, (jint)m->forget_count,
                           forget);
  }
  if (uid) (*env)->DeleteLocalRef(env, uid);
  if (name) (*env)->DeleteLocalRef(env, name);
  if (scrobble) (*env)->DeleteLocalRef(env, scrobble);
  if (forget) (*env)->DeleteLocalRef(env, forget);
  if (string_cls) (*env)->DeleteLocalRef(env, string_cls);
  detach(env, attached);
}

static void on_sync_merge_progress(void *user, const mc_merge_progress *pr) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  (*env)->CallVoidMethod(env, p->listener, p->on_merge_progress, (jint)pr->phase, (jint)pr->done,
                         (jint)pr->total);
  detach(env, attached);
}

/* The counts go as an int[], in the order CoreSync.onMergeDone reads them. */
static void on_sync_merge_done(void *user, const mc_merge_result *r) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  const jint counts[] = {
    r->tracks_added,      r->tracks_attached, r->tracks_better,    r->tracks_skipped,
    r->tracks_mapped,     r->tracks_left_out, r->tracks_failed,    r->playlists,
    r->playlists_renamed, r->album_groups,    r->smart_lists,      r->plays,
    r->recents,           r->scrobbles_sent,  r->scrobbles_dropped,
  };
  const jsize n = (jsize)(sizeof counts / sizeof *counts);
  jintArray jcounts = (*env)->NewIntArray(env, n);
  if (jcounts) (*env)->SetIntArrayRegion(env, jcounts, 0, n, counts);
  jstring backup = new_string(env, r->backup_path ? r->backup_path : "");
  jstring scrobble = new_string(env, r->scrobble_signed_out ? r->scrobble_signed_out : "");
  if (jcounts && backup && scrobble) {
    (*env)->CallVoidMethod(env, p->listener, p->on_merge_done, (jint)r->status, backup, jcounts,
                           scrobble);
  }
  if (jcounts) (*env)->DeleteLocalRef(env, jcounts);
  if (backup) (*env)->DeleteLocalRef(env, backup);
  if (scrobble) (*env)->DeleteLocalRef(env, scrobble);
  detach(env, attached);
}

static void on_sync_progress(void *user, const mc_sync_progress *pr) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, pr->device_uid ? pr->device_uid : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_progress, uid, (jint)pr->phase,
                         (jlong)pr->rows_sent, (jlong)pr->rows_received, (jint)pr->files_done,
                         (jint)pr->files_total, (jlong)pr->bytes_received, (jlong)pr->bytes_sent);
  if (uid) (*env)->DeleteLocalRef(env, uid);
  detach(env, attached);
}

static void on_sync_done(void *user, const mc_sync_result *r) {
  jni_sync *p = user;
  int attached;
  JNIEnv *env = attach(&attached);
  if (!env) return;
  jstring uid = new_string(env, r->device_uid ? r->device_uid : "");
  (*env)->CallVoidMethod(env, p->listener, p->on_sync_done, uid, (jint)r->status,
                         (jboolean)(r->library_synced != 0), (jboolean)(r->converged != 0),
                         (jlong)r->rows_sent, (jlong)r->rows_received, (jint)r->files_received,
                         (jint)r->files_missing, (jint)r->files_corrupt, (jint)r->files_deleted,
                         (jint)r->files_unknown, (jint)r->files_sent, (jlong)r->bytes_received,
                         (jlong)r->bytes_sent, (jint)r->covers_received,
                         (jboolean)(r->cancelled != 0));
  if (uid) (*env)->DeleteLocalRef(env, uid);
  detach(env, attached);
}

static mc_sync *sync_of(jlong handle) {
  return ((jni_sync *)(intptr_t)handle)->sync;
}

mc_sync *jni_sync_of(jlong handle) {
  return sync_of(handle);
}

JNIEXPORT jlong JNICALL Java_ng_kithso_core_CoreSync_nativeNew(JNIEnv *env, jclass cls,
                                                              jlong library, jstring config_dir,
                                                              jstring device_name, jint kind,
                                                              jobject listener) {
  (void)cls;
  jni_sync *p = calloc(1, sizeof *p);
  if (!p) {
    throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  jclass lcls = (*env)->GetObjectClass(env, listener);
  static const struct {
    const char *name, *sig;
  } methods[] = {
    {"onPeer", "(Ljava/lang/String;ZZ)V"},
    {"onPairCode", "(Ljava/lang/String;Ljava/lang/String;I)V"},
    {"onPairResult", "(Ljava/lang/String;Ljava/lang/String;ZIIIZLjava/lang/String;IZZLjava/lang/String;)V"},
    {"onProgress", "(Ljava/lang/String;IJJIIJJ)V"},
    {"onSyncDone", "(Ljava/lang/String;IZZJJIIIIIIJJIZ)V"},
    {"onPairMerge", "(Ljava/lang/String;Ljava/lang/String;ZZZJJJJZLjava/lang/String;I[Ljava/lang/String;)V"},
    {"onMergeProgress", "(III)V"},
    {"onMergeDone", "(ILjava/lang/String;[ILjava/lang/String;)V"},
  };
  jmethodID *ids[] = {&p->on_peer,       &p->on_pair_code,      &p->on_pair_result,
                      &p->on_progress,   &p->on_sync_done,      &p->on_pair_merge,
                      &p->on_merge_progress, &p->on_merge_done};
  int found = 1;
  for (size_t i = 0; found && i < sizeof methods / sizeof *methods; i++) {
    *ids[i] = (*env)->GetMethodID(env, lcls, methods[i].name, methods[i].sig);
    found = *ids[i] != NULL;
  }
  p->listener = found ? (*env)->NewGlobalRef(env, listener) : NULL;
  if (!p->listener) {
    free(p);
    return 0;
  }

  const char *cdir = (*env)->GetStringUTFChars(env, config_dir, NULL);
  const char *cname = cdir ? (*env)->GetStringUTFChars(env, device_name, NULL) : NULL;
  mc_status st = MC_ERR_NOMEM;
  if (cname) {
    mc_sync_events events = {
      .on_peer = on_sync_peer,
      .on_pair_code = on_sync_pair_code,
      .on_pair_result = on_sync_pair_result,
      .on_progress = on_sync_progress,
      .on_sync_done = on_sync_done,
      /* Setting on_pair_merge is what lets pairing merge two libraries with music. */
      .on_pair_merge = on_sync_pair_merge,
      .on_merge_progress = on_sync_merge_progress,
      .on_merge_done = on_sync_merge_done,
    };
    /* mc_sync_new copies the strings and the events. */
    st = mc_sync_new((mc_library *)(intptr_t)library, cdir, cname, (mc_device_kind)kind, &events,
                     p, &p->sync);
  }
  if (cname) (*env)->ReleaseStringUTFChars(env, device_name, cname);
  if (cdir) (*env)->ReleaseStringUTFChars(env, config_dir, cdir);
  if (st != MC_OK) {
    (*env)->DeleteGlobalRef(env, p->listener);
    free(p);
    if (!(*env)->ExceptionCheck(env)) throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)p;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeFree(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)cls;
  jni_sync *p = (jni_sync *)(intptr_t)handle;
  if (!p) return;
  mc_sync_free(p->sync); /* stops first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreSync_nativeDeviceUid(JNIEnv *env, jclass cls,
                                                                     jlong handle) {
  (void)cls;
  return new_string(env, mc_sync_device_uid(sync_of(handle)));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeStart(JNIEnv *env, jclass cls,
                                                               jlong handle) {
  (void)cls;
  mc_status st = mc_sync_start(sync_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeStop(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_stop(sync_of(handle));
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreSync_nativePort(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)env;
  (void)cls;
  return mc_sync_port(sync_of(handle));
}

/* The calls that take a device uid: 0 pair request, 1 remove device, 2 sync now, 3 confirm
 * and sync now (the Sync button), 4 background sync (rate-limited), 5 cancel. For 2 to 5, uid
 * null is every member. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeWithDevice(JNIEnv *env, jclass cls,
                                                                    jlong handle, jint call,
                                                                    jstring uid) {
  (void)cls;
  static mc_status (*const calls[])(mc_sync *, const char *) = {
    mc_sync_pair_request,
    mc_sync_remove_device,
    mc_sync_now,
    mc_sync_confirm_now,
    mc_sync_background,
    mc_sync_cancel,
  };
  if (call < 0 || call >= (jint)(sizeof calls / sizeof *calls)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  const char *cuid = uid ? (*env)->GetStringUTFChars(env, uid, NULL) : NULL;
  if (uid && !cuid) return;
  mc_status st = calls[call](sync_of(handle), cuid);
  if (cuid) (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativePairRequestAddress(JNIEnv *env, jclass cls,
                                                                            jlong handle,
                                                                            jstring host,
                                                                            jint port) {
  (void)cls;
  const char *chost = (*env)->GetStringUTFChars(env, host, NULL);
  if (!chost) return;
  mc_status st = mc_sync_pair_request_address(sync_of(handle), chost, (uint16_t)port);
  (*env)->ReleaseStringUTFChars(env, host, chost);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativePairEnter(JNIEnv *env, jclass cls,
                                                                   jlong handle, jstring code) {
  (void)cls;
  const char *ccode = (*env)->GetStringUTFChars(env, code, NULL);
  if (!ccode) return;
  mc_status st = mc_sync_pair_enter(sync_of(handle), ccode);
  (*env)->ReleaseStringUTFChars(env, code, ccode);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativePairCancel(JNIEnv *env, jclass cls,
                                                                    jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_pair_cancel(sync_of(handle));
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativePairMergeConfirm(JNIEnv *env, jclass cls,
                                                                         jlong handle,
                                                                         jboolean merge) {
  (void)cls;
  mc_status st = mc_sync_pair_merge_confirm(sync_of(handle), merge != JNI_FALSE);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT jboolean JNICALL Java_ng_kithso_core_CoreSync_nativeMergePending(JNIEnv *env, jclass cls,
                                                                         jlong handle) {
  (void)env;
  (void)cls;
  return mc_sync_merge_pending(sync_of(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeLeaveGroup(JNIEnv *env, jclass cls,
                                                                    jlong handle) {
  (void)cls;
  mc_status st = mc_sync_leave_group(sync_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeNotifyChanged(JNIEnv *env, jclass cls,
                                                                       jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_notify_changed(sync_of(handle));
}

/* The mc_sync_lib_state. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_CoreSync_nativeLibraryState(JNIEnv *env, jclass cls,
                                                                      jlong handle) {
  (void)cls;
  mc_sync_lib_state state = MC_SYNC_LIBRARY_NO_GROUP;
  mc_status st = mc_sync_library_state(sync_of(handle), &state);
  if (st != MC_OK) throw_status(env, st);
  return (jint)state;
}

JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreSync_nativeListDevices(JNIEnv *env, jclass cls,
                                                                             jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/SyncDevice");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;IZZZZLjava/lang/String;IJJZJJJJJJI)V");
  if (!ctor) return NULL;

  mc_sync_device_list *list = NULL;
  mc_status st = mc_sync_list_devices(sync_of(handle), &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_sync_device *d = &list->items[i];
    jstring uid = new_string(env, d->uid);
    jstring name = new_string(env, d->name ? d->name : "");
    jstring address = new_string(env, d->address);
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)d->kind,
                                     (jboolean)(d->is_self != 0), (jboolean)(d->online != 0),
                                     (jboolean)(d->archive != 0), (jboolean)(d->removed != 0),
                                     address, (jint)d->port, (jlong)d->last_sync_ms,
                                     (jlong)d->auto_after_ms,
                                     (jboolean)(d->has_status != 0), (jlong)d->verified_ms,
                                     (jlong)d->expected, (jlong)d->ok, (jlong)d->missing,
                                     (jlong)d->corrupt, (jlong)d->bytes, (jint)d->confirm);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
    if (address) (*env)->DeleteLocalRef(env, address);
  }
  mc_sync_device_list_free(list);
  return out;
}

JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_CoreSync_nativeListNearby(JNIEnv *env, jclass cls,
                                                                            jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/NearbyDevice");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;IZLjava/lang/String;I)V");
  if (!ctor) return NULL;

  mc_sync_nearby_list *list = NULL;
  mc_status st = mc_sync_list_nearby(sync_of(handle), &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_sync_nearby *n = &list->items[i];
    jstring uid = new_string(env, n->uid);
    jstring name = new_string(env, n->name ? n->name : "");
    jstring address = new_string(env, n->address);
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)n->kind,
                                     (jboolean)(n->in_other_group != 0), address, (jint)n->port);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, name);
    if (address) (*env)->DeleteLocalRef(env, address);
  }
  mc_sync_nearby_list_free(list);
  return out;
}

/* --- Profiles --- */

/* Fills p, zeroed, from a SyncProfile's fields as CoreSync passes them: kbps 0 for originals,
 * a budget for Auto (kbps ignored), and the items as parallel kinds and uids. Returns 0 with an exception pending on failure;
 * either way free it with profile_args_free(). */
static int profile_from_args(JNIEnv *env, mc_sync_profile *p, jstring name, jboolean archive,
                             jboolean sync_all, jint kbps, jboolean budget_enabled,
                             jlong budget_bytes, jintArray kinds, jobjectArray uids) {
  p->archive = archive != JNI_FALSE;
  p->sync_all = sync_all != JNI_FALSE;
  p->budget_enabled = budget_enabled != JNI_FALSE;
  p->format.format = p->budget_enabled ? MC_FORMAT_AUTO : kbps > 0 ? MC_FORMAT_OPUS : MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = !p->budget_enabled && kbps > 0 ? kbps : 0;
  p->budget_bytes = budget_bytes;
  if (name) {
    const char *cname = (*env)->GetStringUTFChars(env, name, NULL);
    if (!cname) return 0;
    p->name = strdup(cname);
    (*env)->ReleaseStringUTFChars(env, name, cname);
    if (!p->name) {
      throw_status(env, MC_ERR_NOMEM);
      return 0;
    }
  }
  jsize count = (*env)->GetArrayLength(env, kinds);
  if (count != (*env)->GetArrayLength(env, uids)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return 0;
  }
  if (count == 0) return 1;
  p->items = calloc((size_t)count, sizeof *p->items);
  jint *ckinds = p->items ? (*env)->GetIntArrayElements(env, kinds, NULL) : NULL;
  if (!ckinds) {
    if (!(*env)->ExceptionCheck(env)) throw_status(env, MC_ERR_NOMEM);
    return 0;
  }
  int ok = 1;
  for (jsize i = 0; ok && i < count; i++) {
    jstring uid = (jstring)(*env)->GetObjectArrayElement(env, uids, i);
    const char *cuid = uid ? (*env)->GetStringUTFChars(env, uid, NULL) : NULL;
    if (cuid) {
      p->items[i].kind = (mc_profile_item_kind)ckinds[i];
      snprintf(p->items[i].ref_uid, sizeof p->items[i].ref_uid, "%s", cuid);
      (*env)->ReleaseStringUTFChars(env, uid, cuid);
      p->count++;
    } else {
      ok = 0;
      if (!(*env)->ExceptionCheck(env)) throw_status(env, MC_ERR_INVALID_ARG);
    }
    if (uid) (*env)->DeleteLocalRef(env, uid);
  }
  (*env)->ReleaseIntArrayElements(env, kinds, ckinds, JNI_ABORT);
  return ok;
}

static void profile_args_free(mc_sync_profile *p) {
  free(p->name);
  free(p->items);
}

/* device_uid's profile as a SyncProfile, made with its private constructor that takes the
 * items as parallel kinds and uids. */
JNIEXPORT jobject JNICALL Java_ng_kithso_core_CoreSync_nativeGetProfile(JNIEnv *env, jclass cls,
                                                                       jlong handle, jstring uid) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/SyncProfile");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;ZZIZJ[I[Ljava/lang/String;)V");
  jclass string_cls = ctor ? (*env)->FindClass(env, "java/lang/String") : NULL;
  if (!string_cls) return NULL;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return NULL;
  mc_sync_profile *p = NULL;
  mc_status st = mc_sync_get_profile(sync_of(handle), cuid, &p);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobject out = NULL;
  jstring name = new_string(env, p->name);
  jintArray kinds = (*env)->NewIntArray(env, p->count);
  jobjectArray uids = kinds ? (*env)->NewObjectArray(env, p->count, string_cls, NULL) : NULL;
  int ok = uids != NULL && (!p->name || name);
  for (int i = 0; ok && i < p->count; i++) {
    jint kind = (jint)p->items[i].kind;
    (*env)->SetIntArrayRegion(env, kinds, i, 1, &kind);
    jstring ref = new_string(env, p->items[i].ref_uid);
    ok = ref != NULL;
    if (ref) {
      (*env)->SetObjectArrayElement(env, uids, i, ref);
      (*env)->DeleteLocalRef(env, ref);
    }
  }
  if (ok) {
    jint kbps = p->format.format == MC_FORMAT_OPUS ? p->format.bitrate_kbps : 0;
    out = (*env)->NewObject(env, info, ctor, name, (jboolean)(p->archive != 0),
                            (jboolean)(p->sync_all != 0), kbps, (jboolean)(p->budget_enabled != 0),
                            (jlong)p->budget_bytes, kinds, uids);
  }
  mc_sync_profile_free(p);
  return out;
}

/* Stores device_uid's profile. There's no archive argument: mc_sync_set_profile() ignores it. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeSetProfile(
  JNIEnv *env, jclass cls, jlong handle, jstring uid, jstring name, jboolean sync_all, jint kbps,
  jboolean budget_enabled, jlong budget_bytes, jintArray kinds, jobjectArray uids) {
  (void)cls;
  mc_sync_profile p;
  memset(&p, 0, sizeof p);
  if (profile_from_args(env, &p, name, JNI_FALSE, sync_all, kbps, budget_enabled, budget_bytes,
                        kinds, uids)) {
    const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
    if (cuid) {
      mc_status st = mc_sync_set_profile(sync_of(handle), cuid, &p);
      (*env)->ReleaseStringUTFChars(env, uid, cuid);
      if (st != MC_OK) throw_status(env, st);
    }
  }
  profile_args_free(&p);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeSetArchive(JNIEnv *env, jclass cls,
                                                                    jlong handle, jstring uid,
                                                                    jboolean archive) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_sync_set_archive(sync_of(handle), cuid, archive != JNI_FALSE);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* Fills out with what device_uid would hold with the profile: tracks, total_bytes, kbps,
 * archive, over_budget, shortfall_bytes, limit_bytes and free_bytes, then step_bytes. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeEstimate(
  JNIEnv *env, jclass cls, jlong handle, jstring uid, jstring name, jboolean archive,
  jboolean sync_all, jint kbps, jboolean budget_enabled, jlong budget_bytes, jintArray kinds,
  jobjectArray uids, jlongArray out) {
  (void)cls;
  mc_sync_profile p;
  memset(&p, 0, sizeof p);
  if (profile_from_args(env, &p, name, archive, sync_all, kbps, budget_enabled, budget_bytes,
                        kinds, uids)) {
    const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
    if (cuid) {
      mc_sync_estimate e;
      mc_status st = mc_sync_estimate_profile(sync_of(handle), cuid, &p, &e);
      (*env)->ReleaseStringUTFChars(env, uid, cuid);
      if (st == MC_OK) {
        jlong values[8 + MC_SYNC_ESTIMATE_STEPS] = {
          e.tracks,      e.total_bytes,     e.kbps,        e.archive,
          e.over_budget, e.shortfall_bytes, e.limit_bytes, e.free_bytes,
        };
        for (int i = 0; i < MC_SYNC_ESTIMATE_STEPS; i++) values[8 + i] = e.step_bytes[i];
        (*env)->SetLongArrayRegion(env, out, 0, 8 + MC_SYNC_ESTIMATE_STEPS, values);
      } else {
        throw_status(env, st);
      }
    }
  }
  profile_args_free(&p);
}

/* Checks this device's files (deep: re-hashes them) and fills out with checked, ok, failed and
 * deleted. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeVerify(JNIEnv *env, jclass cls,
                                                                jlong handle, jboolean deep,
                                                                jlongArray out) {
  (void)cls;
  mc_verify_result r;
  memset(&r, 0, sizeof r);
  mc_status st = mc_sync_verify(sync_of(handle), deep ? MC_VERIFY_DEEP : MC_VERIFY_FAST, &r);
  if (st != MC_OK) {
    throw_status(env, st);
    return;
  }
  jlong values[4] = {r.checked, r.ok, r.failed, r.deleted};
  (*env)->SetLongArrayRegion(env, out, 0, 4, values);
}

/* Renames this device, for the whole group when in one. */
JNIEXPORT void JNICALL Java_ng_kithso_core_CoreSync_nativeSetDeviceName(JNIEnv *env, jclass cls,
                                                                       jlong handle, jstring name) {
  (void)cls;
  const char *cname = (*env)->GetStringUTFChars(env, name, NULL);
  if (!cname) return;
  mc_status st = mc_sync_set_device_name(sync_of(handle), cname);
  (*env)->ReleaseStringUTFChars(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

/* This device's name as it is now: the group may have renamed it. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_CoreSync_nativeGetDeviceName(JNIEnv *env, jclass cls,
                                                                         jlong handle) {
  (void)cls;
  char name[4 * MC_SYNC_DEVICE_NAME_MAX + 4];
  mc_status st = mc_sync_get_device_name(sync_of(handle), name, sizeof name);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return new_string(env, name);
}
