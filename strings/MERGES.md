# String merge log

How the UI strings of the three frontends and the core were merged into `en.json`, for
reviewing the wording. Locations are as of commit feda373, before the frontends were
converted. `L` = desktop/linux/src, `W` = desktop/windows/src, `A` =
android/app/src/main/res/values/strings.xml (now generated), `core` = core/src.

## Rules applied throughout

1. **Sentence case.** Where platforms disagreed (Linux used GNOME's title case for titles,
   buttons and headings; Android and most of Windows use sentence case), the shared string
   is in sentence case: "Add Device" -> "Add device", "Stop the Import?" -> "Stop the
   import?". Linux-only strings were moved to sentence case too, so a Linux window doesn't
   mix the two.
2. **Straight apostrophes.** Linux wrote `’` (U+2019) in some strings, everything else
   `'`. All are `'` now. The curly quotes in `Delete “{0}”?` stay (both desktops had them).
3. **Mnemonics are marked with `_`** (GTK style) in the JSON: `S_kip`. `mc_str()` and the
   Android resources drop the marker; `mc_str_mnemonic()` keeps it. So Linux `_Keep
   Existing Tags` and Windows `&IP address or hostname:` become one string each.
4. **Short error statements have no final period** ("Couldn't create the playlist"), as on
   Linux and Android; Windows message boxes had one.
5. **Windows-only decorations are separate strings**: a label's colon is
   `windows.field_label` ("{0}:") and menu accelerators are `windows.shortcut_*`, so
   "Rename…\tF2" is `playlist_rename_menu` + `shortcut_rename`.
6. **En dash between names.** Android's ` - ` separators (track_byline, album_byline_year,
   import_folder, import_folder_before) became ` – `, as the desktops already used.

## Merges (variants that differed)

