/*
 * player_service.c - IMFMediaEngine-backed video service, pure C, MinGW UCRT64.
 *
 * Backend choice: per dispatch amendment 1, the v0.2.0 video stack is
 * IMFMediaEngine (Media Foundation Media Engine), HWND-hosted via the
 * MF_MEDIA_ENGINE_PLAYBACK_HWND attribute, with hardware decode via an
 * IMFDXGIDeviceManager bound to a D3D11 device. HLS (.m3u8) is handled
 * by Media Foundation's built-in network source resolver on Windows 10+.
 *
 * Aspect-ratio handling: in PLAYBACK_HWND mode the engine renders to
 * the entire client area of the assigned HWND. Per dispatch amendment
 * 2 / Phase L the letterbox / pillarbox math lives in the caller
 * (livetv_module): the caller creates a child HWND sized to the
 * inscribed video rect and hands it to player_create as the playback
 * surface, and recomputes / SetWindowPos's that child on every
 * WM_SIZE. player_service neither owns the video HWND nor resizes it.
 *
 * Pure-C COM consumption: every WinRT- or COM-style call goes through
 * the explicit lpVtbl member, e.g. eng->lpVtbl->Play(eng). IIDs are
 * instantiated locally via INITGUID since libmfuuid.a does not ship
 * IID_IMFMediaEngineEx / IID_IMFMediaEngineClassFactory /
 * IID_IMFMediaEngineNotify / IID_IMFDXGIDeviceManager / IID_IMFAttributes
 * as exported symbols.
 *
 * Threading: IMFMediaEngineNotify::EventNotify fires on an MF worker
 * thread. The service marshals every event to the UI thread by
 * PostMessage'ing WM_PLAYER_ENGINE_EVENT to the host HWND, with the
 * Player* carried as lParam. The host's WndProc calls
 * player_handle_window_event, which validates the lParam against the
 * single registered active player (guards against dangling pointers
 * if a queued event arrives after player_destroy) and invokes the
 * caller's PlayerStateCb. For load_hls the service also signals a
 * Win32 event so the caller can synchronously block on metadata arrival.
 */

#define INITGUID
#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <d3d11.h>
#include <d3d11_4.h>      /* ID3D11Multithread lives here, not d3d11.h */
#include <dxgi.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mfmediaengine.h>
#include <audiosessiontypes.h>  /* AudioCategory_Media */
#include <oleauto.h>
#include <stdarg.h>
#include <stdio.h>

#include "player_service.h"

/* ------------------------------------------------------------------ */
/* Diagnostic logging (dispatch amendment 2026-06-15-4 Phase A).       */
/*                                                                     */
/* Gated behind DEBUG_PLAYER_SERVICE so the shipping build can turn it */
/* off with a single define flip (Phase E). For this dispatch it is    */
/* on by default. The log captures:                                    */
/*   * every player_service entrypoint entry / return                  */
/*   * every IMFMediaEngineNotify event (decoded by name)              */
/*   * 100 ms polls of GetCurrentTime / Buffered / ReadyState /        */
/*     NetworkState / GetError during the first ~15 s of a session     */
/* The poll fires from a dedicated worker started at SetSource time.   */
/*                                                                     */
/* Path: %TEMP%\mgt_player_service.log (append mode, truncate at 1 MB) */
/* Threading: a process-wide CRITICAL_SECTION serializes writes; the   */
/* MF notify thread, the consumer's load worker, the poll thread, and  */
/* the UI thread all funnel through plog().                            */
/* ------------------------------------------------------------------ */

/* Default off for shipping builds. Re-enable by passing
 * -DDEBUG_PLAYER_SERVICE=1 to the compiler (the v0.2.0 build_x64.bat
 * does not), or by flipping the literal here for a one-off
 * investigation. */
#ifndef DEBUG_PLAYER_SERVICE
#define DEBUG_PLAYER_SERVICE 0
#endif

#if DEBUG_PLAYER_SERVICE

#define PLAYER_LOG_MAX_BYTES  (1 * 1024 * 1024)

static FILE             *g_log_fp        = NULL;
static ULONGLONG         g_log_t0_tick   = 0;
static CRITICAL_SECTION  g_log_cs;
static BOOL              g_log_cs_ready  = FALSE;
static ULONGLONG         g_log_last_flush_tick = 0;
static long              g_log_bytes_written   = 0;

/* Diag poll thread state. Lifetime: armed by player_load_hls
 * before SetSource, runs ~15 s polling at 100 ms cadence, exits
 * on its own. player_destroy signals the stop event so a destroy
 * mid-poll does not race the engine teardown. The thread takes an
 * IMFMediaEngine* (not Player*) so it can live above the struct
 * Player definition without circular ordering. */
static HANDLE                       g_poll_thread       = NULL;
static HANDLE                       g_poll_stop_event   = NULL;
static IMFMediaEngine * volatile    g_poll_target       = NULL;

