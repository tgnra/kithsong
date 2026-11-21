package ng.kithso.core

/**
 * The core's per-device settings (mc_settings): settings.json in folder [dir], as the desktops
 * keep theirs. Changes are kept in memory until [save]. It may be used from any thread; each
 * call takes a lock, as mc_settings isn't thread-safe. Throws KithCoreException if the
 * settings can't be loaded (a missing or unreadable file gives empty ones).
 */
class CoreSettings(dir: String) : AutoCloseable {
    private var handle = nativeLoad(dir)

    /** The name this device shows to others, or null for none saved. */
    var deviceName: String?
        @Synchronized get() = nativeDeviceName(handle)
        // null or "" clears it.
        @Synchronized set(value) = nativeSetDeviceName(handle, value)

    /** A yes/no preference of the app's own under [key], or [def] if none is saved. */
    @Synchronized fun bool(key: String, def: Boolean): Boolean = nativeBool(handle, key, def)

    @Synchronized fun setBool(key: String, value: Boolean) = nativeSetBool(handle, key, value)

    /** Text of the app's own under [key], or null if none is saved. */
    @Synchronized fun string(key: String): String? = nativeString(handle, key)

    /** null clears it. */
    @Synchronized fun setString(key: String, value: String?) = nativeSetString(handle, key, value)

    /** Whether playback shuffles, as it was left (CorePlayer.setShuffle). Off if never set. */
    var shuffle: Boolean
        @Synchronized get() = nativeShuffle(handle)
        @Synchronized set(value) = nativeSetShuffle(handle, value)

    /**
     * Whether playing a track from an album's track list turns shuffle off, so the album plays
     * in order (mc_settings_album_in_order). On if never set.
     */
    var albumInOrder: Boolean
        @Synchronized get() = nativeAlbumInOrder(handle)
        @Synchronized set(value) = nativeSetAlbumInOrder(handle, value)

    /**
     * Whether what's added to the queue goes in as a row per track rather than one row
     * (mc_settings_queue_tracks). Off if never set.
     */
    var queueTracks: Boolean
        @Synchronized get() = nativeQueueTracks(handle)
        @Synchronized set(value) = nativeSetQueueTracks(handle, value)

    /** The playback volume, 0 to 100, as it was left (CorePlayer.setVolume). 100 if never set. */
    var volume: Int
        @Synchronized get() = nativeVolume(handle)
        @Synchronized set(value) = nativeSetVolume(handle, value)

    /** Whether this device scrobbles its plays while its group is signed in. On if never set. */
    var scrobble: Boolean
        @Synchronized get() = nativeScrobble(handle)
        @Synchronized set(value) = nativeSetScrobble(handle, value)

    /** Writes the settings to settings.json, keeping keys this version doesn't know. */
    @Synchronized fun save() = nativeSave(handle)

    @Synchronized override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    private companion object {
        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeLoad(dir: String): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativeSave(handle: Long)
        @JvmStatic external fun nativeDeviceName(handle: Long): String?
        @JvmStatic external fun nativeSetDeviceName(handle: Long, name: String?)
        @JvmStatic external fun nativeString(handle: Long, key: String): String?
        @JvmStatic external fun nativeSetString(handle: Long, key: String, value: String?)
        @JvmStatic external fun nativeBool(handle: Long, key: String, def: Boolean): Boolean
        @JvmStatic external fun nativeSetBool(handle: Long, key: String, value: Boolean)
        @JvmStatic external fun nativeShuffle(handle: Long): Boolean
        @JvmStatic external fun nativeSetShuffle(handle: Long, on: Boolean)
        @JvmStatic external fun nativeAlbumInOrder(handle: Long): Boolean
        @JvmStatic external fun nativeSetAlbumInOrder(handle: Long, on: Boolean)
        @JvmStatic external fun nativeQueueTracks(handle: Long): Boolean
        @JvmStatic external fun nativeSetQueueTracks(handle: Long, on: Boolean)
        @JvmStatic external fun nativeVolume(handle: Long): Int
        @JvmStatic external fun nativeSetVolume(handle: Long, volume: Int)
        @JvmStatic external fun nativeScrobble(handle: Long): Boolean
        @JvmStatic external fun nativeSetScrobble(handle: Long, on: Boolean)
    }
}
