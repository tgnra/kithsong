/* JNI bindings for ng.kithso.core.AnalysisNatives: one run of the audio scanner
 * (mc_analysis_scanner_start_once() in musiccore/analysis.h), for the phone's charging job, and
 * what's waiting for it. As thin as it is; the scanner has a thread and a connection of its own. */
#include <jni.h>
#include <stdint.h>

#include "jni_bridge.h"
#include "kithcore/analysis.h"

JNIEXPORT jlong JNICALL Java_ng_kithso_core_AnalysisNatives_nativeStart(JNIEnv *env, jclass cls, jstring dir) {
  (void)cls;
  int ok = 1;
  const char *cdir = jni_chars(env, dir, &ok);
  if (!ok) return 0;
  mc_analysis_scanner *s = NULL;
  mc_status st = mc_analysis_scanner_start_once(cdir, &s);
  jni_release(env, dir, cdir);
  if (st != MC_OK) {
    throw_status(env, st);
    return 0;
  }
  return (jlong)(intptr_t)s;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_AnalysisNatives_nativePending(JNIEnv *env, jclass cls, jlong library) {
  (void)cls;
  int tracks = 0;
  int64_t ms = 0;
  mc_status st = mc_analysis_pending((mc_library *)(intptr_t)library, &tracks, &ms);
  if (st != MC_OK) throw_status(env, st);
  return (jint)tracks;
}

JNIEXPORT jint JNICALL Java_ng_kithso_core_AnalysisNatives_nativeState(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  return (jint)mc_analysis_scanner_state((mc_analysis_scanner *)(intptr_t)handle);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_AnalysisNatives_nativeStop(JNIEnv *env, jclass cls, jlong handle) {
  (void)env;
  (void)cls;
  mc_analysis_scanner_stop((mc_analysis_scanner *)(intptr_t)handle);
}
