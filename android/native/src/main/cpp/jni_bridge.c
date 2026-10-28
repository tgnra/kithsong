/* JNI bindings for app.musicapp.NativeCore, CoreLibrary, CoreImport, CorePlayer and CoreSync. Keep this
 * thin: marshal arguments, call musiccore, convert results. Failures throw
 * app.musicapp.MusicCoreException. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"

static JavaVM *g_vm;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  g_vm = vm;
  return JNI_VERSION_1_6;
}

/* Throws MusicCoreException(status) and returns status. */
static mc_status throw_status(JNIEnv *env, mc_status st) {
  jclass cls = (*env)->FindClass(env, "app/musicapp/MusicCoreException");
  if (!cls) return st; /* NoClassDefFoundError is already pending */
  jmethodID ctor = (*env)->GetMethodID(env, cls, "<init>", "(I)V");
  jobject ex = ctor ? (*env)->NewObject(env, cls, ctor, (jint)st) : NULL;
  if (ex) (*env)->Throw(env, (jthrowable)ex);
  return st;
}

/* A Java string for s, or NULL for NULL. */
static jstring new_string(JNIEnv *env, const char *s) {
  return s ? (*env)->NewStringUTF(env, s) : NULL;
}

JNIEXPORT jstring JNICALL Java_app_musicapp_NativeCore_version(JNIEnv *env, jclass cls) {
  (void)cls;
  return (*env)->NewStringUTF(env, mc_version());
}

JNIEXPORT jint JNICALL Java_app_musicapp_NativeCore_init(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  return mc_init();
}

/* The core's language for its own text, from a BCP 47 tag such as "fr-CA". */
JNIEXPORT void JNICALL Java_app_musicapp_NativeCore_setLocale(JNIEnv *env, jclass cls,
                                                               jstring tag) {
  (void)cls;
  const char *ctag = (*env)->GetStringUTFChars(env, tag, NULL);
  if (!ctag) return; /* OutOfMemoryError is pending */
  mc_strings_set_locale(ctag);
  (*env)->ReleaseStringUTFChars(env, tag, ctag);
}

