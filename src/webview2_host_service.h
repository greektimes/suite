/*
 * webview2_host_service.h - Shared WebView2 host service for the Suite.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING. The Suite became AGPLv3 with the 2026-06-15
 * Newspaper dispatch because the Newspaper module hosts BookReader
 * (also AGPLv3) inside an embedded WebView2.
 *
 * Wraps Microsoft's WebView2 (Chromium-based Edge) embed surface in a
 * pure-C, MinGW UCRT64 API. The COM dance happens through the
 * lpVtbl pattern; no MSVC-only helpers. The WebView2Loader.dll entry
 * point is resolved via LoadLibrary + GetProcAddress at create time so
 * the suite has no link-time dependency on the SDK's import lib.
 *
 * One WV2Host == one HWND child that hosts a WebView2 controller. The
 * caller hands a parent HWND and a per-user data folder; the service
 * creates the child, sizes it to (0,0,1,1) initially, and asynchronously
 * spins up the environment + controller. wv2_set_bounds is the move /
 * resize hook the parent module calls from its WM_SIZE handler.
 *
 * Async lifecycle: wv2_create returns a handle immediately, but the
 * controller / webview pointers land later via the env + ctrl completion
 * handlers. wv2_ready returns FALSE until both arrive; the caller can
 * register a ready callback via wv2_set_ready_cb. Calls made before
 * ready (navigate, eval_js) queue and replay -- there is one slot for
 * a pending navigate URI; eval_js calls before ready are dropped with
 * a NULL result.
 */
#ifndef WEBVIEW2_HOST_SERVICE_H
#define WEBVIEW2_HOST_SERVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WV2Host WV2Host;

typedef void (*WV2NavCb)(WV2Host *h, const wchar_t *uri, void *u);
typedef void (*WV2JSResultCb)(const wchar_t *json_result, void *user);
typedef void (*WV2MsgCb)(WV2Host *h, const wchar_t *msg, void *u);
typedef void (*WV2ReadyCb)(WV2Host *h, void *u);

/* Create a host child of `parent` (must be a real HWND). The
 * `user_data_folder` is forwarded verbatim to
 * CreateCoreWebView2EnvironmentWithOptions and stores Chromium
 * profile data. Pass NULL to use a default folder under
 * %LOCALAPPDATA%\MGT Unicorn Suite\WebView2.
 *
 * Returns NULL if WebView2Loader.dll cannot be located on PATH or
 * alongside the exe (the Evergreen Runtime is required).
 */
WV2Host *wv2_create(HWND parent, const wchar_t *user_data_folder);

void     wv2_destroy(WV2Host *h);

/* The HWND that hosts the WebView2 controller. The caller positions
 * this with MoveWindow or SetWindowPos; wv2_set_bounds keeps the
 * controller's Bounds in sync with the new client rect. */
HWND     wv2_hwnd(WV2Host *h);

BOOL     wv2_ready(WV2Host *h);

/* Update the controller bounds. Pass the rect in the child's client
 * coordinates (typically {0, 0, client_w, client_h}). If wv2_ready
 * is FALSE the new bounds are remembered and applied at ready time. */
void     wv2_set_bounds(WV2Host *h, RECT *rc);

void     wv2_navigate(WV2Host *h, const wchar_t *uri);
void     wv2_navigate_blank(WV2Host *h);

void     wv2_eval_js(WV2Host *h, const wchar_t *script,
                     WV2JSResultCb cb, void *user);

void     wv2_set_msg_cb(WV2Host *h, WV2MsgCb cb, void *u);
void     wv2_set_ready_cb(WV2Host *h, WV2ReadyCb cb, void *u);

/* Show / hide the WebView2 surface. Use this to clear the WebView2
 * from view when a native overlay (e.g. the Newspaper picker) needs
 * to cover its rect: WebView2 paints through a DirectComposition swap
 * chain that does NOT obey Win32 sibling HWND z-order, so
 * BringWindowToTop on a sibling overlay is not enough. This call hits
 * ICoreWebView2Controller::put_IsVisible (the documented Microsoft
 * pattern for the overlay scenario) AND issues SW_SHOW / SW_HIDE on
 * the host child HWND for belt-and-suspenders -- some DirectComposition
 * renderers continue to composite a hidden controller if the host
 * window is still WS_VISIBLE. Calls made before wv2_ready queue and
 * apply at ready time (default at ready is visible == TRUE). */
void     wv2_set_visible(WV2Host *h, BOOL visible);

/* Optional navigation-complete hook. Fires after every top-level
 * navigation; the URI is the destination. */
void     wv2_set_nav_cb(WV2Host *h, WV2NavCb cb, void *u);

/* ------------------------------------------------------------------ */
/* Website-module additions (2026-06-16). Strictly additive; the       */
/* Newspaper module does not use any of these.                         */
/* ------------------------------------------------------------------ */

/* Navigation actions (thin wrappers over ICoreWebView2). The back /
 * forward variants check CanGoBack / CanGoForward first and return
 * TRUE only when a step was actually issued. All are no-ops before
 * wv2_ready. */
BOOL     wv2_go_back   (WV2Host *h);
BOOL     wv2_go_forward(WV2Host *h);
void     wv2_refresh   (WV2Host *h);   /* Reload() */
void     wv2_stop      (WV2Host *h);   /* Stop() any pending navigation */

/* Synchronous history-state queries (FALSE before ready). */
BOOL     wv2_can_go_back   (WV2Host *h);
BOOL     wv2_can_go_forward(WV2Host *h);

/* Navigation-starting hook (the domain-lock seam). Return TRUE to allow
 * the navigation, FALSE to cancel it (the service then calls
 * put_Cancel(args, TRUE)). Fires on the UI thread inside the WebView2
 * message pump, once per top-level navigation start. */
typedef BOOL (*WV2NavStartCb)(const wchar_t *uri, void *user);
void     wv2_set_nav_start_cb(WV2Host *h, WV2NavStartCb cb, void *user);

/* Navigation-completed / history-changed hook. Fires after every
 * top-level NavigationCompleted (with its success flag) AND on every
 * HistoryChanged event (success reported TRUE) -- the latter also covers
 * intra-page history.pushState navigation. Consumers use it to refresh
 * CanGoBack / CanGoForward driven UI. */
typedef void (*WV2NavCompletedCb)(BOOL success, void *user);
void     wv2_set_nav_completed_cb(WV2Host *h, WV2NavCompletedCb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* WEBVIEW2_HOST_SERVICE_H */
