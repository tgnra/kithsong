/* JNI bindings for ng.kithso.core.SmartNatives: making, editing and describing smart playlists
 * and smart album groups (kithcore/smartlist.h), for the rule editor. Listing them, and their
 * previews, are in jni_bridge.c and jni_browse.c. Failures throw
 * ng.kithso.core.KithCoreException. */
#include <jni.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* A Kotlin isAlbums as an mc_smart_subject. */
static mc_smart_subject subject_of(jboolean albums) {
  return albums ? MC_SMART_ALBUMS : MC_SMART_TRACKS;
}

/* The fields the subject's rules can use, in the core's order for a picker (mc_smart_field_get()). */
JNI_FN(jobjectArray, SmartNatives, nativeFields, jboolean albums) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SmartField"), "(" J_STRING J_STRING J_STRINGS "Z)V", &info);
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
    jobject item = type ? (*env)->NewObject(env, info, ctor, name, type, ops, JNI_BOOL(f.sortable)) : NULL;
    if (!item) {
      out = NULL;
      break;
    }
    (*env)->SetObjectArrayElement(env, out, i, item);
    JNI_DELETE(item, name, type, ops);
  }
  return out;
}

/* A field's label for a picker (mc_smart_field_label()), or null for a name the rule format
 * doesn't have. */
JNI_FN(jstring, SmartNatives, nativeFieldLabel, jstring field) {
  JNI_STR(cfield, field, NULL);
  return new_string(env, mc_smart_field_label(JNI_OR_EMPTY(cfield)));
}

/* An operator's label after a field of type (mc_smart_op_label()), or null. */
JNI_FN(jstring, SmartNatives, nativeOpLabel, jstring type, jstring op) {
  JNI_STR(ctype, type, NULL);
  JNI_STR(cop, op, NULL);
  return new_string(env, mc_smart_op_label(JNI_OR_EMPTY(ctype), JNI_OR_EMPTY(cop)));
}

/* A sort order's label (mc_smart_order_label()), or null for a field nothing sorts on. */
JNI_FN(jstring, SmartNatives, nativeOrderLabel, jstring field, jboolean desc) {
  JNI_STR(cfield, field, NULL);
  return new_string(env, mc_smart_order_label(JNI_OR_EMPTY(cfield), desc ? 1 : 0));
}

/* Whether rule is valid for the subject (mc_smart_validate()); nothing is saved. */
JNI_FN(jboolean, SmartNatives, nativeValidate, jboolean albums, jstring rule) {
  JNI_STR(crule, rule, JNI_FALSE);
  return JNI_BOOL(mc_smart_validate(subject_of(albums), JNI_OR_EMPTY(crule)) == MC_OK);
}

/* rule as a sentence (mc_smart_describe()), naming playlists and groups as they're called now. */
JNI_FN(jstring, SmartNatives, nativeDescribe, jlong handle, jboolean albums, jstring rule) {
  JNI_STR(crule, rule, NULL);
  char *sentence = NULL;
  JNI_CHECK(mc_smart_describe(jni_lib_of(handle), subject_of(albums), JNI_OR_EMPTY(crule), &sentence), NULL);
  jstring out = new_string(env, JNI_OR_EMPTY(sentence));
  mc_free(sentence);
  return out;
}

/* How many tracks or albums rule picks (mc_smart_count()), at most its limit. */
JNI_FN(jint, SmartNatives, nativeCount, jlong handle, jboolean albums, jstring rule) {
  JNI_STR(crule, rule, 0);
  int n = 0;
  jni_check(env, mc_smart_count(jni_lib_of(handle), subject_of(albums), JNI_OR_EMPTY(crule), &n));
  return (jint)n;
}

/* A new smart list called name with rule; returns its uid. */
JNI_FN(jstring, SmartNatives, nativeCreate, jlong handle, jstring name, jboolean albums, jstring rule) {
  JNI_STR(cname, name, NULL);
  JNI_STR(crule, rule, NULL);
  char uid[MC_UID_STRLEN];
  JNI_CHECK(mc_smart_create(jni_lib_of(handle), JNI_OR_EMPTY(cname), subject_of(albums), JNI_OR_EMPTY(crule), uid), NULL);
  return (*env)->NewStringUTF(env, uid);
}

JNI_FN(void, SmartNatives, nativeRename, jlong handle, jstring uid, jstring name) {
  JNI_STR(cuid, uid);
  JNI_STR(cname, name);
  JNI_CHECK(mc_smart_rename(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(cname)));
}

JNI_FN(void, SmartNatives, nativeSetRule, jlong handle, jstring uid, jstring rule) {
  JNI_STR(cuid, uid);
  JNI_STR(crule, rule);
  JNI_CHECK(mc_smart_set_rule(jni_lib_of(handle), JNI_OR_EMPTY(cuid), JNI_OR_EMPTY(crule)));
}

JNI_FN(void, SmartNatives, nativeDelete, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_smart_delete(jni_lib_of(handle), JNI_OR_EMPTY(cuid)));
}
