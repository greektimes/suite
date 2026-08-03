/*
 * ripscrip_module.c - RIPscrip 1.54 graphical terminal tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The tab is a private container (MGTRipPanel) holding a connect strip
 * (Host / Port / Connect / Disconnect) over a render canvas
 * (MGTRipCanvasV1). Everything below the wire is in rip_ega.[ch] and
 * rip_parser.[ch], which are clean-room from the published RIPscrip 1.54
 * specification and contain no Win32 code; this file is the session,
 * the transport and the paint.
 *
 * TRANSPORT. The tab reuses telnet_proto.[ch] exactly as the Terminal
 * tab does: the worker thread does the TCP connect and answers RFC 854
 * option negotiation, and the module receives only payload bytes. The
 * difference is where the payload goes: the Terminal tab feeds the VT220
 * emulator, this tab feeds the RIP parser.
 *
 * HANDSHAKE. The host auto-senses a RIPscrip terminal by sending the
 * ANSI query ESC [ ! (1.54, "ANSI SEQUENCES (AUTO-SENSING)"). A RIPscrip
 * terminal answers RIPSCRIPxxyyvs with no terminator, where xx is the
 * major version, yy the minor, v the vendor code and s the vendor's
 * sub-revision. We are not RIPterm (vendor 1) or Qmodem Pro (vendor 2),
 * so we answer as vendor 0, "Generic RIPscrip terminal", at version
 * 1.54: RIPSCRIP015400. Verified against the live service on
 * 2026-08-01: the query arrives and this answer gets the GUI.
 *
 * ESC [ 1 ! and ESC [ 2 ! disable and enable RIP processing; both are
 * honoured here. The query and mode sequences are consumed by the
 * scanner so they never reach the parser as raw text.
 *
 * KEYBOARD. Typing in the scene sends raw characters to the host, which
 * is how a host command token such as RWX followed by Return reaches the
 * service. That is transport, not RIPscrip: RIP buttons, mouse regions
 * and hit-testing are still deferred, so a hotkey is not translated into
 * its button's host command for you. Characters are derived from the
 * virtual key rather than WM_CHAR so a non-Latin keyboard layout still
 * produces ASCII tokens.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "ripscrip_module.h"
#include "suite_shell.h"
#include "telnet_proto.h"
#include "zmodem_recv.h"
#include "rip_ega.h"
#include "rip_parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-local control IDs (6400+, clear of IRC's 6200 block). */
#define IDC_RIP_HOST_EDIT    6401
#define IDC_RIP_PORT_EDIT    6402
#define IDC_RIP_CONNECT_BTN  6403
#define IDC_RIP_DISCONN_BTN  6404
#define IDC_RIP_CANVAS       6405

#define RIP_PANEL_CLASS   "MGTRipPanel"
#define RIP_CANVAS_CLASS  "MGTRipCanvasV1"

#define WM_RIP_NOTIFY  (WM_USER + 41)
#define WM_ZMR_NOTIFY  (WM_USER + 42)

/* Our answer to the auto-sensing query: version 1.54, vendor code 0
 * (generic RIPscrip terminal), sub-revision 0. */
#define RIP_VERSION_ANSWER  "RIPSCRIP015400"

/* The EGA display aspect. A 640x350 EGA screen filled a 4:3 monitor, so
 * the surface is presented as 640x480 before being scaled to fit. */
#define RIP_ASPECT_W  640
#define RIP_ASPECT_H  480

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HWND g_hContent   = NULL;
static HWND g_hPanel     = NULL;
static HWND g_hCanvas    = NULL;
static HWND g_hHostLbl   = NULL, g_hHostEdit = NULL;
static HWND g_hPortLbl   = NULL, g_hPortEdit = NULL;
static HWND g_hConnBtn   = NULL, g_hDiscBtn  = NULL;

static BOOL g_controls_created  = FALSE;
static BOOL g_panel_class_reg   = FALSE;
static BOOL g_canvas_class_reg  = FALSE;

static HBRUSH g_hBrushCanvasBk = NULL;   /* letterbox black */

static TelnetProto *g_telnet    = NULL;
static BOOL         g_connected = FALSE;
static BOOL         g_ever_drew = FALSE;
static char         g_active_host[256] = RIPSCRIP_DEFAULT_HOST;
static int          g_active_port      = RIPSCRIP_DEFAULT_PORT;

/* The surface, the parser, and the DIB the surface is blitted through.
 * The DIB is rebuilt from the surface whenever the parser reports that
 * something changed, which also covers palette-only changes. */
static RipEga    g_surf;
static RipParser g_parser;
static HBITMAP   g_hDib     = NULL;
static void     *g_dib_bits = NULL;
static HDC       g_hMemDC   = NULL;
static HBITMAP   g_hOldDib  = NULL;

/* RIP processing can be switched off by the host with ESC [ 1 !. */
static BOOL g_rip_enabled = TRUE;

/* A short message painted over the scene, used for the download guard.
 * It lives in the overlay rather than in the EGA surface so it never
 * disturbs the rendered scene underneath. */
static char  g_notice[128];
static DWORD g_notice_until = 0;
/* A notice that stays up until it is replaced or cleared, rather than
 * expiring. The transfer progress line uses it. */
static BOOL  g_notice_persist = FALSE;

#define RIP_NOTICE_MS       3500
#define RIP_NOTICE_TIMER_ID 0xB101

/* ---- Zmodem receive -------------------------------------------------
 *
 * The Digital Replica screen's issue buttons ask the service to send a
 * file, and the service answers with Synchronet sexyz running Zmodem
 * over the same telnet session. Until this build there was no receiver,
 * so those commands were recognised and held back rather than sent.
 *
 * Now they are sent, and the tab switches the payload stream away from
 * the RIP parser for the duration. Nothing else changes: the same
 * socket, the same worker thread, the same telnet layer. Only the
 * consumer of the bytes moves.
 *
 *   terminal / RIP mode      g_zmodem_active == FALSE, bytes -> parser
 *   Zmodem receive mode      g_zmodem_active == TRUE,  bytes -> zmodem_recv
 *
 * Coming back out is driven by ZMR_EVT_DONE. Both the telnet data
 * events and that event are posted to the canvas, and the canvas
 * processes its queue in order, so a chunk posted before the transfer
 * ended is pushed into the receiver and drained back out as the result
 * tail, while a chunk posted after it is already in terminal mode and
 * goes to the parser. The scene stream is therefore never reordered and
 * never loses a byte across the switch.
 *
 * A transfer is EXEMPT from any pacing of the scene: the canvas simply
 * is not fed while one is running, so there is nothing to pace. */
