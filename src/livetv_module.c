/*
 * livetv_module.c - Live TV pane (SPLASH / LOADING / PLAYING / ERROR).
 *
 * State machine:
 *   SPLASH  : black bg, Greek Times icon centered, "Live TV - Click to
 *             Start" overlay, Start button below. Click anywhere or
 *             press Space to enter LOADING.
 *   LOADING : same chrome, caption "Connecting...", Start disabled.
 *             player_service is created, HLS load started.
 *   PLAYING : player's video child fills the pane minus a 44 px bottom
 *             control bar. Stop button bottom-center. Space stops.
 *   ERROR   : black bg, "Stream unavailable. Click to retry." Retry
 *             button. Click anywhere or Space goes to LOADING.
 *
 * The shell hosts player_service: the player creates its own video
 * child HWND inside this pane and is letterboxed via player_resize
 * to the (pane_w, pane_h - 44) rect when in PLAYING.
 *
 * WM_PLAYER_ENGINE_EVENT delivered to this pane is forwarded to
 * player_handle_window_event, which dispatches to lt_player_state_cb.
 */

#include "livetv_module.h"
#include "player_service.h"
#include "suite_shell.h"   /* WM_APP_RESIZE_ENDED + suite_shell_in_user_resize */
#include "suite_logo.h"    /* high-quality GDI+ logo draw from RCDATA */
#include "suite_res.h"     /* IDR_LOGO_TV */
#include <stdio.h>

#define LT_CLASS "MGTUnicornLiveTV"
#define LT_VIDEO_CLASS "MGTUnicornLiveTVVideo"
#define LT_HLS_URL L"https://live.greektv.ca/hls1/greektv.m3u8"

#define LT_CONTROL_BAR_H 44
#define LT_LOGO_PX       96
#define LT_BTN_W         120
#define LT_BTN_H         36
#define LT_BTN_RADIUS    4

/* "Connecting..." caption animation (LOADING state only). 4-frame cycle
 * "Connecting" / "." / ".." / "..." at 400 ms each (1.6 s loop), driven
 * by a module-private timer. Mirrors liveradio_module. */
#define LT_CONNECT_TIMER_ID    1
#define LT_CONNECT_TIMER_MS  300
#define LT_CONNECT_FRAMES      4

#define LT_STATE_SPLASH   0
#define LT_STATE_LOADING  1
#define LT_STATE_PLAYING  2
#define LT_STATE_ERROR    3

typedef struct LtState {
    int     state;
    Player *player;
    HWND    video_hwnd;       /* the MF_MEDIA_ENGINE_PLAYBACK_HWND surface */
    int     aspect_w;         /* cached native width  at LOADEDMETADATA */
    int     aspect_h;         /* cached native height at LOADEDMETADATA */
    HFONT   font_caption;     /* 18 pt Segoe UI Semibold for splash text */
    HFONT   font_error;       /* 14 pt Segoe UI for error text */
    HFONT   font_button;      /* 12 pt Segoe UI for button labels */
    RECT    btn_rect;         /* current main-button hit rect */
    RECT    caption_rect;     /* cached caption region for tight invalidate */
    int     connect_anim_frame; /* 0..3 dot count, "Connecting" animation */
    BOOL    btn_pressed;
    BOOL    btn_hover;
    HANDLE  load_thread;      /* worker that calls player_load_hls + play */
} LtState;

static LtState *lt_get(HWND h) { return (LtState *)GetWindowLongPtrA(h, GWLP_USERDATA); }

static int lt_dpi(HWND h)
{
    typedef UINT (WINAPI *F)(HWND);
    HMODULE u = GetModuleHandleA("user32.dll");
    F f = u ? (F)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (f) return (int)f(h);
    {
        HDC dc = GetDC(h);
        int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(h, dc);
        return d ? d : 96;
    }
}

static HFONT lt_make_font(int pt, int weight, int dpi)
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
/* Video host child window class.                                      */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK LtVideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        /* MF owns the swap chain; only visible in the brief window
         * before the first frame lands. Paint black so any flash
         * matches the surrounding letterbox bars. */
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void lt_register_video_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LtVideoProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = LT_VIDEO_CLASS;
    RegisterClassExA(&wc);
}