| Key | Final text | Variants |
|---|---|---|
| `add_device` | Add device | "Add Device" L add_device.c:62, W add_device.c:84; "Add device" A:163 |
| `add_device_host` | \_IP address or hostname | "IP Address or Hostname" L add_device.c:77; "&IP address or hostname:" W add_device.c:94; "IP address or hostname" A:165 |
| `add_device_port` | \_Port | "Port" L add_device.c:84, A:166; "&Port:" W add_device.c:98 |
| `add_device_by_address` | Add device by address | "Add Device by Address" L main.c:187, W sidebar.c:287; A:148 |
| `add_to_playlist` | Add to playlist | "Add to Playlist" L add_to_playlist.c:86 (dialog title), W track_list.c:475 (submenu), W add_to_playlist.c:53 (caption) |
| `artist` | Artist | "Artist" L import_view.c:763, W import_view.c:460/658, W app.rc:21; A:31 `recent_artist` (renamed) |
| `close` | \_Close | "Close" L main.c:92/317, L playlist_dialogs.c:41, L track_list.c:348 (tooltip), W track_list.c:907 (tooltip); "\_Close" L import_view.c:134 |
| `devices` | Devices | "Devices" L main.c:179, A:140 `settings_devices` (renamed); "DEVICES" W sidebar.c:23 (Windows uppercases it at run time) |
| `done` | \_Done | "\_Done" L import_view.c:907; "Done" W import_view.c:652, A:79 |
| `empty_title` | No music yet | "No Albums" L main.c:434; "No albums yet." W album_grid.c:584; "No music yet" A:33 |
| `empty_text` | Use Import to add music. | "Use Import to add music." L main.c:79, W album_grid.c:584; "Import music to fill your library." A:34 |
| `import_as_tagged` | \_Keep existing tags | "\_Keep Existing Tags" L import_view.c:794; "Use Existing Tags" W app.rc:26; "Import as tagged" A:78. Linux's wording matches the descriptions that say "keep the existing tags" |
| `import_button` | \_Import | "\_Import" L import_view.c:418 (folder dialog's accept button); "Import" A:68 |
| `import_choose_folders` | \_Choose folders… | "\_Choose Folders…" L import_view.c:884; "Choose Folders…" W import_view.c:649 |
| `import_choose_folders_title` | Choose folders to import | "Choose Folders to Import" L import_view.c:417, W import_view.c:287 |
| `import_error_no_matches` | No matches on MusicBrainz | core tagger.c:583; "No match found" L import_view.c:281 (shown where the core gives no reason) |
| `import_group_active` | In progress | "In Progress" L import_view.c:13; "In progress" W import_view.c:665 |
| `import_matches` | Matches | "Matches" L import_view.c:779; "Matches on MusicBrainz" W app.rc:13 |
| `import_matching` | Importing {0:d} of {1:d} | "Importing %1$d of %2$d" A:82; "Matching %d of %d…" L import_view.c:229, W import_view.c:207 (the desktop status line is now `import_matching` + `import_imported` + `import_need_decision`) |
| `import_need_decision` | {0:d} needs a decision / {0:d} need a decision | A:97-100; "%d need your attention" L import_view.c:229/233, W import_view.c:207/211 |
| `import_needs_decision` | Needs a decision | group "Needs Your Attention" L import_view.c:13, "Needs your attention" W import_view.c:664; state "Needs review" L import_view.c:92, W import_view.c:118. One wording for the state, its group and the counts above |
| `import_no_library_text` | The library could not be opened, so nothing can be imported. | L import_view.c:357, W import_view.c:257 (identical) |
| `import_no_music` | No music found | "No audio files found." L import_view.c:227, W import_view.c:205; "No music found" A:113 |
| `import_not_matched` | Not matched; left as is | L import_view.c:570; "(not matched; left as is)" W import_view.c:370 |
| `import_review_title` | Review import | "Review Import" L import_view.c:818; "Choose a Match" W app.rc:9 |
| `import_review_album` | Album: {0} – {1} ({2:d} file/files) | L import_view.c:600-601; W import_view.c:391-394 ("%s: %s – %s" + " (%d files)", no singular) |
| `import_review_single` | Single: {0} – {1} | L import_view.c:603; W import_view.c:391-392 |
| `import_running_title` | Import still running | "Import Still Running" L import_view.c:361; A:51 |
| `import_running_text` | Wait for the current import to finish first. | L import_view.c:361, A:52; "An import is still running. Wait for it to finish first." W import_view.c:262 |
| `import_scanning_files` | Scanning… {0:d} audio file found / …files found | L import_view.c:225, W import_view.c:203 (no singular before) |
| `import_search_again` | Search again | "Search Again" L import_view.c:772; "Search again:" W app.rc:20 |
| `import_single` | Single | "Single" L import_view.c:276; "single" W import_view.c:235 |
| `import_skip` | S\_kip | "S\_kip" L import_view.c:793; "Skip" W app.rc:27, A:77 |
| `import_sheet_title` | Import music | "Import Music" L import_view.c:891; A:60 |
| `import_status_finished` | Finished: {0}, {1}, {2}. | "Finished: %d imported, %d skipped, %d failed." L import_view.c:236, W import_view.c:214; the counts are now the `import_imported`/`_skipped`/`_failed` plurals (A:93-108) |
| `import_status_review` | {0}. {1}: click one to choose a match. | "%d imported. %d need your attention: choose one to pick a match." L import_view.c:233; "…: click one to choose a match." W import_view.c:211 |
| `import_stop_title` + `import_stop_text` | Stop the import? / Anything already imported stays in your library. | "Stop the Import?" + text L import_view.c:484-485; one string W import_view.c:320 |
| `import_track_missing` | — missing | L import_view.c:556, W import_view.c:362 (identical) |
| `import_use_match` | \_Use this match | "\_Use This Match" L import_view.c:795; "Apply Match" W app.rc:28 |
| `library_folder` | Library folder | "Library Folder" L settings_view.c:103 (row title), W main.c:710 (caption) |
| `library_folder_choose` | Choose your music library folder | "Choose Your Music Library Folder" L settings_view.c:71, W main.c:700 |
| `library_location` | Library location | "Library Location" L settings_view.c:116; "Library location" W settings_view.c:33 |
| `library_location_text` | Opening another library … stays where it is. | L settings_view.c:119-120 (curly apostrophe), W settings_view.c:39-40 |
| `library_move` | Move library… | "Move Library…" L settings_view.c:112, W settings_view.c:36 |
| `library_open_another` | Open another library… | "Open Another Library…" L settings_view.c:108, W settings_view.c:37 |
| `library_open_failed` | Couldn't open the library | "Couldn't Open the Library" L main.c:91; "Could not open the library." L main.c:80, W album_grid.c:582; "Couldn't open the library" A:114 `import_no_library` (renamed), core tagger.c:1020/1081 |
| `move_up` / `move_down` | Move up / Move down | "Move Up"/"Move Down" L track_list.c:435-436; "Move Up\tAlt+Up"/"Move Down\tAlt+Down" W track_list.c:481-482 |
| `new_playlist` | New playlist | "New Playlist" L add_to_playlist.c:100 (entry row), W playlist_dialogs.c:132 (dialog title) |
| `next_track` / `previous_track` | Next track / Previous track | "Next"/"Previous" L main.c:500/504, W main.c:334, W taskbar.c:75; A:44-45 |
| `no_playlists_title` | No playlists yet | "No Playlists" L main.c:440; "No playlists yet" L add_to_playlist.c:110; "No playlists yet." W album_grid.c:583 |
| `no_playlists_text` | Right-click a track and choose Add to playlist to start one. | L main.c:442; W album_grid.c:583 (after "No playlists yet.") |
| `now_playing_artist` | — {0} | "<b>%s</b> <span alpha=60%>— %s</span>" L now_playing.c:152; L" \x2014 %ls" W now_playing.c:205 |
| `playlist` | Playlist | L track_list.c:550; W track_list.c:692, W main.c:400/404 (caption), W app.rc:36; A:32 `recent_playlist` (renamed) |
| `playlist_add_failed` | Couldn't add the track to the playlist | "Couldn't add the track" L add_to_playlist.c:65; "Couldn't add the track to the playlist." W add_to_playlist.c:53 |
| `playlist_create_failed` | Couldn't create the playlist | L add_to_playlist.c:55; W playlist_dialogs.c:136 (with period) |
| `playlist_delete_failed` | Couldn't delete the playlist | "Couldn't Delete the Playlist" L playlist_dialogs.c:108; W playlist_dialogs.c:196 |
| `playlist_delete_menu` | Delete playlist… | "Delete Playlist…" L track_list.c:335, W track_list.c:454 |
| `playlist_move_failed` | Couldn't move the track | "Couldn't Move the Track" L main.c:329; W main.c:404 |
| `playlist_remove_failed` | Couldn't remove the track | "Couldn't Remove the Track" L main.c:323; W main.c:400 |
| `playlist_rename_failed` | Couldn't rename the playlist | "Couldn't Rename the Playlist" L playlist_dialogs.c:72; W playlist_dialogs.c:146 |
| `playlist_rename_menu` | Rename… | L track_list.c:334; "Rename…\tF2" W track_list.c:453 |
| `playlist_rename_title` | Rename playlist | "Rename Playlist" L playlist_dialogs.c:80, W playlist_dialogs.c:142 |
| `remove_from_playlist` | Remove from playlist | "Remove from Playlist" L track_list.c:440; "Remove from Playlist\tDel" W track_list.c:484 |
| `search` | \_Search | "\_Search" L import_view.c:768; "Search" W app.rc:25 |
| `seek` | Position | "Position" L now_playing.c:123 (seek bar's accessible name); "Seek" A:46. A slider's name should say what it shows |
| `settings_save_failed` | Couldn't save the setting; the old folder will be used next time. | L settings_view.c:57 (no period, curly apostrophe); W main.c:748 |
| `tracks` | {0:d} track / {0:d} tracks | "%d %s" + "track"/"tracks" L add_to_playlist.c:35, L album_grid.c:57, L track_list.c:494, L import_view.c:636, W album_grid.c:366-367, W track_list.c:268-271; A:14-17 |
| `tracks_title` | Tracks | L import_view.c:784 (group), L track_list.c:466 (accessible name), W import_view.c:461 (column); "Tracks for the selected match" W app.rc:17 |
| `track_byline` | {0} – {1} | "%1$s - %2$s" A:12; "%s – %s" L playback.c:48, L import_view.c:121/629, W playback.c:63, W import_view.c:142 |
| `import_match_year` | {0} – {1} ({2:d}) | L import_view.c:120, W import_view.c:141 (identical) |
| `import_match_into` | {0} → {1} | W import_view.c:412; L import_view.c:626 builds "artist – release → track", now `track_byline` of the artist and this |
| `unknown_artist` | Unknown artist | "Unknown Artist" L album_grid.c:58, L track_list.c:552, W album_grid.c:567, W track_list.c:692; A:10 |
| `windows.col_*`, `windows.field_label` | — | Windows-only; listed for completeness of rule 5 |

## Unified (identical on two or more platforms)

`app_name` (A:3, L main.c:555, W main.c:731/800, playback titles), `cancel`, `ok` (A:9, W
app.rc:41), `connect`, `delete`, `rename`, `name`, `untitled`, `album`, `title`, `no_tracks`,
`duration_minutes`, `duration_hours_minutes`, `separator_dot` (" · ", L track_list.c:491 and
import_view.c:129, W track_list.c:266), `import_list_separator` (A:112, now also the desktop
status lines), `settings`, `section_albums`/`_artists`/`_playlists`, `import_music`
("Import": A:24, L main.c:508, W sidebar.c:26 and the Import message boxes' captions),
`play`, `pause`, `add_device_text`, `devices_none`, `import_imported`, `import_skipped`,
`import_failed`, `import_state_*` and their group headings, `import_existing_tags`,
`import_item_busy`, `import_searching`, `import_start_failed`, `playlist_delete_title`,
`playlist_delete_text`, `import_error_*` and `status_*` (core text that every frontend shows).

## Considered and kept separate

- `linux.add_to_playlist_menu` "Add to playlist…" vs `add_to_playlist`: the ellipsis means
  the item opens a dialog; Windows' item is a submenu.
- `windows.new_playlist_menu` "New playlist…" vs `new_playlist`: the same.
- `windows.playlist_delete_caption` "Delete playlist" vs `playlist_delete_menu`.
- `import_scanning_files` (desktop status line) vs `android.import_scanning` +
  `android.import_files_found` (Android's notification title and text).
- `android.import_review` "{0:d} items need a decision" (a notification) vs
  `import_need_decision`.
- `linux.import_start_text` vs `android.import_sheet_text` vs `windows.import_drop_here`:
  different content (dropping folders, MusicBrainz naming).
- `status_nomem` "out of memory" vs `import_error_nomem` "Out of memory": status reasons
  sit inside other sentences, so they're lower case.
- The several Android "Import" strings (`import_button`, `import_channel`,
  `settings_import`, `import_music`) keep their own keys: they're different contexts that
  a translation may word differently.

## Android renames

The Kotlin references were updated in the same change.

| Old | New | Why |
|---|---|---|
| `recent_artist` | `artist` | Also the desktops' field and column name |
| `recent_playlist` | `playlist` | Also the desktops' byline and captions |
| `settings_devices` | `devices` | Also the desktops' sidebar heading |
| `import_no_library` | `library_open_failed` | Also the core's import error and the desktops' message |

## Wording changes Android users will see

`track_byline`, `album_byline_year`, `import_folder`, `import_folder_before` (en dash),
`empty_text`, `import_as_tagged`, `seek` (accessibility only).

## Not in en.json on purpose

Number and time formats ("%d:%02d", "%.0f%%", "%d-%d" disc-track, "%s. %s" track lines),
icon glyphs, CSS, D-Bus/MPRIS names and errors, file names, settings keys, class names,
log and debug output, Windows' version resource (version.rc.in), and tag data the core
writes into files ("Various Artists", "Unknown Artist", "Unknown Album", "<artist>
Singles" in core/src/tagger/organize.c and tagger.c), which isn't UI text.