static ZmodemRecv *g_zmr           = NULL;
static BOOL        g_zmodem_active = FALSE;
static unsigned long g_zmr_done    = 0;   /* completed this session   */

/* Rolling window over the payload stream, used only while NOT in
 * receive mode, to notice a transfer the service starts on its own
 * initiative rather than in answer to a button. The signature is the
 * opening of any Zmodem hex header: ZPAD ZPAD ZDLE 'B'. */
static unsigned char g_zsig[4];
static int           g_zsig_len = 0;

/* ---- press feedback ------------------------------------------------
 *
 * 1.54 gives the pressed look for both region kinds, and in both cases
 * it is an INVERT rather than a second, darker button image:
 *
 *   RIP_MOUSE <clk>, "if 1, indicates that the region should be visibly
 *   inverted while the mouse button is down. This offers visual
 *   feedback."
 *
 *   RIP_BUTTON_STYLE flag 2, "Button is Invertable ... the button will
 *   be inverted when clicked ... Even if the button has special
 *   effects, those will be inverted as well as they are considered part
 *   of the button - all except for the Recessed effect."
 *
 * So the terminal inverts the button's image rectangle on mouse-down
 * and inverts it a second time to take the look back off, which is
 * exact because the complement is its own inverse. Nothing is cached
 * and no copy of the scene is kept.
 *
 * g_press_idx is the region the mouse went down on, or -1. g_press_in
 * says whether the inversion is currently applied, which is false while
 * the pointer has been dragged off the region. g_press_armed
 * distinguishes a press that may still activate from one that a scene
 * change has cancelled underneath.
 */
static int  g_press_idx   = -1;
static BOOL g_press_in    = FALSE;
static BOOL g_press_armed = FALSE;

/* Hot-key flash: the same inversion, applied for a moment before the
 * command goes out, so the keyboard gets the feedback the mouse gets. */
static int  g_flash_idx = -1;
static char g_flash_cmd[RIP_BTN_CMD];
static char g_flash_label[RIP_BTN_LABEL];

/* Long enough to register as a press and short enough not to feel like
 * a delay. Below about a tenth of a second the flash is easy to miss on
 * a slow repaint, which is also why a cross-process test harness has to
 * capture a burst to catch it. */
#define RIP_FLASH_MS        130
#define RIP_FLASH_TIMER_ID  0xB102

/* Optional capture of the payload stream for the diff harness. Set the
 * MGT_RIP_CAPTURE environment variable to a file path before launching
 * the Suite and every payload byte fed to the parser is appended there,
 * which is the same byte sequence rip_render.exe consumes. */
static FILE *g_capture = NULL;

/* Handshake scanner state: how much of an ESC [ ... ! sequence we have
 * seen and are holding back from the parser. */
static int  g_esc_state = 0;     /* 0 none, 1 ESC, 2 ESC[, 3 ESC[<digit> */
static char g_esc_digit = 0;

static void rip_set_session_ui(BOOL connected);
static void rip_status(const char *fmt, ...);
static void rip_press_cancel_for_redraw(void);
static int  rip_consume(const unsigned char *data, int len);

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

static void rip_status(const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';
    suite_set_status(buf);
}

/* ------------------------------------------------------------------ */
/* Surface -> DIB                                                      */
/* ------------------------------------------------------------------ */

static void rip_dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_hDib) return;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = RIP_EGA_W;
    bi.bmiHeader.biHeight      = -RIP_EGA_H;   /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                              &g_dib_bits, NULL, 0);
    if (g_hDib) {
        g_hMemDC  = CreateCompatibleDC(screen);
        g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
    }
    ReleaseDC(NULL, screen);
}

static void rip_dib_destroy(void)
{
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
        g_hOldDib = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; }
    g_dib_bits = NULL;
}

static void rip_dib_refresh(void)
{
    rip_dib_create();
    if (g_dib_bits) rip_ega_to_bgra(&g_surf, (unsigned char *)g_dib_bits);
}

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

/* Largest 4:3 rectangle that fits the client area, centred. */
static void rip_dest_rect(int cw, int ch, RECT *out)
{
    int dw, dh;
    if (cw <= 0 || ch <= 0) { SetRect(out, 0, 0, 0, 0); return; }

    dw = cw;
    dh = (int)((long)cw * RIP_ASPECT_H / RIP_ASPECT_W);
    if (dh > ch) {
        dh = ch;
        dw = (int)((long)ch * RIP_ASPECT_W / RIP_ASPECT_H);
    }
    out->left   = (cw - dw) / 2;
    out->top    = (ch - dh) / 2;
    out->right  = out->left + dw;
    out->bottom = out->top  + dh;
}

/* Client point to surface point: the exact inverse of the blit below.
 * The parser stores region rectangles in the same 640x350 space it hands
 * to the drawing primitives, so a click has to be carried back into that
 * space through the same centred, aspect-corrected rectangle the paint
 * stretches into. Returns 0 for a click in the letterbox. */
static int rip_client_to_surface(HWND hwnd, int px, int py, int *sx, int *sy)
{
    RECT rc, dst;
    int cw, ch, dw, dh;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return 0;

    rip_dest_rect(cw, ch, &dst);
    dw = dst.right - dst.left;
    dh = dst.bottom - dst.top;
    if (dw <= 0 || dh <= 0) return 0;

    if (px < dst.left || px >= dst.right) return 0;
    if (py < dst.top  || py >= dst.bottom) return 0;

    *sx = (int)(((long)(px - dst.left) * RIP_EGA_W) / dw);
    *sy = (int)(((long)(py - dst.top)  * RIP_EGA_H) / dh);
    if (*sx < 0) *sx = 0;
    if (*sy < 0) *sy = 0;
    if (*sx > RIP_EGA_W - 1) *sx = RIP_EGA_W - 1;
    if (*sy > RIP_EGA_H - 1) *sy = RIP_EGA_H - 1;
    return 1;
}

