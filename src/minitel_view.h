/*
 * minitel_view.h - the Minitel terminal inside the Videotex tab: the
 *                  40 x 25 screen and the Minitel keyboard.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The decoder and the raster are minitel_decode.[ch] and
 * minitel_screen.[ch], clean-room from STUM1B and free of Win32. This is
 * only the window side: a canvas that blits the raster, the on-screen
 * keyboard, and the PC key bindings. It owns no socket; the Videotex tab
 * (naplps_module.c) owns the one telnet_proto session and hands bytes
 * down, and this view hands key bytes back up through a callback.
 */

#ifndef MINITEL_VIEW_H_INCLUDED
#define MINITEL_VIEW_H_INCLUDED

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

/*
 * Defaults. The host is the umbrella name videotex.greektimes.ca, as for
 * NAPLPS; the port is 516, the IANA "videotex" port, which the Unicorn
 * service listens on publicly since 2026-09-27 (Phase 3 go-live).
 */
#define MINITEL_DEFAULT_PORT      516
#define MINITEL_DEFAULT_PORT_STR  "516"

/* Control ids, 6620 and up, clear of the tab's own 6601-6608. */
#define IDC_MTL_CANVAS    6620
#define IDC_MTL_KEY_FIRST 6621    /* the keyboard occupies 6621..6641 */
#define IDC_MTL_KEY_LAST  6641

/* What the view asks of the tab that hosts it. */
typedef struct minitel_view_host {
    /* Send bytes up the line; ignored by the host when not connected. */
    void (*send)(const unsigned char *buf, size_t len);
    /* CONNEXION/FIN pressed while disconnected: connect. */
    void (*connect)(void);
    /* CONNEXION/FIN pressed twice in a row while connected: hang up
     * locally, as the terminal does (STUM1B p33-34). */
    void (*disconnect)(void);
    /* A line for the status bar (counters, the key just sent). */
    void (*status)(const char *text);
    /* A local page was opened (Ctrl+O): cancel anything that would
     * replace the screen, such as a pending logoff timer. */
    void (*file_opened)(void);
} minitel_view_host_t;

void minitel_view_create(HWND parent, HINSTANCE hInst,
                         const minitel_view_host_t *host);
void minitel_view_destroy(void);

void minitel_view_show(BOOL show);
/* The area the screen and the keyboard share, in parent coordinates. */
void minitel_view_layout(int x, int y, int w, int h);
/* The long CONNEXION/FIN key, placed in the tab's top row. */
void minitel_view_place_cnx(int x, int y, int w, int h);

/* CRT scan lines (observed on MiniPavi's emulator), default on. */
void minitel_view_set_scanlines(BOOL on);

/* The function keys are owner-drawn (light brown, ENVOI light green);
 * the parent forwards WM_DRAWITEM. FALSE if it was not one of them. */
BOOL minitel_view_on_drawitem(const DRAWITEMSTRUCT *di);

/* A new session or a new file: the power-on state (STUM1B p87-88). */
void minitel_view_reset(void);

/* The default (idle) screen: power-on state, the guidance over it. */
void minitel_view_default_screen(void);
/* The Suite's own logoff message, drawn as a Minitel page. */
void minitel_view_show_logoff(const char *host);
void minitel_view_feed(const unsigned char *buf, size_t len);
/* Open a local .vdt page through the file dialog. */
void minitel_view_open_file(HWND owner);

void minitel_view_set_connected(BOOL connected);
void minitel_view_set_mono(BOOL mono);
void minitel_view_focus(void);

/* Keyboard buttons live in the parent, so the parent forwards them. */
BOOL minitel_view_on_command(WPARAM wParam);

/* The counter line, under the counter-honesty rule. */
void minitel_view_counts(char *buf, size_t n);

#endif /* MINITEL_VIEW_H_INCLUDED */
