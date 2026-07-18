/*
 * suite_logo.c - Shared high-quality logo renderer (GDI+ flat C API).
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_logo.h for the contract. Implementation notes:
 *
 *   - The suite already drives GDI+ through its flat C entry points (the
 *     same pattern as newspaper_print.c); this file mirrors those extern
 *     decls so no C++ <gdiplus.h> is pulled in.
 *   - PNG bytes live in an RCDATA resource. We copy them into an HGLOBAL,
 *     wrap it in an IStream, and hand that to GdipCreateBitmapFromStream.
 *     GDI+ keeps a reference to the stream for the bitmap's lifetime, so
 *     the stream (and its HGLOBAL) are cached alongside the bitmap and
 *     never released -- the OS reclaims them at process exit.
 *   - High-quality scaling (bicubic + smoothing + half-pixel offset) is
 *     set on the Graphics context before every DrawImage so logos drawn
 *     at a size other than their native resolution stay crisp.
 */

#include "suite_logo.h"

#include <objbase.h>
#include <ole2.h>

/* ------------------------------------------------------------------ */
/* GDI+ flat extern decls (same subset the rest of the suite uses).    */
/* ------------------------------------------------------------------ */

typedef struct MgtLogoGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtLogoGdiplusStartupInput;

extern int WINAPI GdiplusStartup(ULONG_PTR *token,
                                 const MgtLogoGdiplusStartupInput *input,
                                 void *output);
extern int WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int WINAPI GdipGetImageWidth(void *image, UINT *w);
extern int WINAPI GdipGetImageHeight(void *image, UINT *h);
extern int WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
extern int WINAPI GdipDeleteGraphics(void *graphics);
extern int WINAPI GdipDrawImageRectI(void *graphics, void *image,
                                     INT x, INT y, INT w, INT h);
extern int WINAPI GdipSetInterpolationMode(void *graphics, int mode);
extern int WINAPI GdipSetSmoothingMode(void *graphics, int mode);
extern int WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);
extern int WINAPI GdipCreateImageAttributes(void **imageattr);
extern int WINAPI GdipDisposeImageAttributes(void *imageattr);
extern int WINAPI GdipSetImageAttributesColorMatrix(void *imageattr, int type,
                                 BOOL enableFlag, const void *colorMatrix,
                                 const void *grayMatrix, int flags);
extern int WINAPI GdipDrawImageRectRectI(void *graphics, void *image,
                                 INT dstx, INT dsty, INT dstw, INT dsth,
                                 INT srcx, INT srcy, INT srcw, INT srch,
                                 int srcUnit, void *imageAttributes,
                                 void *callback, void *callbackData);

/* GDI+ enum values (avoid pulling in <gdiplus.h>). */
#define MGT_INTERPOLATION_HIGHQUALITY_BICUBIC  7   /* InterpolationModeHighQualityBicubic */
#define MGT_SMOOTHING_HIGHQUALITY              2   /* SmoothingModeHighQuality */
#define MGT_PIXEL_OFFSET_HIGHQUALITY           2   /* PixelOffsetModeHighQuality */

/* ------------------------------------------------------------------ */
/* Lazy GDI+ startup.                                                  */
/* ------------------------------------------------------------------ */

static ULONG_PTR g_logo_gdiplus_token  = 0;
static BOOL      g_logo_gdiplus_inited = FALSE;
static BOOL      g_logo_gdiplus_failed = FALSE;

static BOOL logo_ensure_gdiplus(void)
{
    MgtLogoGdiplusStartupInput in;
    if (g_logo_gdiplus_inited) return TRUE;
    if (g_logo_gdiplus_failed)  return FALSE;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_logo_gdiplus_token, &in, NULL) == 0) {
        g_logo_gdiplus_inited = TRUE;
        return TRUE;
    }
    g_logo_gdiplus_failed = TRUE;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Per-resource decoded-bitmap cache.                                  */
/* ------------------------------------------------------------------ */

typedef struct LogoCacheEntry {
    int      res_id;
    void    *bitmap;   /* GpBitmap* */
    IStream *stream;   /* kept alive for the bitmap's lifetime */
} LogoCacheEntry;

#define LOGO_CACHE_MAX 16
static LogoCacheEntry g_logo_cache[LOGO_CACHE_MAX];
static int            g_logo_cache_count = 0;

/* Decode the RCDATA PNG `res_id` into a cached GDI+ bitmap. Returns the
 * GpBitmap* or NULL on failure. Repeated calls return the cached value
 * (including a cached NULL is avoided -- a failed decode simply retries,
 * which is harmless because failures are rare and cheap relative to a
 * repaint). */
