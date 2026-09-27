/*
 * minitel_view.c - the Minitel terminal inside the Videotex tab: the
 *                  40 x 25 screen and the Minitel keyboard.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Everything below the wire is minitel_decode.[ch] and minitel_screen.[ch]
 * (clean-room from STUM1B, no Win32; provenance in minitel_decode.h).
 * This file is the window side only.
 *
 * THE KEYBOARD. STUM1B table 2-3-6 (p123-124): in the standard state
 * every function key is SEP (0x13) followed by one letter, and letters
 * are CAPITALS (p110). There is NO LOCAL ECHO: connected, the terminal
 * sends what is typed and the SERVER echoes it (p80-81, trap M-14), so
 * nothing typed here is drawn here.
 *
 *     ENVOI      SEP A     RETOUR     SEP B     REPETITION SEP C
 *     GUIDE      SEP D     ANNULATION SEP E     SOMMAIRE   SEP F
 *     CORRECTION SEP G     SUITE      SEP H     CONNEXION/FIN SEP I
 *
 * PC KEYS, chosen to match MiniPavi's hosted emulator exactly as the
 * 2026-09-27 emulator gate measured it (docs/2026-09-27_MINITEL_GATE_
 * EMULATOR.md section 3), so a reader moving between the two finds the
 * same keys: Enter or Right = ENVOI, Up = RETOUR, F2 = REPETITION, F1 =
 * GUIDE, Escape or Left = ANNULATION, Home = SOMMAIRE, Backspace =
 * CORRECTION, Tab or Down = SUITE. That emulator has NO PC key for
 * CONNEXION/FIN; this tab uses F9, the key the Unicorn feasibility sketch
 * (section 6.3) gave it, and the key the NAPLPS side already uses for its
 * own exit. RECORDED as the operator asked: CONNEXION/FIN = F9.
 *
 * CONNEXION/FIN does what the terminal's key does (STUM1B p33-34): while
 * disconnected it CONNECTS; connected, one press sends SEP I, "que le
 * serveur ... peut interpreter" as leaving the service, and a SECOND
 * press in a row hangs up locally.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

#include "minitel_view.h"
#include "minitel_decode.h"
#include "minitel_screen.h"
#include "suite_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MTL_CANVAS_CLASS "MGTMinitelCanvasV1"
#define IDT_MTL_BLINK    1
#define MTL_BLINK_MS     500      /* cursor half-period; flash is 0.5 Hz (p84) */

/* ------------------------------------------------------------------ */
/* The keyboard                                                        */
/* ------------------------------------------------------------------ */

enum { K_CNX = 0 };

typedef struct {
    const char *label;
    unsigned char code;    /* SEP letter, or the character for a key cap */
    int is_sep;
} mtlkey_t;

/* Index i has control id IDC_MTL_KEY_FIRST + i. */
static const mtlkey_t g_keys[] = {
    { "CONNEXION/FIN", 'I', 1 },
    { "SOMMAIRE",      'F', 1 },
    { "ANNULATION",    'E', 1 },
    { "RETOUR",        'B', 1 },
    { "REPETITION",    'C', 1 },
    { "GUIDE",         'D', 1 },
    { "CORRECTION",    'G', 1 },
    { "SUITE",         'H', 1 },
    { "ENVOI",         'A', 1 },
    { "1", '1', 0 }, { "2", '2', 0 }, { "3", '3', 0 },
    { "4", '4', 0 }, { "5", '5', 0 }, { "6", '6', 0 },
    { "7", '7', 0 }, { "8", '8', 0 }, { "9", '9', 0 },
    { "*", '*', 0 }, { "0", '0', 0 }, { "#", '#', 0 }
};
#define MTL_KEY_COUNT ((int)(sizeof g_keys / sizeof g_keys[0]))

