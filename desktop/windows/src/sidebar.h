/* The navigation list on the left: an owner-drawn list box of pages under section headers,
 * with Import and Settings at the bottom. Only pages can be selected, by mouse or keyboard;
 * selecting one sends the parent WM_COMMAND with LBN_SELCHANGE. The DEVICES heading has a +
 * at its end, to add a device by address (SBN_ADD_DEVICE).
 *
 * Under DEVICES: this PC once it's in a group, the paired devices (each a PAGE_DEVICE row),
 * and an "Unpaired devices (n)" accordion of nearby devices, which ask to pair when clicked
 * (SBN_PAIR_DEVICE). A two-line footer (version, address) sits below Settings. */
#ifndef MUSICAPP_SIDEBAR_H
#define MUSICAPP_SIDEBAR_H

#include <windows.h>

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

/* sidebar_device.flags */
enum {
  SB_DEV_ONLINE = 1,
  SB_DEV_SELF = 2,        /* this PC, shown "(this PC)" */
  SB_DEV_ARCHIVE = 4,
  SB_DEV_OTHER_GROUP = 8, /* nearby but in another group: greyed out, can't pair */
  SB_DEV_SYNCING = 16,
};

/* A device row. The sidebar copies what it needs. */
typedef struct sidebar_device {
  char uid[37];
  const wchar_t *name;
  int kind; /* an mc_device_kind */
  unsigned flags;
} sidebar_device;

/* Creates the list as a child of parent, with Albums selected. Only one exists at a time. */
HWND sidebar_create(HWND parent, int id);

/* Rebuilds fonts, row heights and colours. Call after creating it, and on DPI or theme
 * changes (including theme.mica). */
void sidebar_update(UINT dpi);

sidebar_page sidebar_selection(void);
/* Selects a page without notifying the parent. */
void sidebar_select(sidebar_page page);
/* While locked, every page but the selected one is greyed out and can't be selected, and
 * the + is greyed out too. */
void sidebar_set_locked(BOOL locked);

/* Replaces the device rows: paired are the group's members (this PC included, with
 * SB_DEV_SELF), nearby the devices discovery sees outside it. Keeps the selection by page
 * and uid; if the selected device is gone, selects Albums. Doesn't notify the parent. */
void sidebar_set_devices(const sidebar_device *paired, int npaired, const sidebar_device *nearby, int nnearby);
/* The two small lines at the bottom (NULL for none). */
void sidebar_set_footer(const wchar_t *line1, const wchar_t *line2);
/* The uid of the device whose page is selected, or NULL if it isn't PAGE_DEVICE. Valid until
 * the devices change. */
const char *sidebar_selected_device(void);
/* Selects a device's page without notifying the parent. */
void sidebar_select_device(const char *uid);
/* The nearby device of the last SBN_PAIR_DEVICE, or NULL. Valid until the devices change. */
const char *sidebar_clicked_nearby(void);

/* Paints a row: call from the parent's WM_DRAWITEM for the sidebar. */
void sidebar_draw(const DRAWITEMSTRUCT *dis);

/* The background brush for the parent's WM_CTLCOLORLISTBOX, or NULL if wnd isn't the sidebar. */
HBRUSH sidebar_ctlcolor(HWND wnd);

#endif
