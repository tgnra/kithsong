/* JNI bindings for app.musicapp.NativeCore, CoreLibrary and CorePlayer. Keep this layer
 * thin: marshal arguments, call musiccore, convert results. Failures throw
 * app.musicapp.MusicCoreException. */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListAlbums(JNIEnv *env,
                                                                               jclass cls,
                                                                               jlong handle) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/AlbumInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(
    env, info, "<init>",
    "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZZLjava/lang/String;)V");
  if (!ctor) return NULL;

  mc_album_list *list = NULL;
  mc_status st = mc_library_list_albums((mc_library *)(intptr_t)handle, &list);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
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

JNIEXPORT jobjectArray JNICALL Java_app_musicapp_CoreLibrary_nativeListAlbumTracks(
  JNIEnv *env, jclass cls, jlong handle, jstring album_uid) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "app/musicapp/TrackInfo");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;IIILjava/lang/String;)V");
  if (!ctor) return NULL;

  const char *cuid = (*env)->GetStringUTFChars(env, album_uid, NULL);
  if (!cuid) return NULL;
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_album_tracks((mc_library *)(intptr_t)handle, cuid, &list);
  (*env)->ReleaseStringUTFChars(env, album_uid, cuid);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jobjectArray out = (*env)->NewObjectArray(env, list->count, info, NULL);
  for (int i = 0; out && i < list->count; i++) {
    const mc_track_info *t = &list->items[i];
    jstring uid = new_string(env, t->uid);
    jstring title = new_string(env, t->title ? t->title : "");
    jstring path = new_string(env, t->path);
    jobject item = (*env)->NewObject(env, info, ctor, uid, title, (jint)t->track_no,
                                     (jint)t->disc_no, (jint)t->duration_ms, path);
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, uid);
    (*env)->DeleteLocalRef(env, title);
    if (path) (*env)->DeleteLocalRef(env, path);
  }
  mc_track_list_free(list);
  return out;
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