/* One binding per line, so the legend fits the keyboard's width. */
static const char *g_legend =
    "PC keys\r\n"
    "Enter, Right ENVOI\r\n"
    "Home         SOMMAIRE\r\n"
    "Up           RETOUR\r\n"
    "Backspace    CORRECTION\r\n"
    "Esc, Left    ANNULATION\r\n"
    "Tab, Down    SUITE\r\n"
    "F1           GUIDE\r\n"
    "F2           REPETITION\r\n"
    "F9           CONNEXION/FIN\r\n"
    "F9 twice     hang up\r\n"
    "Ctrl+O       open a page";

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HWND g_hParent = NULL;
static HWND g_hCanvas = NULL;
static HWND g_hKey[MTL_KEY_COUNT];
static HWND g_hLegend = NULL;
static BOOL g_class_reg = FALSE;
static BOOL g_created = FALSE;
static BOOL g_shown = FALSE;

static minitel_view_host_t g_host;
static minitel_decoder_t *g_dec = NULL;
static BOOL g_connected = FALSE;
static BOOL g_mono = TRUE;          /* the period terminal: DEFAULT ON */
static BOOL g_last_was_cnx = FALSE;
static unsigned g_tick = 0;

static BOOL g_scan = TRUE;          /* scan lines: DEFAULT ON (operator) */

/* The 320 x 250 raster, and the presented copy at the current integer
 * scale (with the scan lines applied by minitel_present_bgra). */
static HBITMAP g_hDib = NULL, g_hOldDib = NULL;
static void   *g_bits = NULL;
static HDC     g_hMemDC = NULL;
static HBITMAP g_hBig = NULL, g_hOldBig = NULL;
static void   *g_big_bits = NULL;
static HDC     g_hBigDC = NULL;
static int     g_big_scale = 0;

/* The Minitel keyboard's colours: the function keys light brown, ENVOI
 * light green, as on the real keyboard. The keypad keeps the Suite's
 * ordinary buttons. */
#define MTL_KEY_BROWN        RGB(205, 170, 125)
#define MTL_KEY_BROWN_DOWN   RGB(170, 135,  95)
#define MTL_KEY_GREEN        RGB(165, 208, 128)
#define MTL_KEY_GREEN_DOWN   RGB(130, 175,  95)
#define MTL_KEY_EDGE         RGB( 95,  70,  45)
#define MTL_K_ENVOI          8
#define MTL_FKEY_LAST        8      /* indices 0..8 are owner-drawn */

/* ------------------------------------------------------------------ */
/* Raster                                                              */
/* ------------------------------------------------------------------ */

static void dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;
    if (g_hDib) return;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = MINITEL_W;
    bi.bmiHeader.biHeight      = -MINITEL_H;       /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    if (g_hDib) {
        g_hMemDC  = CreateCompatibleDC(screen);
        g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
    }
    ReleaseDC(NULL, screen);
}

/* The presented copy, (re)made when the integer scale changes. */
static BOOL big_ensure(int scale)
{
    BITMAPINFO bi;
    HDC screen;
    if (scale < 1) scale = 1;
    if (g_hBig && g_big_scale == scale) return TRUE;
    if (g_hBig) {
        SelectObject(g_hBigDC, g_hOldBig);
        DeleteDC(g_hBigDC);
        DeleteObject(g_hBig);
        g_hBig = NULL; g_hBigDC = NULL; g_big_bits = NULL;
    }
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = MINITEL_W * scale;
    bi.bmiHeader.biHeight      = -(MINITEL_H * scale);
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    g_hBig = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_big_bits, NULL, 0);
    if (g_hBig) {
        g_hBigDC  = CreateCompatibleDC(screen);
        g_hOldBig = (HBITMAP)SelectObject(g_hBigDC, g_hBig);
        g_big_scale = scale;
    }
    ReleaseDC(NULL, screen);
    return g_hBig != NULL;
}

static void dib_refresh(void)
{
    int opts = 0;
    if (!g_bits || !g_dec) return;
    if (g_mono) opts |= MT_R_MONO;
    /* Flash at about 0.5 Hz (p84): one second on, one second off, i.e.
     * two 500 ms ticks each. The cursor alternates every tick (p99). */
    if ((g_tick / 2) & 1) opts |= MT_R_FLASH;
    if (g_tick & 1) opts |= MT_R_CURSOR;
    minitel_render_bgra(g_dec, (unsigned char *)g_bits, opts);
}

static void repaint(void)
{
    dib_refresh();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
}

