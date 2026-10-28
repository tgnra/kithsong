package app.kithsong

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

    /**
     * What this phone was renamed to, here or by another device in its group; null goes by the
     * name set in the system settings.
     */
    var deviceName: String?
        get() = prefs.getString(KEY_DEVICE_NAME, null)
        set(value) = prefs.edit().putString(KEY_DEVICE_NAME, value).apply()

    private companion object {
        const val KEY_ASKED_FOR_NOTIFICATIONS = "asked_for_notifications"
        const val KEY_DEVICE_NAME = "device_name"
    }
}
