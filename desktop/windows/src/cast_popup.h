/* The Google Cast drop-down, under the toolbar's cast button:
 *
 *      ===O---------------  [shf] [cst] [que] [spk]      _ [] X
 *     +-------------------------------+
 *     | Cast to                       |
 *     |-------------------------------|
 *     | [cst] Living Room TV          |
 *     |       Chromecast              |
 *     | [cst] Kitchen speaker         |
 *     | ...                           |
 *     +-------------------------------+
 *
 * A popup window as queue_popup.c is: a click elsewhere, Escape or another window taking
 * over closes it, and the main window keeps looking active meanwhile.
 *
 * Not casting, it lists the devices found (playback_cast_devices()), each by its name, else
 * its model, and says it's looking while there are none; picking one casts to it. Connecting,
 * it says so ("Connecting to Living Room TV…") over Stop casting; casting ("Casting to Living
 * Room TV"), with the device's volume, a slider as the volume drop-down's, between the two.
 * The popup stays open through it all, and keeps up as cast_popup_update() says things
 * changed. The keyboard does it all (on_key()), and UI Automation sees it. Only one exists at
 * a time.
 */
#ifndef KITHSONG_CAST_POPUP_H
#define KITHSONG_CAST_POPUP_H

#include <windows.h>

/* What main.c keeps in step: the device's volume, which the speaker button shows too, and the
 * cast button, which stays while the popup is open, devices or not. Connecting and stopping go
 * to playback.h directly. */
typedef struct cast_popup_host {
  void (*set_volume)(int volume); /* 0 to 100; playback_set_volume() while casting */
  void (*closed)(void);           /* it closed, however it did */
} cast_popup_host;

/* Opens the popup under anchor, the cast button, at the window's dpi, or closes it if it's
 * open. anchor is redrawn when it opens and closes, as it shows which. */
void cast_popup_toggle(HWND anchor, UINT dpi, const cast_popup_host *host);
BOOL cast_popup_is_open(void);
/* Closes it, if it's open, as for a theme or DPI change. */
void cast_popup_close(void);
/* The devices, the cast's state or the device's volume changed: shows them, if it's open. */
void cast_popup_update(void);

#endif
