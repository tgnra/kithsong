package ng.kithso.android

import android.content.SharedPreferences
import android.util.Log
import ng.kithso.core.CoreSettings
import ng.kithso.core.MusicCoreException

/**
 * The app's preferences, and what it remembers having asked for, in the core's settings.json
 * ([core]) as the desktops keep theirs; each change is saved at once. They were kept in [old]
 * before, and are moved from there once. The library folder isn't set: on a phone it's always
 * in the app's storage.
 */
class AppSettings(private val core: CoreSettings, old: SharedPreferences) {
    init {
        if (OLD_KEYS.any(old::contains)) {
            if (old.getBoolean(OLD_ASKED_FOR_NOTIFICATIONS, false)) core.setBool(KEY_ASKED_FOR_NOTIFICATIONS, true)
            old.getString(OLD_DEVICE_NAME, null)?.let { core.deviceName = it }
            if (old.contains(OLD_SCROBBLE)) core.scrobble = old.getBoolean(OLD_SCROBBLE, true)
            if (save()) old.edit().apply { OLD_KEYS.forEach(::remove) }.apply()
        }
    }

    /** The notification permission is asked for once, unprompted; settings can ask again. */
    var askedForNotifications: Boolean
        get() = core.bool(KEY_ASKED_FOR_NOTIFICATIONS, false)
        set(value) {
            core.setBool(KEY_ASKED_FOR_NOTIFICATIONS, value)
            save()
        }

    /**
     * What this phone was renamed to, here or by another device in its group; null goes by the
     * name set in the system settings.
     */
    var deviceName: String?
        get() = core.deviceName
        set(value) {
            core.deviceName = value
            save()
        }

    /** Whether this phone scrobbles its plays; the account itself is the sync group's. */
    var scrobble: Boolean
        get() = core.scrobble
        set(value) {
            core.scrobble = value
            save()
        }

    /**
     * Whether this phone has said which device is the archive, which it does once, after it
     * first joins a group. The same key as the desktops'.
     */
    var archiveExplained: Boolean
        get() = core.bool(KEY_ARCHIVE_EXPLAINED, false)
        set(value) {
            core.setBool(KEY_ARCHIVE_EXPLAINED, value)
            save()
        }

    /** Whether playback shuffles, as it was left. */
    var shuffle: Boolean
        get() = core.shuffle
        set(value) {
            if (value == core.shuffle) return
            core.shuffle = value
            save()
        }

    /** The playback volume, 0 to 100, as it was left; nothing sets it yet. */
    val volume: Int get() = core.volume

    // Failing, it's saved with the next change, as on the desktops.
    private fun save(): Boolean = try {
        core.save()
        true
    } catch (e: MusicCoreException) {
        Log.e(TAG, "Couldn't save the settings", e)
        false
    }

    private companion object {
        const val TAG = "kithsong"
        // A frontend's own bool in settings.json (the rest have the core's own keys).
        const val KEY_ASKED_FOR_NOTIFICATIONS = "android_asked_for_notifications"
        const val KEY_ARCHIVE_EXPLAINED = "archive_explained"
        // What they were in [old].
        const val OLD_ASKED_FOR_NOTIFICATIONS = "asked_for_notifications"
        const val OLD_DEVICE_NAME = "device_name"
        const val OLD_SCROBBLE = "scrobble"
        val OLD_KEYS = listOf(OLD_ASKED_FOR_NOTIFICATIONS, OLD_DEVICE_NAME, OLD_SCROBBLE)
    }
}
