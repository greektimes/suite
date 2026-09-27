/*
 * naplps_module.c - the Videotex tab: NAPLPS and Minitel.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The tab is a private container (MGTNapPanel) holding a strip of
 * controls over a render canvas (MGTNapCanvasV1). Everything below the
 * wire is in naplps_decode.[ch] and naplps_surface.[ch], which are
 * clean-room from ANSI X3.110-1983 / CSA T500-1983 and contain no Win32
 * code; the full provenance statement lives at the top of
 * naplps_decode.h.
 *
 * TRANSPORT. The tab reuses telnet_proto.[ch], exactly as the RIPscrip
 * and Terminal tabs do, rather than opening a second socket layer. That
 * is safe here for a reason worth stating: NAPLPS is a SEVEN-BIT format.
 * Opcodes are 0x20-0x3F, operands 0x40-0x7F and text is ASCII, so the
 * byte 0xFF that telnet uses as IAC cannot occur in valid NAPLPS data.
 * The IAC filter is therefore a pass-through on a NAPLPS stream, and in
 * exchange the tab gets RFC 854 option negotiation for free if the far
 * end happens to be a telnet service or a serial-to-TCP bridge that
 * negotiates on its own behalf. A frame that arrives 8-bit is a local
 * file, not a wire stream, and files do not go through this path.
 *
 * THERE IS NO HANDSHAKE, and that is a real difference from the RIPscrip
 * tab next door. RIPscrip 1.54 defines an auto-sensing query and a
 * version answer; NAPLPS defines no terminal identification, no
 * capability query and no status report, and the transmission-control
 * characters are reserved to lower layers and ignored at the
 * presentation layer (6.1.4). So there is nothing to send on connect and
 * nothing to wait for. A caller declares its capability by choosing the
 * port.
 *
 * TWO FORMATS, ONE TAB (operator decision, 2026-09-27). The tab is
 * labelled "Videotex" and carries a Format dropdown, NAPLPS or Minitel,
 * which can only be changed while disconnected. Both formats use the same
 * host (videotex.greektimes.ca), the same single telnet_proto session and
 * the same status line; the port follows the format (1983 NAPLPS, 516
 * Minitel). Minitel mode is a complete terminal, screen and keyboard,
 * built by minitel_view.[ch] over the clean-room minitel_decode.[ch] and
 * minitel_screen.[ch]. The NAPLPS paths below are the Phase 2a code,
 * unchanged: the Minitel paths branch off before they reach them.
 *
 * KEYBOARD. Upstream keys follow the ALEXtel Subset profile documented in
 * README.PP3 section 7.2: printable ASCII, CR, BS, DEL, TAB and ESC go up
 * as themselves, and F1 to F9 send the Subset default sequences
 * ESC # <c> CR. Characters are derived from the virtual key rather than
 * from WM_CHAR, because WM_CHAR is layout-translated and a non-Latin
 * keyboard layout would otherwise produce something other than ASCII.
 * That lesson is recorded in the F6 Telnet work and applies unchanged.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

#include "naplps_module.h"
#include "suite_shell.h"
#include "telnet_proto.h"
#include "naplps_decode.h"
#include "naplps_surface.h"
#include "minitel_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-local control IDs (6600+, clear of the RIPscrip 6400 block). */
#define IDC_NAP_HOST_EDIT   6601
#define IDC_NAP_PORT_EDIT   6602
#define IDC_NAP_CONNECT_BTN 6603
#define IDC_NAP_DISCONN_BTN 6604
/* 6605 was the Open frame / Open page button, removed 2026-09-27: local
 * files are for testing only and open with Ctrl+O on the screen. */
#define IDC_NAP_MONO_CHK    6606
#define IDC_NAP_CANVAS      6607
#define IDC_NAP_FORMAT_CMB  6608
#define IDC_NAP_SCAN_CHK    6609

/* The two formats of the Format dropdown, in its order. */
#define FMT_NAPLPS   0
#define FMT_MINITEL  1

#define NAP_PANEL_CLASS   "MGTNapPanel"
#define NAP_CANVAS_CLASS  "MGTNapCanvasV1"

#define WM_NAP_NOTIFY  (WM_USER + 43)

/* The WAIT control's timer. 5.3.2.8 gives the interval in tenths. */
#define IDT_NAP_WAIT   1

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HWND g_hContent  = NULL;
static HWND g_hPanel    = NULL;
static HWND g_hCanvas   = NULL;
static HWND g_hHostLbl  = NULL, g_hHostEdit = NULL;
static HWND g_hPortLbl  = NULL, g_hPortEdit = NULL;
static HWND g_hConnBtn  = NULL, g_hDiscBtn  = NULL;
static HWND g_hMonoChk  = NULL;
static HWND g_hStatus   = NULL;
static HWND g_hFmtLbl   = NULL, g_hFmtCmb   = NULL;
static HWND g_hScanChk  = NULL;

/* MINITEL IS THE DEFAULT FORMAT (operator, 2026-09-27); NAPLPS stays one
 * click away in the dropdown. */
static int  g_format         = FMT_MINITEL; /* the dropdown           */
static int  g_session_format = FMT_MINITEL; /* the format of the live
                                               or last session         */
static BOOL g_mono_mtl       = TRUE;        /* Minitel: monochrome by
                                               default, the 1B's tube  */
static BOOL g_scan_mtl       = TRUE;        /* Minitel: scan lines by
                                               default (operator)      */

static BOOL g_controls_created = FALSE;
static BOOL g_panel_class_reg  = FALSE;
static BOOL g_canvas_class_reg = FALSE;

static TelnetProto *g_telnet    = NULL;
static BOOL         g_connected = FALSE;
static char         g_active_host[256] = NAPLPS_DEFAULT_HOST;
static int          g_active_port      = NAPLPS_DEFAULT_PORT;

static naplps_decoder_t *g_dec = NULL;
static BOOL              g_mono = FALSE;
static char              g_loaded_name[MAX_PATH] = "";

static HBITMAP g_hDib     = NULL;
static void   *g_dib_bits = NULL;
static HDC     g_hMemDC   = NULL;
static HBITMAP g_hOldDib  = NULL;

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

