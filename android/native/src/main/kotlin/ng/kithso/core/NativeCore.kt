package ng.kithso.core

import android.content.res.Configuration

/** Bindings to musiccore (android/native/src/main/cpp/jni_bridge.c). */
object NativeCore {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun version(): String

    /** mc_init(): process-wide setup for the network stack. Call once, before anything else. */
    @JvmStatic external fun init(): Int

    /**
     * mc_strings_set_locale(): the language of the text the core writes itself, such as
     * import errors and status reasons, from a BCP 47 tag. English until called.
     */
    @JvmStatic external fun setLocale(tag: String)

    @Volatile private var locale: String? = null

    /**
     * Gives the core [configuration]'s language, the one the app's resources are in: call it
     * at startup and on every configuration change. Returns whether the language changed
     * from an earlier call, so text the app built from the core's can be built again.
     */
    @JvmStatic fun followLocale(configuration: Configuration): Boolean {
        val tag = configuration.locales.get(0)?.toLanguageTag().orEmpty()
        val old = locale
        if (tag == old) return false
        locale = tag
        setLocale(tag)
        return old != null
    }

    /** Album count of the library in folder [libraryDir], or a negative mc_status code. */
    @JvmStatic external fun albumCount(libraryDir: String): Long
}