/*
 * Where the 320 x 250 raster lands: INTEGER scaling, the same on both
 * axes, centred.
 *
 * THE CHOICE, recorded as the operator asked. STUM1B gives the matrix
 * (8 x 10 dots, 40 x 25 cells, p29-30) but not the dot's shape, and a
 * Minitel's tube is 4:3. 320 x 250 at 4:3 would need dots 1.042 times
 * wider than tall, and no integer pair of scales gets closer than the
 * square one short of absurd sizes (25:24). So the dots stay square and
 * the picture is 1.28:1, 4% narrower than 4:3: the same 4% the NAPLPS
 * tab accepted and wrote down, and for the same reason, which is that
 * integer scaling keeps the 5 x 7 glyphs and the mosaic blocks crisp at
 * every window size where a fractional stretch would make the 40
 * columns wobble.
 */
static void dest_rect(int cw, int ch, RECT *out)
{
    int s = cw / MINITEL_W, w, h;
    if (ch / MINITEL_H < s) s = ch / MINITEL_H;
    if (s < 1) s = 1;
    w = MINITEL_W * s;
    h = MINITEL_H * s;
    out->left = (cw - w) / 2;
    out->top = (ch - h) / 2;
    out->right = out->left + w;
    out->bottom = out->top + h;
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */
/* ------------------------------------------------------------------ */

static void report_counts(void)
{
    char buf[256];
    minitel_view_counts(buf, sizeof buf);
    if (g_host.status) g_host.status(buf);
}

static void send_bytes(const unsigned char *b, size_t n)
{
    if (g_host.send && g_connected) g_host.send(b, n);
}

static void press_key(int k)
{
    const mtlkey_t *key = &g_keys[k];
    char msg[64];

    if (k == K_CNX) {
        if (!g_connected) {
            g_last_was_cnx = FALSE;
            if (g_host.connect) g_host.connect();
            return;
        }
        if (g_last_was_cnx) {
            /* The second press in a row: the terminal drops the line
             * itself (STUM1B p33-34). */
            g_last_was_cnx = FALSE;
            if (g_host.disconnect) g_host.disconnect();
            return;
        }
    }

    if (key->is_sep) {
        unsigned char seq[2];
        seq[0] = 0x13;
        seq[1] = key->code;
        send_bytes(seq, 2);
        _snprintf(msg, sizeof msg - 1, "Sent %s (SEP %c)", key->label, key->code);
    } else {
        send_bytes(&key->code, 1);
        _snprintf(msg, sizeof msg - 1, "Sent %c", key->code);
    }
    msg[sizeof msg - 1] = '\0';
    g_last_was_cnx = (k == K_CNX);
    if (g_host.status && g_connected) g_host.status(msg);
}

/* The PC key bindings (see the header comment). -1 if not bound. */
static int pc_function_key(int vk)
{
    switch (vk) {
    case VK_RETURN: case VK_RIGHT:  return 8;   /* ENVOI      */
    case VK_UP:                     return 3;   /* RETOUR     */
    case VK_F2:                     return 4;   /* REPETITION */
    case VK_F1:                     return 5;   /* GUIDE      */
    case VK_ESCAPE: case VK_LEFT:   return 2;   /* ANNULATION */
    case VK_HOME:                   return 1;   /* SOMMAIRE   */
    case VK_BACK:                   return 6;   /* CORRECTION */
    case VK_TAB: case VK_DOWN:      return 7;   /* SUITE      */
    case VK_F9:                     return K_CNX;
    default: break;
    }
    return -1;
}

/*
 * A printable character from the virtual key, never from WM_CHAR, which
 * is layout-translated (the F6 Telnet lesson). Letters go up as CAPITALS
 * whatever the shift state: a Minitel keyboard in its standard state
 * "les touches alphabetiques correspondent aux lettres MAJUSCULES" (p110).
 */
static int vk_to_char(int vk, BOOL shift)
{
    static const char *digits_shift = ")!@#$%^&*(";
    if (vk >= 'A' && vk <= 'Z') return vk;
    if (vk >= '0' && vk <= '9') return shift ? digits_shift[vk - '0'] : vk;
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_MULTIPLY:   return '*';
    case VK_ADD:        return '+';
    case VK_SUBTRACT:   return '-';
    case VK_DECIMAL:    return '.';
    case VK_DIVIDE:     return '/';
    case VK_OEM_1:      return shift ? ':' : ';';
    case VK_OEM_PLUS:   return shift ? '+' : '=';
    case VK_OEM_COMMA:  return shift ? '<' : ',';
    case VK_OEM_MINUS:  return shift ? '_' : '-';
    case VK_OEM_PERIOD: return shift ? '>' : '.';
    case VK_OEM_2:      return shift ? '?' : '/';
    case VK_OEM_4:      return '[';
    case VK_OEM_6:      return ']';
    case VK_OEM_7:      return shift ? '"' : '\'';
    default: break;
    }
    return -1;
}

