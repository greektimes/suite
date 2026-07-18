/*
 * gopher_search_dialog.c - Modal "Gopher Search" prompt for type-7 items.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The dialog is described by an in-memory DLGTEMPLATE the C code
 * assembles at runtime, then passed to DialogBoxIndirectParam. This
 * keeps the dialog self-contained: no new RC entries, no new resource
 * IDs in suite_res.h. Modal: DialogBoxIndirectParam runs its own
 * message pump and returns when EndDialog is called.
 *
 * Self-imposed limits on the in-memory template (all comfortable here):
 *   - Control text is stored as UTF-16; we run the strings through
 *     MultiByteToWideChar to keep Greek displays in the prompt label
 *     working the same way the render area does.
 *   - DLGTEMPLATE buffer is upper-bounded by what we write (a few KB);
 *     no caller-influenced unbounded loops.
 */

#include "gopher_search_dialog.h"
#include "suite_shell.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Control IDs we own inside the modal. IDOK / IDCANCEL are standard
 * Windows IDs; the EDIT and LABEL ids only need to be unique within
 * the dialog. */
#define IDC_GSD_PROMPT   1001
#define IDC_GSD_CONTEXT  1002
#define IDC_GSD_EDIT     1003

/* Dialog dimensions, in dialog base units (per LOWORD(GetDialogBaseUnits)). */
#define DLG_W      260
#define DLG_H      110

/* Output buffer the dialog proc fills on OK. Lives at module scope so
 * the dialog can reach it without a custom lParam payload chain. The
 * dialog is modal and the function is documented non-reentrant, so a
 * single static slot is sufficient for v0.1.0-mvp. */
static char       *g_gsd_out_query   = NULL;
static const char *g_gsd_in_display  = NULL;
static const char *g_gsd_in_selector = NULL;
static HWND        g_gsd_parent_top  = NULL;

/* ---------- DLGTEMPLATE assembly helpers ---------- */

static void *align_dword(unsigned char *p)
{
    ULONG_PTR a = (ULONG_PTR)p;
    a = (a + 3) & ~(ULONG_PTR)3;
    return (void *)a;
}

/* Append a UTF-16 string (NUL-terminated) to a buffer; returns the
 * advanced pointer. text=NULL emits a single zero word. */
static unsigned char *emit_utf16(unsigned char *p, const wchar_t *text)
{
    if (!text || !*text) {
        WORD *w = (WORD *)p;
        *w = 0;
        return p + sizeof(WORD);
    }
    {
        size_t n = wcslen(text) + 1;
        memcpy(p, text, n * sizeof(wchar_t));
        return p + n * sizeof(wchar_t);
    }
}

static unsigned char *emit_word(unsigned char *p, WORD v)
{
    WORD *w = (WORD *)p;
    *w = v;
    return p + sizeof(WORD);
}

/* Emit the standard control "class atom" sequence (0xFFFF followed by
 * the predefined control class atom). Atoms per WTypes.h:
 *   0x0080 button, 0x0081 edit, 0x0082 static. */
static unsigned char *emit_class_atom(unsigned char *p, WORD atom)
{
    p = emit_word(p, 0xFFFF);
    p = emit_word(p, atom);
    return p;
}

/* Emit a DLGITEMTEMPLATE followed by class+title+0 creation-data. */
static unsigned char *emit_dlgitem(unsigned char *p,
                                   DWORD style, DWORD ex_style,
                                   short x, short y, short cx, short cy,
                                   WORD id, WORD class_atom,
                                   const wchar_t *title)
{
    DLGITEMTEMPLATE *it;
    p  = (unsigned char *)align_dword(p);
    it = (DLGITEMTEMPLATE *)p;
    it->style           = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = ex_style;
    it->x  = x;  it->y  = y;
    it->cx = cx; it->cy = cy;
    it->id = id;
    p = (unsigned char *)(it + 1);
    p = emit_class_atom(p, class_atom);
    {
        wchar_t wbuf[512];
        wbuf[0] = 0;
        if (title) wcsncpy(wbuf, title, 511);
        wbuf[511] = 0;
        p = emit_utf16(p, wbuf);
    }
    /* No creation data: emit a single zero word. */
    p = emit_word(p, 0);
    return p;
}

