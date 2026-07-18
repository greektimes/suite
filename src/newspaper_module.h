/*
 * newspaper_module.h - Newspaper tab content pane.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Hosts BookReader (AGPLv3) inside a WebView2 embed pointed at
 * https://newspaper.greektimes.ca/reader.html?issue=<latest>. A native
 * toolbar wraps the WebView2 with Prev / Next / Zoom- / Zoom+ / FitW
 * (active) and Back Issues / Save PDF / Print (placeholders, disabled
 * until follow-up dispatches land them). A "Page N of M" indicator
 * tracks position via a 1 s eval-js poll against MGT_BR.currentIndex().
 */
#ifndef NEWSPAPER_MODULE_H
#define NEWSPAPER_MODULE_H

#include <windows.h>

HWND newspaper_module_create(HWND parent);

#endif /* NEWSPAPER_MODULE_H */
