/* A question or a message in the app's theme, for what would be a task dialog or a message
 * box if those could follow it: neither has a dark mode.
 *
 *   +- Kithsong ----------------------------+
 *   | (i)  Heading, if there is one         |
 *   |      The text, as tall as its lines.  |
 *   |      [ ] The checkbox, if there's one |
 *   |                                       |
 *   |                [ Allow ] [ Not now ]  |
 *   +---------------------------------------+
 *
 * It's sized to its text at run time and centred on its owner. The first button is the
 * default, unless the question is one to answer with care; Escape and the close box cancel,
 * as the last button does.
 */
#ifndef KITHSONG_PROMPT_DIALOG_H
#define KITHSONG_PROMPT_DIALOG_H

#include <windows.h>

typedef struct prompt_dialog {
  const wchar_t *caption;
  UINT icon;              /* MB_ICONINFORMATION, _WARNING, _QUESTION or _ERROR; 0 for the app's */
  const wchar_t *heading; /* or NULL */
  const wchar_t *text;
  const wchar_t *link;    /* a web address to show under the text, which opens it, or NULL */
  /* The buttons' labels, with their mnemonics. other, between the two, and cancel may be NULL. */
  const wchar_t *ok, *other, *cancel;
  BOOL cancel_default;    /* Cancel is the default, for what's hard to take back */
  BOOL shield;            /* ok shows the UAC shield */
  const wchar_t *check;   /* the checkbox's label, with its mnemonic, or NULL */
  BOOL checked;           /* out: it was ticked, whichever way the dialog closed */
  HWND *shown;            /* if not NULL, the dialog while it's open, then NULL */
} prompt_dialog;

/* Shows p, modal to owner. Returns IDOK, IDNO for other or IDCANCEL, or <= 0 if the dialog
 * couldn't open. */
INT_PTR prompt_dialog_show(HWND owner, prompt_dialog *p);

/* New text for a dialog that's open (prompt_dialog.shown), which is sized to it again. */
void prompt_dialog_set_text(HWND dlg, const wchar_t *text);

/* A message box with an OK button, sounding as one does. icon as above. */
void message_dialog(HWND owner, const wchar_t *caption, const wchar_t *text, UINT icon);

/* A question with two buttons, sounding as one does. TRUE for ok. cancel_default as above. */
BOOL confirm_dialog(HWND owner, const wchar_t *caption, const wchar_t *text, UINT icon, const wchar_t *ok,
                    const wchar_t *cancel, BOOL cancel_default);

#endif