static void *logo_get_bitmap(int res_id)
{
    int      i;
    HRSRC    hres;
    HGLOBAL  hresdata;
    DWORD    size;
    const void *src;
    HGLOBAL  hg = NULL;
    IStream *stream = NULL;
    void    *bitmap = NULL;
    void    *dst;

    for (i = 0; i < g_logo_cache_count; i++)
        if (g_logo_cache[i].res_id == res_id && g_logo_cache[i].bitmap)
            return g_logo_cache[i].bitmap;

    if (!logo_ensure_gdiplus()) return NULL;

    hres = FindResourceA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(res_id),
                         (LPCSTR)RT_RCDATA);
    if (!hres) return NULL;
    size = SizeofResource(GetModuleHandleA(NULL), hres);
    hresdata = LoadResource(GetModuleHandleA(NULL), hres);
    if (!hresdata || size == 0) return NULL;
    src = LockResource(hresdata);
    if (!src) return NULL;

    hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return NULL;
    dst = GlobalLock(hg);
    if (!dst) { GlobalFree(hg); return NULL; }
    memcpy(dst, src, size);
    GlobalUnlock(hg);

    /* TRUE => the stream takes ownership of hg and frees it on release. */
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK || !stream) {
        GlobalFree(hg);
        return NULL;
    }
    if (GdipCreateBitmapFromStream(stream, &bitmap) != 0 || !bitmap) {
        stream->lpVtbl->Release(stream);
        return NULL;
    }

    if (g_logo_cache_count < LOGO_CACHE_MAX) {
        g_logo_cache[g_logo_cache_count].res_id = res_id;
        g_logo_cache[g_logo_cache_count].bitmap = bitmap;
        g_logo_cache[g_logo_cache_count].stream = stream;   /* keep alive */
        g_logo_cache_count++;
    }
    /* If the cache is full we still return the bitmap; it (and its stream)
     * leak until process exit, but LOGO_CACHE_MAX comfortably exceeds the
     * handful of distinct logos the suite ships. */
    return bitmap;
}

/* ------------------------------------------------------------------ */
/* Public draw.                                                        */
/* ------------------------------------------------------------------ */

BOOL suite_logo_draw(HDC hdc, int res_id, int x, int y, int dim)
{
    void *bitmap;
    void *graphics = NULL;
    BOOL  ok = FALSE;

    if (!hdc || dim <= 0) return FALSE;

    bitmap = logo_get_bitmap(res_id);
    if (!bitmap) return FALSE;

    if (GdipCreateFromHDC(hdc, &graphics) != 0 || !graphics)
        return FALSE;

    /* High-quality scaling for a logo drawn below its native resolution. */
    GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
    GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HIGHQUALITY);

    if (GdipDrawImageRectI(graphics, bitmap, x, y, dim, dim) == 0)
        ok = TRUE;

    GdipDeleteGraphics(graphics);
    return ok;
}

BOOL suite_logo_draw_gray(HDC hdc, int res_id, int x, int y, int dim)
{
    /* Luminance desaturation matrix (Rec.601). GDI+ multiplies the row vector
     * [R G B A 1] by this 5x5, so column j is each input channel's weight into
     * output channel j; columns 0..2 (R,G,B out) all use the luminance weights,
     * alpha passes through (m[3][3]=1). */
    static const float lum[5][5] = {
        { 0.299f, 0.299f, 0.299f, 0.0f, 0.0f },
        { 0.587f, 0.587f, 0.587f, 0.0f, 0.0f },
        { 0.114f, 0.114f, 0.114f, 0.0f, 0.0f },
        { 0.0f,   0.0f,   0.0f,   1.0f, 0.0f },
        { 0.0f,   0.0f,   0.0f,   0.0f, 1.0f }
    };
    void *bitmap;
    void *graphics = NULL;
    void *attr = NULL;
    UINT  iw = 0, ih = 0;
    BOOL  ok = FALSE;

    if (!hdc || dim <= 0) return FALSE;

    bitmap = logo_get_bitmap(res_id);
    if (!bitmap) return FALSE;
    if (GdipGetImageWidth(bitmap, &iw) != 0 || GdipGetImageHeight(bitmap, &ih) != 0
        || iw == 0 || ih == 0)
        return FALSE;

    if (GdipCreateFromHDC(hdc, &graphics) != 0 || !graphics)
        return FALSE;

    GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
    GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HIGHQUALITY);

    if (GdipCreateImageAttributes(&attr) == 0 && attr) {
        /* type 0 = ColorAdjustTypeDefault, flags 0 = ColorMatrixFlagsDefault */
        GdipSetImageAttributesColorMatrix(attr, 0, TRUE, lum, NULL, 0);
        /* srcUnit 2 = UnitPixel */
        if (GdipDrawImageRectRectI(graphics, bitmap, x, y, dim, dim,
                                   0, 0, (INT)iw, (INT)ih, 2, attr, NULL, NULL) == 0)
            ok = TRUE;
        GdipDisposeImageAttributes(attr);
    }

    GdipDeleteGraphics(graphics);
    return ok;
}