static const char *evt_name(DWORD e)
{
    switch (e) {
    case 1:    return "LOADSTART";
    case 2:    return "PROGRESS";
    case 3:    return "SUSPEND";
    case 4:    return "ABORT";
    case 5:    return "ERROR";
    case 6:    return "EMPTIED";
    case 7:    return "STALLED";
    case 8:    return "PLAY";
    case 9:    return "PAUSE";
    case 10:   return "LOADEDMETADATA";
    case 11:   return "LOADEDDATA";
    case 12:   return "WAITING";
    case 13:   return "PLAYING";
    case 14:   return "CANPLAY";
    case 15:   return "CANPLAYTHROUGH";
    case 16:   return "SEEKING";
    case 17:   return "SEEKED";
    case 18:   return "TIMEUPDATE";
    case 19:   return "ENDED";
    case 20:   return "RATECHANGE";
    case 21:   return "DURATIONCHANGE";
    case 22:   return "VOLUMECHANGE";
    case 1000: return "FORMATCHANGE";
    case 1001: return "PURGEQUEUEDEVENTS";
    case 1002: return "TIMELINE_MARKER";
    case 1003: return "BALANCECHANGE";
    case 1004: return "DOWNLOADCOMPLETE";
    case 1005: return "BUFFERINGSTARTED";
    case 1006: return "BUFFERINGENDED";
    case 1007: return "FRAMESTEPCOMPLETED";
    case 1008: return "NOTIFYSTABLESTATE";
    case 1009: return "FIRSTFRAMEREADY";
    case 1010: return "TRACKSCHANGE";
    case 1011: return "OPMINFO";
    case 1012: return "RESOURCELOST";
    case 1013: return "DELAYLOADEVENT_CHANGED";
    case 1014: return "STREAMRENDERINGERROR";
    case 1015: return "SUPPORTEDRATES_CHANGED";
    case 1016: return "AUDIOENDPOINTCHANGE";
    default:   return "?";
    }
}

static void plog_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_ready = TRUE;
    } else {
        while (!g_log_cs_ready) Sleep(0);
    }
}

