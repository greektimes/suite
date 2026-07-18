/*
 * webview2_host_service.c - Shared WebView2 host service implementation.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Lifecycle:
 *   wv2_create -> resolve WebView2Loader.dll entry point,
 *                 register child class, create child HWND,
 *                 fire CreateCoreWebView2EnvironmentWithOptions
 *                 with a heap-allocated env-completion handler.
 *   env handler -> stores Environment*, calls
 *                  CreateCoreWebView2Controller(child_hwnd, ctrl handler).
 *   ctrl handler -> stores Controller* + queries Webview*,
 *                   applies the pending bounds + pending nav URI,
 *                   subscribes WebMessageReceived and NavigationCompleted,
 *                   flips ready = TRUE, fires ready callback.
 *
 * All MS-side callbacks fire on the same UI thread that called the
 * entry point. That simplifies thread safety: the WV2Host* is only
 * ever touched from one thread, and refcounted handlers self-Release
 * after Invoke runs.
 */

#define COBJMACROS
#define INITGUID

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "WebView2.h"

#include "webview2_host_service.h"

/* ------------------------------------------------------------------ */
/* Diagnostic logging (dispatch 2026-06-15 follow-up).                 */
/*                                                                     */
/* Gated behind DEBUG_WV2_SERVICE; default off for shipping builds.    */
/* When enabled, logs every WV2 entrypoint, every COM callback Invoke, */
/* every put_Bounds / Navigate / IsVisible call, and every                                          */
/* NavigationStarting / NavigationCompleted / DOMContentLoaded /         */
/* WebMessageReceived event. Path: %TEMP%\mgt_wv2_service.log.         */
/* Single CRITICAL_SECTION serializes writes from the UI thread (which */
/* is where all WV2 callbacks fire per MS docs).                       */
/* ------------------------------------------------------------------ */

/* Default off for shipping builds. Flip to 1 or pass
 * -DDEBUG_WV2_SERVICE=1 to re-enable the log at
 * %TEMP%\mgt_wv2_service.log. */
#ifndef DEBUG_WV2_SERVICE
#define DEBUG_WV2_SERVICE 0
#endif

#if DEBUG_WV2_SERVICE

static FILE             *g_wv2_log_fp        = NULL;
static ULONGLONG         g_wv2_log_t0        = 0;
static CRITICAL_SECTION  g_wv2_log_cs;
static BOOL              g_wv2_log_cs_ready  = FALSE;
static LONG              g_wv2_log_open_refs = 0;

static void wv2log_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_wv2_log_cs);
        g_wv2_log_cs_ready = TRUE;
    } else {
        while (!g_wv2_log_cs_ready) Sleep(0);
    }
}

static void wv2log_open(void)
{
    char path[MAX_PATH];
    DWORD n;
    wv2log_init_cs_once();
    EnterCriticalSection(&g_wv2_log_cs);
    if (InterlockedIncrement(&g_wv2_log_open_refs) == 1) {
        n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) lstrcpyA(path, "C:\\Windows\\Temp");
        lstrcatA(path, "\\mgt_wv2_service.log");
        g_wv2_log_fp = fopen(path, "a");
        g_wv2_log_t0 = GetTickCount64();
        if (g_wv2_log_fp) {
            fprintf(g_wv2_log_fp,
                "\n==== mgt_wv2_service.log session opened t0=%llu ====\n",
                (unsigned long long)g_wv2_log_t0);
            fflush(g_wv2_log_fp);
        }
    }
    LeaveCriticalSection(&g_wv2_log_cs);
}

static void wv2log_close(void)
{
    if (!g_wv2_log_cs_ready) return;
    EnterCriticalSection(&g_wv2_log_cs);
    if (InterlockedDecrement(&g_wv2_log_open_refs) == 0) {
        if (g_wv2_log_fp) {
            fprintf(g_wv2_log_fp, "==== session closed ====\n");
            fclose(g_wv2_log_fp);
            g_wv2_log_fp = NULL;
        }
    }
    LeaveCriticalSection(&g_wv2_log_cs);
}

static void wv2log(const char *fmt, ...)
{
    char    line[512];
    int     n;
    va_list ap;
    ULONGLONG now;
    if (!g_wv2_log_cs_ready) return;
    EnterCriticalSection(&g_wv2_log_cs);
    if (!g_wv2_log_fp) { LeaveCriticalSection(&g_wv2_log_cs); return; }
    now = GetTickCount64();
    n = _snprintf(line, sizeof(line), "%6lu  ",
                  (unsigned long)(now - g_wv2_log_t0));
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = '\0';
    lstrcatA(line, "\n");
    fputs(line, g_wv2_log_fp);
    fflush(g_wv2_log_fp);
    LeaveCriticalSection(&g_wv2_log_cs);
}

#else  /* DEBUG_WV2_SERVICE */

#define wv2log(...) ((void)0)
static void wv2log_open(void)  {}
static void wv2log_close(void) {}

#endif

/* ------------------------------------------------------------------ */
/* Loader: resolve CreateCoreWebView2EnvironmentWithOptions at runtime. */
/* ------------------------------------------------------------------ */

