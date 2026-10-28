/* The navigation list on the left, custom-drawn: pages under section headers, with Import
 * and Settings pinned at the bottom. Pages are selected by mouse or keyboard; selecting one
 * sends the parent WM_COMMAND with LBN_SELCHANGE. The DEVICES heading has a + at its end, to
 * add a device by address (SBN_ADD_DEVICE).
 *
 * Under the pages: RECENTS, what was last played on any device (sidebar_set_recents), each
 * a button that opens it (SBN_OPEN_RECENT); the section is left out while there are none.
 * Under DEVICES: this PC once it's in a group, the paired devices (each a PAGE_DEVICE row;
 * offline ones too, marked with a small glyph), and an "Unpaired devices (n)" accordion of
 * nearby devices, which ask to pair when clicked (SBN_PAIR_DEVICE). A two-line footer
 * (version, address) sits below Settings, with a tooltip of its own.
 *
 * Everything above Import scrolls when it's taller than the space left for it, with the
 * same overlay scrollbar as the album grid (scroller.c), which isn't drawn while everything
 * fits. Import, Settings and the footer stay put below it. The arrow keys, Home and End
 * move the focus through the pages and buttons (selecting a page as they land on it, and
 * scrolling it into view); Enter or Space presses a button. */
#ifndef MUSICAPP_SIDEBAR_H
#define MUSICAPP_SIDEBAR_H

#include <windows.h>

#include "musiccore/musiccore.h"

typedef enum {
  PAGE_ALBUMS,
  PAGE_ARTISTS,
  PAGE_PLAYLISTS,
  PAGE_DEVICE, /* a paired device's page; sidebar_selected_device() says which */
  PAGE_IMPORT,
  PAGE_SETTINGS,
} sidebar_page;

/* Sent to the parent as a WM_COMMAND notification code when the + on the DEVICES heading
 * (Add Device by Address) is clicked. */
#define SBN_ADD_DEVICE 0x100
/* Sent the same way when a nearby device is clicked, to pair with it; sidebar_clicked_nearby()
 * says which. */
#define SBN_PAIR_DEVICE 0x101
/* Sent the same way when a recent is clicked, to open it; sidebar_clicked_recent() says
 * which. */
#define SBN_OPEN_RECENT 0x102

/* sidebar_device.flags */
enum {
  SB_DEV_ONLINE = 1,
  SB_DEV_SELF = 2,        /* this PC, shown "(this PC)" */
  SB_DEV_ARCHIVE = 4,
  SB_DEV_OTHER_GROUP = 8, /* nearby, and we're in different groups: only a tooltip says so,
                           * as pairing itself tells why it can't */
  SB_DEV_SYNCING = 16,
};

/* A device row. The sidebar copies what it needs. */
typedef struct sidebar_device {
  char uid[37];
  const wchar_t *name;
  int kind; /* an mc_device_kind */
  unsigned flags;
} sidebar_device;

/* A recent's row. The sidebar copies what it needs. */
typedef struct sidebar_recent {
  char uid[MC_UID_STRLEN];
  mc_recent_kind kind; /* picks the icon */
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
 * the recents, the nearby devices and the + are greyed out too. */
void sidebar_set_locked(BOOL locked);

/* Replaces the recents, most recent first. */
void sidebar_set_recents(const sidebar_recent *recents, int n);
/* The recent of the last SBN_OPEN_RECENT, copying its kind to *kind, or NULL. Valid until
 * the recents change. */
const char *sidebar_clicked_recent(mc_recent_kind *kind);

/* Replaces the device rows: paired are the group's members (this PC included, with
 * SB_DEV_SELF), nearby the devices discovery sees outside it. Keeps the selection by page
 * and uid; if the selected device is gone, selects Albums. Doesn't notify the parent. */
void sidebar_set_devices(const sidebar_device *paired, int npaired, const sidebar_device *nearby, int nnearby);
/* The two small lines at the bottom (NULL for none). */
void sidebar_set_footer(const wchar_t *line1, const wchar_t *line2);
/* The footer's tooltip, which may have several lines (NULL for none). */
void sidebar_set_footer_tip(const wchar_t *tip);
/* The uid of the device whose page is selected, or NULL if it isn't PAGE_DEVICE. Valid until
 * the devices change. */
const char *sidebar_selected_device(void);
/* Selects a device's page without notifying the parent. */
void sidebar_select_device(const char *uid);
/* The nearby device of the last SBN_PAIR_DEVICE, or NULL. Valid until the devices change. */
const char *sidebar_clicked_nearby(void);

#endif
