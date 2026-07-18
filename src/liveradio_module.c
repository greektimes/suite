/*
 * liveradio_module.c - Live Radio pane: HE-AAC v2 audio + stereo VU.
 *
 * State machine (mirrors livetv_module):
 *   LR_STATE_SPLASH   black bg, Greek Times icon, "Live Radio - Click
 *                     to Start" caption, green Start button.
 *   LR_STATE_LOADING  same chrome, caption "Connecting...", Start
 *                     disabled. player_create_audio + load + play
 *                     spin up on a worker thread.
 *   LR_STATE_PLAYING  logo top 25%, stereo LED VU meter middle 50%,
 *                     red Stop button bottom 25%. SetTimer fires the
 *                     audio_meter poll at 30 Hz.
 *   LR_STATE_ERROR    "Stream unavailable. Click to retry." Retry
 *                     button.
 *
 * VU meter geometry: 30 LED pills per channel, stacked stereo (L
 * above R), DPI-scaled, centered horizontally inside 80% of the
 * content width. dBFS scale -60 dB at LED 0 to 0 dBFS at LED 29
 * (2.07 dB per step). Color zones per dispatch 3 locked decisions:
 *   0..17  bright green (#33CC33)
 *   18..23 yellow       (#E6E600)
 *   24..26 orange       (#FF9900)
 *   27..29 red          (#E60000)
 * Dim / off LEDs paint the same hue at 12% brightness so every
 * position is visible.
 *
 * Persistence: tab and mode switches hide this window; audio keeps
 * playing through the WASAPI default endpoint. The player and meter
 * are torn down only on user Stop or process exit.
 */
#include "liveradio_module.h"
#include "radio_engine.h"
#include "audio_meter_service.h"
#include "suite_shell.h"   /* WM_APP_RESIZE_ENDED + suite_shell_in_user_resize */
#include "suite_logo.h"    /* high-quality GDI+ logo draw from RCDATA */
#include "suite_res.h"     /* IDR_LOGO_RADIO */
#include <stdio.h>
#include <math.h>

/* Engine state transitions arrive on the engine worker thread; marshal
 * them to this window's UI thread so meter creation / SetTimer happen on
 * the owning thread (as the IMFMediaEngine path did via PostMessage).
 * wParam = REState. */
#define WM_LR_ENGINE_STATE  (WM_USER + 1)

#define LR_CLASS    "MGTUnicornLiveRadio"
#define LR_URL      L"http://live.greekradio.ca:8000/live"

/* State enum. */
#define LR_STATE_SPLASH  0
#define LR_STATE_LOADING 1
#define LR_STATE_PLAYING 2
#define LR_STATE_ERROR   3

/* Geometry constants (base 96 DPI, scaled at use). */
#define LR_LOGO_PX            96
#define LR_BTN_W             120
#define LR_BTN_H              36
#define LR_BTN_RADIUS          4
#define LR_LED_W              12
#define LR_LED_H              40
#define LR_LED_GAP             4
#define LR_LEDS_PER_CHANNEL   30
#define LR_METER_CH_GAP       10   /* vertical gap between L and R rows */

#define LR_METER_TIMER_ID      1
#define LR_METER_TIMER_MS     33   /* ~30 Hz */

/* "Connecting..." caption animation (LOADING state only). 4-frame cycle
 * "Connecting" / "." / ".." / "..." at 400 ms each (1.6 s loop). Its own
 * module-private timer ID, distinct from the meter timer. */
#define LR_CONNECT_TIMER_ID    2
#define LR_CONNECT_TIMER_MS  300
#define LR_CONNECT_FRAMES      4

/* Color zones (ARGB-style 0xRRGGBB for COLORREF via macros). */
#define LR_GREEN  RGB(0x33, 0xCC, 0x33)
#define LR_YELLOW RGB(0xE6, 0xE6, 0x00)
#define LR_ORANGE RGB(0xFF, 0x99, 0x00)
#define LR_RED    RGB(0xE6, 0x00, 0x00)