static void plog_open(void)
{
    char path[MAX_PATH];
    DWORD n;
    plog_init_cs_once();
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) { LeaveCriticalSection(&g_log_cs); return; }
    n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) lstrcpyA(path, "C:\\Windows\\Temp");
    lstrcatA(path, "\\mgt_player_service.log");
    g_log_fp = fopen(path, "a");
    g_log_t0_tick = GetTickCount64();
    g_log_last_flush_tick = g_log_t0_tick;
    g_log_bytes_written = 0;
    if (g_log_fp) {
        fprintf(g_log_fp,
                "\n==== mgt_player_service.log session opened (t0_tick=%llu) ====\n",
                (unsigned long long)g_log_t0_tick);
        fflush(g_log_fp);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void plog_close(void)
{
    if (!g_log_cs_ready) return;
    EnterCriticalSection(&g_log_cs);
    if (g_log_fp) {
        fprintf(g_log_fp, "==== session closed ====\n");
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    LeaveCriticalSection(&g_log_cs);
}

static void plog(const char *fmt, ...)
{
    char    line[512];
    int     n;
    va_list ap;
    ULONGLONG now;
    if (!g_log_cs_ready) return;
    EnterCriticalSection(&g_log_cs);
    if (!g_log_fp) { LeaveCriticalSection(&g_log_cs); return; }
    if (g_log_bytes_written >= PLAYER_LOG_MAX_BYTES) {
        LeaveCriticalSection(&g_log_cs);
        return;
    }
    now = GetTickCount64();
    n = _snprintf(line, sizeof(line), "%6lu  ",
                  (unsigned long)(now - g_log_t0_tick));
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = '\0';
    lstrcatA(line, "\n");
    fputs(line, g_log_fp);
    g_log_bytes_written += (long)strlen(line);
    if ((now - g_log_last_flush_tick) >= 1000) {
        fflush(g_log_fp);
        g_log_last_flush_tick = now;
    }
    LeaveCriticalSection(&g_log_cs);
}

/* GetBuffered helper for logging only -- safe to call from the poll
 * thread because the engine outlives load_hls and player_stop drives
 * teardown only after the poll thread signals exit. */
static void log_buffered(IMFMediaEngine *engine, double *out_total_sec,
                         DWORD *out_count)
{
    IMFMediaTimeRange *tr = NULL;
    DWORD i, count = 0;
    double total = 0.0;
    HRESULT hr;
    if (!engine) { *out_total_sec = 0.0; *out_count = 0; return; }
    hr = engine->lpVtbl->GetBuffered(engine, &tr);
    if (SUCCEEDED(hr) && tr) {
        count = tr->lpVtbl->GetLength(tr);
        for (i = 0; i < count; i++) {
            double rs = 0.0, re = 0.0;
            if (SUCCEEDED(tr->lpVtbl->GetStart(tr, i, &rs)) &&
                SUCCEEDED(tr->lpVtbl->GetEnd(tr, i, &re)) && re > rs) {
                total += (re - rs);
            }
        }
        tr->lpVtbl->Release(tr);
    }
    *out_total_sec = total;
    *out_count = count;
}

static DWORD WINAPI poll_thread_proc(LPVOID arg)
{
    IMFMediaEngine *eng = (IMFMediaEngine *)arg;
    int    elapsed_ms = 0;
    const int POLL_MS    = 100;
    const int POLL_TOTAL = 15000;
    plog("poll-thread start");
    while (elapsed_ms < POLL_TOTAL && eng == g_poll_target) {
        if (g_poll_stop_event &&
            WaitForSingleObject(g_poll_stop_event, POLL_MS) == WAIT_OBJECT_0) {
            plog("poll-thread stopped by signal at +%d ms", elapsed_ms);
            return 0;
        }
        elapsed_ms += POLL_MS;
        if (!eng) continue;
        {
            double cur = eng->lpVtbl->GetCurrentTime(eng);
            USHORT rs  = eng->lpVtbl->GetReadyState(eng);
            USHORT ns  = eng->lpVtbl->GetNetworkState(eng);
            double buf_total = 0.0;
            DWORD  buf_count = 0;
            USHORT err_code  = 0;
            IMFMediaError *me = NULL;
            log_buffered(eng, &buf_total, &buf_count);
            if (SUCCEEDED(eng->lpVtbl->GetError(eng, &me)) && me) {
                err_code = me->lpVtbl->GetErrorCode(me);
                me->lpVtbl->Release(me);
            }
            plog("poll  curr=%.3f buf=%.3f(%lu)  ready=%u  net=%u  err=%u",
                 cur, buf_total, (unsigned long)buf_count,
                 (unsigned)rs, (unsigned)ns, (unsigned)err_code);
        }
    }
    plog("poll-thread end (elapsed=%d ms)", elapsed_ms);
    return 0;
}

static void poll_thread_arm(IMFMediaEngine *eng)
{
    if (g_poll_thread) return;
    if (!g_poll_stop_event)
        g_poll_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_poll_target = eng;
    if (g_poll_stop_event) ResetEvent(g_poll_stop_event);
    g_poll_thread = CreateThread(NULL, 0, poll_thread_proc, (LPVOID)eng, 0, NULL);
}

static void poll_thread_stop(void)
{
    if (g_poll_stop_event) SetEvent(g_poll_stop_event);
    if (g_poll_thread) {
        WaitForSingleObject(g_poll_thread, 1000);
        CloseHandle(g_poll_thread);
        g_poll_thread = NULL;
    }
    if (g_poll_stop_event) {
        CloseHandle(g_poll_stop_event);
        g_poll_stop_event = NULL;
    }
    g_poll_target = NULL;
}

#else  /* DEBUG_PLAYER_SERVICE */

#define plog(...) ((void)0)
static void plog_open(void)  {}
static void plog_close(void) {}
static void poll_thread_arm(IMFMediaEngine *eng) { (void)eng; }
static void poll_thread_stop(void) {}
static const char *evt_name(DWORD e) { (void)e; return ""; }

#endif /* DEBUG_PLAYER_SERVICE */

/* ------------------------------------------------------------------ */
/* Process-wide MF + COM init refcount.                                */
/* ------------------------------------------------------------------ */

static LONG            g_init_refcount = 0;
static CRITICAL_SECTION g_init_cs;
static BOOL            g_init_cs_ready = FALSE;
static BOOL            g_mf_started    = FALSE;
static BOOL            g_co_inited     = FALSE;

static void player_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_init_cs);
        g_init_cs_ready = TRUE;
    } else {
        while (!g_init_cs_ready) Sleep(0);
    }
}

BOOL player_init(void)
{
    HRESULT hr_co;
    HRESULT hr_mf;
    BOOL ok = TRUE;

    player_init_cs_once();
    EnterCriticalSection(&g_init_cs);

    if (g_init_refcount == 0) {
        plog_open();
        plog("player_init start");
        hr_co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hr_co == S_OK || hr_co == S_FALSE) {
            g_co_inited = TRUE;
        } else if (hr_co == RPC_E_CHANGED_MODE) {
            /* Some other component already initialized this thread
             * apartment-threaded; leave it alone. MF still works. */
            g_co_inited = FALSE;
        } else {
            ok = FALSE;
        }

        if (ok) {
            hr_mf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
            if (SUCCEEDED(hr_mf)) {
                g_mf_started = TRUE;
            } else {
                if (g_co_inited) { CoUninitialize(); g_co_inited = FALSE; }
                ok = FALSE;
            }
        }
    }

    if (ok) g_init_refcount++;

    plog("player_init done ok=%d refcount=%ld", ok ? 1 : 0, g_init_refcount);
    LeaveCriticalSection(&g_init_cs);
    return ok;
}

void player_shutdown(void)
{
    if (!g_init_cs_ready) return;
    EnterCriticalSection(&g_init_cs);
    plog("player_shutdown refcount=%ld", g_init_refcount);
    if (g_init_refcount > 0) {
        g_init_refcount--;
        if (g_init_refcount == 0) {
            if (g_mf_started) { MFShutdown(); g_mf_started = FALSE; }
            if (g_co_inited)  { CoUninitialize(); g_co_inited = FALSE; }
            plog("player_shutdown closing log");
            plog_close();
        }
    }
    LeaveCriticalSection(&g_init_cs);
}

