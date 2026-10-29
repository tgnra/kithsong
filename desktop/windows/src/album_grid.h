/* The album grid: cover art with the album title and artist beneath, in as many columns as
 * fit. The same grid shows the playlists on the Playlists page, with their track counts,
 * and the album groups and a group's albums on the Album Groups page.
 *
 * Built for large libraries:
 *  - virtualised: only the tiles in view are painted, into an off-screen buffer.
 *  - covers are decoded on a background thread with WIC, scaled down during decoding to
 *    exactly the tile size, visible tiles first and one screen ahead in each direction.
 *    Playlists' pictures are made there too (mc_playlist_art()), on its own connection.
 *  - thumbnails far from the view are freed, so memory stays flat however many albums.
 *  - pixel scrolling, with the mouse wheel eased over a few frames, and a thin overlay
 *    scrollbar that appears while the mouse moves over the grid or it scrolls.
 *
 * Clicking an album (or moving to it with the arrow keys, Home, End, Page Up or Page Down)
 * selects it (an accent ring around the cover). The grid also draws the
 * content area's border and rounded top-left corner, which shows Mica outside it.
 *
 * Screen readers see the tiles as a list, and a group's header buttons (uia.h); those
 * have tooltips too.
 */
#ifndef KITHSONG_ALBUM_GRID_H
#define KITHSONG_ALBUM_GRID_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Creates the grid as a child of parent. Fonts arrive through WM_SETFONT. Only one grid
 * exists at a time. */
HWND album_grid_create(HWND parent, int id);

/* Shows the albums of lib (NULL shows "Could not open the library"). The list is reloaded
 * the next time the grid paints, so calling this while it's hidden (e.g. after each item of
 * an import) costs nothing. Call it again whenever the library changes. */
void album_grid_set_library(mc_library *lib);

/* What the grid shows. Playlists and album groups show their pictures (or a placeholder) with
 * the track or album count beneath the name. Clicking a group (or Enter on it) opens it:
 * AGV_GROUP, its albums under a header with Back (also Backspace, Alt+Left and the mouse's
 * back button), its name, and a More button with Rename and Delete.
 *
 * Smart playlists and smart album groups (smartlist.h) follow the others, those with
 * something to play here, with a placeholder and "Smart playlist" or "Smart album group"
 * beneath the name. A smart group opens like a group, with the albums its rule picks and the
 * rule as a sentence under its name; its More button offers Edit Rules... (smart_editor.h),
 * Rename... and Delete..., and in random order a Reshuffle button sits beside it (which the
 * grid does itself, notifying AGN_LIBRARY_CHANGED). Its albums can't be removed from it.
 *
 * Playlists and album groups have a header too, with the page's name and a + button: New
 * Playlist or New Album Group, which asks for a name (playlist_dialogs.c) and makes an empty
 * one. A new group opens; a new playlist is selected, and stays on the page (though playlists
 * with nothing to play here are hidden) until the grid shows another view. */
typedef enum album_grid_view {
  AGV_ALBUMS,
  AGV_PLAYLISTS,
  AGV_GROUPS, /* the album groups */
  AGV_GROUP,  /* one group's albums; see album_grid_open_group() */
} album_grid_view;

/* Switches the grid to view (not AGV_GROUP), which drops the selection. */
void album_grid_show(album_grid_view view);
album_grid_view album_grid_shown(void);
/* album_grid_show(AGV_PLAYLISTS or AGV_ALBUMS), and whether it's AGV_PLAYLISTS. */
void album_grid_show_playlists(BOOL playlists);
BOOL album_grid_showing_playlists(void);
/* Shows album group uid's albums, as a click on it does (notifying AGN_OPEN_GROUP). If the
 * group goes, the grid goes back to the groups. */
void album_grid_open_group(const char *uid);
/* The shown group's uid, or NULL outside AGV_GROUP. */
const char *album_grid_group(void);
/* In a group, goes back to the groups with it selected, as Backspace there does. FALSE outside
 * AGV_GROUP. */
BOOL album_grid_back(void);

