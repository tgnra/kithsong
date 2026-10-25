/* JNI bindings for app.musicapp.NativeCore. Keep this layer thin: marshal arguments,
 * call musiccore, convert results. */
#include <jni.h>

#include "musiccore/musiccore.h"

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