static DLGTEMPLATE *build_template(WORD *out_total_size, WORD *out_item_count)
{
    /* 4 KB buffer is comfortably more than needed for five controls
     * plus the dialog header and font name. */
    static unsigned char buf[4096];
    unsigned char       *p = buf;
    DLGTEMPLATE         *dt;
    WORD                 items = 5;
    wchar_t              prompt_w[512];
    wchar_t              context_w[1024];

    memset(buf, 0, sizeof(buf));

    dt = (DLGTEMPLATE *)p;
    dt->style           = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt->dwExtendedStyle = 0;
    dt->cdit            = items;
    dt->x  = 0; dt->y  = 0;
    dt->cx = DLG_W; dt->cy = DLG_H;
    p = (unsigned char *)(dt + 1);

    /* Menu, class, title (UTF-16, NUL-terminated). */
    p = emit_utf16(p, NULL);                       /* menu */
    p = emit_utf16(p, NULL);                       /* class (default) */
    p = emit_utf16(p, L"Gopher Search");           /* caption */

    /* Font (DS_SETFONT): point size + face. */
    p = emit_word(p, 9);
    p = emit_utf16(p, L"MS Shell Dlg");

    /* Convert the prompt and context strings from UTF-8 (gophermap
     * source) to UTF-16 for the DIALOGITEMTEMPLATE titles. */
    if (g_gsd_in_display)
        MultiByteToWideChar(CP_UTF8, 0, g_gsd_in_display, -1,
                            prompt_w, (int)(sizeof(prompt_w) / sizeof(wchar_t)) - 1);
    else
        prompt_w[0] = 0;
    {
        char  ctx_a[1024];
        ctx_a[0] = '\0';
        _snprintf(ctx_a, sizeof(ctx_a), "Selector: %s",
                  g_gsd_in_selector ? g_gsd_in_selector : "");
        MultiByteToWideChar(CP_UTF8, 0, ctx_a, -1,
                            context_w, (int)(sizeof(context_w) / sizeof(wchar_t)) - 1);
    }

    /* (1) Prompt label: top of the dialog. */
    p = emit_dlgitem(p,
        SS_LEFT, 0,
        10, 8, DLG_W - 20, 16,
        IDC_GSD_PROMPT, 0x0082, prompt_w);

    /* (2) Context label: muted selector line. */
    p = emit_dlgitem(p,
        SS_LEFT, 0,
        10, 28, DLG_W - 20, 12,
        IDC_GSD_CONTEXT, 0x0082, context_w);

    /* (3) Search EDIT: single-line, autoscroll. */
    p = emit_dlgitem(p,
        ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, WS_EX_CLIENTEDGE,
        10, 50, DLG_W - 20, 14,
        IDC_GSD_EDIT, 0x0081, NULL);

    /* (4) OK default pushbutton. */
    p = emit_dlgitem(p,
        BS_DEFPUSHBUTTON | WS_TABSTOP, 0,
        DLG_W - 110, DLG_H - 24, 50, 14,
        IDOK, 0x0080, L"OK");

    /* (5) Cancel pushbutton. */
    p = emit_dlgitem(p,
        BS_PUSHBUTTON | WS_TABSTOP, 0,
        DLG_W - 55, DLG_H - 24, 45, 14,
        IDCANCEL, 0x0080, L"Cancel");

    if (out_total_size) *out_total_size = (WORD)(p - buf);
    if (out_item_count) *out_item_count = items;
    return dt;
}

/* ---------- dialog proc ---------- */

static INT_PTR CALLBACK gsd_dialog_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        int x = 0, y = 0;
        RECT rc;
        GetWindowRect(h, &rc);
        suite_center_popup(g_gsd_parent_top,
                           rc.right - rc.left, rc.bottom - rc.top,
                           &x, &y);
        SetWindowPos(h, NULL, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetFocus(GetDlgItem(h, IDC_GSD_EDIT));
        return FALSE;   /* we set focus ourselves */
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w);
        if (id == IDOK) {
            char buf[2048];
            UINT n;
            buf[0] = '\0';
            n = GetDlgItemTextA(h, IDC_GSD_EDIT, buf, (int)sizeof(buf));
            if (n == 0) {
                /* Empty input == cancel; close with 0. */
                EndDialog(h, 0);
            } else {
                size_t k = strlen(buf);
                g_gsd_out_query = (char *)malloc(k + 1);
                if (g_gsd_out_query)
                    memcpy(g_gsd_out_query, buf, k + 1);
                EndDialog(h, 1);
            }
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

/* ---------- public ---------- */

int gopher_search_dialog_show(HWND parent_top_level,
                              const char *item_display,
                              const char *item_selector,
                              char **out_query)
{
    DLGTEMPLATE *dt;
    WORD         total = 0, items = 0;
    INT_PTR      rc;

    if (!out_query) return 0;
    *out_query = NULL;

    g_gsd_in_display  = item_display;
    g_gsd_in_selector = item_selector;
    g_gsd_parent_top  = parent_top_level;
    g_gsd_out_query   = NULL;

    dt = build_template(&total, &items);
    rc = DialogBoxIndirectParamA(
        (HINSTANCE)GetModuleHandleA(NULL),
        dt,
        parent_top_level,
        gsd_dialog_proc,
        0);

    g_gsd_in_display  = NULL;
    g_gsd_in_selector = NULL;
    g_gsd_parent_top  = NULL;

    if (rc == 1 && g_gsd_out_query) {
        *out_query = g_gsd_out_query;
        g_gsd_out_query = NULL;
        return 1;
    }
    if (g_gsd_out_query) {
        free(g_gsd_out_query);
        g_gsd_out_query = NULL;
    }
    return 0;
}
