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
 * default; Escape and the close box cancel, as the second button does.
 */
#ifndef KITHSONG_PROMPT_DIALOG_H
#define KITHSONG_PROMPT_DIALOG_H

#include <windows.h>

typedef struct prompt_dialog {
  const wchar_t *caption;
  UINT icon;              /* MB_ICONINFORMATION or MB_ICONERROR */
  const wchar_t *heading; /* or NULL */
  const wchar_t *text;
  const wchar_t *ok, *cancel; /* the buttons' labels, with their mnemonics; cancel may be NULL */
  BOOL shield;                /* ok shows the UAC shield */
  const wchar_t *check;       /* the checkbox's label, with its mnemonic, or NULL */
  BOOL checked;               /* out: it was ticked, whichever way the dialog closed */
} prompt_dialog;

/* Shows p, modal to owner. Returns IDOK or IDCANCEL, or <= 0 if the dialog couldn't open. */
INT_PTR prompt_dialog_show(HWND owner, prompt_dialog *p);

/* A message box with an OK button, sounding as one does. icon as above. */
void message_dialog(HWND owner, const wchar_t *caption, const wchar_t *text, UINT icon);

#endif
