/*
 * newspaper_print.h - Suite-driven print pipeline for the Newspaper tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Two-step flow: a custom scope dialog (radio: Current page /
 * Current spread / All pages), then PrintDlgExW for printer
 * selection. The render loop pulls page JPEGs via the newspaper
 * service, decodes with GDI+, and StretchBlt's onto the printer DC
 * with aspect preserved (0.25 inch margin all sides).
 *
 * Scope computation lives here, Suite-side, using the cached
 * manifest's page_count and BookReader's current view index. The
 * view-to-pages mapping is documented inline at the top of the .c.
 */
#ifndef NEWSPAPER_PRINT_H
#define NEWSPAPER_PRINT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "newspaper_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Drive the full print flow: scope dialog -> PrintDlgEx ->
 * StartDoc + per-page StretchBlt -> EndDoc. Returns TRUE on
 * successful submission (StartDoc/EndDoc bracket); FALSE on
 * cancel or error. The parent HWND is used for modal anchoring;
 * pass the suite top-level window. */
BOOL print_run(HWND parent, NewspaperService *svc,
               const char *issue_id, const NewspaperManifest *manifest,
               int current_view_index_0_based);

#ifdef __cplusplus
}
#endif
#endif /* NEWSPAPER_PRINT_H */
