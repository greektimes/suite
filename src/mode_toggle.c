/*
 * mode_toggle.c - iOS-style Modern / Retro toggle widget, GDI+ anti-aliased.
 *
 * GDI+ is accessed via the flat C entry points (mingw's gdiplus.h is C++
 * only; we declare the handful of entries we need by hand, mirroring the
 * pattern in web_module.c). Track shape is a stadium (rounded rect with
 * semicircular ends), thumb is a circle. Animation is a ~150 ms ease-out
 * quadratic driven by a 16 ms WM_TIMER.
 *
 * DPI-awareness: GetDpiForWindow at create time scales the base
 * dimensions. The widget paints into its full client rect so SetWindowPos
 * with the scaled size works without further code.
 */

#include "mode_toggle.h"
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* GDI+ flat-C decls (mingw gdiplus.h is C++ only).                    */
/* ------------------------------------------------------------------ */

typedef struct GpGraphics GpGraphics;
typedef struct GpBrush    GpBrush;
typedef struct GpPath     GpPath;
typedef int GpStatus;
typedef DWORD ARGB;

typedef struct MgtGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern GpStatus WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **g);
extern GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
extern GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
extern GpStatus WINAPI GdipCreateSolidFill(ARGB color, GpBrush **brush);
extern GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
typedef struct GpPen GpPen;
extern GpStatus WINAPI GdipCreatePen1(ARGB color, float width, int unit, GpPen **pen);
extern GpStatus WINAPI GdipDeletePen(GpPen *pen);
extern GpStatus WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *path);
extern GpStatus WINAPI GdipFillEllipseI(GpGraphics *g, GpBrush *brush,
                                        INT x, INT y, INT w, INT h);
extern GpStatus WINAPI GdipFillPath(GpGraphics *g, GpBrush *brush, GpPath *path);
extern GpStatus WINAPI GdipCreatePath(int fill_mode, GpPath **path);
extern GpStatus WINAPI GdipDeletePath(GpPath *path);
extern GpStatus WINAPI GdipAddPathArcI(GpPath *path, INT x, INT y, INT w, INT h,
                                       float start_angle, float sweep_angle);
extern GpStatus WINAPI GdipAddPathLineI(GpPath *path, INT x1, INT y1,
                                        INT x2, INT y2);
extern GpStatus WINAPI GdipClosePathFigure(GpPath *path);

#define SMOOTHING_MODE_ANTIALIAS 4

static ULONG_PTR g_mt_gdiplus_token = 0;

static void mt_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_mt_gdiplus_token) return;
    in.GdiplusVersion = 1;
    in.DebugEventCallback = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs = FALSE;
    GdiplusStartup(&g_mt_gdiplus_token, &in, NULL);
}

/* GetDpiForWindow is per-monitor v2 (Win 10 1607+). Fall back to system
 * DPI on older Windows. */
typedef UINT (WINAPI *GetDpiForWindow_t)(HWND);
static int mt_dpi(HWND hwnd)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    GetDpiForWindow_t fn = u32 ? (GetDpiForWindow_t)
        GetProcAddress(u32, "GetDpiForWindow") : NULL;
    if (fn) return (int)fn(hwnd);
    {
        HDC dc = GetDC(hwnd);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(hwnd, dc);
        return dpi ? dpi : 96;
    }
}

/* ------------------------------------------------------------------ */
/* Widget state.                                                       */
/* ------------------------------------------------------------------ */

typedef struct ModeToggle {
    int   mode;        /* MODE_MODERN / MODE_RETRO - committed value */
    int   anim_from;   /* mode the animation started from */
    int   anim_to;     /* mode the animation is heading to */
    float anim_t;      /* 0.0 .. 1.0 progress */
    BOOL  animating;
    int   dpi;
} ModeToggle;

#define MT_CLASS "MGTUnicornModeToggle"

/* Base (96 DPI) dimensions per dispatch C. */
#define MT_BASE_W  60
#define MT_BASE_H  28
#define MT_THUMB_BASE_DIA 24
#define MT_INSET_BASE 2
#define MT_ANIM_MS 150
#define MT_TIMER_ID 1
#define MT_TIMER_MS 16

