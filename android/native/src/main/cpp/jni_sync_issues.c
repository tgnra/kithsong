/* JNI bindings for CoreSync's issues (SyncIssue), notices, network changes and taking over a
 * lost archive (SyncArchiveOffer), apart from jni_bridge.c. */
#include <jni.h>

#include "kithcore/kithcore.h"

#include "jni_bridge.h"

/* A SyncIssue for issue, worded with MC_SYNC_TEXT_APP_OPEN. */
static jobject new_issue(JNIEnv *env, jclass cls, jmethodID ctor, const void *item) {
  const mc_sync_issue *issue = item;
  mc_sync_issue_text t;
  mc_sync_issue_describe(issue, MC_SYNC_TEXT_APP_OPEN, &t);
  jstring uid = new_string(env, issue->device_uid);
  jstring name = new_string(env, JNI_OR_EMPTY(issue->device_name));
  jstring title = new_string(env, t.title);
  jstring body = new_string(env, t.body);
  jstring fix = new_string(env, t.fix);
  jobject out = NULL;
  if (uid && name && title && body && fix)
    out = (*env)->NewObject(env, cls, ctor, (jint)issue->kind, uid, name, JNI_BOOL(issue->notice),
                            (jint)issue->tracks, (jint)issue->count, title, body, fix);
  JNI_DELETE(uid, name, title, body, fix);
  return out;
}

jobjectArray jni_issue_array(JNIEnv *env, const mc_sync_problems *p) {
  jclass info;
  jmethodID ctor =
    jni_ctor(env, JNI_CLASS("SyncIssue"), "(I" J_STRING J_STRING "ZII" J_STRING J_STRING J_STRING ")V", &info);
  if (!ctor) return NULL;
  return JNI_OBJECT_ARRAY(info, ctor, p->issues, p->issue_count, new_issue);
}

JNI_FN(void, CoreSync, nativeDismissNotices, jlong handle) {
  mc_sync_dismiss_notices(jni_sync_of(handle));
}

JNI_FN(void, CoreSync, nativeNetworkChanged, jlong handle) {
  mc_sync_network_changed(jni_sync_of(handle));
}

/* The archive takeover on offer as a SyncArchiveOffer, worded for a phone (the app words a
 * tablet's title itself), or null if none. Blocks for a read of every track. */
JNI_FN(jobject, CoreSync, nativeArchiveOffer, jlong handle) {
  jclass info;
  jmethodID ctor = jni_ctor(env, JNI_CLASS("SyncArchiveOffer"),
                            "(" J_STRING J_STRING "ZZZZI" J_STRING J_STRING J_STRING ")V", &info);
  if (!ctor) return NULL;
  mc_sync_archive_offer *offer = NULL;
  JNI_CHECK(mc_sync_get_archive_offer(jni_sync_of(handle), &offer), NULL);
  if (!offer) return NULL;
  mc_sync_offer_text t;
  mc_sync_archive_offer_describe(offer, MC_DEVICE_PHONE, &t);
  jstring uid = new_string(env, offer->device_uid);
  jstring name = new_string(env, JNI_OR_EMPTY(offer->name));
  jstring title = new_string(env, t.title);
  jstring body = new_string(env, t.body);
  jstring button = new_string(env, t.button);
  jobject out = NULL;
  if (uid && name && title && body && button)
    out = (*env)->NewObject(env, info, ctor, uid, name, JNI_BOOL(offer->asked), JNI_BOOL(offer->can_take),
                            JNI_BOOL(offer->nobody_can), JNI_BOOL(offer->reinstall), (jint)offer->unrecoverable,
                            title, body, button);
  JNI_DELETE(uid, name, title, body, button);
  mc_sync_archive_offer_free(offer);
  return out;
}

JNI_FN(void, CoreSync, nativeArchiveOfferSeen, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_sync_archive_offer_seen(jni_sync_of(handle), cuid));
}

JNI_FN(void, CoreSync, nativeTakeOverArchive, jlong handle, jstring uid) {
  JNI_STR(cuid, uid);
  JNI_CHECK(mc_sync_take_over_archive(jni_sync_of(handle), cuid));
}
