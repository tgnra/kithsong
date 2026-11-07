package ng.kithso.android.ui

import android.content.Context
import android.view.KeyEvent
import android.view.KeyboardShortcutGroup
import android.view.KeyboardShortcutInfo
import ng.kithso.android.R

// The keyboard shortcuts, as the desktops have them (on_shortcut() in desktop/linux/src/main.c,
// shortcut() in desktop/windows/src/main.c), for a tablet, a phone in desktop mode or a
// Chromebook with a keyboard:
//
//     Space               play or pause, unless the focus is on a text field or a button
//     Ctrl+Right, Left    next and previous track
//     Shift+Right, Left   forward and back 5 seconds
//     Ctrl+Up, Down, + -  the volume, the system's media volume here
//     Ctrl+M              mute or unmute
//     Ctrl+S              shuffle on or off
//     Ctrl+F              Search
//     Ctrl+1 to 4         Albums, Artists, Playlists, Album groups (the tablet layout's)
//     Ctrl+,              Settings
//     Ctrl+O              import a folder
//     Ctrl+N              a new playlist
//     Ctrl+?              this list, as Meta+/ shows it
//     Escape              back: closes the track list, the player or the page
//
// A text field keeps the keys it uses itself: the arrows, Space, + and -. The menu key and
// Shift+F10 on a tile or row are its own (listItemKeys()), as are Enter, Tab and the arrows,
// which move the focus. The media keys go to the media session without the app's help.
// The desktops' F1 (About), Ctrl+W and Ctrl+Q (quit) have nothing to do on Android.

/** What a keyboard shortcut does. [inText] ones work with the focus in a text field; [repeats] ones repeat while held. */
enum class Shortcut(val inText: Boolean = true, val repeats: Boolean = false) {
    PlayPause(inText = false),
    Next(inText = false),
    Previous(inText = false),
    SeekForward(inText = false, repeats = true),
    SeekBack(inText = false, repeats = true),
    VolumeUp(inText = false, repeats = true),
    VolumeDown(inText = false, repeats = true),
    Mute,
    Shuffle,
    Search,
    Albums,
    Artists,
    Playlists,
    AlbumGroups,
    Settings,
    Import,
    NewPlaylist,
    Shortcuts,
    Back;

    /**
     * Whether it waits for the focused element to turn the key down first: Space, which a
     * button or a checkbox presses, plays or pauses only where nothing else wants it.
     */
    val afterFocused: Boolean get() = this == PlayPause
}

/** How far Shift+Right and Shift+Left seek, as on the desktops. */
const val SEEK_STEP_MS = 5_000L

/**
 * The shortcut that [keyCode] is with those modifiers held, or null. Meta+ keys are the
 * system's own, so none of these has Meta.
 */
fun shortcutFor(keyCode: Int, ctrl: Boolean, shift: Boolean, alt: Boolean, meta: Boolean): Shortcut? {
    if (alt || meta) return null
    if (ctrl && shift) return if (keyCode == KeyEvent.KEYCODE_SLASH) Shortcut.Shortcuts else null
    if (ctrl) {
        return when (keyCode) {
            KeyEvent.KEYCODE_DPAD_RIGHT -> Shortcut.Next
            KeyEvent.KEYCODE_DPAD_LEFT -> Shortcut.Previous
            KeyEvent.KEYCODE_DPAD_UP -> Shortcut.VolumeUp
            KeyEvent.KEYCODE_DPAD_DOWN -> Shortcut.VolumeDown
            KeyEvent.KEYCODE_M -> Shortcut.Mute
            KeyEvent.KEYCODE_S -> Shortcut.Shuffle
            KeyEvent.KEYCODE_F -> Shortcut.Search
            KeyEvent.KEYCODE_1 -> Shortcut.Albums
            KeyEvent.KEYCODE_2 -> Shortcut.Artists
            KeyEvent.KEYCODE_3 -> Shortcut.Playlists
            KeyEvent.KEYCODE_4 -> Shortcut.AlbumGroups
            KeyEvent.KEYCODE_COMMA -> Shortcut.Settings
            KeyEvent.KEYCODE_O -> Shortcut.Import
            KeyEvent.KEYCODE_N -> Shortcut.NewPlaylist
            else -> null
        }
    }
    return when (keyCode) {
        KeyEvent.KEYCODE_DPAD_RIGHT -> if (shift) Shortcut.SeekForward else null // the arrows alone move the focus
        KeyEvent.KEYCODE_DPAD_LEFT -> if (shift) Shortcut.SeekBack else null
        // + is Shift and = on most keyboards, so either does.
        KeyEvent.KEYCODE_PLUS, KeyEvent.KEYCODE_EQUALS, KeyEvent.KEYCODE_NUMPAD_ADD -> Shortcut.VolumeUp
        KeyEvent.KEYCODE_MINUS, KeyEvent.KEYCODE_NUMPAD_SUBTRACT -> Shortcut.VolumeDown
        KeyEvent.KEYCODE_SPACE -> if (shift) null else Shortcut.PlayPause
        KeyEvent.KEYCODE_ESCAPE -> if (shift) null else Shortcut.Back
        else -> null
    }
}