/* ------------------------------------------------------------------ */
/* PlayerNotify: heap-allocated IMFMediaEngineNotify implementation.   */
/* ------------------------------------------------------------------ */

typedef struct PlayerNotify {
    /* MUST be first - this struct is cast to IMFMediaEngineNotify*. */
    IMFMediaEngineNotifyVtbl *lpVtbl;
    LONG     refcount;
    HWND     host_hwnd;
    HANDLE   loaded_event;
    Player  *owner;
} PlayerNotify;

/* Active player pointer, guards against queued WM_PLAYER_ENGINE_EVENT
 * events arriving after player_destroy. v0.2.0 only ever has one Player
 * alive at a time (Live TV is the sole consumer); this asserts that. */
static Player *volatile g_active_player = NULL;

static HRESULT STDMETHODCALLTYPE PlayerNotify_QueryInterface(
    IMFMediaEngineNotify *iface, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IMFMediaEngineNotify)) {
        *ppv = iface;
        iface->lpVtbl->AddRef(iface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE PlayerNotify_AddRef(IMFMediaEngineNotify *iface)
{
    PlayerNotify *self = (PlayerNotify *)iface;
    return (ULONG)InterlockedIncrement(&self->refcount);
}

static ULONG STDMETHODCALLTYPE PlayerNotify_Release(IMFMediaEngineNotify *iface)
{
    PlayerNotify *self = (PlayerNotify *)iface;
    LONG r = InterlockedDecrement(&self->refcount);
    if (r == 0) {
        if (self->loaded_event) { CloseHandle(self->loaded_event); self->loaded_event = NULL; }
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE PlayerNotify_EventNotify(
    IMFMediaEngineNotify *iface, DWORD event, DWORD_PTR param1, DWORD param2)
{
    PlayerNotify *self = (PlayerNotify *)iface;
    Player *owner = self->owner;

    plog("evt   %u (%s)  p1=%lu p2=%lu", (unsigned)event, evt_name(event),
         (unsigned long)param1, (unsigned long)param2);
    (void)param1;
    (void)param2;

    /* Signal the synchronous loader waiter only once MF reports
     * CANPLAYTHROUGH (event 15) or an error. CANPLAYTHROUGH is the MF
     * analog of the HTML5 video event of the same name -- MF has
     * buffered enough to play to the end without stalling on most
     * networks. Releasing the loader on the earlier LOADEDMETADATA /
     * CANPLAY pair (which is what we used to do) starts playback while
     * the network buffer is still ~empty, producing the start ->
     * 1.5 s playback -> 2 s silence -> resume underrun Dimitri
     * reported on the Live Radio HE-AAC stream. Per dispatch
     * amendment 2026-06-15 (item 7). LOADEDMETADATA still fires on
     * the UI thread via player_handle_window_event so video consumers
     * can cache native size before play. */
    if (event == MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH ||
        event == MF_MEDIA_ENGINE_EVENT_ERROR) {
        if (self->loaded_event) SetEvent(self->loaded_event);
    }

    /* Marshal to UI thread. owner may be NULL if the player is mid-destroy. */
    if (owner && self->host_hwnd) {
        PostMessageA(self->host_hwnd, WM_PLAYER_ENGINE_EVENT,
                     (WPARAM)event, (LPARAM)owner);
    }
    return S_OK;
}

static IMFMediaEngineNotifyVtbl g_PlayerNotify_Vtbl = {
    PlayerNotify_QueryInterface,
    PlayerNotify_AddRef,
    PlayerNotify_Release,
    PlayerNotify_EventNotify
};

/* ------------------------------------------------------------------ */
/* Player struct.                                                      */
/* ------------------------------------------------------------------ */

struct Player {
    HWND  video_hwnd;     /* caller-owned PLAYBACK_HWND */

    ID3D11Device         *d3d_device;
    ID3D11DeviceContext  *d3d_context;
    IMFDXGIDeviceManager *dxgi_manager;
    IMFAttributes        *attrs;
    IMFMediaEngineClassFactory *factory;
    IMFMediaEngine       *engine;
    IMFMediaEngineEx     *engine_ex;
    PlayerNotify         *notify;

    PlayerState   state;
    PlayerStateCb cb;
    void         *cb_user;

    int  native_w;
    int  native_h;
    BOOL have_native_size;

    /* Dispatch amendment 2026-06-15-5: TRUE during the muted warm-up
     * window in player_load_hls. Used by player_handle_window_event
     * to suppress the consumer-visible PLAYER_STATE_PLAYING callback
     * that MF naturally fires when warm-up Play() lands -- if the
     * consumer transitions to its PLAYING UI before the unmute, the
     * user sees video frames or VU meter activity for ~5 s with no
     * audible audio, which Dimitri (correctly) reads as broken. The
     * synthetic PLAYING re-fire happens at the end of load_hls. */
    volatile BOOL warming_up;
};

/* ------------------------------------------------------------------ */
/* player_create / destroy.                                            */
/* ------------------------------------------------------------------ */

static const D3D_FEATURE_LEVEL g_feature_levels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0
};

static BOOL player_create_d3d(Player *p)
{
    HRESULT hr;
    D3D_FEATURE_LEVEL got;
    ID3D11Multithread *mt = NULL;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                           g_feature_levels,
                           (UINT)(sizeof(g_feature_levels) / sizeof(g_feature_levels[0])),
                           D3D11_SDK_VERSION,
                           &p->d3d_device, &got, &p->d3d_context);
    if (FAILED(hr)) {
        /* Retry on WARP for environments without a hardware video pipeline
         * (e.g. some VMs and remote-desktop sessions). */
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               g_feature_levels,
                               (UINT)(sizeof(g_feature_levels) / sizeof(g_feature_levels[0])),
                               D3D11_SDK_VERSION,
                               &p->d3d_device, &got, &p->d3d_context);
        if (FAILED(hr)) return FALSE;
    }

    /* MF requires the D3D11 device's immediate context to be multithread-
     * protected when shared across MF worker threads. */
    hr = p->d3d_device->lpVtbl->QueryInterface(p->d3d_device,
                                               &IID_ID3D11Multithread,
                                               (void **)&mt);
    if (SUCCEEDED(hr) && mt) {
        mt->lpVtbl->SetMultithreadProtected(mt, TRUE);
        mt->lpVtbl->Release(mt);
    }
    return TRUE;
}

static Player *player_create_internal(HWND msg_target, HWND video_hwnd)
{
    Player *p;
    PlayerNotify *n;
    UINT reset_token = 0;
    HRESULT hr;
    HWND post_target;

    if (!msg_target) return NULL;  /* WM_PLAYER_ENGINE_EVENT needs a target */

    p = (Player *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Player));
    if (!p) return NULL;
    p->video_hwnd = video_hwnd;     /* NULL = audio-only */
    p->state = PLAYER_STATE_IDLE;

    /* Audio-only path skips the entire D3D11 + DXGI manager setup;
     * MF renders audio through the default WASAPI endpoint. Video
     * path keeps D3D11 + DXGI for hardware decode + HWND-bound swap. */
    if (video_hwnd) {
        if (!player_create_d3d(p)) goto fail;

        hr = MFCreateDXGIDeviceManager(&reset_token, &p->dxgi_manager);
        if (FAILED(hr)) goto fail;
        hr = p->dxgi_manager->lpVtbl->ResetDevice(p->dxgi_manager,
                                                  (IUnknown *)p->d3d_device,
                                                  reset_token);
        if (FAILED(hr)) goto fail;
    }

    post_target = msg_target;

    n = (PlayerNotify *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                  sizeof(PlayerNotify));
    if (!n) goto fail;
    n->lpVtbl       = &g_PlayerNotify_Vtbl;
    n->refcount     = 1;
    n->host_hwnd    = post_target;
    n->owner        = p;
    n->loaded_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    p->notify = n;

    hr = MFCreateAttributes(&p->attrs, 6);
    if (FAILED(hr)) goto fail;
    p->attrs->lpVtbl->SetUnknown(p->attrs, &MF_MEDIA_ENGINE_CALLBACK,
                                 (IUnknown *)n);
    if (video_hwnd) {
        p->attrs->lpVtbl->SetUnknown(p->attrs, &MF_MEDIA_ENGINE_DXGI_MANAGER,
                                     (IUnknown *)p->dxgi_manager);
        p->attrs->lpVtbl->SetUINT64(p->attrs, &MF_MEDIA_ENGINE_PLAYBACK_HWND,
                                    (UINT64)(ULONG_PTR)video_hwnd);
        p->attrs->lpVtbl->SetUINT32(p->attrs, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                    DXGI_FORMAT_B8G8R8A8_UNORM);
    }
    /* Audio-only path leaves DXGI / PLAYBACK_HWND / VIDEO_OUTPUT_FORMAT
     * unset; MF binds only to the default WASAPI audio endpoint. */

    /* Dispatch amendment 2026-06-15-4 (B2): hint MF to use the
     * AudioCategory_Media audio session category. Prior to this the
     * default category landed on "Other" which Windows treats as a
     * low-priority session that can be ducked or stalled by the OS
     * audio scheduler. AudioCategory_Media keeps the session active
     * and is what HE-AAC streaming radios want. The enum value (11)
     * is defined in audiosessiontypes.h. */
    p->attrs->lpVtbl->SetUINT32(p->attrs, &MF_MEDIA_ENGINE_AUDIO_CATEGORY,
                                (UINT32)AudioCategory_Media);
    plog("attr  AUDIO_CATEGORY = %u (Media)", (unsigned)AudioCategory_Media);

    hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL,
                          CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory,
                          (void **)&p->factory);
    if (FAILED(hr)) goto fail;

    hr = p->factory->lpVtbl->CreateInstance(p->factory, 0, p->attrs, &p->engine);
    if (FAILED(hr)) goto fail;

    /* IMFMediaEngineEx for SetSource(BSTR) and richer control. */
    hr = p->engine->lpVtbl->QueryInterface(p->engine, &IID_IMFMediaEngineEx,
                                           (void **)&p->engine_ex);
    if (FAILED(hr)) goto fail;

    /* Dispatch amendment 2026-06-15-4 (B1): mark the engine as a
     * real-time source. Default behavior treats an HTTP audio URL
     * as a seekable VOD file and runs a startup "seek to 0"
     * cycle right after first bytes arrive, which we suspect is
     * the cause of Dimitri's "1.5 s play -> 2 s silence -> resume"
     * underrun. SetRealTimeMode(TRUE) is documented as the toggle
     * to suppress that behavior for live streams. */
    {
        HRESULT hrtm = p->engine_ex->lpVtbl->SetRealTimeMode(p->engine_ex, TRUE);
        plog("SetRealTimeMode(TRUE) -> hr=0x%lx", (unsigned long)hrtm);
    }

    g_active_player = p;
    plog("player_create_internal done  audio_only=%d", video_hwnd ? 0 : 1);
    return p;