/* Returns the number of albums, or a negative mc_status on failure. */
JNIEXPORT jlong JNICALL Java_app_musicapp_NativeCore_albumCount(JNIEnv *env, jclass cls,
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

/* --- CoreLibrary ------------------------------------------------------------------- */

JNIEXPORT jlong JNICALL Java_app_musicapp_CoreLibrary_nativeOpen(JNIEnv *env, jclass cls,
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreLibrary_nativeClose(JNIEnv *env, jclass cls,
                                                                  jlong handle) {
  (void)env;
  (void)cls;
  mc_library_close((mc_library *)(intptr_t)handle);
}

JNIEXPORT jlong JNICALL Java_app_musicapp_CoreLibrary_nativeCount(JNIEnv *env, jclass cls,
                                                                   jlong handle, jint entity) {
  (void)cls;
  int64_t count = 0;
  mc_status st = mc_library_count((mc_library *)(intptr_t)handle, (mc_entity)entity, &count);
  if (st != MC_OK) throw_status(env, st);
  return (jlong)count;
}

/* An AlbumInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
static jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list) {
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "app/musicapp/AlbumInfo");
  jmethodID ctor = info ? (*env)->GetMethodID(
                            env, info, "<init>",
                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZZLjava/lang/String;)V")
                        : NULL;
  if (ctor) out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_album_info *a = &list->items[i];
    jstring uid = new_string(env, a->uid);
    jstring title = new_string(env, a->title ? a->title : "");
    jstring artist = new_string(env, a->artist);
    jstring folder = new_string(env, a->folder);
    jobject item = (*env)->NewObject(env, info, ctor, uid, title, artist, (jint)a->year,
                                     (jboolean)(a->is_singles != 0), (jboolean)(a->has_art != 0),
                                     folder);
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
  }
  mc_album_list_free(list);
  return out;
}

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListAlbums(JNIEnv *env,
                                                                               jclass cls,
                                                                               jlong handle) {
  (void)cls;
  mc_album_list *list = NULL;
  mc_status st = mc_library_list_albums((mc_library *)(intptr_t)handle, &list);
  return album_array(env, st, list);
}

/* What a smart album group with rule would hold, without saving it. */
JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativePreviewSmartAlbums(JNIEnv *env,
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreLibrary_nativeRecordPlay(JNIEnv *env, jclass cls,
                                                                      jlong handle, jstring uid) {
  (void)cls;
  const char *cuid = (*env)->GetStringUTFChars(env, uid, NULL);
  if (!cuid) return;
  mc_status st = mc_library_record_play((mc_library *)(intptr_t)handle, cuid);
  (*env)->ReleaseStringUTFChars(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}

/* A TrackInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
static jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list) {
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = NULL;
  jclass info = (*env)->FindClass(env, "app/musicapp/TrackInfo");
  jmethodID ctor = info ? (*env)->GetMethodID(env, info, "<init>",
                                              "(Ljava/lang/String;Ljava/lang/String;IIILjava/lang/String;"
                                              "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V")
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
    jobject item = (*env)->NewObject(env, info, ctor, uid, title, (jint)t->track_no, (jint)t->disc_no,
                                     (jint)t->duration_ms, path, artist, album, cover_dir);
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
  }
  mc_track_list_free(list);
  return out;
}

/* The tracks of an album (kind 0), a playlist (1) or a smart playlist (2). */
JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListTracks(JNIEnv *env, jclass cls,
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListPlaylists(JNIEnv *env,
                                                                                  jclass cls,
                                                                                  jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/PlaylistInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>", "(Ljava/lang/String;Ljava/lang/String;I)V");
  if (!ctor) return NULL;

  mc_playlist_list *list = NULL;
  mc_status st = mc_library_list_playlists((mc_library *)(intptr_t)handle, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_playlist_info *p = &list->items[i];
    jstring uid = new_string(env, p->uid);
    jstring name = new_string(env, p->name ? p->name : "");
    jobject item = (*env)->NewObject(env, info, ctor, uid, name, (jint)p->track_count);
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListSmart(JNIEnv *env, jclass cls,
                                                                              jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/SmartInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V");
  if (!ctor) return NULL;

  mc_smart_list *list = NULL;
  mc_status st = mc_library_list_smart((mc_library *)(intptr_t)handle, &list);
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
                                     (jboolean)(s->subject == MC_SMART_ALBUMS), rule);
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
  jclass info = (*env)->FindClass(env, "app/musicapp/NamedInfo");
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListArtists(JNIEnv *env,
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListAlbumGroups(JNIEnv *env,
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

JNIEXPORT jlong JNICALL Java_app_musicapp_CoreImport_nativeStart(JNIEnv *env, jclass cls,
                                                                  jlong library, jobjectArray paths,
                                                                  jboolean fetch_art,
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
  /* mc_import_start copies the paths. */
  mc_import_options opts = {.threshold = 0, .fetch_art = fetch_art ? 1 : 0};
  if (st == MC_OK) {
    st = mc_import_start((mc_library *)(intptr_t)library, cpaths, count, &opts, on_import_event, p,
                         &p->imp);
  }
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreImport_nativeFree(JNIEnv *env, jclass cls,
                                                                jlong handle) {
  (void)cls;
  jni_import *p = (jni_import *)(intptr_t)handle;
  if (!p) return;
  mc_import_free(p->imp); /* waits for the import thread, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNIEXPORT jint JNICALL Java_app_musicapp_CoreImport_nativeItemCount(JNIEnv *env, jclass cls,
                                                                     jlong handle) {
  (void)env;
  (void)cls;
  return mc_import_item_count(import_of(handle));
}

/* The item's mc_import_state. */
JNIEXPORT jint JNICALL Java_app_musicapp_CoreImport_nativeItemState(JNIEnv *env, jclass cls,
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
JNIEXPORT jstring JNICALL Java_app_musicapp_CoreImport_nativeItemName(JNIEnv *env, jclass cls,
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
JNIEXPORT jstring JNICALL Java_app_musicapp_CoreImport_nativeItemError(JNIEnv *env, jclass cls,
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

/* The calls that resolve an item needing review: 0 accept as is, 1 skip. */
JNIEXPORT void JNICALL Java_app_musicapp_CoreImport_nativeResolve(JNIEnv *env, jclass cls,
                                                                   jlong handle, jint item,
                                                                   jint how) {
  (void)cls;
  static mc_status (*const calls[])(mc_import *, int) = {mc_import_accept_as_is, mc_import_skip};
  if (how < 0 || how >= (jint)(sizeof calls / sizeof *calls)) {
    throw_status(env, MC_ERR_INVALID_ARG);
    return;
  }
  mc_status st = calls[how](import_of(handle), item);
  if (st != MC_OK) throw_status(env, st);
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

JNIEXPORT jlong JNICALL Java_app_musicapp_CorePlayer_nativeNew(JNIEnv *env, jclass cls,
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

JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeFree(JNIEnv *env, jclass cls,
                                                                jlong handle) {
  (void)cls;
  jni_player *p = (jni_player *)(intptr_t)handle;
  if (!p) return;
  mc_player_free(p->player); /* first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeSetQueue(JNIEnv *env, jclass cls,
                                                                    jlong handle,
                                                                    jobjectArray paths,
                                                                    jint start) {
  (void)cls;
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
  /* mc_player_set_queue copies the paths. */
  if (st == MC_OK) st = mc_player_set_queue(player_of(handle), cpaths, count, start);
  for (jsize i = 0; i < got; i++) {
    if (cpaths[i]) (*env)->ReleaseStringUTFChars(env, jpaths[i], cpaths[i]);
    if (jpaths[i]) (*env)->DeleteLocalRef(env, jpaths[i]);
  }
  free(cpaths);
  free(jpaths);
  if (st != MC_OK && !(*env)->ExceptionCheck(env)) throw_status(env, st);
}

/* The player calls that take no arguments: 0 toggle, 1 pause, 2 resume, 3 stop, 4 next,
 * 5 previous. */
JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeCommand(JNIEnv *env, jclass cls,
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

JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeSkipTo(JNIEnv *env, jclass cls,
                                                                  jlong handle, jint index) {
  (void)cls;
  mc_status st = mc_player_skip_to(player_of(handle), index);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeSeekMs(JNIEnv *env, jclass cls,
                                                                  jlong handle, jlong ms) {
  (void)cls;
  mc_status st = mc_player_seek_ms(player_of(handle), ms);
  if (st != MC_OK) throw_status(env, st);
}

/* Fills out with state, index, count, position_ms, duration_ms. */
JNIEXPORT void JNICALL Java_app_musicapp_CorePlayer_nativeStatus(JNIEnv *env, jclass cls,
                                                                  jlong handle, jlongArray out) {
  (void)cls;
  mc_player_status st;
  mc_player_status_get(player_of(handle), &st);
  jlong values[5] = {st.state, st.index, st.count, st.position_ms, st.duration_ms};
  (*env)->SetLongArrayRegion(env, out, 0, 5, values);
}

/* Fills out with the item and start (wall clock ms) of the oldest play that counts; false if
 * there's none. */
JNIEXPORT jboolean JNICALL Java_app_musicapp_CorePlayer_nativeTakePlayed(JNIEnv *env, jclass cls,
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
  (*env)->CallVoidMethod(env, p->listener, p->on_pair_result, uid, name,
                         (jboolean)(r->initiator != 0), (jint)r->reason, (jint)r->attempts_left,
                         (jint)r->status);
  if (uid) (*env)->DeleteLocalRef(env, uid);
  if (name) (*env)->DeleteLocalRef(env, name);
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
                         (jlong)r->bytes_sent);
  if (uid) (*env)->DeleteLocalRef(env, uid);
  detach(env, attached);
}

static mc_sync *sync_of(jlong handle) {
  return ((jni_sync *)(intptr_t)handle)->sync;
}

JNIEXPORT jlong JNICALL Java_app_musicapp_CoreSync_nativeNew(JNIEnv *env, jclass cls,
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
    {"onPairResult", "(Ljava/lang/String;Ljava/lang/String;ZIII)V"},
    {"onProgress", "(Ljava/lang/String;IJJIIJJ)V"},
    {"onSyncDone", "(Ljava/lang/String;IZZJJIIIIIIJJ)V"},
  };
  jmethodID *ids[] = {&p->on_peer, &p->on_pair_code, &p->on_pair_result, &p->on_progress,
                      &p->on_sync_done};
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeFree(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)cls;
  jni_sync *p = (jni_sync *)(intptr_t)handle;
  if (!p) return;
  mc_sync_free(p->sync); /* stops first, so no more events arrive */
  (*env)->DeleteGlobalRef(env, p->listener);
  free(p);
}

JNIEXPORT jstring JNICALL Java_app_musicapp_CoreSync_nativeDeviceUid(JNIEnv *env, jclass cls,
                                                                     jlong handle) {
  (void)cls;
  return new_string(env, mc_sync_device_uid(sync_of(handle)));
}

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeStart(JNIEnv *env, jclass cls,
                                                               jlong handle) {
  (void)cls;
  mc_status st = mc_sync_start(sync_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeStop(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_stop(sync_of(handle));
}

JNIEXPORT jint JNICALL Java_app_musicapp_CoreSync_nativePort(JNIEnv *env, jclass cls,
                                                              jlong handle) {
  (void)env;
  (void)cls;
  return mc_sync_port(sync_of(handle));
}

/* The calls that take a device uid: 0 pair request, 1 remove device, 2 sync now, 3 confirm
 * and sync now (the Sync button), 4 background sync (rate-limited). For 2 to 4, uid null is
 * every reachable member. */
JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeWithDevice(JNIEnv *env, jclass cls,
                                                                    jlong handle, jint call,
                                                                    jstring uid) {
  (void)cls;
  static mc_status (*const calls[])(mc_sync *, const char *) = {
    mc_sync_pair_request,
    mc_sync_remove_device,
    mc_sync_now,
    mc_sync_confirm_now,
    mc_sync_background,
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativePairRequestAddress(JNIEnv *env, jclass cls,
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativePairEnter(JNIEnv *env, jclass cls,
                                                                   jlong handle, jstring code) {
  (void)cls;
  const char *ccode = (*env)->GetStringUTFChars(env, code, NULL);
  if (!ccode) return;
  mc_status st = mc_sync_pair_enter(sync_of(handle), ccode);
  (*env)->ReleaseStringUTFChars(env, code, ccode);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativePairCancel(JNIEnv *env, jclass cls,
                                                                    jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_pair_cancel(sync_of(handle));
}

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeLeaveGroup(JNIEnv *env, jclass cls,
                                                                    jlong handle) {
  (void)cls;
  mc_status st = mc_sync_leave_group(sync_of(handle));
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeNotifyChanged(JNIEnv *env, jclass cls,
                                                                       jlong handle) {
  (void)env;
  (void)cls;
  mc_sync_notify_changed(sync_of(handle));
}

/* The mc_sync_lib_state. */
JNIEXPORT jint JNICALL Java_app_musicapp_CoreSync_nativeLibraryState(JNIEnv *env, jclass cls,
                                                                      jlong handle) {
  (void)cls;
  mc_sync_lib_state state = MC_SYNC_LIBRARY_NO_GROUP;
  mc_status st = mc_sync_library_state(sync_of(handle), &state);
  if (st != MC_OK) throw_status(env, st);
  return (jint)state;
}

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreSync_nativeListDevices(JNIEnv *env, jclass cls,
                                                                             jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/SyncDevice");
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreSync_nativeListNearby(JNIEnv *env, jclass cls,
                                                                            jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/NearbyDevice");
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
JNIEXPORT jobject JNICALL Java_app_musicapp_CoreSync_nativeGetProfile(JNIEnv *env, jclass cls,
                                                                       jlong handle, jstring uid) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/SyncProfile");
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
JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeSetProfile(
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

JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeSetArchive(JNIEnv *env, jclass cls,
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
JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeEstimate(
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
JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeVerify(JNIEnv *env, jclass cls,
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
JNIEXPORT void JNICALL Java_app_musicapp_CoreSync_nativeSetDeviceName(JNIEnv *env, jclass cls,
                                                                       jlong handle, jstring name) {
  (void)cls;
  const char *cname = (*env)->GetStringUTFChars(env, name, NULL);
  if (!cname) return;
  mc_status st = mc_sync_set_device_name(sync_of(handle), cname);
  (*env)->ReleaseStringUTFChars(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

/* This device's name as it is now: the group may have renamed it. */
JNIEXPORT jstring JNICALL Java_app_musicapp_CoreSync_nativeGetDeviceName(JNIEnv *env, jclass cls,
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
