package app.musicapp

import android.content.SharedPreferences

/**
 * The app's own preferences, and what it remembers having asked for. The core's settings.json
 * only holds the library folder, which on a phone is always in the app's storage.
 */
class AppSettings(private val prefs: SharedPreferences) {
    /** Download covers for imported albums that have none (mc_import_options.fetch_art). */
    var fetchArt: Boolean
        get() = prefs.getBoolean(KEY_FETCH_ART, true)
        set(value) = prefs.edit().putBoolean(KEY_FETCH_ART, value).apply()

    /** The notification permission is asked for once, unprompted; settings can ask again. */
    var askedForNotifications: Boolean
        get() = prefs.getBoolean(KEY_ASKED_FOR_NOTIFICATIONS, false)
        set(value) = prefs.edit().putBoolean(KEY_ASKED_FOR_NOTIFICATIONS, value).apply()

    /**
     * Whether the audio permission has been asked for: once it has, a denial with no
     * rationale to show means it was denied for good, and only the system settings can grant it.
     */
    var askedForAudio: Boolean
        get() = prefs.getBoolean(KEY_ASKED_FOR_AUDIO, false)
        set(value) = prefs.edit().putBoolean(KEY_ASKED_FOR_AUDIO, value).apply()

    /**
     * Folders imported before. The core copies files in without checking for ones it already
     * has, so importing a folder again would duplicate it; the import sheet leaves these unticked.
     */
    var importedFolders: Set<String>
        get() = prefs.getStringSet(KEY_IMPORTED_FOLDERS, null).orEmpty()
        set(value) = prefs.edit().putStringSet(KEY_IMPORTED_FOLDERS, value).apply()

    private companion object {
        const val KEY_FETCH_ART = "fetch_art"
        const val KEY_ASKED_FOR_NOTIFICATIONS = "asked_for_notifications"
        const val KEY_ASKED_FOR_AUDIO = "asked_for_audio"
        const val KEY_IMPORTED_FOLDERS = "imported_folders"
    }
}