typedef struct LrState {
    int         state;
    RadioEngine *engine;
    AudioMeter *meter;
    HFONT      font_caption;   /* 18 pt Segoe UI Semibold */
    HFONT      font_error;     /* 14 pt Segoe UI */
    HFONT      font_button;    /* 12 pt Segoe UI */
    RECT       btn_rect;       /* current main-button hit rect */
    RECT       meter_rect;     /* cached meter region for InvalidateRect */
    RECT       caption_rect;   /* cached caption region for tight invalidate */
    int        connect_anim_frame; /* 0..3 dot count, "Connecting" animation */
    BOOL       btn_pressed;
    BOOL       btn_hover;
    float      peak_l;
    float      peak_r;
} LrState;

static LrState *lr_get(HWND h) { return (LrState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

static int lr_dpi(HWND h)
{
    typedef UINT (WINAPI *F)(HWND);
    HMODULE u = GetModuleHandleA("user32.dll");
    F f = u ? (F)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (f) return (int)f(h);
    { HDC dc = GetDC(h); int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
      if (dc) ReleaseDC(h, dc); return d ? d : 96; }
}

static HFONT lr_make_font(int pt, int weight, int dpi)
{
    LOGFONTA lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

/* ------------------------------------------------------------------ */
/* Color helpers.                                                      */
/* ------------------------------------------------------------------ */

static COLORREF lr_led_color(int idx)
{
    /* Zone counts retuned 2026-05-26 (visual gate amendment):
     *   0..20 green (21 LEDs) - matches level on most program material
     *   21..24 yellow (4)
     *   25..28 orange (4)
     *   29 red (1) */
    if (idx < 21) return LR_GREEN;
    if (idx < 25) return LR_YELLOW;
    if (idx < 29) return LR_ORANGE;
    return LR_RED;
}

static COLORREF lr_dim(COLORREF c, float scale)
{
    int r = (int)(GetRValue(c) * scale + 0.5f);
    int g = (int)(GetGValue(c) * scale + 0.5f);
    int b = (int)(GetBValue(c) * scale + 0.5f);
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return RGB(r, g, b);
}

/* ------------------------------------------------------------------ */
/* Geometry helpers.                                                   */
/* ------------------------------------------------------------------ */

/* Compute meter rect: middle 50% vertical, centered horizontally
 * inside 80% of content width, sized to the LED grid. */
static void lr_compute_meter_rect(HWND hwnd, RECT *out)
{
    RECT rc;
    int dpi = lr_dpi(hwnd);
    int total_w, total_h, mx, my;
    int led_w = MulDiv(LR_LED_W, dpi, 96);
    int led_h = MulDiv(LR_LED_H, dpi, 96);
    int gap   = MulDiv(LR_LED_GAP, dpi, 96);
    int ch_gap = MulDiv(LR_METER_CH_GAP, dpi, 96);
    GetClientRect(hwnd, &rc);

    total_w = LR_LEDS_PER_CHANNEL * led_w + (LR_LEDS_PER_CHANNEL - 1) * gap;
    total_h = 2 * led_h + ch_gap;
    /* Cap total_w to 80% of content width: if it overflows, shrink. */
    {
        int max_w = (rc.right - rc.left) * 80 / 100;
        if (total_w > max_w) {
            /* Shrink LED width to fit, preserving gap. */
            led_w = (max_w - (LR_LEDS_PER_CHANNEL - 1) * gap) / LR_LEDS_PER_CHANNEL;
            if (led_w < 4) led_w = 4;
            total_w = LR_LEDS_PER_CHANNEL * led_w + (LR_LEDS_PER_CHANNEL - 1) * gap;
        }
    }
    mx = ((rc.right - rc.left) - total_w) / 2;
    my = rc.top + (rc.bottom - rc.top) * 25 / 100;
    /* Vertical: center inside the middle 50% of content. */
    my = my + ((rc.bottom - rc.top) * 50 / 100 - total_h) / 2;
    out->left   = mx;
    out->top    = my;
    out->right  = mx + total_w;
    out->bottom = my + total_h;
}

/* Splash logo geometry: a responsive square ~32% of the shorter content
 * dimension (clamped 120..300 px at 96 DPI), centered horizontally. The
 * logo, caption and Start button form one vertically-centered block so
 * the larger MGR logo never collides with the controls below it. Both
 * the painter and the button hit-rect derive from this helper, keeping
 * them in lock-step regardless of panel size. */
static void lr_compute_logo_rect(HWND hwnd, RECT *out)
{
    RECT rc;
    int w, h, dpi, shorter, dim, max_dim, min_dim, block_h, top, min_top;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    dpi = lr_dpi(hwnd);
    shorter = (w < h) ? w : h;
    dim = shorter * 32 / 100;
    max_dim = MulDiv(300, dpi, 96);
    min_dim = MulDiv(120, dpi, 96);
    if (dim > max_dim) dim = max_dim;
    if (dim < min_dim) dim = min_dim;
    /* Block height = logo + 60 px gap + button; center it vertically. */
    block_h = dim + MulDiv(60 + LR_BTN_H, dpi, 96);
    top = rc.top + (h - block_h) / 2;
    min_top = rc.top + MulDiv(20, dpi, 96);
    if (top < min_top) top = min_top;
    out->left   = rc.left + (w - dim) / 2;
    out->top    = top;
    out->right  = out->left + dim;
    out->bottom = out->top + dim;
}

static void lr_compute_button_rect(HWND hwnd, RECT *out)
{
    LrState *st = lr_get(hwnd);
    RECT rc;
    int dpi = lr_dpi(hwnd);
    int bw = MulDiv(LR_BTN_W, dpi, 96);
    int bh = MulDiv(LR_BTN_H, dpi, 96);
    int cx, cy;
    GetClientRect(hwnd, &rc);
    cx = (rc.right - rc.left - bw) / 2;
    if (st && st->state == LR_STATE_PLAYING) {
        /* Bottom 25% band, button centered vertically inside it. */
        int band_top = rc.top + (rc.bottom - rc.top) * 75 / 100;
        int band_h   = (rc.bottom - rc.top) * 25 / 100;
        cy = band_top + (band_h - bh) / 2;
    } else {
        /* SPLASH / LOADING / ERROR: button 60 px below the responsive
         * logo block (see lr_compute_logo_rect). */
        RECT lrc;
        lr_compute_logo_rect(hwnd, &lrc);
        cy = lrc.bottom + MulDiv(60, dpi, 96);
    }
    out->left = cx;
    out->top = cy;
    out->right = cx + bw;
    out->bottom = cy + bh;
}

/* ------------------------------------------------------------------ */
/* Paint helpers.                                                      */
/* ------------------------------------------------------------------ */

static void lr_draw_button(HDC hdc, const RECT *rc, COLORREF bg, COLORREF fg,
                           BOOL pressed, BOOL hover, BOOL enabled,
                           HFONT font, const char *label)
{
    HBRUSH br;
    HPEN pen, oldp;
    HBRUSH oldb;
    HFONT oldf;
    COLORREF actual_bg = bg;
    int r = LR_BTN_RADIUS, adj_r, h, w;
    if (!enabled) {
        actual_bg = RGB(140, 140, 140);
        fg        = RGB(240, 240, 240);
    } else if (pressed) {
        actual_bg = RGB(GetRValue(bg)*7/10, GetGValue(bg)*7/10, GetBValue(bg)*7/10);
    } else if (hover) {
        actual_bg = RGB(GetRValue(bg)*9/10, GetGValue(bg)*9/10, GetBValue(bg)*9/10);
    }
    h = rc->bottom - rc->top; w = rc->right - rc->left;
    adj_r = r * 2;
    if (adj_r > h) adj_r = h; if (adj_r > w) adj_r = w;
    br = CreateSolidBrush(actual_bg);
    pen = CreatePen(PS_NULL, 0, actual_bg);
    oldb = (HBRUSH)SelectObject(hdc, br);
    oldp = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, adj_r, adj_r);
    SelectObject(hdc, oldb); SelectObject(hdc, oldp);
    DeleteObject(br); DeleteObject(pen);
    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    oldf = (HFONT)SelectObject(hdc, font);
    DrawTextA(hdc, label, -1, (RECT *)rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldf);
}

static void lr_draw_led(HDC hdc, int x, int y, int w, int h, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_NULL, 0, color);
    HBRUSH oldb = (HBRUSH)SelectObject(hdc, br);
    HPEN oldp   = (HPEN)SelectObject(hdc, pen);
    int radius = w;          /* full corner radius gives the pill */
    if (radius > h) radius = h;
    RoundRect(hdc, x, y, x + w, y + h, radius, radius);
    SelectObject(hdc, oldb); SelectObject(hdc, oldp);
    DeleteObject(br); DeleteObject(pen);
}

/* Paint one channel row (used twice: L and R). */
static void lr_paint_channel(HDC hdc, int row_x, int row_y, int led_w, int led_h,
                             int gap, float peak_linear)
{
    int n_lit, i;
    float db;
    db = 20.0f * log10f(peak_linear > 1e-7f ? peak_linear : 1e-7f);
    if (db < -60.0f) db = -60.0f;
    if (db >   0.0f) db =  0.0f;
    n_lit = (int)floorf((db + 60.0f) / 60.0f * LR_LEDS_PER_CHANNEL + 0.5f);
    if (n_lit > LR_LEDS_PER_CHANNEL) n_lit = LR_LEDS_PER_CHANNEL;
    if (n_lit < 0)                   n_lit = 0;
    for (i = 0; i < LR_LEDS_PER_CHANNEL; i++) {
        int x = row_x + i * (led_w + gap);
        COLORREF c = lr_led_color(i);
        COLORREF paint = (i < n_lit) ? c : lr_dim(c, 0.12f);
        lr_draw_led(hdc, x, row_y, led_w, led_h, paint);
    }
}

static void lr_paint_splash_chrome(HWND hwnd, HDC hdc, int state)
{
    LrState *st = lr_get(hwnd);
    RECT rc;
    int w, h, dpi;
    HICON icon;
    HFONT oldf;
    const char *caption;
    char connect_buf[24];
    BOOL btn_enabled = TRUE;
    const char *btn_text;
    COLORREF btn_bg;
    RECT btn;
    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left; h = rc.bottom - rc.top;
    dpi = lr_dpi(hwnd);

    icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    if (!icon) icon = LoadIconA(NULL, IDI_APPLICATION);
    {
        RECT lrc;
        int logo_dim, logo_x, logo_y;
        lr_compute_logo_rect(hwnd, &lrc);
        logo_dim = lrc.right - lrc.left;
        logo_x = lrc.left;
        logo_y = lrc.top;
        /* Montreal Greek Radio logo, alpha-composited over the black
         * splash via GDI+ high-quality bicubic. Falls back to the app
         * icon only if the embedded PNG cannot be decoded. */
        if (!suite_logo_draw(hdc, IDR_LOGO_RADIO, logo_x, logo_y, logo_dim)
            && icon)
            DrawIconEx(hdc, logo_x, logo_y, icon, logo_dim, logo_dim, 0, NULL, DI_NORMAL);
        {
            RECT tr;
            HFONT cf = (state == LR_STATE_ERROR) ? st->font_error
                                                 : st->font_caption;
            if (state == LR_STATE_SPLASH) {
                caption = "Live Radio - Click to Start"; btn_text = "Start";
                btn_bg = RGB(0x34, 0xC7, 0x59);
            } else if (state == LR_STATE_LOADING) {
                /* Animated dots: connect_anim_frame cycles 0..3 on the
                 * connect timer. Each frame is padded with trailing spaces
                 * to a constant 13-char width ("Connecting" + 3) so the
                 * caption keeps the same logical length; the draw below
                 * left-anchors it inside a centered fixed-width box (see
                 * there) so "Connecting" stays pinned and only the dots
                 * vary. The off-screen mem DC is cleared to black each
                 * paint, so fewer-dot frames leave no ghost dots. */
                static const char *dots[LR_CONNECT_FRAMES] = {
                    "   ",   /* 0 dots + 3 trailing spaces */
                    ".  ",   /* 1 dot  + 2 trailing spaces */
                    ".. ",   /* 2 dots + 1 trailing space  */
                    "...",   /* 3 dots, no padding         */
                };
                int f = st->connect_anim_frame % LR_CONNECT_FRAMES;
                _snprintf(connect_buf, sizeof(connect_buf), "Connecting%s", dots[f]);
                connect_buf[sizeof(connect_buf) - 1] = '\0';
                caption = connect_buf; btn_text = "Start";
                btn_bg = RGB(0x34, 0xC7, 0x59); btn_enabled = FALSE;
            } else {
                caption = "Stream unavailable. Click to retry."; btn_text = "Retry";
                btn_bg = RGB(0xD7, 0x00, 0x15);
            }
            tr.left = 0; tr.right = w;
            tr.top = lrc.bottom + MulDiv(24, dpi, 96);
            tr.bottom = tr.top + MulDiv(40, dpi, 96);
            /* Cache the caption band so the connect timer can invalidate
             * just this strip (full width => the widest frame is never
             * clipped and narrower frames clear cleanly). */
            st->caption_rect = tr;
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);
            oldf = (HFONT)SelectObject(hdc, cf);
            if (state == LR_STATE_LOADING) {
                /* Pin "Connecting" so only the dots move. Trailing-space
                 * padding keeps a constant char count, but Segoe UI is
                 * proportional ('.' and ' ' advance widths differ), so
                 * DT_CENTER would still re-center each frame by a few px.
                 * Instead measure the widest frame once, center a fixed-
                 * width box, and draw left-aligned inside it -> the word's
                 * left edge is pixel-identical every frame. */
                RECT mr;
                int fw;
                mr.left = 0; mr.top = tr.top; mr.right = w; mr.bottom = tr.bottom;
                DrawTextA(hdc, "Connecting...", -1, &mr,
                          DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                fw = mr.right - mr.left;
                tr.left = (w - fw) / 2;
                DrawTextA(hdc, caption, -1, &tr,
                          DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
            } else {
                DrawTextA(hdc, caption, -1, &tr,
                          DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            SelectObject(hdc, oldf);
        }
    }
    lr_compute_button_rect(hwnd, &btn);
    st->btn_rect = btn;
    lr_draw_button(hdc, &btn, btn_bg, RGB(255, 255, 255),
                   st->btn_pressed, st->btn_hover, btn_enabled,
                   st->font_button, btn_text);
}

static void lr_paint(HWND hwnd, HDC hdc_target)
{
    LrState *st = lr_get(hwnd);
    RECT rc;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    int w, h, dpi;
    HBRUSH bk;
    RECT btn;
    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left; h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    dpi = lr_dpi(hwnd);

    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, h);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    bk = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(mem_dc, &rc, bk);
    SetBkMode(mem_dc, TRANSPARENT);

    if (st->state == LR_STATE_SPLASH ||
        st->state == LR_STATE_LOADING ||
        st->state == LR_STATE_ERROR) {
        lr_paint_splash_chrome(hwnd, mem_dc, st->state);
    } else if (st->state == LR_STATE_PLAYING) {
        /* Top 25%: the MGR logo, sized to the band (smaller than the
         * splash). High-quality GDI+ draw, app-icon fallback. */
        int logo_dim = MulDiv(LR_LOGO_PX, dpi, 96);
        int band_h   = h * 25 / 100;
        int logo_y   = rc.top + (band_h - logo_dim) / 2;
        int logo_x   = (w - logo_dim) / 2;
        if (!suite_logo_draw(mem_dc, IDR_LOGO_RADIO, logo_x, logo_y, logo_dim)) {
            HICON icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
            if (!icon) icon = LoadIconA(NULL, IDI_APPLICATION);
            if (icon)
                DrawIconEx(mem_dc, logo_x, logo_y, icon, logo_dim, logo_dim,
                           0, NULL, DI_NORMAL);
        }

        /* Middle 50%: stereo LED VU. */
        {
            RECT mr;
            int led_w, led_h, gap, ch_gap;
            int total_w, row_x, row_y_l, row_y_r;
            lr_compute_meter_rect(hwnd, &mr);
            st->meter_rect = mr;
            led_h = MulDiv(LR_LED_H, dpi, 96);
            gap   = MulDiv(LR_LED_GAP, dpi, 96);
            ch_gap = MulDiv(LR_METER_CH_GAP, dpi, 96);
            total_w = mr.right - mr.left;
            /* Derive led_w from total_w to honor lr_compute_meter_rect's shrink. */
            led_w = (total_w - (LR_LEDS_PER_CHANNEL - 1) * gap) / LR_LEDS_PER_CHANNEL;
            if (led_w < 1) led_w = 1;
            row_x   = mr.left;
            row_y_l = mr.top;
            row_y_r = row_y_l + led_h + ch_gap;
            lr_paint_channel(mem_dc, row_x, row_y_l, led_w, led_h, gap, st->peak_l);
            lr_paint_channel(mem_dc, row_x, row_y_r, led_w, led_h, gap, st->peak_r);
        }

        /* Bottom 25%: Stop button. */
        lr_compute_button_rect(hwnd, &btn);
        st->btn_rect = btn;
        lr_draw_button(mem_dc, &btn, RGB(0xD7, 0x00, 0x15), RGB(255, 255, 255),
                       st->btn_pressed, st->btn_hover, TRUE,
                       st->font_button, "Stop");
    }

    BitBlt(hdc_target, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);
}

/* ------------------------------------------------------------------ */
/* State transitions.                                                  */
/* ------------------------------------------------------------------ */

/* Fired by the radio engine on its worker thread. Marshal to the UI
 * thread (see WM_LR_ENGINE_STATE) so the meter + timer touch only the
 * window-owning thread. */
static void lr_engine_state_cb(REState s, const char *detail, void *user)
{
    HWND hwnd = (HWND)user;
    (void)detail;
    if (hwnd) PostMessageA(hwnd, WM_LR_ENGINE_STATE, (WPARAM)s, 0);
}

static void lr_enter_loading(HWND hwnd)
{
    LrState *st = lr_get(hwnd);
    if (!st) return;
    st->engine = re_create();
    if (!st->engine) {
        st->state = LR_STATE_ERROR;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    re_set_state_cb(st->engine, lr_engine_state_cb, (void *)hwnd);
    st->state = LR_STATE_LOADING;
    /* Start the "Connecting..." dot animation. Seed on the full
     * three-dot frame so the first paint matches the prior static
     * "Connecting..." caption (no pop before the first tick); the timer
     * then advances "" -> "." -> ".." -> "..." every 400 ms. */
    st->connect_anim_frame = LR_CONNECT_FRAMES - 1;
    SetTimer(hwnd, LR_CONNECT_TIMER_ID, LR_CONNECT_TIMER_MS, NULL);
    InvalidateRect(hwnd, NULL, FALSE);
    /* re_start is non-blocking: it spawns the engine's own worker thread
     * (network read + ADTS framing + AAC MFT decode + ring write) and
     * reports PLAYING / ERROR via the state callback. */
    if (!re_start(st->engine, LR_URL)) {
        re_destroy(st->engine);
        st->engine = NULL;
        KillTimer(hwnd, LR_CONNECT_TIMER_ID);
        st->state = LR_STATE_ERROR;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void lr_enter_stopped(HWND hwnd)
{
    LrState *st = lr_get(hwnd);
    if (!st) return;
    KillTimer(hwnd, LR_METER_TIMER_ID);
    KillTimer(hwnd, LR_CONNECT_TIMER_ID);
    if (st->meter)  { audio_meter_destroy(st->meter); st->meter = NULL; }
    if (st->engine) {
        re_stop(st->engine);
        re_destroy(st->engine);
        st->engine = NULL;
    }
    st->peak_l = st->peak_r = 0.0f;
    st->state = LR_STATE_SPLASH;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void lr_handle_main_action(HWND hwnd)
{
    LrState *st = lr_get(hwnd);
    if (!st) return;
    switch (st->state) {
    case LR_STATE_SPLASH:
    case LR_STATE_ERROR:
        lr_enter_loading(hwnd); break;
    case LR_STATE_LOADING:
        break;
    case LR_STATE_PLAYING:
        lr_enter_stopped(hwnd); break;
    }
}

/* ------------------------------------------------------------------ */
/* WndProc.                                                            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK LrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LrState *st;
    switch (msg) {
    case WM_CREATE: {
        int dpi = lr_dpi(hwnd);
        st = (LrState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LrState));
        if (!st) return -1;
        st->state = LR_STATE_SPLASH;
        st->font_caption = lr_make_font(18, FW_SEMIBOLD, dpi);
        st->font_error   = lr_make_font(14, FW_NORMAL,   dpi);
        st->font_button  = lr_make_font(12, FW_NORMAL,   dpi);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        audio_meter_init();
        return 0;
    }
    case WM_DESTROY:
        st = lr_get(hwnd);
        if (st) {
            KillTimer(hwnd, LR_METER_TIMER_ID);
            KillTimer(hwnd, LR_CONNECT_TIMER_ID);
            if (st->meter)  { audio_meter_destroy(st->meter); st->meter = NULL; }
            if (st->engine) {
                re_stop(st->engine);
                re_destroy(st->engine);
                st->engine = NULL;
            }
            if (st->font_caption) DeleteObject(st->font_caption);
            if (st->font_error)   DeleteObject(st->font_error);
            if (st->font_button)  DeleteObject(st->font_button);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        audio_meter_shutdown();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        lr_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == LR_METER_TIMER_ID) {
            st = lr_get(hwnd);
            if (st && st->state == LR_STATE_PLAYING && st->meter) {
                float pl = 0.0f, pr = 0.0f;
                if (audio_meter_poll(st->meter, &pl, &pr)) {
                    st->peak_l = pl; st->peak_r = pr;
                }
                /* Always invalidate even if poll failed so the dim
                 * LEDs are still drawn (poll returning FALSE leaves
                 * peaks at 0 which paints the dim background). */
                InvalidateRect(hwnd, &st->meter_rect, FALSE);
            }
        } else if (wp == LR_CONNECT_TIMER_ID) {
            st = lr_get(hwnd);
            /* Advance the dot cycle only while still connecting; invalidate
             * just the cached caption strip to avoid logo redraw thrash. */
            if (st && st->state == LR_STATE_LOADING) {
                st->connect_anim_frame =
                    (st->connect_anim_frame + 1) % LR_CONNECT_FRAMES;
                InvalidateRect(hwnd, &st->caption_rect, FALSE);
            }
        }
        return 0;
    case WM_LR_ENGINE_STATE:
        st = lr_get(hwnd);
        if (!st) return 0;
        switch ((REState)wp) {
        case RE_STATE_PLAYING:
            if (st->state != LR_STATE_PLAYING) {
                KillTimer(hwnd, LR_CONNECT_TIMER_ID);   /* stop dot anim */
                st->state = LR_STATE_PLAYING;
                if (!st->meter) st->meter = audio_meter_create();
                SetTimer(hwnd, LR_METER_TIMER_ID, LR_METER_TIMER_MS, NULL);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        case RE_STATE_ERROR:
            if (st->state != LR_STATE_ERROR) {
                KillTimer(hwnd, LR_CONNECT_TIMER_ID);   /* stop dot anim */
                st->state = LR_STATE_ERROR;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        default: break;
        }
        return 0;
    case WM_APP_RESIZE_ENDED:
        /* Audio-only: nothing to refresh on the player side. The
         * paint repaints on its own through the WM_SIZE chain. */
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        st = lr_get(hwnd);
        if (!st) return 0;
        SetFocus(hwnd);
        if (PtInRect(&st->btn_rect, pt)) {
            st->btn_pressed = TRUE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (st->state == LR_STATE_SPLASH || st->state == LR_STATE_ERROR) {
            lr_handle_main_action(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        st = lr_get(hwnd);
        if (!st) return 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (st->btn_pressed) {
            st->btn_pressed = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
            if (PtInRect(&st->btn_rect, pt))
                lr_handle_main_action(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        BOOL hover;
        st = lr_get(hwnd);
        if (!st) return 0;
        hover = PtInRect(&st->btn_rect, pt);
        if (hover != st->btn_hover) {
            st->btn_hover = hover;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_SPACE || wp == VK_RETURN) {
            lr_handle_main_action(hwnd);
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void lr_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LrProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = LR_CLASS;
    RegisterClassExA(&wc);
}

HWND liveradio_module_create(HWND parent)
{
    RECT rc = {0, 0, 100, 100};
    lr_register_class_once();
    if (parent) GetClientRect(parent, &rc);
    return CreateWindowExA(0, LR_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, NULL, GetModuleHandleA(NULL), NULL);
}
