/* Helpers jni_bridge.c shares with the bridge's other files (jni_browse.c, jni_settings.c). */
#ifndef KITHSONG_JNI_BRIDGE_H
#define KITHSONG_JNI_BRIDGE_H

#include <jni.h>

#include "musiccore/musiccore.h"

/* Throws MusicCoreException(status) and returns status. */
mc_status throw_status(JNIEnv *env, mc_status st);

/* The mc_sync of a CoreSync's handle (jni_sync_here.c). */
mc_sync *jni_sync_of(jlong handle);

/* A Java string for s, or NULL for NULL. */
jstring new_string(JNIEnv *env, const char *s);

/* An AlbumInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray album_array(JNIEnv *env, mc_status st, mc_album_list *list);

/* A TrackInfo[] of list, which it frees, or NULL with an exception pending. st is the
 * status of the call that made the list. */
jobjectArray track_array(JNIEnv *env, mc_status st, mc_track_list *list);

#endif
