/*
 * website_module.h - Website tab content pane (Modern Mode).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * A WebView2-backed reader for https://greektimes.ca/ with a four-button
 * flat toolbar (Home / Back / Forward / Refresh) styled to match the
 * Newspaper toolbar. A domain-lock navigation policy keeps the user
 * inside greektimes.ca (and its subdomains); any outbound link is opened
 * in the system default browser instead. This is a publication reader,
 * not a browser: no address bar, no tabs, no bookmarks.
 *
 * Reuses src/webview2_host_service.[ch] (shared with the Newspaper
 * module) plus its 2026-06-16 navigation additions (nav-start /
 * nav-completed callbacks, go back/forward/refresh, can-go queries).
 *
 * The signature mirrors the other Modern Mode panes
 * (newspaper_module_create / livetv_module_create): the shell calls this
 * with the Modern Mode pane as parent and installs the result with
 * modern_mode_set_tab_child, which owns sizing.
 */
#ifndef WEBSITE_MODULE_H_INCLUDED
#define WEBSITE_MODULE_H_INCLUDED

#include <windows.h>

HWND website_module_create(HWND parent);

#endif /* WEBSITE_MODULE_H_INCLUDED */
