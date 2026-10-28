#define COBJMACROS
#include "uia.h"

#include <ole2.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>
#include <wchar.h>

#include "wstr.h"

/* Not in every SDK's headers. */
#ifndef UIA_E_ELEMENTNOTENABLED
#define UIA_E_ELEMENTNOTENABLED 0x80040200
#endif
#ifndef UIA_E_INVALIDOPERATION
#define UIA_E_INVALIDOPERATION 0x80131509
#endif

/* What the posted action message asks for, in the low bits of its lParam; the host's gen
 * is above them, so an action for nodes that have since changed is dropped. */
enum { ACT_INVOKE, ACT_EXPAND, ACT_COLLAPSE, ACT_BITS = 2 };

static UINT action_msg(void) {
  static UINT msg;
  if (!msg) msg = RegisterWindowMessageW(L"kithsong_uia_action");
  return msg;
}

/* One element: the window's root (index UIA_ROOT) or a node. Each interface is a member,
 * and PROV() gets back to the whole from any of them. Called on the window's thread only
 * (see simple_options). */
typedef struct prov {
  IRawElementProviderSimple simple;
  IRawElementProviderFragment fragment;
  IRawElementProviderFragmentRoot root;
  ISelectionProvider selection;
  ISelectionItemProvider item;
  IInvokeProvider invoke;
  IScrollItemProvider scroll;
  IExpandCollapseProvider expand;
  LONG refs;
  uia_host *host; /* static in the control, so it outlives every element */
  int index;
  UINT gen; /* the host's, when it was made */
} prov;

#define PROV(ptr, member) ((prov *)((char *)(ptr) - offsetof(prov, member)))

static prov *prov_new(uia_host *h, int index);

/* --- Nodes --- */

static BOOL node_of(uia_host *h, int i, uia_node *n) {
  memset(n, 0, sizeof *n);
  return h->wnd && h->ops && h->ops->node(i, n);
}

/* p's node, if it's still the one p was made for. */
static BOOL get(prov *p, uia_node *n) {
  if (p->index != UIA_ROOT && p->gen != p->host->gen) return FALSE;
  return node_of(p->host, p->index, n);
}

static prov *root_of(uia_host *h) {
  if (!h->root && h->wnd) h->root = prov_new(h, UIA_ROOT);
  return h->root;
}

/* The element for node i (or the root), with a reference for the caller; NULL if there's no
 * such node. */
static prov *prov_at(uia_host *h, int i) {
  uia_node n;
  if (i == UIA_NONE || !node_of(h, i, &n)) return NULL;
  if (i == UIA_ROOT) {
    prov *root = root_of(h);
    if (root) InterlockedIncrement(&root->refs);
    return root;
  }
  return prov_new(h, i);
}

/* The part of node n that shows, in client coordinates. */
static RECT visible(uia_host *h, const uia_node *n) {
  RECT client, out;
  GetClientRect(h->wnd, &client);
  if (!IsWindowVisible(h->wnd) || !IntersectRect(&out, &client, &n->rect)) SetRectEmpty(&out);
  return out;
}

static void set_bstr(VARIANT *v, const wchar_t *s) {
  if (!s || !*s) return;
  v->bstrVal = SysAllocString(s);
  if (v->bstrVal) v->vt = VT_BSTR;
}

static void set_bool(VARIANT *v, BOOL b) {
  v->vt = VT_BOOL;
  v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

/* --- IUnknown, for all the interfaces --- */

static HRESULT prov_qi(prov *p, REFIID riid, void **out) {
  if (!out) return E_POINTER;
  *out = NULL;
  if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRawElementProviderSimple)) *out = &p->simple;
  else if (IsEqualIID(riid, &IID_IRawElementProviderFragment)) *out = &p->fragment;
  else if (IsEqualIID(riid, &IID_IRawElementProviderFragmentRoot) && p->index == UIA_ROOT) *out = &p->root;
  else if (IsEqualIID(riid, &IID_ISelectionProvider)) *out = &p->selection;
  else if (IsEqualIID(riid, &IID_ISelectionItemProvider)) *out = &p->item;
  else if (IsEqualIID(riid, &IID_IInvokeProvider)) *out = &p->invoke;
  else if (IsEqualIID(riid, &IID_IScrollItemProvider)) *out = &p->scroll;
  else if (IsEqualIID(riid, &IID_IExpandCollapseProvider)) *out = &p->expand;
  else return E_NOINTERFACE;
  InterlockedIncrement(&p->refs);
  return S_OK;
}

