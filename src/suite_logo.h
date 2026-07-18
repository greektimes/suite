/*
 * suite_logo.h - Shared high-quality logo renderer (GDI+ flat C API).
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Decodes a PNG embedded as an RCDATA resource (alpha preserved) once,
 * caches the GDI+ bitmap for the process lifetime, and draws it scaled
 * with InterpolationModeHighQualityBicubic + SmoothingModeHighQuality +
 * PixelOffsetModeHighQuality so transparent corners composite cleanly
 * over whatever the caller already painted into hdc.
 *
 * Used by the Modern-mode Live Radio and Live TV splash panes to show
 * the Montreal Greek Radio / Montreal Greek TV logos. Retro mode (F1-F6)
 * does not use this service.
 */

#ifndef SUITE_LOGO_H
#define SUITE_LOGO_H

#include <windows.h>

/* Draw the PNG stored in RCDATA resource `res_id` into `hdc`, scaled to
 * a `dim` x `dim` square with its top-left corner at (x, y). The source
 * PNG's alpha channel is preserved and alpha-blended over the existing
 * pixels in hdc -- no background fill, no flatten, no re-encode.
 *
 * The bitmap is decoded on first use and cached; subsequent calls reuse
 * the cached GDI+ image. GDI+ is started lazily on first call.
 *
 * Returns TRUE if the logo was drawn, FALSE on any failure (missing
 * resource, decode error, GDI+ unavailable). Callers should fall back to
 * their prior drawing when FALSE is returned. */
BOOL suite_logo_draw(HDC hdc, int res_id, int x, int y, int dim);

/* Same as suite_logo_draw, but desaturates the real bitmap to grayscale by
 * luminance (Rec.601 weights 0.299/0.587/0.114) via a GDI+ ImageAttributes
 * ColorMatrix. The source PNG's alpha channel is preserved, so transparent
 * corners still composite cleanly. Used by the F7 CU-SeeMe Start screen so the
 * MGT-TV logo matches the grayscale video. Returns FALSE on any failure. */
BOOL suite_logo_draw_gray(HDC hdc, int res_id, int x, int y, int dim);

#endif /* SUITE_LOGO_H */
