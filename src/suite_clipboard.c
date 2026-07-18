/*
 * suite_clipboard.c - CF_TEXT clipboard helpers. See suite_clipboard.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * OpenClipboard can fail with ERROR_ACCESS_DENIED when another
 * process is holding it open (browsers, password managers); we retry
 * once after an 80ms back-off, which is enough for the rare collision.
 */

#include "suite_clipboard.h"

#include <stdlib.h>
#include <string.h>

static int open_clipboard_with_retry(HWND owner)
{
    if (OpenClipboard(owner)) return 1;
    Sleep(80);
    return OpenClipboard(owner) ? 1 : 0;
}

int suite_clipboard_put_text(HWND owner, const char *text)
{
    size_t  len;
    HGLOBAL hMem;
    void   *p;

    if (!text) return 0;
    len = strlen(text);

    if (!open_clipboard_with_retry(owner)) return 0;
    if (!EmptyClipboard()) { CloseClipboard(); return 0; }

    hMem = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!hMem) { CloseClipboard(); return 0; }
    p = GlobalLock(hMem);
    if (!p) { GlobalFree(hMem); CloseClipboard(); return 0; }
    memcpy(p, text, len + 1);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_TEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return 0;
    }
    /* Clipboard owns hMem now; do not free. */
    CloseClipboard();
    return 1;
}

char *suite_clipboard_get_text(HWND owner)
{
    HANDLE  hMem;
    LPCSTR  src;
    size_t  len;
    char   *dup;

    if (!open_clipboard_with_retry(owner)) return NULL;

    hMem = GetClipboardData(CF_TEXT);
    if (!hMem) { CloseClipboard(); return NULL; }

    src = (LPCSTR)GlobalLock(hMem);
    if (!src) { CloseClipboard(); return NULL; }

    len = strlen(src);
    dup = (char *)malloc(len + 1);
    if (dup) {
        memcpy(dup, src, len);
        dup[len] = '\0';
    }

    GlobalUnlock(hMem);
    CloseClipboard();
    return dup;
}

int suite_clipboard_has_text(void)
{
    return IsClipboardFormatAvailable(CF_TEXT) ? 1 : 0;
}
