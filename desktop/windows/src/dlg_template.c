#include "dlg_template.h"

static void put_word(dlg_template *t, WORD w) {
  if (t->n < (int)(sizeof t->buf / sizeof t->buf[0])) t->buf[t->n++] = w;
}

static void put_dword(dlg_template *t, DWORD d) {
  put_word(t, LOWORD(d));
  put_word(t, HIWORD(d));
}

static void put_string(dlg_template *t, const wchar_t *str) {
  do put_word(t, *str); while (*str++);
}

static void align_dword(dlg_template *t) {
  if (t->n % 2) put_word(t, 0);
}

void dlg_template_header(dlg_template *t, int num_items, int w, int h, const wchar_t *title) {
  t->n = 0;
  put_word(t, 1);      /* version */
  put_word(t, 0xFFFF); /* DLGTEMPLATEEX */
  put_dword(t, 0);     /* help id */
  put_dword(t, 0);     /* extended style */
  put_dword(t, DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
  put_word(t, (WORD)num_items);
  put_word(t, 0);
  put_word(t, 0);
  put_word(t, (WORD)w);
  put_word(t, (WORD)h);
  put_word(t, 0); /* no menu */
  put_word(t, 0); /* the standard dialog class */
  put_string(t, title ? title : L"");
  put_word(t, 9);         /* point size */
  put_word(t, FW_NORMAL); /* weight */
  put_word(t, 0);         /* italic, charset */
  put_string(t, L"Segoe UI");
}

void dlg_template_item(dlg_template *t, WORD atom, const wchar_t *cls, const wchar_t *text, DWORD style, int x,
                       int y, int w, int h, int id) {
  align_dword(t);
  put_dword(t, 0); /* help id */
  put_dword(t, 0); /* extended style */
  put_dword(t, WS_CHILD | WS_VISIBLE | style);
  put_word(t, (WORD)x);
  put_word(t, (WORD)y);
  put_word(t, (WORD)w);
  put_word(t, (WORD)h);
  put_dword(t, (DWORD)id);
  if (atom) {
    put_word(t, 0xFFFF);
    put_word(t, atom);
  } else {
    put_string(t, cls);
  }
  put_string(t, text);
  put_word(t, 0); /* no creation data */
}