/**
 * The window's keyboard shortcuts, between the activity, which hands it every key, and [App],
 * which says what each shortcut does ([perform]) while it's showing. Most are taken before the
 * focused element sees the key ([beforeFocused]), as the desktops' are, so the arrows that
 * move the focus don't hide Ctrl+Right; Space waits until it has turned it down
 * ([afterFocused]). A dialog or menu over the window is a window of its own, which keeps the
 * keys to itself, as on the desktops.
 */
class KeyboardShortcuts {
    /** What each shortcut does, set while [App] is showing. */
    var perform: ((Shortcut) -> Unit)? = null

    /** Whether the Ctrl+1 to 4 pages are there to go to, for the list of shortcuts: they're the tablet layout's. */
    var hasPages = false

    // Keys taken as shortcuts, whose release goes with them rather than to an element that never saw the press.
    private val held = mutableSetOf<Int>()

    /** Runs [shortcut], as Space on a tile does. */
    fun run(shortcut: Shortcut) {
        perform?.invoke(shortcut)
    }

    /** [event] before the focused element has it, with [textFocused] if that's a text field. True if it was taken. */
    fun beforeFocused(event: KeyEvent, textFocused: Boolean): Boolean = handle(event, textFocused, after = false)

    /** [event] that the focused element and the focus didn't use. True if it was taken. */
    fun afterFocused(event: KeyEvent, textFocused: Boolean): Boolean = handle(event, textFocused, after = true)

    private fun handle(event: KeyEvent, textFocused: Boolean, after: Boolean): Boolean {
        if (event.action == KeyEvent.ACTION_UP) return held.remove(event.keyCode)
        if (event.action != KeyEvent.ACTION_DOWN || perform == null) return false
        val shortcut = shortcutFor(event.keyCode, event.isCtrlPressed, event.isShiftPressed, event.isAltPressed, event.isMetaPressed)
        if (shortcut == null || shortcut.afterFocused != after || (textFocused && !shortcut.inText)) return false
        held += event.keyCode
        // Toggles act once while a key is held; the volume and seeking go on.
        if (event.repeatCount == 0 || shortcut.repeats) run(shortcut)
        return true
    }

    /** The shortcuts, for the system's list of them (Meta+/ or Ctrl+?, Activity.onProvideKeyboardShortcuts). */
    fun groups(context: Context): List<KeyboardShortcutGroup> {
        fun key(label: Int, keyCode: Int, modifiers: Int = 0) = KeyboardShortcutInfo(context.getString(label), keyCode, modifiers)
        val ctrl = KeyEvent.META_CTRL_ON
        val shift = KeyEvent.META_SHIFT_ON
        val playback = listOf(
            key(R.string.keys_play_pause, KeyEvent.KEYCODE_SPACE),
            key(R.string.next_track, KeyEvent.KEYCODE_DPAD_RIGHT, ctrl),
            key(R.string.previous_track, KeyEvent.KEYCODE_DPAD_LEFT, ctrl),
            key(R.string.keys_seek_forward, KeyEvent.KEYCODE_DPAD_RIGHT, shift),
            key(R.string.keys_seek_back, KeyEvent.KEYCODE_DPAD_LEFT, shift),
            key(R.string.keys_volume_up, KeyEvent.KEYCODE_DPAD_UP, ctrl),
            key(R.string.keys_volume_down, KeyEvent.KEYCODE_DPAD_DOWN, ctrl),
            key(R.string.keys_mute, KeyEvent.KEYCODE_M, ctrl),
            key(R.string.keys_shuffle, KeyEvent.KEYCODE_S, ctrl),
        )
        val general = buildList {
            add(key(R.string.search, KeyEvent.KEYCODE_F, ctrl))
            if (hasPages) {
                add(key(R.string.section_albums, KeyEvent.KEYCODE_1, ctrl))
                add(key(R.string.section_artists, KeyEvent.KEYCODE_2, ctrl))
                add(key(R.string.section_playlists, KeyEvent.KEYCODE_3, ctrl))
                add(key(R.string.section_album_groups, KeyEvent.KEYCODE_4, ctrl))
            }
            add(key(R.string.settings, KeyEvent.KEYCODE_COMMA, ctrl))
            add(key(R.string.keys_import, KeyEvent.KEYCODE_O, ctrl))
            add(key(R.string.new_playlist_menu, KeyEvent.KEYCODE_N, ctrl))
            add(key(R.string.keys_back, KeyEvent.KEYCODE_ESCAPE))
            add(key(R.string.keys_shortcuts, KeyEvent.KEYCODE_SLASH, ctrl or shift))
        }
        val lists = listOf(
            key(R.string.keys_open, KeyEvent.KEYCODE_ENTER),
            key(R.string.keys_item_menu, KeyEvent.KEYCODE_MENU),
            key(R.string.keys_item_menu, KeyEvent.KEYCODE_F10, shift),
        )
        return listOf(
            KeyboardShortcutGroup(context.getString(R.string.keys_playback), playback),
            KeyboardShortcutGroup(context.getString(R.string.keys_general), general),
            KeyboardShortcutGroup(context.getString(R.string.keys_lists), lists),
        )
    }
}