static ULONG prov_addref(prov *p) {
  return (ULONG)InterlockedIncrement(&p->refs);
}

static ULONG prov_release(prov *p) {
  LONG refs = InterlockedDecrement(&p->refs);
  if (!refs) free(p);
  return (ULONG)refs;
}

#define UNKNOWN_METHODS(iface, member)                                                        \
  static HRESULT STDMETHODCALLTYPE member##_qi(iface *This, REFIID riid, void **out) {        \
    return prov_qi(PROV(This, member), riid, out);                                            \
  }                                                                                           \
  static ULONG STDMETHODCALLTYPE member##_addref(iface *This) {                               \
    return prov_addref(PROV(This, member));                                                   \
  }                                                                                           \
  static ULONG STDMETHODCALLTYPE member##_release(iface *This) {                              \
    return prov_release(PROV(This, member));                                                  \
  }

UNKNOWN_METHODS(IRawElementProviderSimple, simple)
UNKNOWN_METHODS(IRawElementProviderFragment, fragment)
UNKNOWN_METHODS(IRawElementProviderFragmentRoot, root)
UNKNOWN_METHODS(ISelectionProvider, selection)
UNKNOWN_METHODS(ISelectionItemProvider, item)
UNKNOWN_METHODS(IInvokeProvider, invoke)
UNKNOWN_METHODS(IScrollItemProvider, scroll)
UNKNOWN_METHODS(IExpandCollapseProvider, expand)

/* --- IRawElementProviderSimple --- */

