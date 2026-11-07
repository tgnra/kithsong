/* Settings' Plugins section, below its heading (settings_view.c has that): every plugin found
 * (kithcore/plugins.h), each with a box that turns it on or off, its version, file and state,
 * and, while it's running, the settings it declares.
 *
 *   Plugins add what Kithsong doesn't come with ... only turn on plugins you trust.
 *   [x] iPod sync
 *       Version 1.0 · C:\Users\me\AppData\Local\Kithsong\plugins\kithsong-ipod.dll
 *       On
 *       [x] Write artwork                         (a switch it declares)
 *       Sound check  [Off               v]        (a choice)
 *       Max tracks   [5000 ]                      (a number, from its range)
 *   [ ] kithsong-other
 *       C:\...\kithsong-other.dll
 *       Off
 *
 * A plugin is native code with the app's full rights, so turning one on asks first, saying so
 * and naming its file. Turning it on loads it at once (mc_plugins_set_enabled()); turning it off
 * stops its devices and encoders being used at once, and it's unloaded at the next start, which
 * its state line says. The settings are saved after each change, and the main window is told
 * (changed), as a device target turned on may know a volume already mounted.
 *
 * The controls are children of the Settings page's content window, so they scroll with it, and
 * their messages reach the main window, which passes them on through settings_view.c. They're
 * made again from the list whenever it changes. Control ids are 600-799. */
#ifndef KITHSONG_PLUGINS_VIEW_H
#define KITHSONG_PLUGINS_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* parent is the window the controls go in, after the control after in the tab order; relayout is
 * told when they need more or less room. */
void plugins_view_create(HWND parent, HWND after, void (*relayout)(void));
void plugins_view_destroy(void);
/* The settings mc_plugins_load() was given, saved after a change, and what to call after one. */
void plugins_view_init(mc_settings *settings, void (*changed)(void));
void plugins_view_set_font(HFONT font, UINT dpi);
void plugins_view_show(BOOL shown);
/* Lays the section out from top at left, text_w wide, and returns where it ends. */
int plugins_view_layout(int left, int top, int text_w, UINT dpi);
BOOL plugins_view_on_command(int id, int code);
/* As settings_view_ctlcolor(). */
HBRUSH plugins_view_ctlcolor(HWND wnd, HDC dc);

#endif
