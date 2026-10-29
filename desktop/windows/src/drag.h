/* Dragging tracks onto playlists and albums onto album groups, from one window to another.
 *
 * The window a drag starts in (the track list for a track, the album grid for an album)
 * holds the mouse capture throughout, and handles the drag itself while it's over that
 * window: a playlist's tracks or a group's albums move into another order there. Anywhere
 * else it calls drag_over() as the mouse moves, which asks the window under the pointer
 * what's there, if it takes drops (drag_add_target()): the sidebar's recents and the
 * grid's playlist and group tiles do. Over anything that can't take what's dragged, the
 * pointer is the not-allowed cursor. Dropping is the source's to do, with the uid
 * drag_over() last found. */
#ifndef KITHSONG_DRAG_H
#define KITHSONG_DRAG_H

#include <windows.h>

#include "musiccore/musiccore.h"

typedef enum drag_kind {
  DRAG_TRACK, /* onto a manual playlist */
  DRAG_ALBUM, /* onto a manual album group */
} drag_kind;

/* A target's answer: whether kind dropped at pt (in the target's client area) lands on
 * something that takes it, copying its uid if so. The target marks what's under the
 * pointer until the next call; with pt NULL, the drag has left it, or ended, and it
 * unmarks. */
typedef BOOL (*drag_hover_fn)(drag_kind kind, const POINT *pt, char uid[MC_UID_STRLEN]);

/* wnd takes drops; hover answers for it. A handful of targets at most. */
void drag_add_target(HWND wnd, drag_hover_fn hover);

/* The pointer is at screen, outside the source: asks the target under it, if any, and sets
 * the cursor to say whether a drop there would land. uid gets the target's uid, or ""
 * (also the return value: FALSE). */
BOOL drag_over(drag_kind kind, POINT screen, char uid[MC_UID_STRLEN]);
/* The source is handling the pointer itself again: the last target unmarks. */
void drag_leave(void);
/* For the source, over itself: the arrow if a drop there would land, else not-allowed. */
void drag_set_cursor(BOOL allowed);
/* The drag has ended, dropped or not: drag_leave(), and the arrow cursor back. */
void drag_done(void);

#endif