static void nap_status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = '\0';
    va_end(ap);
    /* ONE status line (operator, 2026-09-27): the tab's own, which also
     * carries the counters. It is no longer mirrored into the window's
     * status bar, which the tab blanks while it is shown, so nothing is
     * ever printed twice. */
    if (g_hStatus) SetWindowTextA(g_hStatus, buf);
}

/*
 * The counter line.
 *
 * This exists because of the counter-honesty rule the project adopted
 * during the RIPscrip work: a feature REGISTERED is not a feature
 * RENDERED. The decoder deliberately skips macros, DRCS, mosaics,
 * unprotected fields, BLINK and the INCREMENTAL encodings, and it would
 * be easy for a frame that leans on one of them to look merely sparse
 * rather than incompletely drawn. Putting the skip count on screen means
 * the operator can tell those two apart without reading a log.
 */
static void nap_update_counts(void)
{
    const naplps_stats_t *s;
    char buf[256];

    if (!g_dec || !g_hStatus) return;
    s = naplps_get_stats(g_dec);

    _snprintf(buf, sizeof buf - 1,
              "%lu bytes  %lu drawn  %lu chars  %lu skipped  "
              "%lu esc?  %lu esc-bad  %lu short%s",
              s->bytes_in, s->pdi_executed, s->chars_drawn, s->pdi_skipped,
              s->esc_unknown, s->esc_malformed, s->operand_short,
              naplps_get_mode(g_dec) == NAPLPS_MODE_ASCII ? "  [ASCII mode]" : "");
    buf[sizeof buf - 1] = '\0';
    SetWindowTextA(g_hStatus, buf);
}

/* ------------------------------------------------------------------ */
/* DIB                                                                 */
/* ------------------------------------------------------------------ */

static void nap_dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_hDib) return;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = NAPLPS_W;
    bi.bmiHeader.biHeight      = -NAPLPS_H;    /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_dib_bits,
                              NULL, 0);
    if (g_hDib) {
        g_hMemDC  = CreateCompatibleDC(screen);
        g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
    }
    ReleaseDC(NULL, screen);
}

static void nap_dib_refresh(void)
{
    if (!g_dib_bits || !g_dec) return;
    naplps_surface_to_bgra(naplps_get_surface(g_dec),
                           (unsigned char *)g_dib_bits, g_mono ? 1 : 0);
}

/*
 * Where the 256x200 surface lands in the canvas.
 *
 * INTEGER SCALING, like the NABU tab and unlike the RIPscrip tab, and the
 * reason is in the raster rather than in taste. The videotex SRM puts the
 * visible unit screen at X 0 to 1 and Y 0 to 0.78125 over 256 by 200
 * pixels, and 200/256 is exactly 0.78125, so one pixel is 1/256 of the
 * unit screen on BOTH axes: the SRM's pixels are square. An integer scale
 * therefore preserves the specified geometry exactly and keeps the 5x7
 * glyphs crisp. A fractional scale would give uneven pixel blocks and
 * make 40-column text wobble, which is the same finding the NABU integer
 * aspect fit was written for.
 *
 * The RIPscrip tab does the opposite because its raster is different:
 * 640x350 is NOT square-pixel, it was displayed on a 4:3 monitor, so that
 * tab has to declare a presentation aspect and stretch to it.
 */