/* Right-clicking an album (on Albums or in a group) offers Add to Album Group, and in a
 * group Remove from Album Group; right-clicking a group (or its header) offers Rename and
 * Delete, and a smart playlist or group Edit Rules..., Rename... and Delete.... F2 renames
 * the open group, or the selected group or smart playlist. Right-clicking the Playlists or
 * Album Groups page away from the tiles (or the menu key with nothing selected) offers New
 * Smart Playlist... or New Smart Album Group.... The grid makes those changes itself
 * (add_to_playlist.c, playlist_dialogs.c, smart_editor.c), then notifies
 * AGN_LIBRARY_CHANGED. */

/* New Smart Playlist... on the Playlists page, New Smart Album Group... on the Album Groups
 * page, as their menus do: then selects the new playlist, or opens the new group. */
void album_grid_new_smart(void);

/* This PC's place in sync, for the empty pages: with no albums and no group, Albums suggests
 * pairing, with a Pair with a Device... button (AGN_PAIR); on Playlists, when every playlist
 * is hidden for having nothing to play here and this PC is in a group, a Choose What to Sync
 * button (AGN_SYNC_SETTINGS). */
typedef enum album_grid_sync {
  AG_SYNC_OFF,     /* sync isn't running: neither */
  AG_SYNC_ALONE,   /* running, and this PC isn't in a group */
  AG_SYNC_GROUPED, /* this PC is in a group */
} album_grid_sync;
void album_grid_set_sync_state(album_grid_sync state);
/* The empty page's button, in screen coordinates (for AGN_PAIR's menu); FALSE if none. */
BOOL album_grid_action_rect(RECT *screen);

/* Where playlists' pictures are cached (UTF-8; see mc_playlist_art()). Without one,
 * playlists show the placeholder. */
void album_grid_set_playlist_art_dir(const char *dir);

/* Sent to the parent as a WM_COMMAND notification code (posted) when the selected album
 * changes, by a click or because it left the library, or when it was renamed or its cover
 * changed. */
#define AGN_SELCHANGE 1
/* Sent the same way when a group is opened (album_grid_group() says which). */
#define AGN_OPEN_GROUP 2
/* Sent the same way after the grid changed the library (see above), for the parent to
 * refresh what shows it. */
#define AGN_LIBRARY_CHANGED 3
/* Sent the same way when the empty page's Pair with a Device... is pressed, for the parent to
 * offer the devices to pair with (at album_grid_action_rect()). */
#define AGN_PAIR 4
/* Sent the same way when Choose What to Sync is pressed, for the parent to show this PC's
 * page. */
#define AGN_SYNC_SETTINGS 5

/* The selected album (or playlist): copies its uid and returns its title, or returns NULL
 * if there's none. */
const wchar_t *album_grid_selection(char uid[MC_UID_STRLEN]);
/* The selected album's artist, or NULL. Valid until the grid next reloads. */
const wchar_t *album_grid_selection_artist(void);
/* The folder holding the selected album's cover, or for a playlist its picture file, or
 * NULL if it has none (or none is selected). Valid until the grid next reloads or loads
 * the cover. */
const wchar_t *album_grid_selection_cover(void);
/* Whether the selected playlist is a smart one, and if so in *random (may be NULL) whether
 * it's in random order. */
BOOL album_grid_selection_smart(BOOL *random);
/* The selected album's year, or 0 if it's unknown (or none is selected). */
int album_grid_selection_year(void);
/* Clears the selection, which notifies the parent (AGN_SELCHANGE) as a click would. */
void album_grid_unselect(void);
/* Selects the album (or playlist, on the Playlists page) with uid and scrolls it into view,
 * which notifies the parent as a click would. FALSE if the grid hasn't got it. */
BOOL album_grid_select(const char *uid);

/* Sizes the tiles for a grid full_w wide, even while it's narrower, so a pane beside it
 * that comes and goes takes whole columns without resizing the tiles. Returns the width the
 * pane should take from the grid to be at least min_w: a whole number of columns, or 0 if
 * that would leave the grid none. Call before each layout. */
int album_grid_set_full_width(int full_w, int min_w);

/* Call after theme_update. */
void album_grid_theme_changed(void);

#endif