/* ------------------------------------------------------------------ */
/* Layout helpers.                                                     */
/* ------------------------------------------------------------------ */

static int lt_video_area_h(HWND hwnd, int client_h)
{
    int bar = MulDiv(LT_CONTROL_BAR_H, lt_dpi(hwnd), 96);
    int h = client_h - bar;
    return h < 0 ? 0 : h;
}

/* Phase L core (amended for Phase P / amendment 3): compute the
 * inscribed video rect inside the available area (client minus the
 * 44 px control strip) using the cached aspect. Centered. When the
 * aspect is NOT yet known (called pre-LOADEDMETADATA, e.g. right
 * after lt_enter_loading creates the player), fill the full available
 * area so MF binds its DXGI swap chain to a properly-sized surface
 * instead of the 1x1 starter rect the video child is created with. */
static void lt_layout_video(HWND hwnd)
{
    LtState *st = lt_get(hwnd);
    RECT rc;
    int dpi, bar_h, avail_w, avail_h;
    int video_w, video_h, video_x, video_y;

    if (!st || !st->video_hwnd) return;

    GetClientRect(hwnd, &rc);
    dpi = lt_dpi(hwnd);
    bar_h = MulDiv(LT_CONTROL_BAR_H, dpi, 96);
    avail_w = rc.right - rc.left;
    avail_h = (rc.bottom - rc.top) - bar_h;
    if (avail_w <= 0 || avail_h <= 0) return;

    if (st->aspect_w <= 0 || st->aspect_h <= 0) {
        /* Aspect not yet cached: fill the full available area so MF
         * has a real swap-chain surface to bind at player_create time.
         * Once LOADEDMETADATA arrives we relayout to the inscribed
         * rect with the proper aspect. */
        SetWindowPos(st->video_hwnd, NULL,
                     0, 0, avail_w, avail_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    if ((long long)avail_w * st->aspect_h >
        (long long)avail_h * st->aspect_w) {
        /* pillarbox: full height, narrower width. */
        video_h = avail_h;
        video_w = (int)(((long long)avail_h * st->aspect_w) / st->aspect_h);
    } else {
        /* letterbox: full width, shorter height. */
        video_w = avail_w;
        video_h = (int)(((long long)avail_w * st->aspect_h) / st->aspect_w);
    }
    video_x = (avail_w - video_w) / 2;
    video_y = (avail_h - video_h) / 2;

    SetWindowPos(st->video_hwnd, NULL,
                 video_x, video_y, video_w, video_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    /* Swap-chain refresh policy (amendments 3 + 4):
     *   - Amendment 3 added UpdateVideoStream here because MF's
     *     auto-rebind fails on large discrete size jumps (Win11
     *     maximize from a small window).
     *   - Amendment 4 gates it: during a continuous user drag,
     *     WM_SIZE fires faster than MF can finish each rebuild and
     *     the swap chain never gets to present, producing solid
     *     black for the duration of the drag. The shell sets a flag
     *     across WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE; while set, we
     *     skip UpdateVideoStream and let MF auto-handle the small
     *     incremental resizes. WM_APP_RESIZE_ENDED (handled below)
     *     fires one explicit refresh after the drag completes.
     *     Maximize / restore / programmatic SetWindowPos do not
     *     enter ENTERSIZEMOVE, so they still get the per-resize
     *     refresh that amendment 3 needs. */
    if (st->player && !suite_shell_in_user_resize())
        player_update_video_stream(st->player);
}

/* Splash logo geometry: a responsive square ~32% of the shorter content
 * dimension (clamped 120..300 px at 96 DPI), centered horizontally. The
 * logo, caption and Start button form one vertically-centered block so
 * the larger MGTV logo never collides with the controls below it. Both
 * the painter and the button hit-rect derive from this helper. (Mirrors
 * lr_compute_logo_rect in liveradio_module.c.) */
static void lt_compute_logo_rect(HWND hwnd, RECT *out)
{
    RECT rc;
    int w, h, dpi, shorter, dim, max_dim, min_dim, block_h, top, min_top;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    dpi = lt_dpi(hwnd);
    shorter = (w < h) ? w : h;
    dim = shorter * 32 / 100;
    max_dim = MulDiv(300, dpi, 96);
    min_dim = MulDiv(120, dpi, 96);
    if (dim > max_dim) dim = max_dim;
    if (dim < min_dim) dim = min_dim;
    block_h = dim + MulDiv(60 + LT_BTN_H, dpi, 96);
    top = rc.top + (h - block_h) / 2;
    min_top = rc.top + MulDiv(20, dpi, 96);
    if (top < min_top) top = min_top;
    out->left   = rc.left + (w - dim) / 2;
    out->top    = top;
    out->right  = out->left + dim;
    out->bottom = out->top + dim;
}

static void lt_compute_button_rect(HWND hwnd, RECT *out)
{
    LtState *st = lt_get(hwnd);
    RECT rc;
    int dpi = lt_dpi(hwnd);
    int bw = MulDiv(LT_BTN_W, dpi, 96);
    int bh = MulDiv(LT_BTN_H, dpi, 96);
    int cx, cy;
    GetClientRect(hwnd, &rc);
    cx = (rc.right - rc.left - bw) / 2;
    if (st && st->state == LT_STATE_PLAYING) {
        cy = rc.bottom - MulDiv(LT_CONTROL_BAR_H, dpi, 96)
             + (MulDiv(LT_CONTROL_BAR_H, dpi, 96) - bh) / 2;
    } else {
        /* Splash / Loading / Error: button 60 px below the responsive
         * logo block (see lt_compute_logo_rect). */
        RECT lrc;
        lt_compute_logo_rect(hwnd, &lrc);
        cy = lrc.bottom + MulDiv(60, dpi, 96);
    }
    out->left = cx;
    out->top = cy;
    out->right = cx + bw;
    out->bottom = cy + bh;
}

/* ------------------------------------------------------------------ */
/* Paint.                                                              */
/* ------------------------------------------------------------------ */

static void lt_draw_button(HDC hdc, const RECT *rc, COLORREF bg, COLORREF fg,
                           BOOL pressed, BOOL hover, BOOL enabled,
                           HFONT font, const char *label)
{
    HBRUSH br;
    HPEN pen, oldp;
    HBRUSH oldb;
    HFONT oldf;
    COLORREF actual_bg = bg;
    int r = LT_BTN_RADIUS;
    int adj_r, h, w;

    if (!enabled) {
        actual_bg = RGB(140, 140, 140);
        fg        = RGB(240, 240, 240);
    } else if (pressed) {
        actual_bg = RGB(GetRValue(bg) * 7 / 10,
                        GetGValue(bg) * 7 / 10,
                        GetBValue(bg) * 7 / 10);
    } else if (hover) {
        actual_bg = RGB(GetRValue(bg) * 9 / 10,
                        GetGValue(bg) * 9 / 10,
                        GetBValue(bg) * 9 / 10);
    }

    h = rc->bottom - rc->top;
    w = rc->right - rc->left;
    adj_r = r * 2;
    if (adj_r > h) adj_r = h;
    if (adj_r > w) adj_r = w;

    br = CreateSolidBrush(actual_bg);
    pen = CreatePen(PS_NULL, 0, actual_bg);
    oldb = (HBRUSH)SelectObject(hdc, br);
    oldp = (HPEN)SelectObject(hdc, pen);
    RoundRect(hdc, rc->left, rc->top, rc->right, rc->bottom, adj_r, adj_r);
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(br);
    DeleteObject(pen);

    SetTextColor(hdc, fg);
    SetBkMode(hdc, TRANSPARENT);
    oldf = (HFONT)SelectObject(hdc, font);
    DrawTextA(hdc, label, -1, (RECT *)rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, oldf);
}

static void lt_paint(HWND hwnd, HDC hdc_target)
{
    LtState *st = lt_get(hwnd);
    RECT rc;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    int w, h, dpi;
    HBRUSH bk;
    HICON icon;
    HFONT oldf;
    RECT btn;

    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    dpi = lt_dpi(hwnd);

    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, h);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    /* All states paint black bg (PLAYING leaves the video child to paint
     * its own rect; the surrounding letterbox / control bar remain black). */
    bk = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(mem_dc, &rc, bk);

    SetBkMode(mem_dc, TRANSPARENT);

    if (st->state == LT_STATE_SPLASH ||
        st->state == LT_STATE_LOADING ||
        st->state == LT_STATE_ERROR) {

        const char *caption  = NULL;
        const char *btn_text = NULL;
        char connect_buf[24];
        BOOL btn_enabled = TRUE;

        if (st->state == LT_STATE_SPLASH) {
            caption  = "Live TV - Click to Start";
            btn_text = "Start";
        } else if (st->state == LT_STATE_LOADING) {
            /* Animated dots: connect_anim_frame cycles 0..3 on the connect
             * timer. Each frame is padded with trailing spaces to a
             * constant 13-char width ("Connecting" + 3) so the caption
             * keeps the same logical length; the draw below left-anchors
             * it inside a centered fixed-width box (see there) so
             * "Connecting" stays pinned and only the dots vary. The mem DC
             * is cleared to black each paint, so fewer-dot frames leave no
             * ghosts. */
            static const char *dots[LT_CONNECT_FRAMES] = {
                "   ",   /* 0 dots + 3 trailing spaces */
                ".  ",   /* 1 dot  + 2 trailing spaces */
                ".. ",   /* 2 dots + 1 trailing space  */
                "...",   /* 3 dots, no padding         */
            };
            int f = st->connect_anim_frame % LT_CONNECT_FRAMES;
            _snprintf(connect_buf, sizeof(connect_buf), "Connecting%s", dots[f]);
            connect_buf[sizeof(connect_buf) - 1] = '\0';
            caption  = connect_buf;
            btn_text = "Start";
            btn_enabled = FALSE;
        } else {
            caption  = "Stream unavailable. Click to retry.";
            btn_text = "Retry";
        }

        icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
        if (!icon) icon = LoadIconA(NULL, IDI_APPLICATION);
        {
            RECT lrc;
            int logo_dim, logo_x, logo_y;
            lt_compute_logo_rect(hwnd, &lrc);
            logo_dim = lrc.right - lrc.left;
            logo_x = lrc.left;
            logo_y = lrc.top;
            /* Montreal Greek TV logo, alpha-composited over the black
             * splash via GDI+ high-quality bicubic. This is only the
             * pre-playback splash; the WinRT MediaPlayer video child
             * still owns the pane once PLAYING. App-icon fallback. */
            if (!suite_logo_draw(mem_dc, IDR_LOGO_TV, logo_x, logo_y, logo_dim)
                && icon)
                DrawIconEx(mem_dc, logo_x, logo_y, icon, logo_dim, logo_dim,
                           0, NULL, DI_NORMAL);

            /* Caption 24 px below logo. */
            {
                RECT tr;
                int caption_pt = (st->state == LT_STATE_ERROR) ? 14 : 18;
                HFONT cf = (st->state == LT_STATE_ERROR) ? st->font_error
                                                         : st->font_caption;
                tr.left = 0; tr.right = w;
                tr.top = lrc.bottom + MulDiv(24, dpi, 96);
                tr.bottom = tr.top + MulDiv(40, dpi, 96);
                /* Cache the caption band so the connect timer can
                 * invalidate just this strip (full width => the widest
                 * frame is never clipped; narrower frames clear). */
                st->caption_rect = tr;
                SetTextColor(mem_dc, RGB(255, 255, 255));
                oldf = (HFONT)SelectObject(mem_dc, cf);
                if (st->state == LT_STATE_LOADING) {
                    /* Pin "Connecting" so only the dots move. Trailing-
                     * space padding keeps a constant char count, but Segoe
                     * UI is proportional ('.' and ' ' advance widths
                     * differ), so DT_CENTER would still re-center each
                     * frame by a few px. Instead measure the widest frame
                     * once, center a fixed-width box, and draw left-aligned
                     * inside it -> the word's left edge is pixel-identical
                     * every frame. */
                    RECT mr;
                    int fw;
                    mr.left = 0; mr.top = tr.top; mr.right = w; mr.bottom = tr.bottom;
                    DrawTextA(mem_dc, "Connecting...", -1, &mr,
                              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                    fw = mr.right - mr.left;
                    tr.left = (w - fw) / 2;
                    DrawTextA(mem_dc, caption, -1, &tr,
                              DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
                } else {
                    DrawTextA(mem_dc, caption, -1, &tr,
                              DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                SelectObject(mem_dc, oldf);
                (void)caption_pt;
            }
        }

        lt_compute_button_rect(hwnd, &btn);
        st->btn_rect = btn;
        {
            COLORREF bg = (st->state == LT_STATE_ERROR)
                          ? RGB(0xD7, 0x00, 0x15)
                          : RGB(0x34, 0xC7, 0x59);
            lt_draw_button(mem_dc, &btn, bg, RGB(255, 255, 255),
                           st->btn_pressed, st->btn_hover, btn_enabled,
                           st->font_button, btn_text);
        }
    } else if (st->state == LT_STATE_PLAYING) {
        /* The video child overlays the upper area. Paint only the
         * control bar background here (already black) and the Stop
         * button centered in it. */
        lt_compute_button_rect(hwnd, &btn);
        st->btn_rect = btn;
        lt_draw_button(mem_dc, &btn,
                       RGB(0xD7, 0x00, 0x15), RGB(255, 255, 255),
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

static void lt_player_state_cb(Player *p, PlayerState s, void *user)
{
    HWND hwnd = (HWND)user;
    LtState *st;
    int nw = 0, nh = 0;
    if (!hwnd) return;
    st = lt_get(hwnd);
    if (!st) return;
    switch (s) {
    case PLAYER_STATE_LOADED:
        /* Cache the native aspect ratio ONCE here so every subsequent
         * WM_SIZE can letterbox without poking IMFMediaEngine. */
        if (st->aspect_w <= 0 || st->aspect_h <= 0) {
            if (player_get_native_size(p, &nw, &nh) && nw > 0 && nh > 0) {
                st->aspect_w = nw;
                st->aspect_h = nh;
            }
        }
        lt_layout_video(hwnd);
        break;
    case PLAYER_STATE_PLAYING:
        if (st->state != LT_STATE_PLAYING) {
            KillTimer(hwnd, LT_CONNECT_TIMER_ID);   /* stop dot anim */
            st->state = LT_STATE_PLAYING;
            /* Dispatch amendment 2026-06-15-6 second half: SW_SHOW
             * the video child here so the user sees frames in
             * lockstep with audible audio. player_service's muted
             * warm-up ends just before this callback fires (the
             * synthetic PLAYING is posted right after SetMuted(FALSE)),
             * so SW_SHOW and audio go live at the same moment. */
            if (st->video_hwnd) ShowWindow(st->video_hwnd, SW_SHOW);
            InvalidateRect(hwnd, NULL, FALSE);
            lt_layout_video(hwnd);
        }
        break;
    case PLAYER_STATE_ERROR:
        if (st->state != LT_STATE_ERROR) {
            KillTimer(hwnd, LT_CONNECT_TIMER_ID);   /* stop dot anim */
            st->state = LT_STATE_ERROR;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;
    case PLAYER_STATE_STOPPED:
        /* Triggered by player_stop; the user-initiated Stop path resets
         * to SPLASH below. */
        break;
    default:
        break;
    }
}

/* Worker thread for the synchronous load_hls + play. Keeps the UI thread
 * responsive (load_hls blocks up to 15 s on metadata). */
static DWORD WINAPI lt_load_worker(LPVOID arg)
{
    HWND hwnd = (HWND)arg;
    LtState *st = lt_get(hwnd);
    if (!st) return 0;
    if (!st->player) return 0;
    if (player_load_hls(st->player, LT_HLS_URL)) {
        player_play(st->player);
    } else {
        /* Surface error to UI (cb already fired ERROR, but be defensive). */
        st->state = LT_STATE_ERROR;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return 0;
}

static void lt_enter_loading(HWND hwnd)
{
    LtState *st = lt_get(hwnd);
    if (!st) return;
    if (!st->video_hwnd) {
        st->state = LT_STATE_ERROR;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (!player_init()) {
        st->state = LT_STATE_ERROR;
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    /* Phase P fix: size the video host to the full available area
     * BEFORE handing it to player_create, so MF binds its DXGI swap
     * chain to a useful surface instead of the 1x1 default. The child
     * is currently 1x1 from WM_CREATE. Order matters:
     *   (1) ShowWindow so the child is visible,
     *   (2) lt_layout_video so it fills the available area (aspect is
     *       0 here, so the helper hits the "fill" branch),
     *   (3) player_create binds MF to the correctly-sized HWND.
     * When LOADEDMETADATA arrives lt_player_state_cb calls
     * lt_layout_video again and the helper reshrinks to the inscribed
     * letterbox / pillarbox rect. */
    ShowWindow(st->video_hwnd, SW_SHOW);
    lt_layout_video(hwnd);
    st->player = player_create(st->video_hwnd);
    if (!st->player) {
        ShowWindow(st->video_hwnd, SW_HIDE);
        st->state = LT_STATE_ERROR;
        player_shutdown();
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    /* Dispatch amendment 2026-06-15-6: now that MF has bound its DXGI
     * swap chain to the video child (the Phase P SW_SHOW + lt_layout_video
     * above), hide the child again for the duration of the load + muted
     * warm-up. Without this, MF renders frames into the visible child as
     * soon as the warm-up Play() lands and the user sees video while
     * audio is still muted -- the symptom Dimitri reported at the
     * 2026-06-15-5 re-gate. lt_player_state_cb's PLAYER_STATE_PLAYING
     * branch SW_SHOWs the child again after player_service posts its
     * synthetic PLAYING, so video and audio start in lockstep. */
    ShowWindow(st->video_hwnd, SW_HIDE);
    player_set_state_cb(st->player, lt_player_state_cb, (void *)hwnd);

    st->state = LT_STATE_LOADING;
    /* Start the "Connecting..." dot animation. Seed on the full three-dot
     * frame so the first paint matches the prior static "Connecting..."
     * caption (no pop before the first tick); the timer then advances
     * "" -> "." -> ".." -> "..." every 400 ms. */
    st->connect_anim_frame = LT_CONNECT_FRAMES - 1;
    SetTimer(hwnd, LT_CONNECT_TIMER_ID, LT_CONNECT_TIMER_MS, NULL);
    InvalidateRect(hwnd, NULL, FALSE);

    if (st->load_thread) { CloseHandle(st->load_thread); st->load_thread = NULL; }
    st->load_thread = CreateThread(NULL, 0, lt_load_worker, (LPVOID)hwnd, 0, NULL);
}

static void lt_enter_splash(HWND hwnd)
{
    LtState *st = lt_get(hwnd);
    if (!st) return;
    KillTimer(hwnd, LT_CONNECT_TIMER_ID);   /* defensive: stop dot anim */
    if (st->player) {
        player_stop(st->player);
        player_destroy(st->player);
        st->player = NULL;
        player_shutdown();
    }
    if (st->video_hwnd) ShowWindow(st->video_hwnd, SW_HIDE);
    st->aspect_w = st->aspect_h = 0;
    if (st->load_thread) {
        WaitForSingleObject(st->load_thread, 500);
        CloseHandle(st->load_thread);
        st->load_thread = NULL;
    }
    st->state = LT_STATE_SPLASH;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void lt_handle_main_action(HWND hwnd)
{
    LtState *st = lt_get(hwnd);
    if (!st) return;
    switch (st->state) {
    case LT_STATE_SPLASH:
    case LT_STATE_ERROR:
        lt_enter_loading(hwnd);
        break;
    case LT_STATE_LOADING:
        /* Disabled in this state; ignore. */
        break;
    case LT_STATE_PLAYING:
        lt_enter_splash(hwnd);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* WndProc.                                                            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK LiveTvProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LtState *st;
    switch (msg) {
    case WM_CREATE: {
        int dpi = lt_dpi(hwnd);
        st = (LtState *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(LtState));
        if (!st) return -1;
        st->state = LT_STATE_SPLASH;
        st->font_caption = lt_make_font(18, FW_SEMIBOLD, dpi);
        st->font_error   = lt_make_font(14, FW_NORMAL,   dpi);
        st->font_button  = lt_make_font(12, FW_NORMAL,   dpi);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        /* Video host child: created hidden, shown only in LOADING /
         * PLAYING. Its size is governed by lt_layout_video. */
        lt_register_video_class_once();
        st->video_hwnd = CreateWindowExA(0, LT_VIDEO_CLASS, "",
            WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 1, 1, hwnd, NULL, GetModuleHandleA(NULL), NULL);
        return 0;
    }

    case WM_DESTROY:
        st = lt_get(hwnd);
        if (st) {
            KillTimer(hwnd, LT_CONNECT_TIMER_ID);
            if (st->player) {
                player_stop(st->player);
                player_destroy(st->player);
                st->player = NULL;
                player_shutdown();
            }
            if (st->video_hwnd) { DestroyWindow(st->video_hwnd); st->video_hwnd = NULL; }
            if (st->load_thread) {
                WaitForSingleObject(st->load_thread, 500);
                CloseHandle(st->load_thread);
            }
            if (st->font_caption) DeleteObject(st->font_caption);
            if (st->font_error)   DeleteObject(st->font_error);
            if (st->font_button)  DeleteObject(st->font_button);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        lt_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SIZE:
        (void)lp;  /* lp carries client w/h but lt_layout_video uses GetClientRect */
        st = lt_get(hwnd);
        if (st && (st->state == LT_STATE_LOADING || st->state == LT_STATE_PLAYING))
            lt_layout_video(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == LT_CONNECT_TIMER_ID) {
            st = lt_get(hwnd);
            /* Advance the dot cycle only while still connecting; invalidate
             * just the cached caption strip to avoid logo redraw thrash. */
            if (st && st->state == LT_STATE_LOADING) {
                st->connect_anim_frame =
                    (st->connect_anim_frame + 1) % LT_CONNECT_FRAMES;
                InvalidateRect(hwnd, &st->caption_rect, FALSE);
            }
        }
        return 0;

    case WM_PLAYER_ENGINE_EVENT:
        return player_handle_window_event(wp, lp);

    case WM_APP_RESIZE_ENDED:
        /* Drag-resize ended. Force the final swap-chain rebuild at
         * the stabilized size; gated UpdateVideoStream was suppressed
         * during the drag. lt_layout_video re-runs the SetWindowPos
         * and (now that suite_shell_in_user_resize is FALSE) calls
         * player_update_video_stream. */
        lt_layout_video(hwnd);
        return 0;

    case WM_LBUTTONDOWN: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        st = lt_get(hwnd);
        if (!st) return 0;
        SetFocus(hwnd);
        if (PtInRect(&st->btn_rect, pt)) {
            st->btn_pressed = TRUE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (st->state == LT_STATE_SPLASH || st->state == LT_STATE_ERROR) {
            /* Click anywhere in splash/error triggers main action. */
            lt_handle_main_action(hwnd);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        st = lt_get(hwnd);
        if (!st) return 0;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (st->btn_pressed) {
            st->btn_pressed = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
            if (PtInRect(&st->btn_rect, pt))
                lt_handle_main_action(hwnd);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = {(int)(short)LOWORD(lp), (int)(short)HIWORD(lp)};
        BOOL hover;
        st = lt_get(hwnd);
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
            lt_handle_main_action(hwnd);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        /* F6 polish-4 rule: own all keys so Space/Enter are delivered. */
        return DLGC_WANTALLKEYS;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void lt_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = LiveTvProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = LT_CLASS;
    RegisterClassExA(&wc);
}

HWND livetv_module_create(HWND parent)
{
    RECT rc = {0, 0, 100, 100};
    lt_register_class_once();
    if (parent) GetClientRect(parent, &rc);
    return CreateWindowExA(0, LT_CLASS, "",
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        parent, NULL, GetModuleHandleA(NULL), NULL);
}
