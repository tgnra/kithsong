/* UI Automation for the custom-drawn controls (album grid, track list, sidebar, search
 * page), so screen readers such as Narrator see what they draw.
 *
 * Each control is a fragment root over a small tree of nodes that it describes on demand
 * through uia_ops. Nodes are numbered by the control, from 0; a node's children are always
 * consecutive numbers, so walking the tree is cheap however many albums there are. The
 * window itself is the root, UIA_ROOT. A node is only ever asked about by number, so the
 * control's own state stays the one source of truth: nothing is copied but what a call needs.
 *
 * Elements live as long as the numbering: when the control's nodes change into others (a
 * reload with other albums, say), it calls uia_changed(), and elements a client still holds
 * from before report themselves gone.
 *
 * The control forwards its messages to uia_message() first, and tells us when its keyboard
 * focus or selection moves (uia_focus(), uia_selected()), which screen readers announce. */
#ifndef KITHSONG_UIA_H
#define KITHSONG_UIA_H

#include <windows.h>

#define UIA_ROOT (-1) /* the window, as a node number */
#define UIA_NONE (-2) /* no node */

/* uia_node.flags */
enum {
  UIA_FOCUSABLE = 1,    /* the keyboard can land on it */
  UIA_SELECTABLE = 2,   /* SelectionItem pattern: selecting it is ops.select */
  UIA_SELECTED = 4,
  UIA_SELECTION = 8,    /* Selection pattern: a container of selectable nodes (ops.selection) */
  UIA_INVOKABLE = 16,   /* Invoke pattern: ops.invoke, as a click does */
  UIA_DISABLED = 32,    /* greyed out */
  UIA_COLLAPSED = 64,   /* ExpandCollapse pattern (ops.expand), collapsed or expanded */
  UIA_EXPANDED = 128,
};

/* What a node is, as ops.node fills it in. Everything starts zeroed. */
typedef struct uia_node {
  int parent;                 /* UIA_ROOT, or a node's number */
  int first, count;           /* its children: nodes first to first + count - 1 */
  long type;                  /* a UIA_*ControlTypeId */
  unsigned flags;             /* UIA_* above */
  RECT rect;                  /* in client coordinates; empty while it isn't on screen */
  wchar_t name[512];          /* what a screen reader says for it */
  wchar_t help[256];          /* more, e.g. its tooltip; "" for none */
  wchar_t status[64];         /* ItemStatus, e.g. "Playing"; "" for none */
  const wchar_t *automation_id; /* a stable name for tests and tools, or NULL */
} uia_node;

typedef struct uia_ops {
  /* Fills in node i (or UIA_ROOT, whose parent and rect are ignored). FALSE if there's no
   * such node now. */
  BOOL (*node)(int i, uia_node *n);
  /* The node with the keyboard focus while the window has it, or UIA_ROOT. */
  int (*focus)(void);
  /* The node at a point in the client area, or UIA_ROOT. May be NULL. */
  int (*at)(POINT pt);
  /* The selected child of selection container i, or UIA_NONE. May be NULL. */
  int (*selection)(int i);
  /* What the patterns do, as the mouse or keyboard would. Any may be NULL. Invoke and
   * expand are posted, as they may open menus or dialogs, and arrive later. */
  void (*select)(int i);
  void (*invoke)(int i);
  void (*expand)(int i, BOOL expand);
  void (*reveal)(int i);    /* scroll it into view */
  void (*set_focus)(int i); /* the window already has the focus */
} uia_ops;

typedef struct uia_host {
  HWND wnd;
  const uia_ops *ops;
  UINT gen;           /* bumped when the nodes change into others */
  unsigned long sig;  /* what uia_changed() last saw */
  void *root;         /* the root's provider, once a client asked for it */
} uia_host;

/* Call from WM_CREATE (or once the window exists). */
void uia_init(uia_host *h, HWND wnd, const uia_ops *ops);
/* Call first from the window procedure: TRUE if it handled msg, with *result. Handles
 * WM_GETOBJECT, the posted Invoke and Expand/Collapse, and WM_SETFOCUS (for which it
 * announces the focused node, and returns FALSE so the window sees it too). */
BOOL uia_message(uia_host *h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);
/* Call from WM_DESTROY: elements clients still hold report themselves gone. */
void uia_destroy(uia_host *h);

/* The nodes may have changed: sig sums up which they are (see uia_hash()). If it differs
 * from last time, elements from before go and clients are told the tree changed. */
void uia_changed(uia_host *h, unsigned long sig);
/* An FNV-1a hash of n bytes, continuing from sig (start from 0). */
unsigned long uia_hash(unsigned long sig, const void *data, size_t n);

/* The keyboard focus moved to ops.focus(): tells clients, if the window has the focus. */
void uia_focus(uia_host *h);
/* Node i became the selected one. */
void uia_selected(uia_host *h, int i);

/* Joins the non-empty strings up to a NULL with ", " into n->name (or into buf of n
 * characters), e.g. a tile's "OK Computer, Radiohead". */
void uia_name(uia_node *n, ...);
void uia_join(wchar_t *buf, size_t n, ...);

#endif
