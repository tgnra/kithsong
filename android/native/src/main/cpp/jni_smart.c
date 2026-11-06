/* JNI bindings for ng.kithso.core.SmartNatives: making, editing and describing smart playlists
 * and smart album groups (musiccore/smartlist.h), for the rule editor. Listing them, and their
 * previews, are in jni_bridge.c and jni_browse.c. Thin like jni_bridge.c, whose helpers it
 * shares. Failures throw ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* A Kotlin isAlbums as an mc_smart_subject. */
static mc_smart_subject subject_of(jboolean albums) {
  return albums ? MC_SMART_ALBUMS : MC_SMART_TRACKS;
}

/* The fields the subject's rules can use, in the core's order for a picker (mc_smart_field_get()). */
JNIEXPORT jobjectArray JNICALL Java_ng_kithso_core_SmartNatives_nativeFields(JNIEnv *env, jclass cls,
                                                                           jboolean albums) {
  (void)cls;
  jclass info = (*env)->FindClass(env, "ng/kithso/core/SmartField");
  if (!info) return NULL;
  jmethodID ctor = (*env)->GetMethodID(env, info, "<init>",
                                       "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Z)V");
  if (!ctor) return NULL;
  jclass string = (*env)->FindClass(env, "java/lang/String");
  if (!string) return NULL;

  mc_smart_subject subject = subject_of(albums);
  int n = mc_smart_field_count(subject);
  jobjectArray out = (*env)->NewObjectArray(env, n, info, NULL);
  for (int i = 0; out && i < n; i++) {
    mc_smart_field f;
    if (!mc_smart_field_get(subject, i, &f)) {
      out = NULL;
      break;
    }
    int nops = 0;
    while (f.ops[nops]) nops++;
    jobjectArray ops = (*env)->NewObjectArray(env, nops, string, NULL);
    for (int j = 0; ops && j < nops; j++) {
      jstring op = (*env)->NewStringUTF(env, f.ops[j]);
      if (!op) {
        ops = NULL;
        break;
      }
      (*env)->SetObjectArrayElement(env, ops, j, op);
      (*env)->DeleteLocalRef(env, op);
    }
    jstring name = ops ? new_string(env, f.name) : NULL;
    jstring type = name ? new_string(env, f.type) : NULL;
    jobject item = type ? (*env)->NewObject(env, info, ctor, name, type, ops, (jboolean)(f.sortable != 0)) : NULL;
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    (*env)->DeleteLocalRef(env, item);
    (*env)->DeleteLocalRef(env, name);
    (*env)->DeleteLocalRef(env, type);
    (*env)->DeleteLocalRef(env, ops);
  }
  return out;
}

/* A field's label for a picker (mc_smart_field_label()), or null for a name the rule format
 * doesn't have. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_SmartNatives_nativeFieldLabel(JNIEnv *env, jclass cls,
                                                                          jstring field) {
  (void)cls;
  int ok = 1;
  const char *cfield = jni_chars(env, field, &ok);
  if (!ok) return NULL;
  jstring out = new_string(env, mc_smart_field_label(cfield ? cfield : ""));
  jni_release(env, field, cfield);
  return out;
}

/* An operator's label after a field of type (mc_smart_op_label()), or null. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_SmartNatives_nativeOpLabel(JNIEnv *env, jclass cls,
                                                                       jstring type, jstring op) {
  (void)cls;
  int ok = 1;
  const char *ctype = jni_chars(env, type, &ok);
  const char *cop = jni_chars(env, op, &ok);
  jstring out = ok ? new_string(env, mc_smart_op_label(ctype ? ctype : "", cop ? cop : "")) : NULL;
  jni_release(env, type, ctype);
  jni_release(env, op, cop);
  return out;
}

/* A sort order's label (mc_smart_order_label()), or null for a field nothing sorts on. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_SmartNatives_nativeOrderLabel(JNIEnv *env, jclass cls,
                                                                          jstring field, jboolean desc) {
  (void)cls;
  int ok = 1;
  const char *cfield = jni_chars(env, field, &ok);
  if (!ok) return NULL;
  jstring out = new_string(env, mc_smart_order_label(cfield ? cfield : "", desc ? 1 : 0));
  jni_release(env, field, cfield);
  return out;
}

/* Whether rule is valid for the subject (mc_smart_validate()); nothing is saved. */
JNIEXPORT jboolean JNICALL Java_ng_kithso_core_SmartNatives_nativeValidate(JNIEnv *env, jclass cls,
                                                                         jboolean albums, jstring rule) {
  (void)cls;
  int ok = 1;
  const char *crule = jni_chars(env, rule, &ok);
  if (!ok) return JNI_FALSE;
  mc_status st = mc_smart_validate(subject_of(albums), crule ? crule : "");
  jni_release(env, rule, crule);
  return st == MC_OK ? JNI_TRUE : JNI_FALSE;
}

