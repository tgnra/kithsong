/* The navigation list on the left, custom-drawn: Home and Search, the library's pages and
 * RECENTS (left out while empty), then Sync, Import, Library stats, Settings and About pinned
 * at the bottom. Selecting a page sends the parent WM_COMMAND with LBN_SELCHANGE; everything
 * else is an SBN_ notification. Library stats and About work while the sidebar is locked too.
 *
 * Everything above Sync scrolls when it's taller than the space left for it, with the album
 * grid's overlay scrollbar (scroller.c). Screen readers see the rows as a list (uia.h). */
#ifndef KITHSONG_SIDEBAR_H
#define KITHSONG_SIDEBAR_H

#include <windows.h>

#include "kithcore/kithcore.h"

typedef enum {
  PAGE_HOME,
  PAGE_SEARCH,
  PAGE_ALBUMS,
  PAGE_ARTISTS,
  PAGE_PLAYLISTS,
  PAGE_ALBUM_GROUPS,
  PAGE_SYNC,
  PAGE_IMPORT,
  PAGE_SETTINGS,
} sidebar_page;

/* Sent to the parent as a WM_COMMAND notification code when a recent is clicked, to open it;
 * sidebar_clicked_recent() says which. */
#define SBN_OPEN_RECENT 0x102
/* Sent the same way on Enter on the selected page's row, to move the focus into the page. */
#define SBN_ENTER_PAGE 0x103
/* Sent the same way from the Playlists or Album Groups row's right-click menu (or the menu
 * key on it): New Smart Playlist... or New Smart Album Group.... */
#define SBN_NEW_SMART_PLAYLIST 0x104
#define SBN_NEW_SMART_GROUP 0x105
/* Sent the same way from a recent's right-click menu (or the menu key on it): Remove from
 * Recents (sidebar_clicked_recent() says which) and Clear Recents. */
#define SBN_REMOVE_RECENT 0x106
#define SBN_CLEAR_RECENTS 0x107
/* Sent the same way when About is clicked. */
#define SBN_ABOUT 0x108
/* Sent the same way from an album's or artist's recent's right-click menu: Edit...
 * (sidebar_clicked_recent() says which). */
#define SBN_EDIT_RECENT 0x109
/* Sent the same way when Library stats is clicked. */
#define SBN_STATS 0x10B

/* A recent's row. The sidebar copies what it needs. */
typedef struct sidebar_recent {
  char uid[MC_UID_STRLEN];
  mc_recent_kind kind; /* picks the icon */
  BOOL smart;          /* a smart playlist or group: a track or album dragged onto it isn't taken */
  const wchar_t *name;
  const wchar_t *detail; /* the tooltip's second line, e.g. the album's artist; NULL for none */
} sidebar_recent;

/* Creates the sidebar as a child of parent, with Albums selected. Only one exists at a time. */
HWND sidebar_create(HWND parent, int id);

/* Rebuilds fonts, row heights and colours. Call after creating it, and on DPI or theme
 * changes (including theme.mica). */
void sidebar_update(UINT dpi);

sidebar_page sidebar_selection(void);
/* Selects a page without notifying the parent. */
void sidebar_select(sidebar_page page);
/* While locked, every page but the selected one is greyed out and can't be selected, and
 * the recents are greyed out too. */
void sidebar_set_locked(BOOL locked);
/* Replaces the recents, most recent first. */
void sidebar_set_recents(const sidebar_recent *recents, int n);
/* The recent of the last SBN_OPEN_RECENT, SBN_REMOVE_RECENT or SBN_EDIT_RECENT, copying its kind to *kind, or NULL. Valid until
 * the recents change. */
const char *sidebar_clicked_recent(mc_recent_kind *kind);

/* A warning glyph at the end of the Sync row, with text as its tooltip; NULL for none. */
void sidebar_set_sync_warning(const wchar_t *text);

#endif
