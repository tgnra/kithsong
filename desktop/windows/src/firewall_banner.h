/* The strip across the top of a device's page while the firewall stands between this PC and
 * the user's other devices (firewall.h has what's checked, and how):
 *
 *   +------------------------------------------------------------------+
 *   | /!\ Windows Firewall is blocking your other devices              |
 *   |     They can't find this PC or connect to it on Home Wi-Fi       |
 *   |     (Private network). Allow Kithsong through the firewall ...   |
 *   |     [Allow]                                                      |
 *   +------------------------------------------------------------------+
 *
 * It says what's blocked on which networks, and offers what would really fix it:
 *
 *   Blocked, private or domain networks     Allow (UAC)
 *   Blocked, only public networks           Network settings, Allow on public networks (UAC)
 *   A block rule names Kithsong             Open allowed apps: a rule of ours wouldn't help
 *   The profile blocks everything           Open Windows Security: nor here
 *   Group policy, another firewall, or      no button: nothing Kithsong can do would work, so
 *   a firewall that can't be read           it says so and says who can
 *
 * Those last two can't tell whether anything is blocked at all, so they have the info icon
 * on the accent's tint where the others have a warning on the caution colour, and they stay
 * away once devices have been seen to connect. There's nothing to dismiss: it goes by itself
 * when the check that runs while the page shows finds the problem gone, and it's on this
 * page alone, so it interrupts nobody. In high contrast it's an outline in the text colour.
 *
 * The strip is a child of the main window, like the page's controls, and so are its buttons,
 * which sit on top of it; device_view.c places it and routes their messages here. */
#ifndef KITHSONG_FIREWALL_BANNER_H
#define KITHSONG_FIREWALL_BANNER_H

#include <windows.h>

/* The strip takes id, and its two buttons id + 1 and id + 2. */
void firewall_banner_create(HWND parent, int id);
void firewall_banner_set_font(HFONT font, UINT dpi);
void firewall_banner_theme_changed(void);

/* Reads firewall_get_status() again. Returns TRUE if the strip's size may have changed. */
BOOL firewall_banner_update(void);
BOOL firewall_banner_shown(void);
/* Puts it at x, y, w wide, or hides it. Returns its height: 0 when hidden. */
int firewall_banner_place(BOOL visible, int x, int y, int w);

/* TRUE if id is one of its buttons, which it then acts on. */
BOOL firewall_banner_on_command(int id, int code);
/* For the parent's WM_CTLCOLORBTN: the strip's colour behind wnd's corners, or NULL if wnd
 * isn't one of its buttons. */
HBRUSH firewall_banner_ctlcolor(HWND wnd, HDC dc);

#endif