/* rule as a sentence (mc_smart_describe()), naming playlists and groups as they're called now. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_SmartNatives_nativeDescribe(JNIEnv *env, jclass cls, jlong handle,
                                                                        jboolean albums, jstring rule) {
  (void)cls;
  int ok = 1;
  const char *crule = jni_chars(env, rule, &ok);
  if (!ok) return NULL;
  char *sentence = NULL;
  mc_status st = mc_smart_describe(jni_lib_of(handle), subject_of(albums), crule ? crule : "", &sentence);
  jni_release(env, rule, crule);
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  jstring out = new_string(env, sentence ? sentence : "");
  mc_free(sentence);
  return out;
}

/* How many tracks or albums rule picks (mc_smart_count()), at most its limit. */
JNIEXPORT jint JNICALL Java_ng_kithso_core_SmartNatives_nativeCount(JNIEnv *env, jclass cls, jlong handle,
                                                                  jboolean albums, jstring rule) {
  (void)cls;
  int ok = 1;
  const char *crule = jni_chars(env, rule, &ok);
  if (!ok) return 0;
  int n = 0;
  mc_status st = mc_smart_count(jni_lib_of(handle), subject_of(albums), crule ? crule : "", &n);
  jni_release(env, rule, crule);
  if (st != MC_OK) throw_status(env, st);
  return (jint)n;
}

/* A new smart list called name with rule; returns its uid. */
JNIEXPORT jstring JNICALL Java_ng_kithso_core_SmartNatives_nativeCreate(JNIEnv *env, jclass cls, jlong handle,
                                                                      jstring name, jboolean albums, jstring rule) {
  (void)cls;
  int ok = 1;
  const char *cname = jni_chars(env, name, &ok);
  const char *crule = jni_chars(env, rule, &ok);
  char uid[MC_UID_STRLEN];
  mc_status st = ok ? mc_smart_create(jni_lib_of(handle), cname ? cname : "", subject_of(albums),
                                      crule ? crule : "", uid)
                    : MC_ERR_NOMEM;
  jni_release(env, name, cname);
  jni_release(env, rule, crule);
  if (!ok) return NULL; /* OutOfMemoryError is pending */
  if (st != MC_OK) {
    throw_status(env, st);
    return NULL;
  }
  return (*env)->NewStringUTF(env, uid);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_SmartNatives_nativeRename(JNIEnv *env, jclass cls, jlong handle,
                                                                   jstring uid, jstring name) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *cname = jni_chars(env, name, &ok);
  mc_status st = ok ? mc_smart_rename(jni_lib_of(handle), cuid ? cuid : "", cname ? cname : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, name, cname);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_SmartNatives_nativeSetRule(JNIEnv *env, jclass cls, jlong handle,
                                                                    jstring uid, jstring rule) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  const char *crule = jni_chars(env, rule, &ok);
  mc_status st = ok ? mc_smart_set_rule(jni_lib_of(handle), cuid ? cuid : "", crule ? crule : "") : MC_OK;
  jni_release(env, uid, cuid);
  jni_release(env, rule, crule);
  if (st != MC_OK) throw_status(env, st);
}

JNIEXPORT void JNICALL Java_ng_kithso_core_SmartNatives_nativeDelete(JNIEnv *env, jclass cls, jlong handle,
                                                                   jstring uid) {
  (void)cls;
  int ok = 1;
  const char *cuid = jni_chars(env, uid, &ok);
  if (!ok) return;
  mc_status st = mc_smart_delete(jni_lib_of(handle), cuid ? cuid : "");
  jni_release(env, uid, cuid);
  if (st != MC_OK) throw_status(env, st);
}