static void nap_dest_rect(int cw, int ch, RECT *out)
{
    int scale, w, h;

    scale = cw / NAPLPS_W;
    if (ch / NAPLPS_H < scale) scale = ch / NAPLPS_H;
    if (scale < 1) scale = 1;

    w = NAPLPS_W * scale;
    h = NAPLPS_H * scale;
    out->left   = (cw - w) / 2;
    out->top    = (ch - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static void nap_set_session_ui(BOOL connected)
{
    HWND had_focus;

    g_connected = connected;
    if (!g_controls_created) return;
    had_focus = GetFocus();
    EnableWindow(g_hConnBtn, !connected);
    EnableWindow(g_hDiscBtn, connected);
    EnableWindow(g_hHostEdit, !connected);
    EnableWindow(g_hPortEdit, !connected);
    /* The format can only change while disconnected. */
    EnableWindow(g_hFmtCmb, !connected);
    if (g_session_format == FMT_MINITEL) minitel_view_set_connected(connected);

    /*
     * THE DISCONNECT FOCUS FIX (found in the 2026-09-27 live gate). A
     * click on Disconnect gives that button the keyboard focus, and the
     * line above then disables it, which leaves NO window with the focus:
     * every key went nowhere until the screen was clicked. Give the focus
     * back to the screen of the current format instead.
     */
    if (!connected && (had_focus == g_hDiscBtn || GetFocus() == NULL)) {
        if (g_format == FMT_MINITEL) minitel_view_focus();
        else if (g_hCanvas) SetFocus(g_hCanvas);
    }
}

/*
 * Throw away transport events still queued for the panel. After
 * telnet_proto_close the worker thread has been joined, so nothing new
 * can arrive, but what it posted before it stopped is still in the
 * queue. Left there, a stale TELNET_EVT_CLOSED made a LOCAL disconnect
 * end on "Host closed the connection" (the 2026-09-27 quirk), and could
 * have closed a session opened again straight afterwards. Data chunks
 * are freed; the error text is static.
 */
static void nap_drain_events(void)
{
    MSG m;
    if (!g_hPanel) return;
    while (PeekMessageA(&m, g_hPanel, WM_NAP_NOTIFY, WM_NAP_NOTIFY, PM_REMOVE)) {
        if (m.wParam == TELNET_EVT_DATA && m.lParam)
            telnet_proto_free_chunk((TelnetDataChunk *)m.lParam);
    }
}

static void nap_close_session(void)
{
    if (g_telnet) {
        telnet_proto_close(g_telnet);
        g_telnet = NULL;
        nap_drain_events();
    }
    if (g_connected) {
        /* A PDI sequence is terminated by the next opcode or by a
         * non-numeric byte (5.3.1.1), so the last command of a session
         * has nothing to terminate it. Flush it rather than leaving the
         * final shape undrawn. */
        if (g_session_format == FMT_NAPLPS) {
            if (g_dec) { naplps_flush(g_dec); nap_dib_refresh(); }
            if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        }
    }
    nap_set_session_ui(FALSE);
}

/* When the user last sent something, and whether the host has drawn
 * anything since: the test for "the server's own goodbye was the last
 * thing drawn". */
static DWORD g_last_rx_tick = 0;
static BOOL  g_rx_since_tx  = FALSE;

static void nap_send(const void *buf, int len)
{
    if (g_telnet && g_connected && len > 0) {
        telnet_proto_send(g_telnet, buf, (size_t)len);
        g_rx_since_tx = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Logoff (operator, 2026-09-27)                                       */
/* ------------------------------------------------------------------ */

/*
 * On every disconnect, local or by the host, the screen says so, and 5 s
 * later the format's default screen comes back:
 *
 *   - if the HOST closed the line and the last thing drawn was the
 *     host's own (bytes arrived after the user's last key, within 3 s of
 *     the close), that is the service's goodbye, and it is left alone;
 *   - otherwise (Disconnect, F10, CONNEXION/FIN twice, or a host that
 *     closed without a word) the tab draws its own logoff message, in
 *     the format's own style: NAPLPS in its text presentation (the one
 *     the service's banner uses), Minitel in videotex.
 *
 * Switching format or connecting again cancels the 5 s timer.
 */
#define IDT_NAP_LOGOFF   2
#define NAP_LOGOFF_MS    5000
#define NAP_GOODBYE_MS   3000

#define WHY_LOCAL 0
#define WHY_HOST  1

static void nap_cancel_logoff(void)
{
    if (g_hPanel) KillTimer(g_hPanel, IDT_NAP_LOGOFF);
}

/* The default (idle) screen of a format: power-on state, which brings
 * back the format's own guidance on the canvas. */
static void nap_default_screen(int fmt)
{
    if (fmt == FMT_MINITEL) {
        minitel_view_default_screen();
    } else {
        if (g_dec) naplps_reset(g_dec);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

static void nap_draw_logoff(int fmt)
{
    if (fmt == FMT_MINITEL) {
        minitel_view_show_logoff(g_active_host);
    } else if (g_dec) {
        /* ESC % @ after a reset begins a fresh text presentation (the
         * G7 fix C), the same one the service's ASCII banner is drawn in.
         * NAPLPS text is proportional, so the centring is approximate. */
        char buf[512];
        int n, pad_a, pad_b;
        size_t hl = strlen(g_active_host);
        pad_a = (40 - 17) / 2;                        /* "Disconnected from" */
        pad_b = hl < 40 ? (int)(40 - hl) / 2 : 0;
        n = _snprintf(buf, sizeof buf - 1,
                      "\x1B%%@\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n"
                      "%*sDisconnected from\r\n\r\n%*s%.40s",
                      pad_a, "", pad_b, "", g_active_host);
        if (n < 0) n = (int)sizeof buf - 1;
        naplps_reset(g_dec);
        naplps_feed(g_dec, (const unsigned char *)buf, (size_t)n);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

/* End a session, locally or because the host did, then the logoff. */
static void nap_close_session(void);
static void nap_end_session(int why)
{
    BOOL was_connected = g_connected;
    BOOL goodbye = (why == WHY_HOST) && g_rx_since_tx
                   && (GetTickCount() - g_last_rx_tick) <= NAP_GOODBYE_MS;

    nap_close_session();
    if (!was_connected) return;          /* a failed connect: no logoff */
    if (!goodbye) nap_draw_logoff(g_session_format);
    if (g_hPanel) SetTimer(g_hPanel, IDT_NAP_LOGOFF, NAP_LOGOFF_MS, NULL);
}

/* ------------------------------------------------------------------ */
/* Upstream keys                                                       */
/* ------------------------------------------------------------------ */

/*
 * The ALEXtel Subset function keys, README.PP3 section 7.2.
 *
 * The table there gives, for each key, a Subset MACRO NAME and a DEFAULT
 * SEQUENCE, with the rule "In the subset handshake, if the macro is
 * undefined, the default sequence is transmitted." The host may define
 * transmit-macros (DEFT MACRO, 6.2.2.3) at macro names 2/0 to 2/7 and
 * thereby choose what each key sends; if it defines none, the terminal
 * falls back to these fixed sequences. This tab defines nothing and
 * sends the defaults, which is what a service should expect.
 *
 * THE LABELS ARE BELL'S, NOT PP3's. The ALEX Magazine user guide of June
 * 1990, page VII, lists the terminal's eight function keys as Cancel,
 * Top, Previous, Help, Refresh, Menu, Next and Send, in exactly the order
 * of the Subset macro names 2/0 through 2/7. Six match README.PP3's
 * labels outright; the two that differ, BACK/Previous and
 * REDISPLAY/Refresh, carry identical descriptions. Bell's own words for
 * its own terminal are the better-sourced labels, so those are the ones
 * shown to the operator.
 *
 * F9 EXIT has no counterpart on the hardware: the magazine says "The
 * eight function keys" and lists eight. Leaving ALEX was done with the
 * connect/disconnect key. The sequence is still sent, because a host that
 * understands the Subset defaults understands the ninth, but it is
 * labelled as PP3 and VIA rather than as an ALEXTEL key.
 */
typedef struct { int vk; const char *seq; const char *label; } napkey_t;

static const napkey_t g_fkeys[] = {
    { VK_F1, "\x1B#4\r", "F1 Help"     },   /* macro 2/3 */
    { VK_F2, "\x1B#3\r", "F2 Refresh"  },   /* macro 2/4 */
    { VK_F3, "\x1B#?\r", "F3 Cancel"   },   /* macro 2/0 */
    { VK_F4, "\x1B#6\r", "F4 Top"      },   /* macro 2/1 */
    { VK_F5, "\x1B#2\r", "F5 Previous" },   /* macro 2/2 */
    { VK_F6, "\x1B#:\r", "F6 Menu"     },   /* macro 2/5 */
    { VK_F7, "\x1B#8\r", "F7 Next"     },   /* macro 2/6 */
    { VK_F8, "\x1B#1\r", "F8 Send"     },   /* macro 2/7 */
    { VK_F9, "\x1B#9\r", "F9 Exit (PP3/VIA, not an ALEXTEL key)" }
};
#define NAP_FKEY_COUNT ((int)(sizeof g_fkeys / sizeof g_fkeys[0]))

/*
 * Derive an ASCII byte from a virtual key and the shift state.
 *
 * WM_CHAR is layout-translated, so on a non-Latin layout it delivers
 * something that is not ASCII and the host sees mojibake. The signature
 * of getting this wrong is "numbers work, letters do not". Deriving from
 * the VK keeps the upstream 7-bit whatever the operator types on.
 */
static int nap_vk_to_ascii(int vk, BOOL shift)
{
    static const char *digits_shift = ")!@#$%^&*(";

    if (vk >= 'A' && vk <= 'Z') return shift ? vk : (vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9')
        return shift ? digits_shift[vk - '0'] : vk;

    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_1:      return shift ? ':'  : ';';
    case VK_OEM_PLUS:   return shift ? '+'  : '=';
    case VK_OEM_COMMA:  return shift ? '<'  : ',';
    case VK_OEM_MINUS:  return shift ? '_'  : '-';
    case VK_OEM_PERIOD: return shift ? '>'  : '.';
    case VK_OEM_2:      return shift ? '?'  : '/';
    case VK_OEM_3:      return shift ? '~'  : '`';
    case VK_OEM_4:      return shift ? '{'  : '[';
    case VK_OEM_5:      return shift ? '|'  : '\\';
    case VK_OEM_6:      return shift ? '}'  : ']';
    case VK_OEM_7:      return shift ? '"'  : '\'';
    default: break;
    }
    return -1;
}

static void nap_key_down(int vk)
{
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int  i, c;
    char ch;

    if (!g_connected) return;

    for (i = 0; i < NAP_FKEY_COUNT; i++)
        if (g_fkeys[i].vk == vk) {
            nap_send(g_fkeys[i].seq, (int)strlen(g_fkeys[i].seq));
            nap_status("Sent %s", g_fkeys[i].label);
            return;
        }

    /*
     * F10 BREAK, and what it can and cannot mean over TCP.
     *
     * README.PP3 6.2 says "the key F10-BREAK is the same operation as the
     * phone key on an ALEXtel terminal", and its own table gives F10 as a
     * "250 ms space (break/attention) condition". The ALEX Magazine, page
     * VIII, says what that key does for the user: connect/disconnect, and
     * pressed twice it leaves the service and then ALEX.
     *
     * So on real hardware F10 is not a character at all. It is a
     * LINE-LEVEL signal: a break is a sustained space on the serial line,
     * below the data stream entirely, and the phone key physically
     * originates or terminates the call. Over TCP there is no line to
     * hold in a space condition and no call to drop. The only thing in
     * the neighbourhood is telnet's own IAC BRK, which exists but travels
     * as data on a connection the far end may not be treating as telnet.
     *
     * DECISION, recorded rather than guessed at: F10 is bound to the
     * user-visible behaviour the Bell guide documents, which is
     * disconnect. It does not attempt to synthesise a break. If Phase 2b's
     * service ever wants an attention signal, that is a protocol decision
     * for the service, not something this tab can invent.
     */
    if (vk == VK_F10) { nap_end_session(WHY_LOCAL); nap_status("Disconnected (F10)"); return; }

    switch (vk) {
    case VK_RETURN: ch = '\r';   nap_send(&ch, 1); return;
    case VK_BACK:   ch = '\b';   nap_send(&ch, 1); return;
    case VK_TAB:    ch = '\t';   nap_send(&ch, 1); return;
    case VK_ESCAPE: ch = '\x1B'; nap_send(&ch, 1); return;
    case VK_DELETE: ch = '\x7F'; nap_send(&ch, 1); return;
    default: break;
    }

    c = nap_vk_to_ascii(vk, shift);
    if (c > 0) { ch = (char)c; nap_send(&ch, 1); }
}

/* ------------------------------------------------------------------ */
/* Incoming data                                                       */
/* ------------------------------------------------------------------ */

static void nap_on_connected(void)
{
    nap_set_session_ui(TRUE);
    /*
     * Nothing is sent on connect. There is no NAPLPS handshake to answer
     * and no terminal identification to volunteer (4.8 of the Phase 1
     * report, and 6.1.4 of the standard, which reserves the transmission
     * control characters to lower layers). The host talks first.
     */
    nap_status("Connected to %s:%d", g_active_host, g_active_port);
    if (g_session_format == FMT_MINITEL) minitel_view_focus();
    else SetFocus(g_hCanvas);
}

static void nap_on_data(TelnetDataChunk *chunk)
{
    int tenths;

    if (!chunk) return;
    if (chunk->data && chunk->len > 0) {
        g_last_rx_tick = GetTickCount();     /* the host drew something */
        g_rx_since_tx  = TRUE;
    }
    if (g_session_format == FMT_MINITEL) {
        if (chunk->data && chunk->len > 0)
            minitel_view_feed(chunk->data, (size_t)chunk->len);
        telnet_proto_free_chunk(chunk);
        return;
    }
    if (g_dec && chunk->data && chunk->len > 0) {
        naplps_feed(g_dec, chunk->data, (size_t)chunk->len);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        nap_update_counts();

        /*
         * Honour WAIT, 5.3.2.8. Phase 1 section 5.2 records why this
         * matters more here than it looks: the host cannot know when a
         * frame has finished drawing, there is no acknowledgement and no
         * completion signal, so WAIT is the format's own and only answer
         * to "pause before the next thing". Executing it is the
         * terminal's job.
         */
        tenths = naplps_take_wait(g_dec);
        if (tenths > 0 && g_hCanvas) {
            UpdateWindow(g_hCanvas);
            SetTimer(g_hCanvas, IDT_NAP_WAIT, (UINT)(tenths * 100), NULL);
        }
    }
    telnet_proto_free_chunk(chunk);
}

static void nap_on_error(const char *msg)
{
    nap_status("Connection failed: %s", msg ? msg : "unknown error");
    nap_end_session(WHY_HOST);
}

static void nap_on_closed(void)
{
    /* Only a real host close gets here now: a local close drains the
     * transport's leftover events (nap_drain_events). */
    nap_end_session(WHY_HOST);
    nap_status("Host closed the connection");
}

/* ------------------------------------------------------------------ */
/* Open a local file                                                   */
/* ------------------------------------------------------------------ */

static void nap_open_file(void)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "";
    FILE *f;
    unsigned char *buf;
    long len;
    size_t got;

    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize  = sizeof ofn;
    ofn.hwndOwner    = GetAncestor(g_hPanel, GA_ROOT);
    ofn.lpstrFilter  = "NAPLPS frames (*.nap;*.naplps)\0*.nap;*.naplps\0"
                       "All files (*.*)\0*.*\0";
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = sizeof path;
    ofn.lpstrTitle   = "Open a NAPLPS frame";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) return;

    f = fopen(path, "rb");
    if (!f) { nap_status("Cannot open %s", path); return; }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (16L << 20)) {
        fclose(f);
        nap_status("%s is empty or implausibly large", path);
        return;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); nap_status("Out of memory"); return; }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);

    /*
     * A file is a fresh presentation, so the decoder starts from the
     * power-on default state rather than inheriting whatever the previous
     * frame left set. This matters: DOMAIN lengths, the colour map, the
     * character field and the active field all persist until something
     * changes them, and a frame that relies on the defaults would decode
     * against the last frame's state instead.
     */
    naplps_reset(g_dec);
    naplps_feed(g_dec, buf, got);
    naplps_flush(g_dec);
    free(buf);

    nap_dib_refresh();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);

    {
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        _snprintf(g_loaded_name, sizeof g_loaded_name - 1, "%s", base);
        g_loaded_name[sizeof g_loaded_name - 1] = '\0';
    }
    nap_update_counts();
}

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

static void nap_canvas_paint(HWND hwnd, HDC hdc)
{
    RECT rc, dst;
    HDC  mem;
    HBITMAP bmp, oldbmp;
    int cw, ch;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    /* Composite off-screen and blit once: the letterbox fill and the
     * scaled surface must not appear as two separate paints. */
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, g_hBrushBlack);

    if (g_dec && naplps_ever_drew(g_dec) && g_hMemDC) {
        nap_dest_rect(cw, ch, &dst);
        /* COLORONCOLOR is GDI's nearest neighbour: no smoothing, no
         * averaging, so scaled videotex pixels stay hard-edged blocks
         * and the 5x7 glyphs stay readable. */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, dst.left, dst.top,
                   dst.right - dst.left, dst.bottom - dst.top,
                   g_hMemDC, 0, 0, NAPLPS_W, NAPLPS_H, SRCCOPY);
    } else {
        /*
         * The function key legend uses BELL'S OWN LABELS for the ALEXTEL,
         * from the ALEX Magazine user guide of June 1990, page VII:
         * Cancel, Top, Previous, Help, Refresh, Menu, Next, Send, printed
         * there in exactly the order of the Subset macro names 2/0 to
         * 2/7. Six of the eight match README.PP3's labels outright; the
         * two that differ, BACK/Previous and REDISPLAY/Refresh, carry
         * identical descriptions in both documents, and Bell's words for
         * Bell's own terminal are the better-sourced ones.
         *
         * F9 is shown apart because there is no ninth key on the
         * hardware. The magazine says "The eight function keys" and lists
         * eight; F9 EXIT belongs to Personality+III and to the 1991 VIA
         * recommendation it follows, not to the ALEXTEL.
         */
        static const char *hint[] = {
            "NAPLPS (videotex)",
            "",
            "Connect to a host (Ctrl+O opens a local .nap frame).",
            "",
            "F1 Help     F2 Refresh   F3 Cancel   F4 Top",
            "F5 Previous   F6 Menu   F7 Next   F8 Send",
            "F10 Disconnect",
            "",
            "(F9 Exit is a Personality+III and VIA extension,",
            "not a key the ALEXTEL had.)"
        };
        const int nlines = (int)(sizeof hint / sizeof hint[0]);
        int lh, k, y;
        SIZE ext;

        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        GetTextExtentPoint32A(mem, "Wg", 2, &ext);
        lh = ext.cy + 2;
        y  = (ch - nlines * lh) / 2;
        for (k = 0; k < nlines; k++) {
            RECT lr;
            lr.left = 0; lr.right = cw;
            lr.top = y + k * lh; lr.bottom = lr.top + lh;
            DrawTextA(mem, hint[k], -1, &lr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK NapCanvasProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                       /* the paint owns the whole rect */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        nap_canvas_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_NAP_WAIT) { KillTimer(hwnd, IDT_NAP_WAIT); return 0; }
        break;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;

    /* A custom-painted child must claim the keys or IsDialogMessage eats
     * them before they arrive. Without this the letters never come. */
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

    case WM_KEYDOWN:
        /* Ctrl+O opens a local .nap frame. There is no button for it any
         * more (operator, 2026-09-27): local files are for testing. */
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            nap_cancel_logoff();         /* keep the opened frame */
            nap_open_file();
            return 0;
        }
        nap_key_down((int)wParam);
        return 0;

    /*
     * F10 arrives as WM_SYSKEYDOWN, not WM_KEYDOWN: it is Windows' menu
     * key. So F10 "Disconnect" (the Bell phone key, see nap_key_down)
     * never reached nap_key_down and did nothing, from Phase 2a until the
     * logoff gate of 2026-09-27 found it. Take it here and swallow it, so
     * it does not also open the window menu.
     */
    case WM_SYSKEYDOWN:
        if (wParam == VK_F10) { nap_key_down(VK_F10); return 0; }
        break;

    default: break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK NapPanelProc(HWND hwnd, UINT msg,
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

    /*
     * The controls are children of THIS panel, so their WM_COMMAND
     * arrives here and not at the shared content panel. The content
     * panel does forward WM_COMMAND to the active module, but only for
     * the notifications it receives itself, which are the ones from its
     * own direct children. Leaving this case out gives buttons that
     * draw, highlight on hover and do absolutely nothing, which is the
     * same failure the shell's own tab dispatch has and is just as quiet.
     */
    case WM_COMMAND:
        if (naplps_module_on_command(hwnd, wParam, lParam)) return 0;
        break;

    /* The Minitel function keys are owner-drawn buttons of this panel. */
    case WM_DRAWITEM:
        if (minitel_view_on_drawitem((const DRAWITEMSTRUCT *)lParam)) return TRUE;
        break;

    /* 5 s after a logoff: back to the current format's default screen,
     * unless a session has started since (connect cancels the timer
     * too; this is the second guard). */
    case WM_TIMER:
        if (wParam == IDT_NAP_LOGOFF) {
            KillTimer(hwnd, IDT_NAP_LOGOFF);
            if (!g_telnet && !g_connected) {
                nap_default_screen(g_format);
                nap_status("Back to the %s default screen",
                           g_format == FMT_MINITEL ? "Minitel" : "NAPLPS");
            }
            return 0;
        }
        break;

    case WM_NAP_NOTIFY:
        switch (wParam) {
        case TELNET_EVT_CONNECTED: nap_on_connected();                     return 0;
        case TELNET_EVT_DATA:      nap_on_data((TelnetDataChunk *)lParam); return 0;
        case TELNET_EVT_ERROR:     nap_on_error((const char *)lParam);     return 0;
        case TELNET_EVT_CLOSED:    nap_on_closed();                        return 0;
        default: return 0;
        }

    default: break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void nap_register_classes(HINSTANCE hInst)
{
    WNDCLASSEXA wc;

    if (!g_panel_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.lpfnWndProc   = NapPanelProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = g_hBrushBlack;
        wc.lpszClassName = NAP_PANEL_CLASS;
        RegisterClassExA(&wc);
        g_panel_class_reg = TRUE;
    }
    if (!g_canvas_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        /* Every pixel of the scaled surface depends on the client size,
         * so a grow must invalidate the whole canvas and not just the
         * newly exposed strips. */
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = NapCanvasProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = NAP_CANVAS_CLASS;
        RegisterClassExA(&wc);
        g_canvas_class_reg = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Connecting, and the format                                          */
/* ------------------------------------------------------------------ */

/*
 * One connect path for both formats and for both ways in: the Connect
 * button, and Minitel's CONNEXION/FIN pressed while disconnected. The
 * NAPLPS branch is the Phase 2a code as it was.
 */
static void nap_connect(void)
{
    char host[256], port[16];
    int  p;

    if (g_telnet) return;          /* already connecting or connected */

    GetWindowTextA(g_hHostEdit, host, sizeof host);
    GetWindowTextA(g_hPortEdit, port, sizeof port);
    if (host[0] == '\0') { nap_status("Enter a host name"); return; }
    p = atoi(port);
    if (p <= 0 || p > 65535) { nap_status("Enter a port from 1 to 65535"); return; }

    _snprintf(g_active_host, sizeof g_active_host - 1, "%s", host);
    g_active_host[sizeof g_active_host - 1] = '\0';
    g_active_port = p;
    g_session_format = g_format;

    /* Connecting again inside the 5 s after a logoff cancels its timer,
     * so the new session's screen is never wiped under it. */
    nap_cancel_logoff();
    g_rx_since_tx = FALSE;

    /* A new session is a new presentation: start from the power-on
     * default state so nothing leaks in from the previous one. */
    if (g_session_format == FMT_MINITEL) {
        minitel_view_reset();
    } else {
        if (g_dec) naplps_reset(g_dec);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }

    nap_status("Connecting to %s:%d ...", g_active_host, g_active_port);
    if (telnet_proto_open(g_active_host, g_active_port,
                          g_hPanel, WM_NAP_NOTIFY, &g_telnet) != 0
        || !g_telnet) {
        nap_status("Cannot start a connection to %s:%d",
                   g_active_host, g_active_port);
        nap_set_session_ui(FALSE);
    }
    /* The buttons flip on TELNET_EVT_CONNECTED, not here. */
}

/* Show the screen of the chosen format and set the controls to match. */
static void nap_apply_format(void)
{
    BOOL mtl = (g_format == FMT_MINITEL);

    ShowWindow(g_hCanvas, mtl ? SW_HIDE : SW_SHOW);
    minitel_view_show(mtl);
    /* Minitel has no Connect or Disconnect: CONNEXION/FIN does both, as
     * on the terminal. NAPLPS keeps them as they were. */
    ShowWindow(g_hConnBtn, mtl ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hDiscBtn, mtl ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hScanChk, mtl ? SW_SHOW : SW_HIDE);
    SendMessageA(g_hMonoChk, BM_SETCHECK,
                 (mtl ? g_mono_mtl : g_mono) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_hScanChk, BM_SETCHECK, g_scan_mtl ? BST_CHECKED : BST_UNCHECKED, 0);
    minitel_view_set_mono(g_mono_mtl);
    minitel_view_set_scanlines(g_scan_mtl);
    /* The top row differs by format, so lay the tab out again. */
    if (g_hContent) {
        RECT rc;
        GetClientRect(g_hContent, &rc);
        naplps_module_resize(g_hContent, rc.right - rc.left, rc.bottom - rc.top);
    }
    if (mtl) minitel_view_focus();
    else if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
}

/* What the Minitel view asks of the tab. */
static void mtl_host_send(const unsigned char *buf, size_t len)
{
    if (g_session_format == FMT_MINITEL) nap_send(buf, (int)len);
}

static void mtl_host_connect(void)
{
    nap_connect();
}

static void mtl_host_disconnect(void)
{
    nap_end_session(WHY_LOCAL);
    nap_status("Disconnected (CONNEXION/FIN twice)");
}

static void mtl_host_status(const char *text)
{
    if (g_hStatus) SetWindowTextA(g_hStatus, text);
}

static void mtl_host_file_opened(void)
{
    nap_cancel_logoff();                 /* keep the opened page */
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

void naplps_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int row_h  = 26;
    const int btn_h  = 30;
    int canvas_y, status_h = 20;

    (void)content;
    if (!g_controls_created || !g_hPanel) return;

    MoveWindow(g_hPanel, 0, 0, w, h, TRUE);

    /* Format first, then the address it will use. Minitel mode narrows
     * the Host field by 30 pixels to make room for its extra controls, so
     * the row fits at the Suite's default 980-pixel window. */
    {
        const int host_w = 200;
        const int port_lx = margin + 217 + host_w;
        MoveWindow(g_hFmtLbl,   margin,       18, 55,     22,    TRUE);
        MoveWindow(g_hFmtCmb,   margin + 58,  14, 95,     200,   TRUE);
        MoveWindow(g_hHostLbl,  margin + 163, 18, 40,     22,    TRUE);
        MoveWindow(g_hHostEdit, margin + 207, 14, host_w, row_h, TRUE);
        MoveWindow(g_hPortLbl,  port_lx,      18, 35,     22,    TRUE);
        MoveWindow(g_hPortEdit, port_lx + 38, 14, 55,     row_h, TRUE);
    }
    if (g_format == FMT_MINITEL) {
        /* Monochrome, Scanlines, and the long brown CONNEXION/FIN key
         * where Connect and Disconnect sit for NAPLPS. */
        const int cnx_w = 128, scan_w = 88, mono_w = 100;
        const int gap = 8;
        int cnx_x  = w - margin - cnx_w;
        int scan_x = cnx_x - gap - scan_w;
        int mono_x = scan_x - gap - mono_w;
        if (mono_x < margin + 519) mono_x = margin + 519;
        MoveWindow(g_hMonoChk, mono_x, 16, mono_w, 22,    TRUE);
        MoveWindow(g_hScanChk, scan_x, 16, scan_w, 22,    TRUE);
        minitel_view_place_cnx(cnx_x, 12, cnx_w, btn_h);
    } else {
        /* NAPLPS: the Phase 2a row, with the checkbox label shortened to
         * "Monochrome" so it is never clipped. */
        const int conn_w = 85, disc_w = 95, mono_w = 105;
        const int gap = 8;
        int disc_x = w - margin - disc_w;
        int conn_x = disc_x - gap - conn_w;
        int mono_x = conn_x - gap - mono_w;
        if (mono_x < margin + 519) mono_x = margin + 519;
        MoveWindow(g_hMonoChk, mono_x, 16, mono_w, 22,    TRUE);
        MoveWindow(g_hConnBtn, conn_x, 12, conn_w, btn_h, TRUE);
        MoveWindow(g_hDiscBtn, disc_x, 12, disc_w, btn_h, TRUE);
    }

    if (g_format == FMT_MINITEL) {
        /*
         * Minitel mode shares the area between its screen and its
         * keyboard, and takes 12 pixels less chrome than NAPLPS (a tighter
         * top and bottom): at the Suite's default 980 x 770 window that is
         * the difference between the 320 x 250 screen at 2x and at 1x.
         */
        const int m_y = 48, m_bottom = 8;
        MoveWindow(g_hStatus, margin, h - m_bottom - status_h,
                   w - 2 * margin, status_h, TRUE);
        minitel_view_layout(margin, m_y, w - 2 * margin,
                            h - m_y - m_bottom - status_h - 4);
    } else {
        canvas_y = 52;
        MoveWindow(g_hCanvas, margin, canvas_y,
                   w - 2 * margin,
                   h - canvas_y - margin - status_h - 4, TRUE);
        MoveWindow(g_hStatus, margin, h - margin - status_h,
                   w - 2 * margin, status_h, TRUE);
    }

    if (g_hCanvas) {
        InvalidateRect(g_hCanvas, NULL, FALSE);
        UpdateWindow(g_hCanvas);
    }
}

void naplps_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    /* Re-activation preserves the session and the rendered frame: the
     * panel is hidden on deactivate, never destroyed. */
    if (g_controls_created) {
        ShowWindow(g_hPanel, SW_SHOW);
        suite_set_status("");        /* the tab's own line is the one */
        GetClientRect(content, &rc);
        naplps_module_resize(content, rc.right - rc.left,
                             rc.bottom - rc.top);
        /* Minitel's screen takes the focus so its keys, CONNEXION/FIN
         * (F9) included, work at once. */
        if (g_format == FMT_MINITEL) minitel_view_focus();
        else if (!g_connected) SetFocus(g_hHostEdit);
        return;
    }

    nap_register_classes(hInst);

    GetClientRect(content, &rc);
    /* No WS_EX_CONTROLPARENT: the shared content panel is not a control
     * parent, and the combination hangs IsDialogMessage. That cost the
     * Archie tab an afternoon and the note is worth carrying. */
    g_hPanel = CreateWindowExA(0, NAP_PANEL_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        content, NULL, hInst, NULL);

    g_hFmtLbl = CreateWindowA("STATIC", "Format:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 55, 22, g_hPanel, NULL, hInst, NULL);
    g_hFmtCmb = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 95, 200, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_FORMAT_CMB, hInst, NULL);
    SendMessageA(g_hFmtCmb, CB_ADDSTRING, 0, (LPARAM)"NAPLPS");
    SendMessageA(g_hFmtCmb, CB_ADDSTRING, 0, (LPARAM)"Minitel");
    SendMessageA(g_hFmtCmb, CB_SETCURSEL, g_format, 0);

    g_hHostLbl = CreateWindowA("STATIC", "Host:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 40, 22, g_hPanel, NULL, hInst, NULL);
    g_hHostEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        NAPLPS_DEFAULT_HOST,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_HOST_EDIT, hInst, NULL);
    g_hPortLbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 35, 22, g_hPanel, NULL, hInst, NULL);
    g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        g_format == FMT_MINITEL ? MINITEL_DEFAULT_PORT_STR : NAPLPS_DEFAULT_PORT_STR,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 55, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_PORT_EDIT, hInst, NULL);
    g_hMonoChk = CreateWindowA("BUTTON", "Monochrome",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 105, 22, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_MONO_CHK, hInst, NULL);
    g_hScanChk = CreateWindowA("BUTTON", "Scanlines",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 92, 22, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_SCAN_CHK, hInst, NULL);
    g_hConnBtn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 85, 30, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_CONNECT_BTN, hInst, NULL);
    g_hDiscBtn = CreateWindowA("BUTTON", "Disconnect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 95, 30, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_DISCONN_BTN, hInst, NULL);

    g_hCanvas = CreateWindowExA(WS_EX_CLIENTEDGE, NAP_CANVAS_CLASS, "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_CANVAS, hInst, NULL);

    g_hStatus = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 100, 20, g_hPanel, NULL, hInst, NULL);

    if (g_hFontUI) {
        HWND all[] = { g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit,
                       g_hMonoChk, g_hConnBtn, g_hDiscBtn,
                       g_hStatus, g_hFmtLbl, g_hFmtCmb, g_hScanChk };
        int k;
        for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
            SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    if (!g_dec) g_dec = naplps_create();
    nap_dib_create();
    nap_dib_refresh();

    {
        minitel_view_host_t host;
        host.send       = mtl_host_send;
        host.connect    = mtl_host_connect;
        host.disconnect = mtl_host_disconnect;
        host.status     = mtl_host_status;
        host.file_opened = mtl_host_file_opened;
        minitel_view_create(g_hPanel, hInst, &host);
    }

    g_controls_created = TRUE;
    nap_set_session_ui(FALSE);
    nap_apply_format();
    suite_set_status("");            /* the tab's own line is the one */

    GetClientRect(content, &rc);
    naplps_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
    if (g_format == FMT_MINITEL) minitel_view_focus();
    else SetFocus(g_hHostEdit);
}

void naplps_module_deactivate(HWND content)
{
    (void)content;
    /* Hide, never destroy: the session and the frame survive a tab
     * switch, matching every other Suite module. */
    if (g_hPanel) ShowWindow(g_hPanel, SW_HIDE);
}

BOOL naplps_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam);
    (void)content;
    (void)lParam;

    /* The on-screen Minitel keyboard's buttons are children of this
     * panel too, so their clicks come through here. */
    if (minitel_view_on_command(wParam)) return TRUE;

    switch (id) {
    case IDC_NAP_CONNECT_BTN:
        nap_connect();
        return TRUE;

    case IDC_NAP_FORMAT_CMB:
        /*
         * Act on the choice only once the list is CLOSED: on CBN_CLOSEUP,
         * or on a selection change made with the list closed. With the
         * list open, an arrow key already fires CBN_SELCHANGE, and moving
         * the focus to the screen at that moment cancels the drop-down,
         * which puts the old selection back: the switch undid itself
         * (found by the logoff gate, case N6, 2026-09-27).
         */
        if ((HIWORD(wParam) == CBN_SELCHANGE
             && !SendMessageA(g_hFmtCmb, CB_GETDROPPEDSTATE, 0, 0))
            || HIWORD(wParam) == CBN_CLOSEUP) {
            int sel = (int)SendMessageA(g_hFmtCmb, CB_GETCURSEL, 0, 0);
            if (sel == g_format) return TRUE;      /* nothing changed */
            if (g_connected || g_telnet) {
                /* Never while a session is live; the combo is disabled
                 * then, and this is the belt to that brace. */
                SendMessageA(g_hFmtCmb, CB_SETCURSEL, g_format, 0);
                return TRUE;
            }
            if (sel == FMT_NAPLPS || sel == FMT_MINITEL) {
                g_format = sel;
                SetWindowTextA(g_hPortEdit, sel == FMT_MINITEL
                               ? MINITEL_DEFAULT_PORT_STR
                               : NAPLPS_DEFAULT_PORT_STR);
                /* The new format opens on its DEFAULT screen, at once,
                 * and any pending logoff timer is cancelled. */
                nap_cancel_logoff();
                nap_default_screen(sel);
                nap_apply_format();
                /* Leave the keyboard on the screen, not on the dropdown. */
                if (sel == FMT_MINITEL) minitel_view_focus();
                else if (g_hCanvas) SetFocus(g_hCanvas);
                nap_status(sel == FMT_MINITEL
                    ? "Minitel: port %d, monochrome as the 1B's tube; F9 or "
                      "CONNEXION/FIN connects"
                    : "NAPLPS: port %d",
                    sel == FMT_MINITEL ? MINITEL_DEFAULT_PORT : NAPLPS_DEFAULT_PORT);
            }
        }
        return TRUE;

    case IDC_NAP_DISCONN_BTN:
        nap_end_session(WHY_LOCAL);
        nap_status("Disconnected");
        return TRUE;

    case IDC_NAP_SCAN_CHK:
        /* CRT scan lines, Minitel only, default on; reproduced from
         * MiniPavi's emulator by observation (minitel_screen.c). */
        g_scan_mtl = (SendMessageA(g_hScanChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        minitel_view_set_scanlines(g_scan_mtl);
        nap_status(g_scan_mtl ? "Scan lines on" : "Scan lines off");
        minitel_view_focus();
        return TRUE;

    case IDC_NAP_MONO_CHK:
        if (g_format == FMT_MINITEL) {
            /* STUM1B p32's grey table, "le tube cathodique du Minitel
             * etant en noir et blanc". ON by default for Minitel. */
            g_mono_mtl = (SendMessageA(g_hMonoChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            minitel_view_set_mono(g_mono_mtl);
            nap_status(g_mono_mtl
                ? "Monochrome view: STUM1B's grey levels, as the Minitel 1B tube"
                : "Colour view, as a colour Minitel or an emulator");
            minitel_view_focus();
            return TRUE;
        }
        /*
         * The monochrome view, and it is not decoration.
         *
         * The only confirmed real target for this format is a MONOCHROME
         * terminal: a dealer listing for a 1991 AlexTel describes it as a
         * monochrome terminal with a built-in 1200 baud modem. The default
         * colour map's first half is a grey ramp and its second half is
         * eight full-saturation hues (5.3.2.5.2), and the hues do NOT
         * collapse to distinct or ordered luminances. So a frame that
         * carries meaning in hue loses that meaning on the hardware it was
         * built for, and there is no way to notice without either owning
         * the terminal or rendering by luminance.
         *
         * This toggle is the cheap substitute for owning one.
         */
        g_mono = (SendMessageA(g_hMonoChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        nap_status(g_mono
            ? "Monochrome view: rendering by luminance, as a monochrome "
              "terminal would show it"
            : "Colour view");
        return TRUE;

    default: break;
    }
    return FALSE;
}

BOOL naplps_module_has_unsaved(void)
{
    /* A rendered frame is not unsaved work: there is nothing the operator
     * could lose by switching tabs, because the tab is hidden rather than
     * destroyed and the frame is still there on return. */
    return FALSE;
}

void naplps_module_shutdown(void)
{
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    g_connected = FALSE;
    minitel_view_destroy();
    if (g_dec) { naplps_destroy(g_dec); g_dec = NULL; }
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; g_dib_bits = NULL; }
}