fail:
    plog("player_create_internal FAIL hr=0x%lx", (unsigned long)hr);
    player_destroy(p);
    return NULL;
}

Player *player_create(HWND video_hwnd)
{
    HWND msg_target;
    plog("player_create(video_hwnd=%p)", (void *)video_hwnd);
    if (!video_hwnd) return NULL;
    msg_target = GetParent(video_hwnd);
    if (!msg_target) msg_target = video_hwnd;
    return player_create_internal(msg_target, video_hwnd);
}

Player *player_create_audio(HWND msg_target)
{
    plog("player_create_audio(msg_target=%p)", (void *)msg_target);
    return player_create_internal(msg_target, NULL);
}

void player_destroy(Player *p)
{
    MSG msg;

    plog("player_destroy(p=%p)", (void *)p);
    if (!p) return;

    /* Stop the diag poll thread first so its 100 ms tick cannot race
     * against the engine teardown that follows. */
    poll_thread_stop();

    /* Drop the active-player slot before tearing anything down. Late
     * EventNotify calls now see owner=NULL via notify.owner and skip
     * the PostMessage; any already-queued WM_PLAYER_ENGINE_EVENT
     * messages will fail the g_active_player check and get dropped. */
    if (g_active_player == p) g_active_player = NULL;
    if (p->notify) p->notify->owner = NULL;

    /* Drain any queued events for the message host so the host's
     * WndProc does not see a stale Player* after we return. */
    if (p->notify && p->notify->host_hwnd) {
        while (PeekMessageA(&msg, p->notify->host_hwnd,
                            WM_PLAYER_ENGINE_EVENT,
                            WM_PLAYER_ENGINE_EVENT, PM_REMOVE)) { /* drop */ }
    }

    if (p->engine_ex) { p->engine_ex->lpVtbl->Shutdown(p->engine_ex); }
    else if (p->engine) { p->engine->lpVtbl->Shutdown(p->engine); }

    if (p->engine_ex)   { p->engine_ex->lpVtbl->Release(p->engine_ex);   p->engine_ex = NULL; }
    if (p->engine)      { p->engine->lpVtbl->Release(p->engine);         p->engine = NULL; }
    if (p->factory)     { p->factory->lpVtbl->Release(p->factory);       p->factory = NULL; }
    if (p->attrs)       { p->attrs->lpVtbl->Release(p->attrs);           p->attrs = NULL; }
    if (p->dxgi_manager){ p->dxgi_manager->lpVtbl->Release(p->dxgi_manager); p->dxgi_manager = NULL; }
    if (p->d3d_context) { p->d3d_context->lpVtbl->Release(p->d3d_context); p->d3d_context = NULL; }
    if (p->d3d_device)  { p->d3d_device->lpVtbl->Release(p->d3d_device);   p->d3d_device = NULL; }
    if (p->notify) {
        ((IMFMediaEngineNotify *)p->notify)->lpVtbl->Release(
            (IMFMediaEngineNotify *)p->notify);
        p->notify = NULL;
    }

    /* video_hwnd is caller-owned; do NOT destroy it. */
    p->video_hwnd = NULL;

    HeapFree(GetProcessHeap(), 0, p);
}