static void key_down(int vk)
{
    int k = pc_function_key(vk), c;
    /* Ctrl+O opens a local .vdt page, for testing; it is never sent up
     * the line (there is no Open button any more, operator 2026-09-27). */
    if (vk == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (g_host.file_opened) g_host.file_opened();
        minitel_view_open_file(GetAncestor(g_hCanvas, GA_ROOT));
        minitel_view_focus();
        return;
    }
    if (k >= 0) { press_key(k); return; }
    if (!g_connected) return;
    c = vk_to_char(vk, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
    if (c > 0) {
        unsigned char ch = (unsigned char)c;
        send_bytes(&ch, 1);
        g_last_was_cnx = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

static void canvas_paint(HWND hwnd, HDC hdc)
{
    RECT rc, dst;
    HDC mem;
    HBITMAP bmp, old;
    int cw, ch;

    GetClientRect(hwnd, &rc);
    cw = rc.right; ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;

    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    old = (HBITMAP)SelectObject(mem, bmp);
    FillRect(mem, &rc, g_hBrushBlack);

    if (g_hMemDC && g_bits) {
        int s;
        dest_rect(cw, ch, &dst);
        s = (dst.right - dst.left) / MINITEL_W;
        if (big_ensure(s)) {
            minitel_present_bgra((const unsigned char *)g_bits, s,
                                 (unsigned char *)g_big_bits,
                                 (g_scan ? MT_P_SCANLINES : 0)
                                 | (g_mono ? MT_P_MONO : 0));
            BitBlt(mem, dst.left, dst.top, dst.right - dst.left,
                   dst.bottom - dst.top, g_hBigDC, 0, 0, SRCCOPY);
        }
    }

    if (g_dec && !minitel_ever_drew(g_dec)) {
        /* The power-on screen is black with an "F" on row 00 (p88), so a
         * word of guidance goes over it until the first page arrives. */
        static const char *hint[] = {
            "Minitel (Teletel videotex, 40 x 25)",
            "",
            "Press CONNEXION/FIN (F9) to connect; Ctrl+O opens a .vdt page.",
            "",
            "Enter ENVOI   Home SOMMAIRE   Up RETOUR   F1 GUIDE",
            "Backspace CORRECTION   Esc ANNULATION   Tab SUITE",
            "F2 REPETITION   F9 CONNEXION/FIN"
        };
        const int n = (int)(sizeof hint / sizeof hint[0]);
        SIZE ext;
        int lh, y, k;
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        GetTextExtentPoint32A(mem, "Wg", 2, &ext);
        lh = ext.cy + 2;
        y = (ch - n * lh) / 2;
        for (k = 0; k < n; k++) {
            RECT lr;
            lr.left = 0; lr.right = cw; lr.top = y + k * lh; lr.bottom = lr.top + lh;
            DrawTextA(mem, hint[k], -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK MtlCanvasProc(HWND hwnd, UINT msg, WPARAM wParam,
                                      LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        canvas_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == IDT_MTL_BLINK) {
            g_tick++;
            if (g_shown) repaint();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    /* A custom-painted child must claim the keys, Tab and the arrows
     * included, or IsDialogMessage eats them first. */
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN:
        key_down((int)wParam);
        return 0;
    case WM_SYSKEYDOWN:
        /* F10 arrives as a system key; nothing is bound to it here, so
         * let the default handler keep the menu behaviour. */
        break;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

void minitel_view_create(HWND parent, HINSTANCE hInst,
                         const minitel_view_host_t *host)
{
    int k;

    if (g_created) return;
    g_hParent = parent;
    g_host = *host;

    if (!g_class_reg) {
        WNDCLASSEXA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = MtlCanvasProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.lpszClassName = MTL_CANVAS_CLASS;
        RegisterClassExA(&wc);
        g_class_reg = TRUE;
    }

    g_hCanvas = CreateWindowExA(WS_EX_CLIENTEDGE, MTL_CANVAS_CLASS, "",
        WS_CHILD, 0, 0, 100, 100, parent,
        (HMENU)(INT_PTR)IDC_MTL_CANVAS, hInst, NULL);

    for (k = 0; k < MTL_KEY_COUNT; k++) {
        /* The function keys (CONNEXION/FIN to ENVOI) are owner-drawn in
         * the Minitel's colours; the keypad keeps ordinary buttons. */
        g_hKey[k] = CreateWindowA("BUTTON", g_keys[k].label,
            WS_CHILD | (k <= MTL_FKEY_LAST ? BS_OWNERDRAW : BS_PUSHBUTTON),
            0, 0, 80, 28, parent,
            (HMENU)(INT_PTR)(IDC_MTL_KEY_FIRST + k), hInst, NULL);
        if (g_hFontUI) SendMessageA(g_hKey[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    g_hLegend = CreateWindowA("STATIC", g_legend, WS_CHILD | SS_LEFT,
        0, 0, 100, 80, parent, NULL, hInst, NULL);
    if (g_hFontUI) SendMessageA(g_hLegend, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_dec = minitel_create();
    minitel_set_status_letter(g_dec, 'F');
    dib_create();
    dib_refresh();

    SetTimer(g_hCanvas, IDT_MTL_BLINK, MTL_BLINK_MS, NULL);
    g_created = TRUE;
}

static void reply_to_line(void *ctx, const unsigned char *b, size_t n)
{
    (void)ctx;
    send_bytes(b, n);
}

void minitel_view_destroy(void)
{
    if (g_hCanvas) KillTimer(g_hCanvas, IDT_MTL_BLINK);
    if (g_dec) { minitel_destroy(g_dec); g_dec = NULL; }
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; g_bits = NULL; }
    if (g_hBig) {
        SelectObject(g_hBigDC, g_hOldBig);
        DeleteDC(g_hBigDC);
        DeleteObject(g_hBig);
        g_hBig = NULL; g_hBigDC = NULL; g_big_bits = NULL; g_big_scale = 0;
    }
}

void minitel_view_show(BOOL show)
{
    int k, sw = show ? SW_SHOW : SW_HIDE;
    if (!g_created) return;
    g_shown = show;
    ShowWindow(g_hCanvas, sw);
    for (k = 0; k < MTL_KEY_COUNT; k++) ShowWindow(g_hKey[k], sw);
    ShowWindow(g_hLegend, sw);
    if (show) repaint();
}

void minitel_view_layout(int x, int y, int w, int h)
{
    const int kb_w = 220, gap = 12, bg = 6;
    const int half = (kb_w - bg) / 2, third = (kb_w - 2 * bg) / 3;
    const int legend_lines = 12;
    int kx, ky, k, canvas_w, line_h = 16, legend_h, bh;

    if (!g_created) return;
    canvas_w = w - kb_w - gap;
    if (canvas_w < 100) canvas_w = 100;
    MoveWindow(g_hCanvas, x, y, canvas_w, h, TRUE);

    /* The legend's height comes from the font actually in use, so every
     * line shows; the keys give up height first if the tab is short. */
    {
        HDC hdc = GetDC(g_hLegend);
        TEXTMETRICA tm;
        HGDIOBJ old = g_hFontUI ? SelectObject(hdc, g_hFontUI) : NULL;
        if (GetTextMetricsA(hdc, &tm)) line_h = tm.tmHeight + tm.tmExternalLeading;
        if (old) SelectObject(hdc, old);
        ReleaseDC(g_hLegend, hdc);
    }
    legend_h = legend_lines * line_h + 4;
    /* Eight rows of keys (four function, four keypad), their gaps, a
     * separator, and the legend. */
    bh = (h - legend_h - 8 * bg - 12) / 8;
    if (bh > 30) bh = 30;
    if (bh < 20) bh = 20;

    kx = x + canvas_w + gap;
    ky = y;
    /* The eight function keys in the Minitel's two columns (CONNEXION/FIN
     * lives in the tab's top row, minitel_view_place_cnx), then the
     * digits, then the legend. */
    for (k = 1; k <= 8; k++) {
        int col = (k - 1) % 2, row = (k - 1) / 2;
        MoveWindow(g_hKey[k], kx + col * (half + bg), ky + row * (bh + bg),
                   half, bh, TRUE);
    }
    ky += 4 * (bh + bg) + 6;
    for (k = 9; k < MTL_KEY_COUNT; k++) {
        int i = k - 9, col = i % 3, row = i / 3;
        MoveWindow(g_hKey[k], kx + col * (third + bg), ky + row * (bh + bg),
                   third, bh, TRUE);
    }
    ky += 4 * (bh + bg) + 6;
    MoveWindow(g_hLegend, kx, ky, kb_w, legend_h, TRUE);
}

/* CONNEXION/FIN: the long brown key in the tab's top row, where Connect
 * and Disconnect sit in NAPLPS mode. */
void minitel_view_place_cnx(int x, int y, int w, int h)
{
    if (!g_created) return;
    MoveWindow(g_hKey[K_CNX], x, y, w, h, TRUE);
}

void minitel_view_set_scanlines(BOOL on)
{
    g_scan = on;
    repaint();
}

/* Paint one owner-drawn Minitel key. Returns FALSE for anything else. */
BOOL minitel_view_on_drawitem(const DRAWITEMSTRUCT *di)
{
    int k;
    BOOL down, green;
    HBRUSH br;
    HPEN pen, oldpen;
    HGDIOBJ oldbr, oldfont = NULL;
    RECT rc;
    char text[32];

    if (!di || di->CtlType != ODT_BUTTON) return FALSE;
    k = (int)di->CtlID - IDC_MTL_KEY_FIRST;
    if (k < 0 || k > MTL_FKEY_LAST) return FALSE;

    down  = (di->itemState & ODS_SELECTED) != 0;
    green = (k == MTL_K_ENVOI);
    rc = di->rcItem;
    br  = CreateSolidBrush(green ? (down ? MTL_KEY_GREEN_DOWN : MTL_KEY_GREEN)
                                 : (down ? MTL_KEY_BROWN_DOWN : MTL_KEY_BROWN));
    pen = CreatePen(PS_SOLID, 1, MTL_KEY_EDGE);
    oldbr  = SelectObject(di->hDC, br);
    oldpen = (HPEN)SelectObject(di->hDC, pen);
    RoundRect(di->hDC, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(di->hDC, oldpen);
    SelectObject(di->hDC, oldbr);
    DeleteObject(pen);
    DeleteObject(br);

    GetWindowTextA(di->hwndItem, text, sizeof text);
    if (g_hFontUI) oldfont = SelectObject(di->hDC, g_hFontUI);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, RGB(0, 0, 0));
    if (down) OffsetRect(&rc, 1, 1);
    DrawTextA(di->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (oldfont) SelectObject(di->hDC, oldfont);
    return TRUE;
}

void minitel_view_reset(void)
{
    if (!g_dec) return;
    minitel_reset(g_dec);
    minitel_set_reply(g_dec, reply_to_line, NULL);
    g_last_was_cnx = FALSE;
    repaint();
}

void minitel_view_default_screen(void)
{
    if (!g_dec) return;
    /* Power-on state (the "F" letter is kept: we are local). With
     * nothing drawn, the canvas shows the guidance again. */
    minitel_reset(g_dec);
    minitel_set_reply(g_dec, NULL, NULL);
    g_last_was_cnx = FALSE;
    repaint();
}

/*
 * The Suite's logoff message, drawn AS A MINITEL PAGE through the
 * decoder, so it is in the format's own style: FF, cursor off, then two
 * centred lines, US row then column (p96). No line is attached, so
 * nothing is ever sent.
 */
void minitel_view_show_logoff(const char *host)
{
    unsigned char buf[128];
    size_t n = 0, hl = host ? strlen(host) : 0, i;
    static const char line1[] = "Disconnected from";
    const size_t l1 = sizeof line1 - 1;

    if (!g_dec) return;
    if (hl > MINITEL_COLS) hl = MINITEL_COLS;
    minitel_set_reply(g_dec, NULL, NULL);

    buf[n++] = 0x0C;                                   /* FF              */
    buf[n++] = 0x14;                                   /* Coff            */
    buf[n++] = 0x1F; buf[n++] = (unsigned char)(0x40 + 11);
    buf[n++] = (unsigned char)(0x40 + 1 + (MINITEL_COLS - l1) / 2);
    for (i = 0; i < l1; i++) buf[n++] = (unsigned char)line1[i];
    if (hl) {
        buf[n++] = 0x1F; buf[n++] = (unsigned char)(0x40 + 13);
        buf[n++] = (unsigned char)(0x40 + 1 + (MINITEL_COLS - hl) / 2);
        for (i = 0; i < hl; i++) {
            unsigned char c = (unsigned char)host[i];
            buf[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
    }
    minitel_feed(g_dec, buf, n);
    repaint();
}

void minitel_view_feed(const unsigned char *buf, size_t len)
{
    if (!g_dec || !buf || len == 0) return;
    minitel_feed(g_dec, buf, len);
    repaint();
    report_counts();
}

void minitel_view_open_file(HWND owner)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "";
    unsigned char *buf;
    FILE *f;
    long len;
    size_t got;

    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = "Minitel pages (*.vdt)\0*.vdt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = path;
    ofn.nMaxFile    = sizeof path;
    ofn.lpstrTitle  = "Open a Minitel page";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameA(&ofn)) return;

    f = fopen(path, "rb");
    if (!f) { if (g_host.status) g_host.status("Cannot open that file"); return; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (16L << 20)) {
        fclose(f);
        if (g_host.status) g_host.status("That file is empty or implausibly large");
        return;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); return; }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);

    /* A file is a fresh presentation: power-on state first, and no line
     * to answer on, so replies go nowhere. */
    minitel_reset(g_dec);
    minitel_set_reply(g_dec, NULL, NULL);
    minitel_feed(g_dec, buf, got);
    free(buf);
    repaint();
    report_counts();
}

void minitel_view_set_connected(BOOL connected)
{
    g_connected = connected;
    g_last_was_cnx = FALSE;
    if (g_dec) {
        /* Row 00 column 39: "C" connected, "F" local (p32). At
         * disconnection "le contenu de l'ecran et la position courante du
         * curseur restent inchanges" (p88), so nothing else is touched. */
        minitel_set_status_letter(g_dec, connected ? 'C' : 'F');
        repaint();
    }
}

void minitel_view_set_mono(BOOL mono)
{
    g_mono = mono;
    repaint();
}

void minitel_view_focus(void)
{
    if (g_hCanvas) SetFocus(g_hCanvas);
}

BOOL minitel_view_on_command(WPARAM wParam)
{
    int id = LOWORD(wParam);
    if (id < IDC_MTL_KEY_FIRST || id >= IDC_MTL_KEY_FIRST + MTL_KEY_COUNT) return FALSE;
    if (HIWORD(wParam) != BN_CLICKED) return TRUE;
    press_key(id - IDC_MTL_KEY_FIRST);
    /* Give the keyboard focus back to the screen, so the PC keys go on
     * working after a click on the on-screen keyboard. */
    minitel_view_focus();
    return TRUE;
}

void minitel_view_counts(char *buf, size_t n)
{
    const minitel_stats_t *s;
    if (!g_dec) { if (n) buf[0] = '\0'; return; }
    s = minitel_get_stats(g_dec);
    _snprintf(buf, n - 1,
              "%lu bytes  %lu chars  %lu filtered  %lu resync  %lu error  "
              "%lu row00  %lu pro  %lu unrep  %lu bell%s%s",
              s->bytes_in, s->chars, s->filtered, s->resync, s->errors,
              s->row0, s->protocol, s->unrepresent, s->bells,
              minitel_scroll_mode(g_dec) ? "  [scroll]" : "",
              s->bit8 ? "  [8-bit bytes folded]" : "");
    buf[n - 1] = '\0';
}
