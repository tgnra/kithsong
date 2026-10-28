# R8 rules for the apps that use :native (consumerProguardFiles in build.gradle.kts).
#
# The JNI bridge (src/main/cpp/jni_*.c) reaches into this package by name: FindClass and
# GetMethodID for the value classes it builds (AlbumInfo, TrackInfo, SyncDevice, ...) and the
# exception it throws (MusicCoreException), and GetObjectClass + GetMethodID for the private
# callbacks it calls on the wrappers (CoreImport.onEvent, CorePlayer.onEvent, CoreSync.onPeer,
# onPairCode, onPairResult, onProgress, onSyncDone). The native methods themselves are bound by
# their Java_ng_kithso_core_* names, so their classes and names have to stay too.
#
# None of these are visible to R8 as used, so the package is kept whole, names, constructors
# and all: it's a thin layer, and keeping it costs little next to a crash in a JNI lookup.
-keep class ng.kithso.core.** { *; }