static ModeToggle *mt_get(HWND hwnd)
{
    return (ModeToggle *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
}

static int mt_scale(int base, int dpi)
{
    return MulDiv(base, dpi ? dpi : 96, 96);
}

static float mt_ease_out(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u;
}

static ARGB mt_lerp_argb(ARGB a, ARGB b, float t)
{
    int ai, ri, gi, bi;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = (a >> 24) & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF, ba = (b >> 24) & 0xFF;
    ai = aa + (int)((float)(ba - aa) * t + 0.5f);
    ri = ar + (int)((float)(br - ar) * t + 0.5f);
    gi = ag + (int)((float)(bg - ag) * t + 0.5f);
    bi = ab + (int)((float)(bb - ab) * t + 0.5f);
    return ((ARGB)ai << 24) | ((ARGB)ri << 16) | ((ARGB)gi << 8) | (ARGB)bi;
}

/* ------------------------------------------------------------------ */
/* Paint.                                                              */
/* ------------------------------------------------------------------ */

#define COLOR_MODERN 0xFF34C759
#define COLOR_RETRO  0xFFC0C0C0
#define COLOR_THUMB  0xFFFFFFFF

static void mt_paint(HWND hwnd, HDC hdc_target)
{
    ModeToggle *st = mt_get(hwnd);
    RECT rc;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    int w, h, dia, inset, thumb_dia, thumb_y, thumb_x_left, thumb_x_right;
    float eased;
    int thumb_x;
    GpGraphics *g = NULL;
    GpBrush *br_track = NULL, *br_thumb = NULL;
    GpPath *path = NULL;
    ARGB track_color;

    if (!st) return;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    /* Memory-DC double buffer (mirrors the polish-1 / polish-fix pattern
     * recorded in the F6 / F3 lessons: own the bg fill on a memory DC
     * and BitBlt atomically). */
    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, h);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    /* Background fill for the corners outside the stadium. We paint the
     * SAME color the suite's title-bar strip uses behind us (suite_shell
     * WM_PAINT: white in Modern, RGB(238,238,238) in Retro). The strip
     * color tracks g_mode, and g_mode only flips at the end of our
     * animation (mt_finish_animation), by which point st->mode has flipped
     * too -- so st->mode matches the strip color in every state.
     *
     * This replaces an earlier approach that BitBlt'd the parent's live
     * pixels into the corners. That broke on restore-from-minimize: the
     * parent strip had not repainted yet, so the corners picked up black.
     * A solid fill keyed to the mode has no such timing dependency. */
    {
        COLORREF bgc = (st->mode == MODE_MODERN)
                       ? RGB(255, 255, 255)
                       : RGB(238, 238, 238);
        HBRUSH bgb = CreateSolidBrush(bgc);
        if (bgb) {
            FillRect(mem_dc, &rc, bgb);
            DeleteObject(bgb);
        }
    }

    /* Geometry. */
    dia       = h;                              /* track corner diameter = full height */
    inset     = mt_scale(MT_INSET_BASE, st->dpi);
    thumb_dia = h - 2 * inset;
    thumb_y   = inset;
    thumb_x_left  = inset;
    thumb_x_right = w - thumb_dia - inset;

    /* Track color is constant green (amendment 2026-05-26): the gray
     * Retro variant was removed to keep the cluster's color cue
     * consistent across mode changes. Mode is now signaled only by
     * thumb position relative to the two flanking labels. */
    eased = st->animating ? mt_ease_out(st->anim_t) : 1.0f;
    track_color = COLOR_MODERN;
    (void)mt_lerp_argb;  /* helper retained for future skinning */
    {
        int from_x = (st->animating
                      ? (st->anim_from == MODE_MODERN ? thumb_x_left : thumb_x_right)
                      : (st->mode == MODE_MODERN ? thumb_x_left : thumb_x_right));
        int to_x   = (st->animating
                      ? (st->anim_to == MODE_MODERN ? thumb_x_left : thumb_x_right)
                      : from_x);
        thumb_x = from_x + (int)((float)(to_x - from_x) * eased + 0.5f);
    }

    /* GDI+ paint of track fill + always-on solid black outline + thumb.
     * The stadium path is reused for fill and stroke so the outline
     * follows the same curve. The outline replaces the previous
     * focus-only PS_DOT rectangle (amendment 2026-05-26): a 1 px
     * black stroke, always visible, no focus state involved. */
    if (GdipCreateFromHDC(mem_dc, &g) == 0) {
        GpPen *pen_outline = NULL;
        GdipSetSmoothingMode(g, SMOOTHING_MODE_ANTIALIAS);

        if (GdipCreatePath(0, &path) == 0) {
            /* Stadium: single contiguous outline traced clockwise so
             * each segment's start point equals the previous segment's
             * end point. The previous segment order (arc -> line ->
             * arc -> line) left endpoints disconnected and GDI+ filled
             * those gaps with implicit lines, producing two visible
             * vertical bars inside the pill at the arc-to-line
             * junctions. The new order (line top -> arc right ->
             * line bottom -> arc left) chains naturally and the
             * outline is a single closed curve. w-1 / h-1 keeps the
             * 1 px stroke fully inside the widget rect. */
            int rw = w - 1;
            int rh = h - 1;
            int d  = rh;     /* arc diameter = full height-1 */
            GdipAddPathLineI(path, d / 2, 0, rw - d / 2, 0);
            GdipAddPathArcI (path, rw - d, 0, d, d, 270.0f, 180.0f);
            GdipAddPathLineI(path, rw - d / 2, rh, d / 2, rh);
            GdipAddPathArcI (path, 0, 0, d, d, 90.0f, 180.0f);
            GdipClosePathFigure(path);

            if (GdipCreateSolidFill(track_color, &br_track) == 0) {
                GdipFillPath(g, br_track, path);
                GdipDeleteBrush(br_track);
            }
            if (GdipCreatePen1(0xFF000000, 1.0f, 2 /*UnitPixel*/, &pen_outline) == 0) {
                GdipDrawPath(g, pen_outline, path);
                GdipDeletePen(pen_outline);
            }
            GdipDeletePath(path);
        }

        if (GdipCreateSolidFill(COLOR_THUMB, &br_thumb) == 0) {
            GdipFillEllipseI(g, br_thumb, thumb_x, thumb_y, thumb_dia, thumb_dia);
            GdipDeleteBrush(br_thumb);
        }

        GdipDeleteGraphics(g);
    }

    BitBlt(hdc_target, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);
}

