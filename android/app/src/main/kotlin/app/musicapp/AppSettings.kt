package app.musicapp

import android.content.SharedPreferences

/**
 * The app's own preferences, and what it remembers having asked for. The core's settings.json
 * only holds the library folder, which on a phone is always in the app's storage.
 */
class AppSettings(private val prefs: SharedPreferences) {
    /** The notification permission is asked for once, unprompted; settings can ask again. */
    var askedForNotifications: Boolean
        get() = prefs.getBoolean(KEY_ASKED_FOR_NOTIFICATIONS, false)
        set(value) = prefs.edit().putBoolean(KEY_ASKED_FOR_NOTIFICATIONS, value).apply()

    private companion object {
        const val KEY_ASKED_FOR_NOTIFICATIONS = "asked_for_notifications"
    }
}