static void rip_canvas_paint(HWND hwnd, HDC hdc)
{
    RECT rc, dst;
    HDC     mem;
    HBITMAP bmp, oldbmp;
    int cw, ch;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    /* Compose off-screen: the letterbox fill and the scaled surface land
     * on one memory DC and reach the screen in a single BitBlt, so a
     * fast stream never flickers. */
    mem = CreateCompatibleDC(hdc);
    if (!mem) return;
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    if (!bmp) { DeleteDC(mem); return; }
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, g_hBrushCanvasBk);

    if (g_ever_drew && g_hMemDC) {
        rip_dest_rect(cw, ch, &dst);
        /* COLORONCOLOR is GDI's nearest-neighbour: no smoothing, no
         * averaging. Scaled-up EGA pixels stay hard-edged blocks. */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem,
                   dst.left, dst.top,
                   dst.right - dst.left, dst.bottom - dst.top,
                   g_hMemDC, 0, 0, RIP_EGA_W, RIP_EGA_H, SRCCOPY);
    } else {
        const char *hint = g_connected
            ? "Connected. Waiting for the host to send the RIPscrip scene..."
            : "Not connected. Enter a host and press Connect.";
        /* Typing reaches the host once a scene is up; see the header. */
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(160, 160, 160));
        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        DrawTextA(mem, hint, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* Notice, painted over the scene and never into it. */
    if (g_notice[0] && (g_notice_persist || GetTickCount() < g_notice_until)) {
        RECT nr;
        SIZE sz;
        int pad = 10, bw, bh;

        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        GetTextExtentPoint32A(mem, g_notice, (int)strlen(g_notice), &sz);
        bw = sz.cx + pad * 2;
        bh = sz.cy + pad;
        nr.left   = (cw - bw) / 2;
        nr.top    = ch - bh - 24;
        nr.right  = nr.left + bw;
        nr.bottom = nr.top + bh;
        if (nr.top < 0) nr.top = 0;

        FillRect(mem, &nr, (HBRUSH)GetStockObject(BLACK_BRUSH));
        FrameRect(mem, &nr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        DrawTextA(mem, g_notice, -1, &nr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static void rip_send_raw(const void *buf, int len)
{
    if (g_telnet && len > 0) telnet_proto_send(g_telnet, buf, (size_t)len);
}

static void rip_notice_set(const char *msg, BOOL persist)
{
    if (!msg) return;
    strncpy(g_notice, msg, sizeof g_notice - 1);
    g_notice[sizeof g_notice - 1] = '\0';
    g_notice_persist = persist;
    g_notice_until   = GetTickCount() + RIP_NOTICE_MS;
    if (g_hCanvas) {
        if (persist) KillTimer(g_hCanvas, RIP_NOTICE_TIMER_ID);
        else SetTimer(g_hCanvas, RIP_NOTICE_TIMER_ID, RIP_NOTICE_MS + 100, NULL);
        InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

static void rip_show_notice(const char *msg)
{
    rip_notice_set(msg, FALSE);
}

static void rip_notice_clear(void)
{
    g_notice[0]      = '\0';
    g_notice_persist = FALSE;
    if (g_hCanvas) {
        KillTimer(g_hCanvas, RIP_NOTICE_TIMER_ID);
        InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

/* How the Zmodem session writes back to the far end. Called on the
 * receiver's worker thread; telnet_proto_send() takes the socket lock
 * and does the RFC 854 doubling, which is exactly the outer layer the
 * receiver must not do for itself. */
static int rip_zmr_send(void *ctx, const void *buf, size_t len)
{
    (void)ctx;
    if (!g_telnet) return 1;
    return telnet_proto_send(g_telnet, buf, len);
}

/* Leave terminal/RIP mode and hand the payload stream to the Zmodem
 * receiver. Returns non-zero if the switch happened.
 *
 * The receiver is started BEFORE the command goes out, so there is no
 * window in which the service's first frame could reach the parser. */
static int rip_zmodem_begin(const char *why)
{
    char dir[MAX_PATH];

    if (g_zmodem_active || g_zmr) return 0;
    if (!g_connected || !g_telnet) return 0;

    /* No pressed region may be left inverted across a transfer: the
     * scene is frozen for its duration and the release will not come. */
    rip_press_cancel_for_redraw();

    zmodem_recv_default_dir(dir, sizeof dir);
    if (zmodem_recv_start(dir, g_hCanvas, WM_ZMR_NOTIFY,
                          rip_zmr_send, NULL, &g_zmr) != 0 || !g_zmr) {
        g_zmr = NULL;
        return 0;
    }

    g_zmodem_active = TRUE;
    g_zsig_len      = 0;
    rip_notice_set(why ? why : "Receiving file...", TRUE);
    rip_status("Zmodem receive started. Press Esc to cancel.");
    return 1;
}

/* Send a region's host command. The command already carries its own
 * terminator when the scene used the caret escape (the service writes
 * "RWX^M", which the parser expanded to a real carriage return), so a
 * second one is added only when it is missing.
 *
 * DOWNLOAD COMMANDS. The Digital Replica screen's issue buttons ask the
 * server to send a file by Zmodem. Earlier builds recognised those
 * commands and held them back, because the frames would otherwise have
 * arrived in the RIP parser and been drawn as punctuation. They now go
 * out like any other command, with one difference: the Zmodem receiver
 * is switched in first, so the answering frames go to it instead of to
 * the parser. A download that cannot be started is still held back and
 * still counted, which is the graceful fallback rather than a stream of
 * Zmodem noise across the scene. */
/* Returns non-zero if the command actually went out, so callers never
 * report a send that did not happen. */
static int rip_send_command(const char *cmd)
{
    size_t n;
    int    is_download;

    if (!g_connected || !g_telnet || !cmd || !*cmd) return 0;

    /* Nothing may be sent while a transfer owns the stream. */
    if (g_zmodem_active) {
        rip_status("A file transfer is running. Press Esc to cancel it.");
        return 0;
    }

    is_download = rip_command_is_download(cmd);
    if (is_download && !rip_zmodem_begin("Starting download...")) {
        g_parser.stats.download_blocked++;
        rip_show_notice("Download could not be started");
        rip_status("Download held back: the receiver would not start "
                   "(%lu blocked this session)",
                   g_parser.stats.download_blocked);
        return 0;
    }

    n = strlen(cmd);
    rip_send_raw(cmd, (int)n);
    if (cmd[n - 1] != '\r' && cmd[n - 1] != '\n')
        rip_send_raw("\r", 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Press feedback                                                      */
/* ------------------------------------------------------------------ */

/* Invert one region's image rectangle and push the change to the DIB.
 * Regions that did not ask for the effect are left alone: 1.54 makes it
 * opt-in through RIP_MOUSE <clk> and the Invertable flag, and a region
 * that did not ask must not flicker. */
static void rip_press_flip_quiet(int idx)
{
    const RipButton *b;
    if (idx < 0 || idx >= g_parser.button_count) return;
    b = &g_parser.buttons[idx];
    if (!b->invert) return;
    if (b->ix1 < b->ix0 || b->iy1 < b->iy0) return;
    rip_ega_invert_rect(&g_surf, b->ix0, b->iy0, b->ix1, b->iy1);
}

static void rip_press_flip(int idx)
{
    if (idx < 0 || idx >= g_parser.button_count) return;
    if (!g_parser.buttons[idx].invert) return;

    rip_press_flip_quiet(idx);
    rip_dib_refresh();
    if (g_hCanvas) {
        InvalidateRect(g_hCanvas, NULL, FALSE);
        /* Paint now rather than at the next idle: a press that appears
         * after the button has already been released is not feedback. */
        UpdateWindow(g_hCanvas);
    }
}

/* Take the pressed look off, if it is on, and forget the press. */
static void rip_press_clear(void)
{
    if (g_press_in) rip_press_flip(g_press_idx);
    g_press_idx   = -1;
    g_press_in    = FALSE;
    g_press_armed = FALSE;
}

/* Cancel a press outright, without taking the inversion back off. Used
 * when the host has replaced the region the press belonged to, so there
 * is nothing left to restore. */
static void rip_press_abandon(void)
{
    if (g_flash_idx >= 0 && g_hCanvas) KillTimer(g_hCanvas, RIP_FLASH_TIMER_ID);
    g_flash_idx    = -1;
    g_flash_cmd[0] = '\0';
    g_press_idx    = -1;
    g_press_in     = FALSE;
    g_press_armed  = FALSE;
}

/* Cancel a press and take the look off first. */
static void rip_press_cancel_for_redraw(void)
{
    if (g_press_idx < 0 && g_flash_idx < 0) return;
    if (g_press_in) rip_press_flip(g_press_idx);
    rip_press_abandon();
}

/* ---- keeping a press alive across an incoming repaint ---------------
 *
 * The service does not go quiet while a button is held: the menu's news
 * ticker and the clock keep arriving, so a press that was cancelled
 * whenever bytes turned up would never be visible at all. That was the
 * first attempt and it produced no feedback whatsoever on the live
 * service.
 *
 * So the inversion is lifted before the parser draws and put back
 * afterwards. Lifting it first matters: the host's drawing must land on
 * the true scene, not on inverted pixels, or the region would be left
 * doubly inverted where the two overlap.
 *
 * The press only survives if the region it belongs to is still the same
 * region. A scene change kills the mouse fields and renumbers the list,
 * and re-inverting an index that now means a different button would put
 * the pressed look somewhere the user never pressed. */
typedef struct RipPressSaved {
    int      active;
    int      idx;
    RipButton btn;
} RipPressSaved;

static void rip_press_suspend(RipPressSaved *sv)
{
    sv->active = 0;
    if (!g_press_in || g_press_idx < 0 || g_press_idx >= g_parser.button_count)
        return;
    sv->active = 1;
    sv->idx    = g_press_idx;
    sv->btn    = g_parser.buttons[g_press_idx];
    rip_press_flip_quiet(g_press_idx);
}

/* Returns non-zero if the canvas needs repainting even though the
 * parser reported no drawing, which happens when the press was
 * abandoned after a RIP_KILL_MOUSE_FIELDS that drew nothing. */
static int rip_press_resume(const RipPressSaved *sv)
{
    const RipButton *now;

    if (!sv->active) return 0;
    if (sv->idx >= g_parser.button_count) { rip_press_abandon(); return 1; }

    now = &g_parser.buttons[sv->idx];
    if (now->ix0 != sv->btn.ix0 || now->iy0 != sv->btn.iy0 ||
        now->ix1 != sv->btn.ix1 || now->iy1 != sv->btn.iy1 ||
        now->invert != sv->btn.invert ||
        strcmp(now->command, sv->btn.command) != 0) {
        rip_press_abandon();
        return 1;
    }
    rip_press_flip_quiet(sv->idx);
    return 0;
}

/* Send a region's command and report only a send that happened. Shared
 * by the mouse-up path and the end of the hot-key flash. */
static void rip_activate_region(int idx)
{
    if (idx < 0 || idx >= g_parser.button_count) return;
    if (rip_send_command(g_parser.buttons[idx].command))
        rip_status("Sent \"%s\"%s%s", g_parser.buttons[idx].command,
                   g_parser.buttons[idx].label[0] ? " for " : "",
                   g_parser.buttons[idx].label);
}

/* Hot-key activation. The pressed look goes on, the screen is painted,
 * and a short timer takes it off again and sends the command. The
 * command is deliberately held until the flash is over so the feedback
 * cannot be overtaken by the host's reply, and it is copied out of the
 * region because the reply may replace the region list.
 *
 * A region that did not ask to invert has nothing to show, so its
 * command goes out at once rather than after a pause the user cannot
 * see the reason for. */
static void rip_hotkey_activate(HWND hwnd, int idx)
{
    if (idx < 0 || idx >= g_parser.button_count) return;

    rip_press_cancel_for_redraw();

    if (!g_parser.buttons[idx].invert || !hwnd) {
        rip_activate_region(idx);
        return;
    }

    strncpy(g_flash_cmd, g_parser.buttons[idx].command,
            sizeof g_flash_cmd - 1);
    g_flash_cmd[sizeof g_flash_cmd - 1] = '\0';
    strncpy(g_flash_label, g_parser.buttons[idx].label,
            sizeof g_flash_label - 1);
    g_flash_label[sizeof g_flash_label - 1] = '\0';
    g_flash_idx = idx;

    g_press_idx   = idx;
    g_press_in    = TRUE;
    g_press_armed = FALSE;          /* no mouse-up will arrive for this */
    rip_press_flip(idx);
    SetTimer(hwnd, RIP_FLASH_TIMER_ID, RIP_FLASH_MS, NULL);
}

/* Feed payload bytes to the parser, after pulling out the ANSI
 * auto-sensing sequences. Returns non-zero if the surface changed. */
static int rip_consume(const unsigned char *data, int len)
{
    unsigned char out[8192];
    int i, o = 0, drew = 0;

    for (i = 0; i < len; i++) {
        unsigned char b = data[i];

        switch (g_esc_state) {
        case 0:
            if (b == 0x1B) { g_esc_state = 1; continue; }
            break;

        case 1:
            if (b == '[') { g_esc_state = 2; continue; }
            /* Not ours: release the ESC we held and reprocess this byte
             * as an ordinary one. */
            if (o < (int)sizeof out) out[o++] = 0x1B;
            g_esc_state = 0;
            break;

        case 2:
            if (b == '!') {                       /* ESC [ !  -> query */
                rip_send_raw(RIP_VERSION_ANSWER,
                             (int)strlen(RIP_VERSION_ANSWER));
                g_esc_state = 0;
                continue;
            }
            if (b >= '0' && b <= '2') {
                g_esc_digit = (char)b;
                g_esc_state = 3;
                continue;
            }
            if (o + 2 <= (int)sizeof out) { out[o++] = 0x1B; out[o++] = '['; }
            g_esc_state = 0;
            break;

        case 3:
            if (b == '!') {
                if (g_esc_digit == '0') {         /* same as ESC [ ! */
                    rip_send_raw(RIP_VERSION_ANSWER,
                                 (int)strlen(RIP_VERSION_ANSWER));
                } else if (g_esc_digit == '1') {
                    g_rip_enabled = FALSE;
                } else if (g_esc_digit == '2') {
                    g_rip_enabled = TRUE;
                }
                g_esc_state = 0;
                continue;
            }
            if (o + 3 <= (int)sizeof out) {
                out[o++] = 0x1B; out[o++] = '['; out[o++] = g_esc_digit;
            }
            g_esc_state = 0;
            break;
        }

        if (o < (int)sizeof out) out[o++] = b;

        if (o == (int)sizeof out) {
            if (g_capture) fwrite(out, 1, (size_t)o, g_capture);
            if (g_rip_enabled && rip_parser_feed(&g_parser, out, o)) drew = 1;
            o = 0;
        }
    }

    if (o > 0) {
        if (g_capture) fwrite(out, 1, (size_t)o, g_capture);
        if (g_rip_enabled && rip_parser_feed(&g_parser, out, o)) drew = 1;
    }
    if (g_capture) fflush(g_capture);
    return drew;
}

/* Tear a transfer down without waiting for its DONE event, which is
 * what a disconnect underneath one amounts to. The tail is discarded:
 * there is no session left to hand it back to. */
static void rip_zmodem_abandon(void)
{
    if (!g_zmr) { g_zmodem_active = FALSE; return; }
    zmodem_recv_cancel(g_zmr);
    zmodem_recv_free_result(zmodem_recv_finish(g_zmr));
    g_zmr           = NULL;
    g_zmodem_active = FALSE;
    g_zsig_len      = 0;
    rip_notice_clear();
}

static void rip_close_session(void)
{
    /* A transfer holds the socket the close is about to take away. */
    if (g_zmodem_active || g_zmr) rip_zmodem_abandon();
    /* No session, no pending press: never leave a region inverted or a
     * flash timer waiting to send into a closed socket. */
    rip_press_cancel_for_redraw();
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    g_connected = FALSE;
    g_esc_state = 0;
    g_zsig_len  = 0;
    if (g_capture) { fclose(g_capture); g_capture = NULL; }
    rip_set_session_ui(FALSE);
}

static void rip_do_connect(void)
{
    char host[256], portbuf[16];
    int  port, rc;

    if (g_connected || g_telnet) return;

    GetWindowTextA(g_hHostEdit, host, sizeof host);
    GetWindowTextA(g_hPortEdit, portbuf, sizeof portbuf);
    host[sizeof host - 1] = '\0';
    if (!host[0]) {
        rip_status("Enter a host name first.");
        SetFocus(g_hHostEdit);
        return;
    }
    port = atoi(portbuf);
    if (port <= 0 || port > 65535) port = RIPSCRIP_DEFAULT_PORT;

    strncpy(g_active_host, host, sizeof g_active_host - 1);
    g_active_host[sizeof g_active_host - 1] = '\0';
    g_active_port = port;

    /* Fresh scene per session, and no press state pointing into the
     * region list that is about to be emptied. */
    rip_press_cancel_for_redraw();
    rip_parser_init(&g_parser, &g_surf);
    g_rip_enabled = TRUE;
    g_ever_drew   = FALSE;
    g_zsig_len    = 0;
    rip_dib_refresh();
    InvalidateRect(g_hCanvas, NULL, FALSE);

    {
        const char *cap = getenv("MGT_RIP_CAPTURE");
        if (cap && cap[0] && !g_capture) g_capture = fopen(cap, "wb");
    }

    rip_status("Connecting to %s:%d...", g_active_host, g_active_port);

    rc = telnet_proto_open(g_active_host, g_active_port,
                           g_hCanvas, WM_RIP_NOTIFY, &g_telnet);
    if (rc != 0 || !g_telnet) {
        g_telnet = NULL;
        rip_status("Failed to start the connection to %s:%d.",
                   g_active_host, g_active_port);
        rip_set_session_ui(FALSE);
        return;
    }
    /* The buttons flip on TELNET_EVT_CONNECTED, not here: until the TCP
     * handshake lands there is no session to disconnect from. */
}

/* Enable/disable the strip for the session state. Pre-connect the host
 * and port are editable and Connect is live; once connected they lock
 * and only Disconnect is live. */
static void rip_set_session_ui(BOOL connected)
{
    if (!g_controls_created) return;
    EnableWindow(g_hHostEdit, !connected);
    EnableWindow(g_hPortEdit, !connected);
    EnableWindow(g_hConnBtn,  !connected);
    EnableWindow(g_hDiscBtn,   connected);
}

/* ------------------------------------------------------------------ */
/* Transport events                                                    */
/* ------------------------------------------------------------------ */

static void rip_on_connected(void)
{
    g_connected = TRUE;
    rip_set_session_ui(TRUE);
    rip_status("Connected to %s:%d. Waiting for the RIPscrip scene...",
               g_active_host, g_active_port);
    /* Typing goes to the host, so the scene takes focus on connect. */
    SetFocus(g_hCanvas);
    InvalidateRect(g_hCanvas, NULL, FALSE);
}

/* Watch an ordinary payload chunk for the start of a Zmodem hex header,
 * which is how a transfer the user did not ask for would announce
 * itself. Returns the offset just past the signature if one completed
 * in this chunk, or -1.
 *
 * The four bytes are not held back from the parser. ZPAD is an asterisk
 * and ZDLE is a control code, so at worst two asterisks reach the TTY
 * text the scene ignores; and the receiver, once started, sends ZRINIT
 * unprompted, which makes the sender repeat the ZRQINIT this one
 * belonged to. Nothing depends on catching that first header. */
static int rip_scan_for_zmodem(const unsigned char *data, int len)
{
    static const unsigned char sig[4] = { '*', '*', 0x18, 'B' };
    int i;

    for (i = 0; i < len; i++) {
        if (g_zsig_len < 4 && data[i] == sig[g_zsig_len]) {
            g_zsig[g_zsig_len++] = data[i];
            if (g_zsig_len == 4) { g_zsig_len = 0; return i + 1; }
        } else {
            /* Restart, allowing this byte to open a fresh match. */
            g_zsig_len = (data[i] == sig[0]) ? 1 : 0;
        }
    }
    return -1;
}

static void rip_on_data(TelnetDataChunk *chunk)
{
    if (!chunk) return;

    /* Zmodem receive mode: the parser is out of the loop entirely. */
    if (g_zmodem_active) {
        if (chunk->data && chunk->len > 0)
            zmodem_recv_push(g_zmr, chunk->data, chunk->len);
        telnet_proto_free_chunk(chunk);
        return;
    }

    if (chunk->data && chunk->len > 0) {
        int at = rip_scan_for_zmodem(chunk->data, chunk->len);
        if (at >= 0) {
            /* The service started a transfer on its own. Feed the
             * parser what came before the signature, then switch. */
            RipPressSaved pv;
            rip_press_suspend(&pv);
            if (at > 0) rip_consume(chunk->data, at);
            rip_press_resume(&pv);
            rip_dib_refresh();
            InvalidateRect(g_hCanvas, NULL, FALSE);

            if (rip_zmodem_begin("Receiving file...")) {
                if (at < chunk->len)
                    zmodem_recv_push(g_zmr, chunk->data + at, chunk->len - at);
                telnet_proto_free_chunk(chunk);
                return;
            }
            /* Could not start: fall through and let the parser have the
             * rest, which is the old behaviour and merely untidy. */
            {
                unsigned char *rest = chunk->data + at;
                int rest_len = chunk->len - at;
                if (rest_len > 0) {
                    memmove(chunk->data, rest, (size_t)rest_len);
                    chunk->len = rest_len;
                } else {
                    chunk->len = 0;
                }
            }
        }
    }

    if (chunk->data && chunk->len > 0) {
        RipPressSaved sv;
        int drew, forced;

        /* The host draws on the true scene, never on inverted pixels. */
        rip_press_suspend(&sv);
        drew   = rip_consume(chunk->data, chunk->len);
        forced = rip_press_resume(&sv);

        if (drew || forced) {
            rip_dib_refresh();
            InvalidateRect(g_hCanvas, NULL, FALSE);
        }
        if (drew) {
            g_ever_drew = TRUE;
            rip_status("%s:%d  %lu commands, %lu drawn, %lu deferred",
                       g_active_host, g_active_port,
                       g_parser.stats.commands, g_parser.stats.drawn,
                       rip_stats_deferred(&g_parser.stats));
        }
    }
    telnet_proto_free_chunk(chunk);
}

static void rip_on_error(const char *msg)
{
    rip_status("Connection error: %s", msg ? msg : "unknown");
}

/* ---- Zmodem receive events ---------------------------------------- */

static void rip_on_zmr_progress(ZmrProgress *p)
{
    char line[128];

    if (!p) return;
    if (g_zmodem_active) {
        if (p->total > 0) {
            int pct = (int)((p->received * 100ULL) / p->total);
            if (pct > 100) pct = 100;
            _snprintf(line, sizeof line - 1, "Receiving %s  %d%%  (%llu of %llu bytes)",
                      p->name[0] ? p->name : "file", pct,
                      p->received, p->total);
        } else {
            _snprintf(line, sizeof line - 1, "Receiving %s  %llu bytes",
                      p->name[0] ? p->name : "file", p->received);
        }
        line[sizeof line - 1] = '\0';
        rip_notice_set(line, TRUE);
        rip_status("%s  Esc cancels.", line);
    }
    zmodem_recv_free_progress(p);
}

/* Come back out of receive mode. Whatever arrived after the Zmodem
 * conversation ended belongs to the scene and is fed to the parser
 * here, in the order it arrived, before any later chunk is processed. */
static void rip_on_zmr_done(void)
{
    ZmrResult *r;
    char       line[160];

    if (!g_zmr) { g_zmodem_active = FALSE; return; }

    r = zmodem_recv_finish(g_zmr);
    g_zmr           = NULL;
    g_zmodem_active = FALSE;
    g_zsig_len      = 0;

    if (!r) {
        rip_notice_clear();
        rip_status("The transfer ended without a result.");
        return;
    }

    if (r->status == ZMR_OK) {
        g_zmr_done++;
        _snprintf(line, sizeof line - 1, "Saved %s (%llu bytes)",
                  r->name, r->received);
        line[sizeof line - 1] = '\0';
        rip_notice_set(line, FALSE);
        rip_status("Download complete: %s  (%llu bytes, %lu this session)",
                   r->path[0] ? r->path : r->name, r->received, g_zmr_done);
    } else {
        /* Not a hang and not a crash: say what happened, count it the
         * way the old guard counted a blocked download, and go back to
         * the scene. */
        g_parser.stats.download_blocked++;
        _snprintf(line, sizeof line - 1, "Download stopped: %s", r->detail);
        line[sizeof line - 1] = '\0';
        rip_notice_set(line, FALSE);
        rip_status("Download did not complete: %s (%lu held back this "
                   "session)", r->detail, g_parser.stats.download_blocked);
    }

    /* Bytes that arrived after the last Zmodem frame are scene bytes. */
    if (r->tail && r->tail_len > 0) {
        RipPressSaved sv;
        int drew, forced;
        rip_press_suspend(&sv);
        drew   = rip_consume(r->tail, r->tail_len);
        forced = rip_press_resume(&sv);
        if (drew) g_ever_drew = TRUE;
        if (drew || forced) rip_dib_refresh();
    }

    zmodem_recv_free_result(r);
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
}

static void rip_on_closed(void)
{
    BOOL was = g_connected;
    rip_close_session();
    if (was)
        rip_status("Disconnected from %s:%d.", g_active_host, g_active_port);
    InvalidateRect(g_hCanvas, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Window procedures                                                   */
/* ------------------------------------------------------------------ */

/* Derive a plain ASCII character from a virtual key.
 *
 * WM_CHAR is translated through the active keyboard layout, so on a Greek
 * layout 'T' arrives as tau rather than 0x54 (the lesson the Terminal tab
 * learned). Host command tokens are pure upper case ASCII plus digits, so
 * they are derived from the virtual key and the shift state instead, and
 * the matching WM_CHAR is swallowed. Returns 0 if the key is not one we
 * forward. */
static char rip_ascii_from_vk(WPARAM vk)
{
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    BOOL caps  = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    if (vk >= 'A' && vk <= 'Z') {
        BOOL upper = (shift != caps);
        return (char)(upper ? (int)vk : (int)vk + 32);
    }
    if (vk >= '0' && vk <= '9' && !shift) return (char)vk;
    if (vk == VK_SPACE)  return ' ';
    if (vk == VK_RETURN) return '\r';
    if (vk == VK_BACK)   return '\b';
    if (vk == VK_OEM_MINUS && !shift) return '-';
    return 0;
}

static LRESULT CALLBACK RipCanvasProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        /* rip_canvas_paint owns the whole rect, background included. */
        return 1;

    /* Custom-painted children in this shell must claim all keys or
     * IsDialogMessage swallows letters through its mnemonic search. */
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

    /* A button press is two events, not one. Mouse-down only shows the
     * pressed look; the host command goes out on mouse-up, and only if
     * the pointer is still on the same region. Releasing elsewhere
     * cancels, which is what every windowing system has done since the
     * Macintosh and what a user who has started the wrong action
     * expects to be able to do. */
    case WM_LBUTTONDOWN: {
        int sx, sy, idx;
        SetFocus(hwnd);                 /* keep typing working */
        if (!g_connected || g_zmodem_active) return 0;
        rip_press_cancel_for_redraw();  /* clears any hot-key flash */
        if (rip_client_to_surface(hwnd,
                                  (int)(short)LOWORD(lParam),
                                  (int)(short)HIWORD(lParam), &sx, &sy)) {
            idx = rip_button_at(&g_parser, sx, sy);
            if (idx >= 0) {
                g_press_idx   = idx;
                g_press_armed = TRUE;
                g_press_in    = TRUE;
                /* Capture so the release is seen even if the pointer
                 * has left the canvas by then. */
                SetCapture(hwnd);
                rip_press_flip(idx);
            }
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        int sx, sy;
        BOOL inside;
        if (!g_press_armed || g_press_idx < 0) break;
        inside = rip_client_to_surface(hwnd,
                                       (int)(short)LOWORD(lParam),
                                       (int)(short)HIWORD(lParam),
                                       &sx, &sy) &&
                 rip_button_at(&g_parser, sx, sy) == g_press_idx;
        if (inside != g_press_in) {
            /* Track the pointer: the look comes off when it leaves and
             * back on if it returns, so the user can see whether the
             * release will activate. */
            rip_press_flip(g_press_idx);
            g_press_in = inside;
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        int idx = g_press_idx;
        BOOL activate = g_press_armed && g_press_in && idx >= 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        rip_press_clear();              /* look comes off either way */
        if (activate) rip_activate_region(idx);
        return 0;
    }

    /* Capture lost to something else, an Alt-Tab or a message box:
     * abandon the press rather than leave the region inverted. */
    case WM_CAPTURECHANGED:
        rip_press_clear();
        return 0;

    case WM_KEYDOWN: {
        char c;
        int idx;
        if (!g_connected || !g_telnet) return 0;

        /* While a transfer owns the stream the only key that means
         * anything is the one that stops it. Everything else would go
         * out as Zmodem noise. */
        if (g_zmodem_active) {
            if (wParam == VK_ESCAPE) {
                rip_status("Cancelling the transfer...");
                zmodem_recv_cancel(g_zmr);
            }
            return 0;
        }

        c = rip_ascii_from_vk(wParam);
        if (c) {
            /* A hotkey belongs to its region: the terminal, not the
             * host, turns the key into the region's host command. Keys
             * that match no region are forwarded as typed. */
            idx = rip_button_by_hotkey(&g_parser, (unsigned char)c);
            if (idx >= 0) {
                rip_hotkey_activate(hwnd, idx);
                return 0;
            }
            /* A key that matches no hotkey is forwarded raw only when the
             * scene has no regions at all, which is the plain-terminal
             * case. Inside a GUI scene the host reads whole command
             * tokens delimited by carriage returns, so a stray character
             * would prefix the next hotkey's token and quietly turn it
             * into an unrecognised one. Dropping it keeps the token
             * stream clean; nothing on this service asks the user to
             * type free text into a graphics scene. */
            if (g_parser.button_count == 0) rip_send_raw(&c, 1);
            return 0;
        }
        break;
    }

    case WM_CHAR:
        /* Swallow: WM_KEYDOWN already forwarded the layout-independent
         * character, and letting this through would double it. */
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        rip_canvas_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == RIP_FLASH_TIMER_ID) {
            /* The flash is over: take the look off, then send. Sending
             * from the command copied at key-down time means a region
             * list replaced in the meantime cannot misdirect it. */
            KillTimer(hwnd, RIP_FLASH_TIMER_ID);
            rip_press_clear();
            if (g_flash_idx >= 0 && g_flash_cmd[0]) {
                char cmd[RIP_BTN_CMD], label[RIP_BTN_LABEL];
                memcpy(cmd, g_flash_cmd, sizeof cmd);
                memcpy(label, g_flash_label, sizeof label);
                g_flash_idx = -1;
                g_flash_cmd[0] = '\0';
                if (rip_send_command(cmd))
                    rip_status("Sent \"%s\"%s%s", cmd,
                               label[0] ? " for " : "", label);
            }
            g_flash_idx = -1;
            return 0;
        }
        if (wParam == RIP_NOTICE_TIMER_ID) {
            KillTimer(hwnd, RIP_NOTICE_TIMER_ID);
            g_notice[0] = '\0';
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_RIP_NOTIFY:
        switch (wParam) {
        case TELNET_EVT_CONNECTED: rip_on_connected();                     return 0;
        case TELNET_EVT_DATA:      rip_on_data((TelnetDataChunk *)lParam); return 0;
        case TELNET_EVT_ERROR:     rip_on_error((const char *)lParam);     return 0;
        case TELNET_EVT_CLOSED:    rip_on_closed();                        return 0;
        }
        return 0;

    case WM_ZMR_NOTIFY:
        switch (wParam) {
        case ZMR_EVT_PROGRESS: rip_on_zmr_progress((ZmrProgress *)lParam); return 0;
        case ZMR_EVT_DONE:     rip_on_zmr_done();                          return 0;
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK RipPanelProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC  hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_hBrushBlack);
        return 1;
    }
    /* The tab keeps the Suite's white-on-black retro chrome. */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wParam, RGB(255, 255, 255));
        SetBkColor((HDC)wParam, RGB(0, 0, 0));
        return (LRESULT)g_hBrushBlack;

    case WM_COMMAND:
        if (ripscrip_module_on_command(hwnd, wParam, lParam)) return 0;
        break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void rip_register_classes(HINSTANCE hInst)
{
    WNDCLASSEXA wc;

    if (!g_hBrushCanvasBk) g_hBrushCanvasBk = CreateSolidBrush(RGB(0, 0, 0));

    if (!g_panel_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = RipPanelProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = g_hBrushBlack;
        wc.lpszClassName = RIP_PANEL_CLASS;
        RegisterClassExA(&wc);
        g_panel_class_reg = TRUE;
    }
    if (!g_canvas_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = RipCanvasProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = NULL;      /* the canvas paints its own */
        wc.lpszClassName = RIP_CANVAS_CLASS;
        RegisterClassExA(&wc);
        g_canvas_class_reg = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

void ripscrip_module_resize(HWND content, int w, int h)
{
    const int margin  = 16;
    const int row_h   = 26;
    const int btn_h   = 30;
    int canvas_y;

    (void)content;
    if (!g_controls_created || !g_hPanel) return;

    MoveWindow(g_hPanel, 0, 0, w, h, TRUE);

    /* Connect strip, laid out like the IRC and WAIS bars: label, field,
     * label, field, with Connect and Disconnect anchored right. */
    MoveWindow(g_hHostLbl,  margin,       18, 40,  22,    TRUE);
    MoveWindow(g_hHostEdit, margin + 44,  14, 240, row_h, TRUE);
    MoveWindow(g_hPortLbl,  margin + 300, 18, 35,  22,    TRUE);
    MoveWindow(g_hPortEdit, margin + 338, 14, 55,  row_h, TRUE);
    {
        const int conn_w   = 85;
        const int disc_w   = 95;
        const int conn_gap = 8;
        int disc_x = w - margin - disc_w;
        int conn_x = disc_x - conn_gap - conn_w;
        MoveWindow(g_hConnBtn, conn_x, 12, conn_w, btn_h, TRUE);
        MoveWindow(g_hDiscBtn, disc_x, 12, disc_w, btn_h, TRUE);
    }

    canvas_y = 52;
    MoveWindow(g_hCanvas, margin, canvas_y,
               w - 2 * margin, h - canvas_y - margin, TRUE);
}

void ripscrip_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    /* Re-activation preserves the session and the rendered scene: the
     * panel is hidden on deactivate, never destroyed. */
    if (g_controls_created) {
        ShowWindow(g_hPanel, SW_SHOW);
        GetClientRect(content, &rc);
        ripscrip_module_resize(content, rc.right - rc.left,
                               rc.bottom - rc.top);
        if (!g_connected) SetFocus(g_hHostEdit);
        return;
    }

    rip_register_classes(hInst);

    GetClientRect(content, &rc);
    /* No WS_EX_CONTROLPARENT: the same IsDialogMessage hang the Archie
     * tab hit applies here, since the shared content panel is not a
     * control parent either. */
    g_hPanel = CreateWindowExA(0, RIP_PANEL_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        content, NULL, hInst, NULL);

    g_hHostLbl = CreateWindowA("STATIC", "Host:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 40, 22, g_hPanel, NULL, hInst, NULL);
    g_hHostEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        RIPSCRIP_DEFAULT_HOST,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 240, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_RIP_HOST_EDIT, hInst, NULL);
    g_hPortLbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 35, 22, g_hPanel, NULL, hInst, NULL);
    g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        RIPSCRIP_DEFAULT_PORT_STR,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 55, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_RIP_PORT_EDIT, hInst, NULL);
    g_hConnBtn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 85, 30, g_hPanel,
        (HMENU)(INT_PTR)IDC_RIP_CONNECT_BTN, hInst, NULL);
    g_hDiscBtn = CreateWindowA("BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 95, 30, g_hPanel,
        (HMENU)(INT_PTR)IDC_RIP_DISCONN_BTN, hInst, NULL);

    g_hCanvas = CreateWindowExA(WS_EX_CLIENTEDGE, RIP_CANVAS_CLASS, "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, g_hPanel,
        (HMENU)(INT_PTR)IDC_RIP_CANVAS, hInst, NULL);

    if (g_hFontUI) {
        HWND all[] = { g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit,
                       g_hConnBtn, g_hDiscBtn };
        int k;
        for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
            SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    rip_parser_init(&g_parser, &g_surf);
    rip_dib_refresh();

    g_controls_created = TRUE;
    rip_set_session_ui(FALSE);

    GetClientRect(content, &rc);
    ripscrip_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
    SetFocus(g_hHostEdit);
}

void ripscrip_module_deactivate(HWND content)
{
    (void)content;
    /* Hide, never destroy: the session and the scene survive a tab
     * switch, matching every other Suite module. */
    if (g_hPanel) ShowWindow(g_hPanel, SW_HIDE);
}

BOOL ripscrip_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam);
    (void)content;
    (void)lParam;

    switch (id) {
    case IDC_RIP_CONNECT_BTN:
        rip_do_connect();
        return TRUE;
    case IDC_RIP_DISCONN_BTN:
        if (g_telnet || g_connected) {
            rip_close_session();
            rip_status("Disconnected from %s:%d.",
                       g_active_host, g_active_port);
            InvalidateRect(g_hCanvas, NULL, FALSE);
        }
        return TRUE;
    }
    return FALSE;
}

BOOL ripscrip_module_has_unsaved(void)
{
    return FALSE;
}

void ripscrip_module_shutdown(void)
{
    if (g_zmodem_active || g_zmr) rip_zmodem_abandon();
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    g_connected = FALSE;
    if (g_capture) { fclose(g_capture); g_capture = NULL; }
    rip_dib_destroy();
    if (g_hBrushCanvasBk) {
        DeleteObject(g_hBrushCanvasBk);
        g_hBrushCanvasBk = NULL;
    }
}
