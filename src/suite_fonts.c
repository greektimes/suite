/*
 * suite_fonts.c - Shared GDI font and brush creation for the Suite.
 *
 * Mirrors the WAIS GUI pattern at MGTWAIS/source/src/wais_gui_win32.c lines
 * 470-480 and 706: Consolas 16pt UI / 20pt output, FIXED_PITCH, Courier New
 * fallback, white-on-black brush for backgrounds.
 *
 * Also: at init we register two embedded TTF blobs (Ubuntu Mono Regular
 * and Bold) as process-private fonts via AddFontMemResourceEx. The
 * fonts ride inside the .exe as RCDATA so the Suite remains a single
 * portable binary; nothing is installed system-wide and nothing is
 * required on the host machine. F3's Gopher render area uses them;
 * other modules can opt in by simply naming "Ubuntu Mono" in CreateFont.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "suite_shell.h"
#include "suite_res.h"

HFONT  g_hFontUI     = NULL;
HFONT  g_hFontOutput = NULL;
HBRUSH g_hBrushBlack = NULL;

/* AddFontMemResourceEx returns a handle that must be RemoveFontMemResourceEx'd
 * at shutdown. We keep one handle per embedded face. NULL means either
 * the resource was missing or the loader rejected the blob; in that
 * case CreateFontA("Ubuntu Mono", ...) falls back to whatever the
 * system substitutes and the F3 render area still draws (with a less
 * polished face), so we don't abort startup over it. */
static HANDLE g_ubuntu_mono_reg_handle  = NULL;
static HANDLE g_ubuntu_mono_bold_handle = NULL;

static HANDLE load_embedded_font(HINSTANCE hInst, int rc_id)
{
    HRSRC   res;
    HGLOBAL hg;
    void   *ptr;
    DWORD   sz;
    DWORD   loaded = 0;

    res = FindResourceA(hInst, MAKEINTRESOURCEA(rc_id), (LPCSTR)RT_RCDATA);
    if (!res) return NULL;
    hg = LoadResource(hInst, res);
    if (!hg) return NULL;
    sz  = SizeofResource(hInst, res);
    ptr = LockResource(hg);
    if (!ptr || sz == 0) return NULL;
    return AddFontMemResourceEx(ptr, sz, NULL, &loaded);
}

void suite_fonts_init(void)
{
    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA(NULL);

    /* Register embedded fonts BEFORE creating any HFONT that might
     * name them: CreateFontA resolves face names at HFONT creation
     * time, so the AddFontMemResourceEx call must precede the first
     * CreateFontA("Ubuntu Mono", ...) anywhere in the process. */
    g_ubuntu_mono_reg_handle  = load_embedded_font(hInst, IDR_UBUNTU_MONO_REGULAR);
    g_ubuntu_mono_bold_handle = load_embedded_font(hInst, IDR_UBUNTU_MONO_BOLD);

    g_hFontUI = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
    if (!g_hFontUI)
        g_hFontUI = CreateFontA(16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Courier New");

    g_hFontOutput = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");
    if (!g_hFontOutput)
        g_hFontOutput = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, "Courier New");

    g_hBrushBlack = CreateSolidBrush(RGB(0, 0, 0));
}

void suite_fonts_cleanup(void)
{
    if (g_hFontUI)     { DeleteObject(g_hFontUI);     g_hFontUI = NULL; }
    if (g_hFontOutput) { DeleteObject(g_hFontOutput); g_hFontOutput = NULL; }
    if (g_hBrushBlack) { DeleteObject(g_hBrushBlack); g_hBrushBlack = NULL; }

    if (g_ubuntu_mono_reg_handle) {
        RemoveFontMemResourceEx(g_ubuntu_mono_reg_handle);
        g_ubuntu_mono_reg_handle = NULL;
    }
    if (g_ubuntu_mono_bold_handle) {
        RemoveFontMemResourceEx(g_ubuntu_mono_bold_handle);
        g_ubuntu_mono_bold_handle = NULL;
    }
}