/* ------------------------------------------------------------------ */
/* Animation + click + key handling.                                   */
/* ------------------------------------------------------------------ */

static void mt_begin_animate_to(HWND hwnd, int new_mode)
{
    ModeToggle *st = mt_get(hwnd);
    if (!st) return;
    /* If already animating, reverse smoothly: snap anim_from to current
     * visual position by inverting t from the prior target. */
    if (st->animating && new_mode == st->anim_from) {
        st->anim_t = 1.0f - st->anim_t;
        st->anim_from = st->anim_to;
        st->anim_to = new_mode;
    } else if (st->animating) {
        st->anim_t = 0.0f;
        st->anim_from = st->mode;
        st->anim_to = new_mode;
    } else {
        st->anim_t = 0.0f;
        st->anim_from = st->mode;
        st->anim_to = new_mode;
        st->animating = TRUE;
        SetTimer(hwnd, MT_TIMER_ID, MT_TIMER_MS, NULL);
    }
}

static void mt_finish_animation(HWND hwnd, int final_mode)
{
    ModeToggle *st = mt_get(hwnd);
    if (!st) return;
    KillTimer(hwnd, MT_TIMER_ID);
    st->animating = FALSE;
    st->mode = final_mode;
    st->anim_t = 1.0f;
    InvalidateRect(hwnd, NULL, FALSE);
    PostMessageA(GetParent(hwnd), WM_USER_MODE_CHANGED, (WPARAM)final_mode, 0);
}