/* ------------------------------------------------------------------ */
/* Source / play / stop / size.                                        */
/* ------------------------------------------------------------------ */

BOOL player_load_hls(Player *p, const wchar_t *url)
{
    BSTR bstr;
    HRESULT hr;
    DWORD wait;

    {
        char ascii_url[512];
        int  i;
        for (i = 0; i < (int)sizeof(ascii_url) - 1 && url && url[i]; i++)
            ascii_url[i] = (url[i] > 0 && url[i] < 0x80) ? (char)url[i] : '?';
        ascii_url[i] = '\0';
        plog("player_load_hls(\"%s\")", ascii_url);
    }
    if (!p || !p->engine_ex || !url) return FALSE;

    bstr = SysAllocString((const OLECHAR *)url);
    if (!bstr) return FALSE;

    if (p->notify && p->notify->loaded_event) ResetEvent(p->notify->loaded_event);
    p->have_native_size = FALSE;
    p->state = PLAYER_STATE_LOADING;
    if (p->cb) p->cb(p, p->state, p->cb_user);

    /* Arm the diag poll thread BEFORE SetSource so we capture the
     * full sequence including any pre-PLAYING activity. */
    poll_thread_arm(p->engine);

    hr = p->engine_ex->lpVtbl->SetSource(p->engine_ex, bstr);
    plog("SetSource -> hr=0x%lx", (unsigned long)hr);
    SysFreeString(bstr);
    if (FAILED(hr)) {
        p->state = PLAYER_STATE_ERROR;
        if (p->cb) p->cb(p, p->state, p->cb_user);
        return FALSE;
    }

    /* Wait for CANPLAYTHROUGH (event 15) -- MF has buffered enough for
     * continuous playback -- or an error. Per dispatch amendment
     * 2026-06-15 (item 7) this is the 10 s watchdog: if CANPLAYTHROUGH
     * never fires (slow stream, stream metadata weirdness), fall back
     * to "consider it loaded" so the consumer's Play() still goes
     * through. The trade-off is documented in the v0.2.0_LIVE_RADIO
     * dispatch: start latency may grow from ~2-3 s to ~3-8 s, but
     * playback no longer underruns 1.5 s in. WaitForSingleObject with
     * a 10 s timeout is the watchdog (functionally equivalent to a
     * SetTimer + KillTimer pair, but available to the worker thread
     * that calls player_load_hls without needing a window). */
    plog("waiting for CANPLAYTHROUGH (10 s watchdog)");
    wait = WaitForSingleObject(p->notify ? p->notify->loaded_event : NULL,
                               10000);
    plog("CANPLAYTHROUGH wait -> %lu  state=%d",
         (unsigned long)wait, (int)p->state);
    if (p->state == PLAYER_STATE_ERROR) {
        plog("load_hls early-return ERROR");
        if (p->cb) p->cb(p, p->state, p->cb_user);
        return FALSE;
    }

    /* Dispatch amendment 2026-06-15-4: the GetBuffered-based wait
     * from amendment 2 was disastrously wrong for live Icecast
     * streams. The diag log from 2026-06-15-4 Phase A shows
     * IMFMediaEngine::GetBuffered returning ZERO time ranges
     * (buf=0.000(0)) for the entire 15 s watchdog when the source
     * is a live HE-AAC stream -- the buffered-time-range API is
     * defined only for seekable VOD, not for live HTTP audio.
     *
     * The real green light for live streams is CANPLAYTHROUGH
     * combined with readyState >= HAVE_ENOUGH_DATA (=4), both of
     * which fire about 1.8 s after SetSource on the Greek Radio
     * stream.
     *
     * Even with SetRealTimeMode(TRUE) + AudioCategory_Media set,
     * the cycle 1 diag still showed one natural WAITING /
     * BUFFERINGSTARTED -> BUFFERINGENDED pair at ~2.9 s into
     * playback (2.0 s of stall) before audio settles into
     * continuous delivery. That stall is what Dimitri perceives as
     * the underrun.
     *
     * The cycle-2 fix is a "muted warm-up": start the engine playing
     * while muted, wait through the natural stall cycle, then
     * unmute. The audio session is warmed continuously, the
     * underrun fires while muted (the user hears nothing), and
     * when we unmute the buffer is already in steady state.
     * Total Click-to-audio latency: ~7-8 s, matching the
     * documented trade-off envelope. */
    (void)wait;
    if (p->state != PLAYER_STATE_ERROR && p->engine) {
        const DWORD WARMUP_MS = 5000;
        HRESULT hr_warm;
        /* Set the gate BEFORE Play() so MF's PLAYING event during
         * warm-up is absorbed silently in handle_window_event. The
         * consumer stays in its LOADING UI for the full warm-up. */
        p->warming_up = TRUE;
        /* AddRef the engine so a concurrent player_destroy (e.g.
         * window close mid-warm-up) does not free the engine memory
         * out from under our SetMuted(FALSE). Destroy still runs
         * Shutdown() which makes further calls return MF_E_SHUTDOWN
         * instead of crashing. */
        p->engine->lpVtbl->AddRef(p->engine);
        plog("warm-up start: SetMuted(TRUE) + Play()  warming_up=TRUE");
        hr_warm = p->engine->lpVtbl->SetMuted(p->engine, TRUE);
        plog("  SetMuted(TRUE) -> hr=0x%lx", (unsigned long)hr_warm);
        hr_warm = p->engine->lpVtbl->Play(p->engine);
        plog("  Play (warm) -> hr=0x%lx", (unsigned long)hr_warm);
        Sleep(WARMUP_MS);
        hr_warm = p->engine->lpVtbl->SetMuted(p->engine, FALSE);
        plog("warm-up end: SetMuted(FALSE) -> hr=0x%lx",
             (unsigned long)hr_warm);
        p->warming_up = FALSE;
        /* Re-fire PLAYING to the host hwnd so handle_window_event
         * runs with warming_up == FALSE and drives the consumer to
         * its PLAYING UI in sync with the unmute. Skip if the player
         * was destroyed during the warm-up sleep (g_active_player
         * reset) or if the engine has been Shutdown. */
        if (p == g_active_player && p->notify && p->notify->host_hwnd) {
            PostMessageA(p->notify->host_hwnd, WM_PLAYER_ENGINE_EVENT,
                         (WPARAM)MF_MEDIA_ENGINE_EVENT_PLAYING,
                         (LPARAM)p);
            plog("posted synthetic PLAYING after warm-up");
        }
        p->engine->lpVtbl->Release(p->engine);
    } else {
        /* Error path: never armed the gate, no synthetic re-fire
         * needed. The consumer's error UI takes over from the
         * existing PLAYER_STATE_ERROR callback. */
    }

    if (p->state == PLAYER_STATE_ERROR) {
        plog("load_hls late-return ERROR");
        if (p->cb) p->cb(p, p->state, p->cb_user);
        return FALSE;
    }
    if (p->state == PLAYER_STATE_LOADING) {
        p->state = PLAYER_STATE_LOADED;
        if (p->cb) p->cb(p, p->state, p->cb_user);
    }
    plog("load_hls return TRUE  state=%d", (int)p->state);
    return TRUE;
}

