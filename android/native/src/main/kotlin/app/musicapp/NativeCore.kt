package app.musicapp

/** Bindings to musiccore (android/native/src/main/cpp/jni_bridge.c). */
object NativeCore {
    init {
        System.loadLibrary("musicapp_jni")
    }

    @JvmStatic external fun version(): String

    /** Album count of the library in folder [libraryDir], or a negative mc_status code. */
    @JvmStatic external fun albumCount(libraryDir: String): Long
}