static void mt_toggle(HWND hwnd)
{
    ModeToggle *st = mt_get(hwnd);
    int target;
    if (!st) return;
    target = (st->animating ? st->anim_to : st->mode);
    target = (target == MODE_MODERN) ? MODE_RETRO : MODE_MODERN;
    mt_begin_animate_to(hwnd, target);
}

static LRESULT CALLBACK ModeToggleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ModeToggle *st;

    switch (msg) {
    case WM_CREATE: {
        st = (ModeToggle *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(ModeToggle));
        if (!st) return -1;
        st->mode = MODE_MODERN;
        st->anim_from = st->anim_to = MODE_MODERN;
        st->anim_t = 1.0f;
        st->dpi = mt_dpi(hwnd);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        mt_ensure_gdiplus();
        return 0;
    }

    case WM_DESTROY:
        st = mt_get(hwnd);
        if (st) {
            KillTimer(hwnd, MT_TIMER_ID);
            HeapFree(GetProcessHeap(), 0, st);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        }
        return 0;

    case WM_ERASEBKGND:
        /* Paint handler does the full surface including background blit
         * from parent; suppress the default erase to avoid flicker. */
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        mt_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wp == MT_TIMER_ID) {
            st = mt_get(hwnd);
            if (st && st->animating) {
                st->anim_t += (float)MT_TIMER_MS / (float)MT_ANIM_MS;
                if (st->anim_t >= 1.0f) {
                    mt_finish_animation(hwnd, st->anim_to);
                } else {
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
        }
        return 0;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        mt_toggle(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_SPACE || wp == VK_RETURN) {
            mt_toggle(hwnd);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        /* Per F6 polish-4 rule: custom-painted child windows in the suite
         * MUST handle WM_GETDLGCODE with DLGC_WANTALLKEYS, otherwise
         * IsDialogMessage filters out letter / arrow / space keys. We
         * want Space and Enter delivered as WM_KEYDOWN. */
        return DLGC_WANTALLKEYS;

    /* WM_SETFOCUS / WM_KILLFOCUS deliberately fall through to
     * DefWindowProc (amendment 2026-05-26): the outline is now
     * always-on and does not change on focus, so no repaint is
     * needed when keyboard focus enters or leaves the toggle. */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

static void mt_register_class_once(void)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ModeToggleProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
    wc.hbrBackground = NULL;     /* WM_ERASEBKGND swallows it */
    wc.lpszClassName = MT_CLASS;
    RegisterClassExA(&wc);
}

HWND mode_toggle_create(HWND parent, int x, int y, int id)
{
    int dpi;
    HWND hwnd;
    mt_register_class_once();
    dpi = parent ? mt_dpi(parent) : 96;
    hwnd = CreateWindowExA(0, MT_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        x, y,
        mt_scale(MT_BASE_W, dpi), mt_scale(MT_BASE_H, dpi),
        parent, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    return hwnd;
}

int mode_toggle_get_mode(HWND hwnd)
{
    ModeToggle *st = mt_get(hwnd);
    if (!st) return MODE_MODERN;
    return st->animating ? st->anim_to : st->mode;
}

void mode_toggle_set_mode(HWND hwnd, int mode, BOOL animate)
{
    ModeToggle *st = mt_get(hwnd);
    if (!st) return;
    if (mode != MODE_MODERN && mode != MODE_RETRO) return;
    if (animate) {
        if (mode != (st->animating ? st->anim_to : st->mode))
            mt_begin_animate_to(hwnd, mode);
    } else {
        KillTimer(hwnd, MT_TIMER_ID);
        st->animating = FALSE;
        st->mode = mode;
        st->anim_from = st->anim_to = mode;
        st->anim_t = 1.0f;
        InvalidateRect(hwnd, NULL, FALSE);
        PostMessageA(GetParent(hwnd), WM_USER_MODE_CHANGED, (WPARAM)mode, 0);
    }
}