BOOL player_play(Player *p)
{
    HRESULT hr;
    plog("player_play(p=%p)", (void *)p);
    if (!p || !p->engine) return FALSE;
    hr = p->engine->lpVtbl->Play(p->engine);
    plog("  Play -> hr=0x%lx", (unsigned long)hr);
    if (FAILED(hr)) return FALSE;
    /* PLAYING state will arrive via EventNotify -> handle_window_event. */
    return TRUE;
}

BOOL player_stop(Player *p)
{
    HRESULT hr;
    plog("player_stop(p=%p)", (void *)p);
    if (!p || !p->engine) return FALSE;
    hr = p->engine->lpVtbl->Pause(p->engine);
    plog("  Pause -> hr=0x%lx", (unsigned long)hr);
    if (FAILED(hr)) return FALSE;
    p->state = PLAYER_STATE_STOPPED;
    if (p->cb) p->cb(p, p->state, p->cb_user);
    return TRUE;
}

BOOL player_get_native_size(Player *p, int *w, int *h)
{
    DWORD cx = 0, cy = 0;
    HRESULT hr;

    if (!p || !p->engine) return FALSE;
    hr = p->engine->lpVtbl->GetNativeVideoSize(p->engine, &cx, &cy);
    if (FAILED(hr) || cx == 0 || cy == 0) return FALSE;
    if (w) *w = (int)cx;
    if (h) *h = (int)cy;
    p->native_w = (int)cx;
    p->native_h = (int)cy;
    p->have_native_size = TRUE;
    return TRUE;
}

