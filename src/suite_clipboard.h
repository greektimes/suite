/*
 * suite_clipboard.h - CF_TEXT clipboard helpers for custom-painted
 *                     suite modules.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * F6 telnet_module is the first caller (polish 6, 2026-05-22);
 * F3 gopher_module will be the second (Dispatch B).
 */

#ifndef SUITE_CLIPBOARD_H
#define SUITE_CLIPBOARD_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Put ASCII text on the clipboard. text must be null-terminated.
 * owner is the hwnd that owns the clipboard during the operation
 * (a render hwnd is fine). Returns 1 on success, 0 on failure. */
int   suite_clipboard_put_text(HWND owner, const char *text);

/* Read ASCII text from the clipboard. Returns a malloc'd null-term
 * string the caller must free, or NULL if the clipboard has no
 * CF_TEXT or any step failed. */
char *suite_clipboard_get_text(HWND owner);

/* True if CF_TEXT is currently available. Cheap probe, no Open/Close. */
int   suite_clipboard_has_text(void);

#endif /* SUITE_CLIPBOARD_H */