typedef HRESULT (STDAPICALLTYPE *PFN_CreateEnv)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions *environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *handler);

static HMODULE        g_wv2_loader_dll  = NULL;
static PFN_CreateEnv  g_wv2_create_env  = NULL;

static BOOL wv2_loader_resolve(void)
{
    if (g_wv2_create_env) return TRUE;
    if (!g_wv2_loader_dll) {
        char exe_dir[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            char *slash = strrchr(exe_dir, '\\');
            if (slash) {
                strcpy(slash + 1, "WebView2Loader.dll");
                g_wv2_loader_dll = LoadLibraryExA(exe_dir, NULL,
                    LOAD_WITH_ALTERED_SEARCH_PATH);
                wv2log("loader: LoadLibraryExA(\"%s\") -> %p", exe_dir,
                       (void *)g_wv2_loader_dll);
            }
        }
        if (!g_wv2_loader_dll) {
            g_wv2_loader_dll = LoadLibraryA("WebView2Loader.dll");
            wv2log("loader: LoadLibraryA(\"WebView2Loader.dll\") -> %p",
                   (void *)g_wv2_loader_dll);
        }
    }
    if (!g_wv2_loader_dll) {
        wv2log("loader: ALL LoadLibrary attempts failed");
        return FALSE;
    }
    g_wv2_create_env = (PFN_CreateEnv)GetProcAddress(g_wv2_loader_dll,
        "CreateCoreWebView2EnvironmentWithOptions");
    wv2log("loader: GetProcAddress(CreateCoreWebView2EnvironmentWithOptions) -> %p",
           (void *)g_wv2_create_env);
    return g_wv2_create_env != NULL;
}

/* ------------------------------------------------------------------ */
/* WV2Host struct.                                                     */
/* ------------------------------------------------------------------ */

struct WV2Host {
    HWND        parent;
    HWND        child;          /* MGTUnicornWV2HostV1 instance */

    /* Owned COM pointers; populated asynchronously. */
    ICoreWebView2Environment *env;
    ICoreWebView2Controller  *controller;
    ICoreWebView2            *webview;

    BOOL        ready;          /* TRUE once controller + webview live */

    /* Pending state applied at ready time. */
    RECT        pending_bounds;
    BOOL        pending_bounds_set;
    wchar_t    *pending_uri;    /* heap-allocated, NULL when none */
    BOOL        pending_visible_set;
    BOOL        pending_visible;

    WV2MsgCb    msg_cb;     void *msg_user;
    WV2ReadyCb  ready_cb;   void *ready_user;
    WV2NavCb    nav_cb;     void *nav_user;

    /* Website-module additions (2026-06-16). */
    WV2NavStartCb     nav_start_cb;     void *nav_start_user;
    WV2NavCompletedCb nav_completed_cb; void *nav_completed_user;

    /* Subscription tokens. */
    EventRegistrationToken tok_msg;
    EventRegistrationToken tok_nav;
    EventRegistrationToken tok_navstart;
    EventRegistrationToken tok_hist;

    BOOL        tok_msg_set;
    BOOL        tok_nav_set;
    BOOL        tok_navstart_set;
    BOOL        tok_hist_set;
};

/* Forward decls for COM handlers below. */
static HRESULT wv2_kick_create_env(WV2Host *h, const wchar_t *user_data);
static HRESULT wv2_kick_create_controller(WV2Host *h);
static void    wv2_apply_pending(WV2Host *h);
static void    wv2_subscribe_events(WV2Host *h);

/* ------------------------------------------------------------------ */
/* COM handler scaffolding.                                            */
/*                                                                     */
/* Each Microsoft handler interface is implemented by a small struct   */
/* whose first member is the corresponding Vtbl*. We hand the struct's */
/* address back to MS by casting to the interface type. AddRef /       */
/* Release use refcount; QI returns the same pointer for IID_IUnknown  */
/* and the specific handler IID, E_NOINTERFACE otherwise. Invoke does  */
/* the real work, then the natural Release post-Invoke frees the       */
/* heap-allocated handler.                                              */
/* ------------------------------------------------------------------ */

#define WV2_QI_BODY(IFACE_GUID)                                        \
    do {                                                                \
        if (!ppv) return E_POINTER;                                     \
        if (IsEqualIID(riid, &IID_IUnknown) ||                          \
            IsEqualIID(riid, &IFACE_GUID)) {                            \
            *ppv = self;                                                \
            self->lpVtbl->AddRef((void *)self);                         \
            return S_OK;                                                \
        }                                                               \
        *ppv = NULL;                                                    \
        return E_NOINTERFACE;                                           \
    } while (0)

/* ---- env-completion handler ---- */