void player_set_state_cb(Player *p, PlayerStateCb cb, void *user)
{
    if (!p) return;
    p->cb = cb;
    p->cb_user = user;
}

void player_update_video_stream(Player *p)
{
    RECT rc;
    /* Audio-only players have no video_hwnd; nothing to refresh. */
    if (!p || !p->engine_ex || !p->video_hwnd) return;
    if (!GetClientRect(p->video_hwnd, &rc)) return;
    /* dst rect is in client coords of PLAYBACK_HWND; src NULL = full
     * frame; border NULL = engine default. Forces MF to rebuild the
     * swap chain at the new HWND size. */
    p->engine_ex->lpVtbl->UpdateVideoStream(p->engine_ex, NULL, &rc, NULL);
}

LRESULT player_handle_window_event(WPARAM wParam, LPARAM lParam)
{
    Player *p = (Player *)lParam;
    DWORD   event = (DWORD)wParam;
    PlayerState new_state;
    BOOL emit = FALSE;

    /* Guard against dangling Player* (post-destroy queued event). */
    if (!p || p != g_active_player) return 0;

    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        player_get_native_size(p, NULL, NULL);
        if (p->state == PLAYER_STATE_LOADING) {
            new_state = PLAYER_STATE_LOADED;
            p->state = new_state;
            emit = TRUE;
        }
        /* livetv_module's lt_player_state_cb (fired below by the emit
         * branch) will call player_get_native_size + cache aspect +
         * force-resize the video host child via SendMessage(WM_SIZE).
         * player_service does not own that geometry anymore. */
        break;

    case MF_MEDIA_ENGINE_EVENT_PLAYING:
        /* During the muted warm-up window the consumer must NOT see
         * a PLAYING transition; load_hls will re-post the event after
         * SetMuted(FALSE) so the UI and the audible audio start
         * together. We still absorb MF's event silently (no state
         * write, no callback) -- the post-warm-up re-fire will run
         * this branch with warming_up == FALSE and do the normal
         * transition. */
        if (p->warming_up) break;
        new_state = PLAYER_STATE_PLAYING;
        if (p->state != new_state) { p->state = new_state; emit = TRUE; }
        break;

    case MF_MEDIA_ENGINE_EVENT_PAUSE:
        if (p->state != PLAYER_STATE_STOPPED) {
            new_state = PLAYER_STATE_STOPPED;
            p->state = new_state;
            emit = TRUE;
        }
        break;

    case MF_MEDIA_ENGINE_EVENT_ERROR:
        new_state = PLAYER_STATE_ERROR;
        if (p->state != new_state) { p->state = new_state; emit = TRUE; }
        break;
    }

    if (emit && p->cb) p->cb(p, p->state, p->cb_user);
    return 0;
}