static HRESULT STDMETHODCALLTYPE simple_options(IRawElementProviderSimple *This, enum ProviderOptions *ret) {
  (void)This;
  /* COM threading: the elements are made on the window's thread, an STA, so calls are
   * marshalled to it, where the control's state lives. Without it they come on others. */
  *ret = ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE simple_pattern(IRawElementProviderSimple *This, PATTERNID id, IUnknown **ret) {
  prov *p = PROV(This, simple);
  *ret = NULL;
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  void *iface = NULL;
  switch (id) {
    case UIA_SelectionPatternId:
      if (n.flags & UIA_SELECTION) iface = &p->selection;
      break;
    case UIA_SelectionItemPatternId:
      if (n.flags & UIA_SELECTABLE) iface = &p->item;
      break;
    case UIA_InvokePatternId:
      if (n.flags & UIA_INVOKABLE) iface = &p->invoke;
      break;
    case UIA_ScrollItemPatternId:
      if (p->index != UIA_ROOT && p->host->ops->reveal) iface = &p->scroll;
      break;
    case UIA_ExpandCollapsePatternId:
      if (n.flags & (UIA_COLLAPSED | UIA_EXPANDED)) iface = &p->expand;
      break;
  }
  if (iface) {
    prov_addref(p);
    *ret = iface;
  }
  return S_OK;
}

/* Properties left VT_EMPTY come from UI Automation's defaults, or for the root from the
 * window. */
static HRESULT STDMETHODCALLTYPE simple_property(IRawElementProviderSimple *This, PROPERTYID id, VARIANT *ret) {
  prov *p = PROV(This, simple);
  VariantInit(ret);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  uia_host *h = p->host;
  BOOL root = p->index == UIA_ROOT;
  switch (id) {
    case UIA_ControlTypePropertyId:
      if (n.type) {
        ret->vt = VT_I4;
        ret->lVal = n.type;
      }
      break;
    case UIA_NamePropertyId: set_bstr(ret, n.name); break;
    case UIA_HelpTextPropertyId: set_bstr(ret, n.help); break;
    case UIA_ItemStatusPropertyId: set_bstr(ret, n.status); break;
    case UIA_AutomationIdPropertyId: set_bstr(ret, n.automation_id); break;
    case UIA_HasKeyboardFocusPropertyId: /* the root's is the window's, which UI Automation then looks into */
      if (!root) set_bool(ret, GetFocus() == h->wnd && h->ops->focus() == p->index);
      break;
    case UIA_IsKeyboardFocusablePropertyId:
      if (!root) set_bool(ret, (n.flags & UIA_FOCUSABLE) && !(n.flags & UIA_DISABLED));
      break;
    case UIA_IsEnabledPropertyId:
      if (!root) set_bool(ret, !(n.flags & UIA_DISABLED));
      break;
    case UIA_IsOffscreenPropertyId:
      if (!root) {
        RECT rc = visible(h, &n);
        set_bool(ret, IsRectEmpty(&rc));
      }
      break;
  }
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE simple_host(IRawElementProviderSimple *This, IRawElementProviderSimple **ret) {
  prov *p = PROV(This, simple);
  *ret = NULL;
  if (p->index == UIA_ROOT && p->host->wnd) return UiaHostProviderFromHwnd(p->host->wnd, ret);
  return S_OK;
}

static const IRawElementProviderSimpleVtbl simple_vtbl = {
  .QueryInterface = simple_qi,
  .AddRef = simple_addref,
  .Release = simple_release,
  .get_ProviderOptions = simple_options,
  .GetPatternProvider = simple_pattern,
  .GetPropertyValue = simple_property,
  .get_HostRawElementProvider = simple_host,
};

/* --- IRawElementProviderFragment --- */

static HRESULT STDMETHODCALLTYPE fragment_navigate(IRawElementProviderFragment *This, enum NavigateDirection dir,
                                                   IRawElementProviderFragment **ret) {
  prov *p = PROV(This, fragment);
  *ret = NULL;
  uia_node n, parent;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  uia_host *h = p->host;
  int to = UIA_NONE;
  switch (dir) {
    case NavigateDirection_Parent: /* the root's is the window's, which UI Automation knows */
      if (p->index != UIA_ROOT) to = n.parent;
      break;
    case NavigateDirection_FirstChild:
      if (n.count > 0) to = n.first;
      break;
    case NavigateDirection_LastChild:
      if (n.count > 0) to = n.first + n.count - 1;
      break;
    case NavigateDirection_NextSibling:
    case NavigateDirection_PreviousSibling: {
      if (p->index == UIA_ROOT || !node_of(h, n.parent, &parent)) break;
      int next = p->index + (dir == NavigateDirection_NextSibling ? 1 : -1);
      if (next >= parent.first && next < parent.first + parent.count) to = next;
      break;
    }
  }
  prov *q = prov_at(h, to);
  if (q) *ret = &q->fragment;
  return S_OK;
}

/* The root's comes from the window. A node's is the gen and its number, so it's the same
 * element for as long as the numbering lasts. */
static HRESULT STDMETHODCALLTYPE fragment_runtime_id(IRawElementProviderFragment *This, SAFEARRAY **ret) {
  prov *p = PROV(This, fragment);
  *ret = NULL;
  if (p->index == UIA_ROOT) return S_OK;
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  int ids[] = {UiaAppendRuntimeId, (int)p->gen, p->index};
  SAFEARRAY *sa = SafeArrayCreateVector(VT_I4, 0, 3);
  if (!sa) return E_OUTOFMEMORY;
  for (LONG k = 0; k < 3; k++) SafeArrayPutElement(sa, &k, &ids[k]);
  *ret = sa;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragment_bounds(IRawElementProviderFragment *This, struct UiaRect *ret) {
  prov *p = PROV(This, fragment);
  memset(ret, 0, sizeof *ret);
  if (p->index == UIA_ROOT) return S_OK; /* the window's */
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  RECT rc = visible(p->host, &n);
  if (IsRectEmpty(&rc)) return S_OK;
  MapWindowPoints(p->host->wnd, NULL, (POINT *)&rc, 2);
  ret->left = rc.left;
  ret->top = rc.top;
  ret->width = rc.right - rc.left;
  ret->height = rc.bottom - rc.top;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragment_embedded(IRawElementProviderFragment *This, SAFEARRAY **ret) {
  (void)This;
  *ret = NULL;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragment_set_focus(IRawElementProviderFragment *This) {
  prov *p = PROV(This, fragment);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  if (n.flags & UIA_DISABLED) return UIA_E_ELEMENTNOTENABLED;
  SetFocus(p->host->wnd);
  if (p->index != UIA_ROOT && (n.flags & UIA_FOCUSABLE) && p->host->ops->set_focus) p->host->ops->set_focus(p->index);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE fragment_root(IRawElementProviderFragment *This, IRawElementProviderFragmentRoot **ret) {
  prov *p = PROV(This, fragment), *root = prov_at(p->host, UIA_ROOT);
  *ret = root ? &root->root : NULL;
  return root ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

static const IRawElementProviderFragmentVtbl fragment_vtbl = {
  .QueryInterface = fragment_qi,
  .AddRef = fragment_addref,
  .Release = fragment_release,
  .Navigate = fragment_navigate,
  .GetRuntimeId = fragment_runtime_id,
  .get_BoundingRectangle = fragment_bounds,
  .GetEmbeddedFragmentRoots = fragment_embedded,
  .SetFocus = fragment_set_focus,
  .get_FragmentRoot = fragment_root,
};

/* --- IRawElementProviderFragmentRoot --- */

static HRESULT STDMETHODCALLTYPE root_from_point(IRawElementProviderFragmentRoot *This, double x, double y,
                                                 IRawElementProviderFragment **ret) {
  prov *p = PROV(This, root);
  uia_host *h = p->host;
  *ret = NULL;
  if (!h->wnd) return UIA_E_ELEMENTNOTAVAILABLE;
  POINT pt = {(LONG)x, (LONG)y};
  ScreenToClient(h->wnd, &pt);
  int i = h->ops->at ? h->ops->at(pt) : UIA_ROOT;
  prov *q = i != UIA_ROOT ? prov_at(h, i) : NULL;
  if (q) *ret = &q->fragment;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE root_focus(IRawElementProviderFragmentRoot *This, IRawElementProviderFragment **ret) {
  prov *p = PROV(This, root);
  uia_host *h = p->host;
  *ret = NULL;
  if (!h->wnd) return UIA_E_ELEMENTNOTAVAILABLE;
  int i = GetFocus() == h->wnd ? h->ops->focus() : UIA_ROOT;
  prov *q = i != UIA_ROOT ? prov_at(h, i) : NULL;
  if (q) *ret = &q->fragment;
  return S_OK;
}

static const IRawElementProviderFragmentRootVtbl root_vtbl = {
  .QueryInterface = root_qi,
  .AddRef = root_addref,
  .Release = root_release,
  .ElementProviderFromPoint = root_from_point,
  .GetFocus = root_focus,
};

/* --- Patterns --- */

static HRESULT STDMETHODCALLTYPE selection_get(ISelectionProvider *This, SAFEARRAY **ret) {
  prov *p = PROV(This, selection);
  *ret = NULL;
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  prov *sel = p->host->ops->selection ? prov_at(p->host, p->host->ops->selection(p->index)) : NULL;
  SAFEARRAY *sa = SafeArrayCreateVector(VT_UNKNOWN, 0, sel ? 1 : 0);
  if (sa && sel) {
    LONG k = 0;
    SafeArrayPutElement(sa, &k, (IUnknown *)&sel->simple); /* takes its own reference */
  }
  if (sel) prov_release(sel);
  *ret = sa;
  return sa ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE selection_multiple(ISelectionProvider *This, WINBOOL *ret) {
  (void)This;
  *ret = FALSE;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE selection_required(ISelectionProvider *This, WINBOOL *ret) {
  (void)This;
  *ret = FALSE;
  return S_OK;
}

static const ISelectionProviderVtbl selection_vtbl = {
  .QueryInterface = selection_qi,
  .AddRef = selection_addref,
  .Release = selection_release,
  .GetSelection = selection_get,
  .get_CanSelectMultiple = selection_multiple,
  .get_IsSelectionRequired = selection_required,
};

static HRESULT STDMETHODCALLTYPE item_select(ISelectionItemProvider *This) {
  prov *p = PROV(This, item);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  if (n.flags & UIA_DISABLED) return UIA_E_ELEMENTNOTENABLED;
  if (p->host->ops->select) p->host->ops->select(p->index);
  return S_OK;
}

/* One selection at a time: adding is selecting, if nothing else is. */
static HRESULT STDMETHODCALLTYPE item_add(ISelectionItemProvider *This) {
  prov *p = PROV(This, item);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  if (n.flags & UIA_SELECTED) return S_OK;
  const uia_ops *ops = p->host->ops;
  if (ops->selection && ops->selection(n.parent) != UIA_NONE) return UIA_E_INVALIDOPERATION;
  return item_select(This);
}

static HRESULT STDMETHODCALLTYPE item_remove(ISelectionItemProvider *This) {
  prov *p = PROV(This, item);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  return n.flags & UIA_SELECTED ? UIA_E_INVALIDOPERATION : S_OK;
}

static HRESULT STDMETHODCALLTYPE item_selected(ISelectionItemProvider *This, WINBOOL *ret) {
  prov *p = PROV(This, item);
  uia_node n;
  *ret = FALSE;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  *ret = (n.flags & UIA_SELECTED) != 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE item_container(ISelectionItemProvider *This, IRawElementProviderSimple **ret) {
  prov *p = PROV(This, item);
  uia_node n;
  *ret = NULL;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  prov *parent = prov_at(p->host, n.parent);
  if (parent) *ret = &parent->simple;
  return S_OK;
}

static const ISelectionItemProviderVtbl item_vtbl = {
  .QueryInterface = item_qi,
  .AddRef = item_addref,
  .Release = item_release,
  .Select = item_select,
  .AddToSelection = item_add,
  .RemoveFromSelection = item_remove,
  .get_IsSelected = item_selected,
  .get_SelectionContainer = item_container,
};

/* Invoke and Expand/Collapse are posted: they may open a menu or a dialog, whose message
 * loop would hold up the client until it closed. */
static HRESULT post_action(prov *p, int action) {
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  if (n.flags & UIA_DISABLED) return UIA_E_ELEMENTNOTENABLED;
  LPARAM lp = (LPARAM)(((ULONG_PTR)p->gen << ACT_BITS) | (ULONG_PTR)action);
  return PostMessageW(p->host->wnd, action_msg(), (WPARAM)p->index, lp) ? S_OK : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE invoke_invoke(IInvokeProvider *This) {
  return post_action(PROV(This, invoke), ACT_INVOKE);
}

static const IInvokeProviderVtbl invoke_vtbl = {
  .QueryInterface = invoke_qi,
  .AddRef = invoke_addref,
  .Release = invoke_release,
  .Invoke = invoke_invoke,
};

static HRESULT STDMETHODCALLTYPE scroll_into_view(IScrollItemProvider *This) {
  prov *p = PROV(This, scroll);
  uia_node n;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  if (p->host->ops->reveal) p->host->ops->reveal(p->index);
  return S_OK;
}

static const IScrollItemProviderVtbl scroll_vtbl = {
  .QueryInterface = scroll_qi,
  .AddRef = scroll_addref,
  .Release = scroll_release,
  .ScrollIntoView = scroll_into_view,
};

static HRESULT STDMETHODCALLTYPE expand_expand(IExpandCollapseProvider *This) {
  return post_action(PROV(This, expand), ACT_EXPAND);
}

static HRESULT STDMETHODCALLTYPE expand_collapse(IExpandCollapseProvider *This) {
  return post_action(PROV(This, expand), ACT_COLLAPSE);
}

static HRESULT STDMETHODCALLTYPE expand_state(IExpandCollapseProvider *This, enum ExpandCollapseState *ret) {
  prov *p = PROV(This, expand);
  uia_node n;
  *ret = ExpandCollapseState_LeafNode;
  if (!get(p, &n)) return UIA_E_ELEMENTNOTAVAILABLE;
  *ret = n.flags & UIA_EXPANDED ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
  return S_OK;
}

static const IExpandCollapseProviderVtbl expand_vtbl = {
  .QueryInterface = expand_qi,
  .AddRef = expand_addref,
  .Release = expand_release,
  .Expand = expand_expand,
  .Collapse = expand_collapse,
  .get_ExpandCollapseState = expand_state,
};

static prov *prov_new(uia_host *h, int index) {
  prov *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->simple.lpVtbl = (IRawElementProviderSimpleVtbl *)&simple_vtbl;
  p->fragment.lpVtbl = (IRawElementProviderFragmentVtbl *)&fragment_vtbl;
  p->root.lpVtbl = (IRawElementProviderFragmentRootVtbl *)&root_vtbl;
  p->selection.lpVtbl = (ISelectionProviderVtbl *)&selection_vtbl;
  p->item.lpVtbl = (ISelectionItemProviderVtbl *)&item_vtbl;
  p->invoke.lpVtbl = (IInvokeProviderVtbl *)&invoke_vtbl;
  p->scroll.lpVtbl = (IScrollItemProviderVtbl *)&scroll_vtbl;
  p->expand.lpVtbl = (IExpandCollapseProviderVtbl *)&expand_vtbl;
  p->refs = 1;
  p->host = h;
  p->index = index;
  p->gen = h->gen;
  return p;
}

/* --- The host --- */

void uia_init(uia_host *h, HWND wnd, const uia_ops *ops) {
  h->wnd = wnd;
  h->ops = ops;
  h->gen++; /* elements from a window before this one stay gone */
  h->sig = 0;
  h->root = NULL;
}

BOOL uia_message(uia_host *h, UINT msg, WPARAM wp, LPARAM lp, LRESULT *result) {
  if (!h->wnd) return FALSE;
  if (msg == WM_GETOBJECT && (LONG)lp == UiaRootObjectId) {
    prov *root = root_of(h);
    if (!root) return FALSE;
    *result = UiaReturnRawElementProvider(h->wnd, wp, lp, &root->simple);
    return TRUE;
  }
  if (msg == action_msg()) {
    int i = (int)wp, action = (int)(lp & ((1 << ACT_BITS) - 1));
    uia_node n;
    BOOL current = ((ULONG_PTR)lp >> ACT_BITS) == (ULONG_PTR)h->gen || i == UIA_ROOT;
    if (current && node_of(h, i, &n) && !(n.flags & UIA_DISABLED)) {
      if (action == ACT_INVOKE && h->ops->invoke) h->ops->invoke(i);
      else if (action != ACT_INVOKE && h->ops->expand) h->ops->expand(i, action == ACT_EXPAND);
    }
    *result = 0;
    return TRUE;
  }
  if (msg == WM_SETFOCUS && h->ops->focus() != UIA_ROOT) uia_focus(h); /* the window's own is raised for it */
  return FALSE;
}

void uia_destroy(uia_host *h) {
  if (h->root) {
    prov *root = h->root;
    if (h->wnd) UiaReturnRawElementProvider(h->wnd, 0, 0, NULL);
    UiaDisconnectProvider(&root->simple);
    h->root = NULL;
    prov_release(root);
  }
  h->wnd = NULL;
  h->gen++;
}

unsigned long uia_hash(unsigned long sig, const void *data, size_t n) {
  const unsigned char *b = data;
  unsigned long hash = sig ? sig : 2166136261UL;
  for (size_t i = 0; i < n; i++) hash = (hash ^ b[i]) * 16777619UL;
  return hash & 0xFFFFFFFFUL;
}

void uia_changed(uia_host *h, unsigned long sig) {
  if (sig == h->sig) return;
  h->sig = sig;
  h->gen++;
  if (h->root && UiaClientsAreListening())
    UiaRaiseStructureChangedEvent(&((prov *)h->root)->simple, StructureChangeType_ChildrenInvalidated, NULL, 0);
}

static void raise_event(uia_host *h, int i, EVENTID event) {
  if (!h->wnd || !h->root || !UiaClientsAreListening()) return; /* no root yet: nobody's asked */
  prov *p = prov_at(h, i);
  if (!p) return;
  UiaRaiseAutomationEvent(&p->simple, event);
  prov_release(p);
}

void uia_focus(uia_host *h) {
  if (h->wnd && GetFocus() == h->wnd) raise_event(h, h->ops->focus(), UIA_AutomationFocusChangedEventId);
}

void uia_selected(uia_host *h, int i) {
  if (i >= 0) raise_event(h, i, UIA_SelectionItem_ElementSelectedEventId);
}

static void join(wchar_t *buf, size_t n, va_list ap) {
  const wchar_t *sep = mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR);
  size_t len = 0;
  buf[0] = 0;
  for (const wchar_t *s; (s = va_arg(ap, const wchar_t *)) != NULL;) {
    if (!*s) continue;
    int w = swprintf(buf + len, n - len, L"%ls%ls", len ? sep : L"", s);
    if (w < 0) { /* full: what fitted stays */
      buf[len] = 0;
      break;
    }
    len += (size_t)w;
  }
}

void uia_name(uia_node *n, ...) {
  va_list ap;
  va_start(ap, n);
  join(n->name, sizeof n->name / sizeof n->name[0], ap);
  va_end(ap);
}

void uia_join(wchar_t *buf, size_t n, ...) {
  va_list ap;
  va_start(ap, n);
  join(buf, n, ap);
  va_end(ap);
}