typedef struct WV2EnvHandler {
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2EnvHandler;

static HRESULT STDMETHODCALLTYPE
WV2EnvH_QI(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *iface,
           REFIID riid, void **ppv)
{
    WV2EnvHandler *self = (WV2EnvHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler);
}

static ULONG STDMETHODCALLTYPE
WV2EnvH_AddRef(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *iface)
{
    WV2EnvHandler *self = (WV2EnvHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2EnvH_Release(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *iface)
{
    WV2EnvHandler *self = (WV2EnvHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2EnvH_Invoke(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *iface,
               HRESULT hr, ICoreWebView2Environment *created)
{
    WV2EnvHandler *self = (WV2EnvHandler *)iface;
    WV2Host *h = self->host;
    wv2log("EnvH_Invoke hr=0x%lx  env=%p  host=%p",
           (unsigned long)hr, (void *)created, (void *)h);
    if (FAILED(hr) || !created || !h) return S_OK;
    h->env = created;
    h->env->lpVtbl->AddRef(h->env);
    {
        HRESULT chr = wv2_kick_create_controller(h);
        wv2log("kick_create_controller -> hr=0x%lx", (unsigned long)chr);
    }
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_WV2EnvH_Vtbl = {
    WV2EnvH_QI, WV2EnvH_AddRef, WV2EnvH_Release, WV2EnvH_Invoke
};

/* ---- controller-completion handler ---- */

typedef struct WV2CtrlHandler {
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2CtrlHandler;

static HRESULT STDMETHODCALLTYPE
WV2CtrlH_QI(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *iface,
            REFIID riid, void **ppv)
{
    WV2CtrlHandler *self = (WV2CtrlHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler);
}

static ULONG STDMETHODCALLTYPE
WV2CtrlH_AddRef(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *iface)
{
    WV2CtrlHandler *self = (WV2CtrlHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2CtrlH_Release(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *iface)
{
    WV2CtrlHandler *self = (WV2CtrlHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2CtrlH_Invoke(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *iface,
                HRESULT hr, ICoreWebView2Controller *controller)
{
    WV2CtrlHandler *self = (WV2CtrlHandler *)iface;
    WV2Host *h = self->host;
    wv2log("CtrlH_Invoke hr=0x%lx  ctrl=%p  host=%p",
           (unsigned long)hr, (void *)controller, (void *)h);
    if (FAILED(hr) || !controller || !h) return S_OK;
    h->controller = controller;
    h->controller->lpVtbl->AddRef(h->controller);
    {
        HRESULT cwv2_hr = h->controller->lpVtbl->get_CoreWebView2(
            h->controller, &h->webview);
        wv2log("get_CoreWebView2 -> hr=0x%lx  webview=%p",
               (unsigned long)cwv2_hr, (void *)h->webview);
        if (FAILED(cwv2_hr)) return S_OK;
    }
    h->ready = TRUE;
    wv2_subscribe_events(h);
    wv2_apply_pending(h);
    {
        BOOL vis = FALSE;
        h->controller->lpVtbl->get_IsVisible(h->controller, &vis);
        wv2log("controller IsVisible at ready = %d", vis ? 1 : 0);
    }
    if (h->ready_cb) {
        wv2log("firing ready_cb");
        h->ready_cb(h, h->ready_user);
    }
    return S_OK;
}

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_WV2CtrlH_Vtbl = {
    WV2CtrlH_QI, WV2CtrlH_AddRef, WV2CtrlH_Release, WV2CtrlH_Invoke
};

/* ---- WebMessageReceived handler ---- */

typedef struct WV2MsgHandler {
    ICoreWebView2WebMessageReceivedEventHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2MsgHandler;

static HRESULT STDMETHODCALLTYPE
WV2MsgH_QI(ICoreWebView2WebMessageReceivedEventHandler *iface,
           REFIID riid, void **ppv)
{
    WV2MsgHandler *self = (WV2MsgHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2WebMessageReceivedEventHandler);
}

static ULONG STDMETHODCALLTYPE
WV2MsgH_AddRef(ICoreWebView2WebMessageReceivedEventHandler *iface)
{
    WV2MsgHandler *self = (WV2MsgHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2MsgH_Release(ICoreWebView2WebMessageReceivedEventHandler *iface)
{
    WV2MsgHandler *self = (WV2MsgHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2MsgH_Invoke(ICoreWebView2WebMessageReceivedEventHandler *iface,
               ICoreWebView2 *sender,
               ICoreWebView2WebMessageReceivedEventArgs *args)
{
    WV2MsgHandler *self = (WV2MsgHandler *)iface;
    WV2Host *h = self->host;
    LPWSTR raw = NULL;
    (void)sender;
    if (!h || !args) return S_OK;
    if (SUCCEEDED(args->lpVtbl->TryGetWebMessageAsString(args, &raw)) && raw) {
        char tmp[200]; int i;
        for (i = 0; i < (int)sizeof(tmp) - 1 && raw[i]; i++)
            tmp[i] = (raw[i] > 0 && raw[i] < 0x80) ? (char)raw[i] : '?';
        tmp[i] = '\0';
        wv2log("MsgH_Invoke msg=\"%s\"", tmp);
        if (h->msg_cb) h->msg_cb(h, raw, h->msg_user);
        CoTaskMemFree(raw);
    }
    return S_OK;
}

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_WV2MsgH_Vtbl = {
    WV2MsgH_QI, WV2MsgH_AddRef, WV2MsgH_Release, WV2MsgH_Invoke
};

/* ---- NavigationCompleted handler ---- */

typedef struct WV2NavHandler {
    ICoreWebView2NavigationCompletedEventHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2NavHandler;

static HRESULT STDMETHODCALLTYPE
WV2NavH_QI(ICoreWebView2NavigationCompletedEventHandler *iface,
           REFIID riid, void **ppv)
{
    WV2NavHandler *self = (WV2NavHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2NavigationCompletedEventHandler);
}

static ULONG STDMETHODCALLTYPE
WV2NavH_AddRef(ICoreWebView2NavigationCompletedEventHandler *iface)
{
    WV2NavHandler *self = (WV2NavHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2NavH_Release(ICoreWebView2NavigationCompletedEventHandler *iface)
{
    WV2NavHandler *self = (WV2NavHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2NavH_Invoke(ICoreWebView2NavigationCompletedEventHandler *iface,
               ICoreWebView2 *sender,
               ICoreWebView2NavigationCompletedEventArgs *args)
{
    WV2NavHandler *self = (WV2NavHandler *)iface;
    WV2Host *h = self->host;
    LPWSTR uri = NULL;
    BOOL    ok      = FALSE;
    UINT32  err_st  = 0;
    if (!h || !sender) return S_OK;
    if (args) {
        args->lpVtbl->get_IsSuccess(args, &ok);
        args->lpVtbl->get_WebErrorStatus(args, (COREWEBVIEW2_WEB_ERROR_STATUS *)&err_st);
    }
    if (SUCCEEDED(sender->lpVtbl->get_Source(sender, &uri)) && uri) {
        char tmp[400]; int i;
        for (i = 0; i < (int)sizeof(tmp) - 1 && uri[i]; i++)
            tmp[i] = (uri[i] > 0 && uri[i] < 0x80) ? (char)uri[i] : '?';
        tmp[i] = '\0';
        wv2log("NavH_Invoke ok=%d err=%lu uri=\"%s\"",
               ok ? 1 : 0, (unsigned long)err_st, tmp);
        if (h->nav_cb) h->nav_cb(h, uri, h->nav_user);
        CoTaskMemFree(uri);
    } else {
        wv2log("NavH_Invoke ok=%d err=%lu uri=<unavailable>",
               ok ? 1 : 0, (unsigned long)err_st);
    }
    if (h->nav_completed_cb) h->nav_completed_cb(ok, h->nav_completed_user);
    return S_OK;
}

static ICoreWebView2NavigationCompletedEventHandlerVtbl g_WV2NavH_Vtbl = {
    WV2NavH_QI, WV2NavH_AddRef, WV2NavH_Release, WV2NavH_Invoke
};

/* ---- NavigationStarting handler (the domain-lock seam) ---- */

typedef struct WV2NavStartHandler {
    ICoreWebView2NavigationStartingEventHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2NavStartHandler;

static HRESULT STDMETHODCALLTYPE
WV2NavStartH_QI(ICoreWebView2NavigationStartingEventHandler *iface,
                REFIID riid, void **ppv)
{
    WV2NavStartHandler *self = (WV2NavStartHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2NavigationStartingEventHandler);
}

static ULONG STDMETHODCALLTYPE
WV2NavStartH_AddRef(ICoreWebView2NavigationStartingEventHandler *iface)
{
    WV2NavStartHandler *self = (WV2NavStartHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2NavStartH_Release(ICoreWebView2NavigationStartingEventHandler *iface)
{
    WV2NavStartHandler *self = (WV2NavStartHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2NavStartH_Invoke(ICoreWebView2NavigationStartingEventHandler *iface,
                    ICoreWebView2 *sender,
                    ICoreWebView2NavigationStartingEventArgs *args)
{
    WV2NavStartHandler *self = (WV2NavStartHandler *)iface;
    WV2Host *h = self->host;
    LPWSTR uri = NULL;
    (void)sender;
    if (!h || !args) return S_OK;
    if (SUCCEEDED(args->lpVtbl->get_Uri(args, &uri)) && uri) {
        BOOL allow = TRUE;
        if (h->nav_start_cb) allow = h->nav_start_cb(uri, h->nav_start_user);
        if (!allow) args->lpVtbl->put_Cancel(args, TRUE);
        {
            char tmp[400]; int i;
            for (i = 0; i < (int)sizeof(tmp) - 1 && uri[i]; i++)
                tmp[i] = (uri[i] > 0 && uri[i] < 0x80) ? (char)uri[i] : '?';
            tmp[i] = '\0';
            wv2log("NavStartH_Invoke allow=%d uri=\"%s\"", allow ? 1 : 0, tmp);
        }
        CoTaskMemFree(uri);
    }
    return S_OK;
}

static ICoreWebView2NavigationStartingEventHandlerVtbl g_WV2NavStartH_Vtbl = {
    WV2NavStartH_QI, WV2NavStartH_AddRef, WV2NavStartH_Release, WV2NavStartH_Invoke
};

/* ---- HistoryChanged handler ---- */

typedef struct WV2HistHandler {
    ICoreWebView2HistoryChangedEventHandlerVtbl *lpVtbl;
    LONG     refcount;
    WV2Host *host;
} WV2HistHandler;

static HRESULT STDMETHODCALLTYPE
WV2HistH_QI(ICoreWebView2HistoryChangedEventHandler *iface,
            REFIID riid, void **ppv)
{
    WV2HistHandler *self = (WV2HistHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2HistoryChangedEventHandler);
}

static ULONG STDMETHODCALLTYPE
WV2HistH_AddRef(ICoreWebView2HistoryChangedEventHandler *iface)
{
    WV2HistHandler *self = (WV2HistHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2HistH_Release(ICoreWebView2HistoryChangedEventHandler *iface)
{
    WV2HistHandler *self = (WV2HistHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2HistH_Invoke(ICoreWebView2HistoryChangedEventHandler *iface,
                ICoreWebView2 *sender, IUnknown *args)
{
    WV2HistHandler *self = (WV2HistHandler *)iface;
    WV2Host *h = self->host;
    (void)sender; (void)args;
    if (h && h->nav_completed_cb)
        h->nav_completed_cb(TRUE, h->nav_completed_user);
    return S_OK;
}

static ICoreWebView2HistoryChangedEventHandlerVtbl g_WV2HistH_Vtbl = {
    WV2HistH_QI, WV2HistH_AddRef, WV2HistH_Release, WV2HistH_Invoke
};

/* ---- ExecuteScript completion handler ---- */

typedef struct WV2ScriptHandler {
    ICoreWebView2ExecuteScriptCompletedHandlerVtbl *lpVtbl;
    LONG          refcount;
    WV2JSResultCb cb;
    void         *user;
} WV2ScriptHandler;

static HRESULT STDMETHODCALLTYPE
WV2ScriptH_QI(ICoreWebView2ExecuteScriptCompletedHandler *iface,
              REFIID riid, void **ppv)
{
    WV2ScriptHandler *self = (WV2ScriptHandler *)iface;
    WV2_QI_BODY(IID_ICoreWebView2ExecuteScriptCompletedHandler);
}

static ULONG STDMETHODCALLTYPE
WV2ScriptH_AddRef(ICoreWebView2ExecuteScriptCompletedHandler *iface)
{
    WV2ScriptHandler *self = (WV2ScriptHandler *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE
WV2ScriptH_Release(ICoreWebView2ExecuteScriptCompletedHandler *iface)
{
    WV2ScriptHandler *self = (WV2ScriptHandler *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
WV2ScriptH_Invoke(ICoreWebView2ExecuteScriptCompletedHandler *iface,
                  HRESULT err, LPCWSTR result)
{
    WV2ScriptHandler *self = (WV2ScriptHandler *)iface;
    if (self->cb) self->cb(SUCCEEDED(err) ? result : NULL, self->user);
    return S_OK;
}

static ICoreWebView2ExecuteScriptCompletedHandlerVtbl g_WV2ScriptH_Vtbl = {
    WV2ScriptH_QI, WV2ScriptH_AddRef, WV2ScriptH_Release, WV2ScriptH_Invoke
};

/* ------------------------------------------------------------------ */
/* Child window class.                                                 */
/* ------------------------------------------------------------------ */

#define WV2_CHILD_CLASS  "MGTUnicornWV2HostV1"

static LRESULT CALLBACK WV2ChildProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    WV2Host *h = (WV2Host *)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE: {
        RECT rc = { 0, 0, LOWORD(lParam), HIWORD(lParam) };
        if (h) wv2_set_bounds(h, &rc);
        return 0;
    }
    case WM_ERASEBKGND:
        /* The controller paints the entire client rect once ready;
         * before then a black background avoids a white flash. */
        if (h && h->ready) return 1;
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void wv2_register_child_class_once(HINSTANCE hInst)
{
    static LONG done = 0;
    WNDCLASSEXA wc;
    if (InterlockedCompareExchange(&done, 1, 0) != 0) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WV2ChildProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WV2_CHILD_CLASS;
    RegisterClassExA(&wc);
}

/* ------------------------------------------------------------------ */
/* Internal helpers.                                                   */
/* ------------------------------------------------------------------ */

static HRESULT wv2_kick_create_env(WV2Host *h, const wchar_t *user_data)
{
    WV2EnvHandler *eh;
    HRESULT hr;
    if (!g_wv2_create_env) return E_FAIL;
    eh = (WV2EnvHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(*eh));
    if (!eh) return E_OUTOFMEMORY;
    eh->lpVtbl   = &g_WV2EnvH_Vtbl;
    eh->refcount = 1;
    eh->host     = h;
    hr = g_wv2_create_env(NULL, user_data, NULL,
        (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)eh);
    {
        char tmp[256]; int i;
        for (i = 0; i < (int)sizeof(tmp) - 1 && user_data && user_data[i]; i++)
            tmp[i] = (user_data[i] > 0 && user_data[i] < 0x80) ? (char)user_data[i] : '?';
        tmp[i] = '\0';
        wv2log("CreateCoreWebView2EnvironmentWithOptions(NULL, \"%s\") -> hr=0x%lx",
               tmp, (unsigned long)hr);
    }
    return hr;
}

static HRESULT wv2_kick_create_controller(WV2Host *h)
{
    WV2CtrlHandler *ch;
    HRESULT hr;
    if (!h->env) return E_FAIL;
    ch = (WV2CtrlHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                     sizeof(*ch));
    if (!ch) return E_OUTOFMEMORY;
    ch->lpVtbl   = &g_WV2CtrlH_Vtbl;
    ch->refcount = 1;
    ch->host     = h;
    wv2log("CreateCoreWebView2Controller(child=%p)", (void *)h->child);
    hr = h->env->lpVtbl->CreateCoreWebView2Controller(h->env, h->child,
        (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)ch);
    return hr;
}

static void wv2_subscribe_events(WV2Host *h)
{
    WV2MsgHandler *mh;
    WV2NavHandler *nh;
    if (!h->webview) return;
    mh = (WV2MsgHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(*mh));
    nh = (WV2NavHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(*nh));
    if (mh) {
        mh->lpVtbl   = &g_WV2MsgH_Vtbl;
        mh->refcount = 1;
        mh->host     = h;
        if (SUCCEEDED(h->webview->lpVtbl->add_WebMessageReceived(h->webview,
            (ICoreWebView2WebMessageReceivedEventHandler *)mh,
            &h->tok_msg)))
            h->tok_msg_set = TRUE;
        ((ICoreWebView2WebMessageReceivedEventHandler *)mh)
            ->lpVtbl->Release((ICoreWebView2WebMessageReceivedEventHandler *)mh);
    }
    if (nh) {
        nh->lpVtbl   = &g_WV2NavH_Vtbl;
        nh->refcount = 1;
        nh->host     = h;
        if (SUCCEEDED(h->webview->lpVtbl->add_NavigationCompleted(h->webview,
            (ICoreWebView2NavigationCompletedEventHandler *)nh,
            &h->tok_nav)))
            h->tok_nav_set = TRUE;
        ((ICoreWebView2NavigationCompletedEventHandler *)nh)
            ->lpVtbl->Release((ICoreWebView2NavigationCompletedEventHandler *)nh);
    }
    /* Website-module additions: NavigationStarting (domain lock) +
     * HistoryChanged (toolbar back/forward state). */
    {
        WV2NavStartHandler *sh = (WV2NavStartHandler *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sh));
        if (sh) {
            sh->lpVtbl   = &g_WV2NavStartH_Vtbl;
            sh->refcount = 1;
            sh->host     = h;
            if (SUCCEEDED(h->webview->lpVtbl->add_NavigationStarting(h->webview,
                (ICoreWebView2NavigationStartingEventHandler *)sh,
                &h->tok_navstart)))
                h->tok_navstart_set = TRUE;
            ((ICoreWebView2NavigationStartingEventHandler *)sh)
                ->lpVtbl->Release((ICoreWebView2NavigationStartingEventHandler *)sh);
        }
    }
    {
        WV2HistHandler *hh = (WV2HistHandler *)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*hh));
        if (hh) {
            hh->lpVtbl   = &g_WV2HistH_Vtbl;
            hh->refcount = 1;
            hh->host     = h;
            if (SUCCEEDED(h->webview->lpVtbl->add_HistoryChanged(h->webview,
                (ICoreWebView2HistoryChangedEventHandler *)hh,
                &h->tok_hist)))
                h->tok_hist_set = TRUE;
            ((ICoreWebView2HistoryChangedEventHandler *)hh)
                ->lpVtbl->Release((ICoreWebView2HistoryChangedEventHandler *)hh);
        }
    }
}

static void wv2_apply_pending(WV2Host *h)
{
    if (!h->controller) return;
    if (h->pending_bounds_set) {
        wv2log("apply_pending put_Bounds={%d,%d,%d,%d}",
               (int)h->pending_bounds.left,  (int)h->pending_bounds.top,
               (int)h->pending_bounds.right, (int)h->pending_bounds.bottom);
        h->controller->lpVtbl->put_Bounds(h->controller, h->pending_bounds);
        h->pending_bounds_set = FALSE;
    } else {
        /* No pending bounds means the parent never sized us before we
         * became ready. Default to the child's current client rect so
         * the controller has SOMETHING to render into. */
        RECT cr;
        if (GetClientRect(h->child, &cr)) {
            wv2log("apply_pending NO pending_bounds; fallback put_Bounds=GetClientRect={%d,%d,%d,%d}",
                   (int)cr.left, (int)cr.top, (int)cr.right, (int)cr.bottom);
            h->controller->lpVtbl->put_Bounds(h->controller, cr);
        }
    }
    {
        BOOL want = h->pending_visible_set ? h->pending_visible : TRUE;
        h->controller->lpVtbl->put_IsVisible(h->controller, want);
        if (h->child) ShowWindow(h->child, want ? SW_SHOW : SW_HIDE);
        wv2log("apply_pending put_IsVisible(%d) done", want ? 1 : 0);
        h->pending_visible_set = FALSE;
    }
    if (h->pending_uri && h->webview) {
        char tmp[400]; int i;
        for (i = 0; i < (int)sizeof(tmp) - 1 && h->pending_uri[i]; i++)
            tmp[i] = (h->pending_uri[i] > 0 && h->pending_uri[i] < 0x80)
                     ? (char)h->pending_uri[i] : '?';
        tmp[i] = '\0';
        wv2log("apply_pending Navigate(\"%s\")", tmp);
        h->webview->lpVtbl->Navigate(h->webview, h->pending_uri);
        HeapFree(GetProcessHeap(), 0, h->pending_uri);
        h->pending_uri = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Public API.                                                         */
/* ------------------------------------------------------------------ */

WV2Host *wv2_create(HWND parent, const wchar_t *user_data_folder)
{
    WV2Host *h;
    HINSTANCE hInst;
    wchar_t default_folder[MAX_PATH];
    const wchar_t *folder = user_data_folder;

    wv2log_open();
    {
        APTTYPE apt_t = APTTYPE_CURRENT;
        APTTYPEQUALIFIER apt_q = APTTYPEQUALIFIER_NONE;
        HRESULT apt_hr = CoGetApartmentType(&apt_t, &apt_q);
        wv2log("wv2_create(parent=%p, parent_visible=%d, apt_hr=0x%lx, apt_t=%d)",
               (void *)parent, parent ? IsWindowVisible(parent) : -1,
               (unsigned long)apt_hr, (int)apt_t);
    }
    if (!parent) return NULL;
    if (!wv2_loader_resolve()) return NULL;

    hInst = (HINSTANCE)GetModuleHandleA(NULL);
    wv2_register_child_class_once(hInst);

    h = (WV2Host *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*h));
    if (!h) return NULL;
    h->parent = parent;
    h->child  = CreateWindowExA(0, WV2_CHILD_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 1, 1, parent, NULL, hInst, NULL);
    if (!h->child) { HeapFree(GetProcessHeap(), 0, h); return NULL; }
    SetWindowLongPtrA(h->child, GWLP_USERDATA, (LONG_PTR)h);

    if (!folder) {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA",
                                          default_folder, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            lstrcpyW(default_folder, L"C:\\Temp");
        lstrcatW(default_folder, L"\\MGT Unicorn Suite\\WebView2");
        folder = default_folder;
    }

    wv2log("wv2_create child_hwnd=%p", (void *)h->child);
    if (FAILED(wv2_kick_create_env(h, folder))) {
        wv2log("wv2_create FAILED at kick_create_env");
        DestroyWindow(h->child);
        HeapFree(GetProcessHeap(), 0, h);
        return NULL;
    }
    return h;
}

void wv2_destroy(WV2Host *h)
{
    if (!h) return;
    wv2log("wv2_destroy(%p)", (void *)h);
    if (h->webview && h->tok_msg_set)
        h->webview->lpVtbl->remove_WebMessageReceived(h->webview, h->tok_msg);
    if (h->webview && h->tok_nav_set)
        h->webview->lpVtbl->remove_NavigationCompleted(h->webview, h->tok_nav);
    if (h->webview && h->tok_navstart_set)
        h->webview->lpVtbl->remove_NavigationStarting(h->webview, h->tok_navstart);
    if (h->webview && h->tok_hist_set)
        h->webview->lpVtbl->remove_HistoryChanged(h->webview, h->tok_hist);
    if (h->controller) {
        h->controller->lpVtbl->Close(h->controller);
        h->controller->lpVtbl->Release(h->controller);
        h->controller = NULL;
    }
    if (h->webview)   { h->webview->lpVtbl->Release(h->webview);   h->webview = NULL; }
    if (h->env)       { h->env->lpVtbl->Release(h->env);           h->env = NULL; }
    if (h->pending_uri) { HeapFree(GetProcessHeap(), 0, h->pending_uri); h->pending_uri = NULL; }
    if (h->child)     { DestroyWindow(h->child); h->child = NULL; }
    HeapFree(GetProcessHeap(), 0, h);
    wv2log_close();
}

HWND wv2_hwnd(WV2Host *h)    { return h ? h->child : NULL; }
BOOL wv2_ready(WV2Host *h)   { return h ? h->ready : FALSE; }

void wv2_set_bounds(WV2Host *h, RECT *rc)
{
    if (!h || !rc) return;
    if (h->ready && h->controller) {
        wv2log("set_bounds(ready) {%d,%d,%d,%d}",
               (int)rc->left, (int)rc->top, (int)rc->right, (int)rc->bottom);
        h->controller->lpVtbl->put_Bounds(h->controller, *rc);
    } else {
        wv2log("set_bounds(QUEUED) {%d,%d,%d,%d}  ready=%d",
               (int)rc->left, (int)rc->top, (int)rc->right, (int)rc->bottom,
               h->ready ? 1 : 0);
        h->pending_bounds     = *rc;
        h->pending_bounds_set = TRUE;
    }
}

void wv2_navigate(WV2Host *h, const wchar_t *uri)
{
    if (!h || !uri) return;
    {
        char tmp[400]; int i;
        for (i = 0; i < (int)sizeof(tmp) - 1 && uri[i]; i++)
            tmp[i] = (uri[i] > 0 && uri[i] < 0x80) ? (char)uri[i] : '?';
        tmp[i] = '\0';
        wv2log("wv2_navigate(\"%s\") ready=%d", tmp, h->ready ? 1 : 0);
    }
    if (h->ready && h->webview) {
        h->webview->lpVtbl->Navigate(h->webview, uri);
        return;
    }
    /* Queue. Free any previous pending URI -- last one wins. */
    if (h->pending_uri) HeapFree(GetProcessHeap(), 0, h->pending_uri);
    {
        size_t n = lstrlenW(uri) + 1;
        h->pending_uri = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                              n * sizeof(wchar_t));
        if (h->pending_uri) lstrcpyW(h->pending_uri, uri);
    }
}

void wv2_navigate_blank(WV2Host *h)
{
    wv2_navigate(h, L"about:blank");
}

void wv2_eval_js(WV2Host *h, const wchar_t *script,
                 WV2JSResultCb cb, void *user)
{
    WV2ScriptHandler *sh;
    if (!h || !script) { if (cb) cb(NULL, user); return; }
    if (!h->ready || !h->webview) { if (cb) cb(NULL, user); return; }
    sh = (WV2ScriptHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       sizeof(*sh));
    if (!sh) { if (cb) cb(NULL, user); return; }
    sh->lpVtbl   = &g_WV2ScriptH_Vtbl;
    sh->refcount = 1;
    sh->cb       = cb;
    sh->user     = user;
    h->webview->lpVtbl->ExecuteScript(h->webview, script,
        (ICoreWebView2ExecuteScriptCompletedHandler *)sh);
}

void wv2_set_visible(WV2Host *h, BOOL visible)
{
    if (!h) return;
    wv2log("wv2_set_visible(%d) ready=%d", visible ? 1 : 0,
           h->ready ? 1 : 0);
    if (h->ready && h->controller) {
        h->controller->lpVtbl->put_IsVisible(h->controller, visible);
    } else {
        h->pending_visible_set = TRUE;
        h->pending_visible     = visible;
    }
    /* Belt and suspenders: hiding / showing the child HWND too is
     * what actually stops a DirectComposition composition pass when
     * the controller alone is not enough. */
    if (h->child) ShowWindow(h->child, visible ? SW_SHOW : SW_HIDE);
}

void wv2_set_msg_cb(WV2Host *h, WV2MsgCb cb, void *u)
{
    if (!h) return; h->msg_cb = cb; h->msg_user = u;
}
void wv2_set_ready_cb(WV2Host *h, WV2ReadyCb cb, void *u)
{
    if (!h) return; h->ready_cb = cb; h->ready_user = u;
}
void wv2_set_nav_cb(WV2Host *h, WV2NavCb cb, void *u)
{
    if (!h) return; h->nav_cb = cb; h->nav_user = u;
}

/* ------------------------------------------------------------------ */
/* Website-module additions (2026-06-16).                              */
/* ------------------------------------------------------------------ */

BOOL wv2_go_back(WV2Host *h)
{
    BOOL can = FALSE;
    if (!h || !h->ready || !h->webview) return FALSE;
    if (SUCCEEDED(h->webview->lpVtbl->get_CanGoBack(h->webview, &can)) && can) {
        h->webview->lpVtbl->GoBack(h->webview);
        return TRUE;
    }
    return FALSE;
}

BOOL wv2_go_forward(WV2Host *h)
{
    BOOL can = FALSE;
    if (!h || !h->ready || !h->webview) return FALSE;
    if (SUCCEEDED(h->webview->lpVtbl->get_CanGoForward(h->webview, &can)) && can) {
        h->webview->lpVtbl->GoForward(h->webview);
        return TRUE;
    }
    return FALSE;
}

void wv2_refresh(WV2Host *h)
{
    if (h && h->ready && h->webview) h->webview->lpVtbl->Reload(h->webview);
}

void wv2_stop(WV2Host *h)
{
    if (h && h->ready && h->webview) h->webview->lpVtbl->Stop(h->webview);
}

BOOL wv2_can_go_back(WV2Host *h)
{
    BOOL can = FALSE;
    if (h && h->ready && h->webview)
        h->webview->lpVtbl->get_CanGoBack(h->webview, &can);
    return can;
}

BOOL wv2_can_go_forward(WV2Host *h)
{
    BOOL can = FALSE;
    if (h && h->ready && h->webview)
        h->webview->lpVtbl->get_CanGoForward(h->webview, &can);
    return can;
}

void wv2_set_nav_start_cb(WV2Host *h, WV2NavStartCb cb, void *user)
{
    if (!h) return; h->nav_start_cb = cb; h->nav_start_user = user;
}

void wv2_set_nav_completed_cb(WV2Host *h, WV2NavCompletedCb cb, void *user)
{
    if (!h) return; h->nav_completed_cb = cb; h->nav_completed_user = user;
}
