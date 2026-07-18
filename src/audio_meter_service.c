/*
 * audio_meter_service.c - per-session WASAPI peak metering, pure C.
 *
 * Per-session path (preferred, dispatch 3 / locked decision):
 *   IMMDeviceEnumerator -> default eRender / eConsole endpoint
 *   IMMDevice::Activate(IID_IAudioSessionManager2) -> manager
 *   manager->GetSessionEnumerator -> enumerator
 *   for each session: QI IAudioSessionControl2 -> GetProcessId
 *   when pid == GetCurrentProcessId(): QI IAudioMeterInformation
 *
 * Device-level fallback (if the per-session QI is rejected on this
 * driver, which is rare but the documented fallback for deadlock C):
 *   IMMDevice::Activate(IID_IAudioMeterInformation) on the endpoint
 *
 * The two paths share the same IAudioMeterInformation surface, so
 * audio_meter_poll is identical for both. The session lookup retries
 * on each poll until our process appears in the enumerator (the
 * session is only registered with the audio engine after MF actually
 * starts pushing samples), then caches.
 */

#define INITGUID
#define COBJMACROS

#include <windows.h>
#include <initguid.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <audiosessiontypes.h>

#include "audio_meter_service.h"

/* ------------------------------------------------------------------ */
/* IAudioMeterInformation hand-roll.                                   */
/*                                                                     */
/* MinGW UCRT64's endpointvolume.h ships only the forward typedef for  */
/* IAudioMeterInformation: no vtable, no IID. The full ABI is defined  */
/* by Microsoft in EndpointVolume.idl and we replicate the C-callable  */
/* slice here (4 interface methods plus the 3 IUnknown methods).       */
/* IID source: https://learn.microsoft.com/en-us/windows/win32/api/    */
/*   endpointvolume/nn-endpointvolume-iaudiometerinformation           */
/* ------------------------------------------------------------------ */

DEFINE_GUID(IID_IAudioMeterInformation,
            0xc02216f6, 0x8c67, 0x4b5b, 0x9d, 0x00, 0xd0, 0x08, 0xe7, 0x3e, 0x00, 0x64);

typedef struct IAudioMeterInformationVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAudioMeterInformation *This,
                                                REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAudioMeterInformation *This);
    ULONG   (STDMETHODCALLTYPE *Release)(IAudioMeterInformation *This);
    HRESULT (STDMETHODCALLTYPE *GetPeakValue)(IAudioMeterInformation *This,
                                              float *pfPeak);
    HRESULT (STDMETHODCALLTYPE *GetMeteringChannelCount)(IAudioMeterInformation *This,
                                                        UINT *pnChannelCount);
    HRESULT (STDMETHODCALLTYPE *GetChannelsPeakValues)(IAudioMeterInformation *This,
                                                      UINT u32ChannelCount,
                                                      float *afPeakValues);
    HRESULT (STDMETHODCALLTYPE *QueryHardwareSupport)(IAudioMeterInformation *This,
                                                     DWORD *pdwHardwareSupportMask);
} IAudioMeterInformationVtbl;

struct IAudioMeterInformation {
    CONST_VTBL IAudioMeterInformationVtbl *lpVtbl;
};

/* ------------------------------------------------------------------ */
/* Process-wide init refcount.                                         */
/* ------------------------------------------------------------------ */

static LONG            g_init_refcount = 0;
static CRITICAL_SECTION g_init_cs;
static BOOL            g_init_cs_ready = FALSE;
static BOOL            g_co_inited     = FALSE;

static IMMDeviceEnumerator *g_enum    = NULL;
static IMMDevice           *g_device  = NULL;
static IAudioSessionManager2 *g_mgr   = NULL;

static void am_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_init_cs);
        g_init_cs_ready = TRUE;
    } else {
        while (!g_init_cs_ready) Sleep(0);
    }
}

static BOOL am_endpoint_bind_locked(void)
{
    HRESULT hr;
    if (g_mgr) return TRUE;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMMDeviceEnumerator, (void **)&g_enum);
    if (FAILED(hr)) return FALSE;

    hr = g_enum->lpVtbl->GetDefaultAudioEndpoint(g_enum, eRender, eConsole, &g_device);
    if (FAILED(hr)) return FALSE;

    hr = g_device->lpVtbl->Activate(g_device, &IID_IAudioSessionManager2,
                                    CLSCTX_INPROC_SERVER, NULL, (void **)&g_mgr);
    if (FAILED(hr)) return FALSE;

    return TRUE;
}

BOOL audio_meter_init(void)
{
    HRESULT hr_co;
    BOOL ok = TRUE;

    am_init_cs_once();
    EnterCriticalSection(&g_init_cs);

    if (g_init_refcount == 0) {
        hr_co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (hr_co == S_OK || hr_co == S_FALSE) {
            g_co_inited = TRUE;
        } else if (hr_co == RPC_E_CHANGED_MODE) {
            g_co_inited = FALSE;
        } else {
            ok = FALSE;
        }
        if (ok && !am_endpoint_bind_locked()) {
            /* Endpoint bind failed; leave CoInit alone, fall back to
             * a no-op meter (audio_meter_poll will keep returning FALSE). */
        }
    }
    if (ok) g_init_refcount++;
    LeaveCriticalSection(&g_init_cs);
    return ok;
}

void audio_meter_shutdown(void)
{
    if (!g_init_cs_ready) return;
    EnterCriticalSection(&g_init_cs);
    if (g_init_refcount > 0) {
        g_init_refcount--;
        if (g_init_refcount == 0) {
            if (g_mgr)    { g_mgr->lpVtbl->Release(g_mgr);     g_mgr = NULL; }
            if (g_device) { g_device->lpVtbl->Release(g_device); g_device = NULL; }
            if (g_enum)   { g_enum->lpVtbl->Release(g_enum);   g_enum = NULL; }
            if (g_co_inited) { CoUninitialize(); g_co_inited = FALSE; }
        }
    }
    LeaveCriticalSection(&g_init_cs);
}

/* ------------------------------------------------------------------ */
/* AudioMeter handle.                                                  */
/* ------------------------------------------------------------------ */

/* How often we re-run the session enumeration to re-pick the loudest
 * own-process session. The enumeration (IAudioSessionManager2::
 * GetSessionEnumerator) is a cross-process call to the audio service and
 * measures ~15 ms; reading peaks from the already-bound meter is ~0.1 ms,
 * so we enumerate rarely and read at the caller's full poll rate (~30 Hz).
 *
 * The regression itself is fixed by the FIRST poll's resolve: because
 * audio_meter_create is called fresh each time playback begins, by the
 * first poll the active source's session is the loudest and a stale prior
 * session (e.g. an expired MF Live TV session) can never win. The periodic
 * re-resolve below is only insurance for a hypothetical source change
 * within a single meter lifetime. 3 s keeps the amortized enumeration cost
 * well under 1% of one core during playback (and zero at idle, since the
 * UI only polls while PLAYING). */
#define AM_RESOLVE_INTERVAL_MS 3000

struct AudioMeter {
    /* Fix Bx (2026-06-16): re-resolve the loudest own-process session
     * periodically (not every poll) and cache it; read peaks from the
     * cache at full rate. session_meter is the currently bound session;
     * device_meter is the endpoint-level fallback, used ONLY when this
     * driver rejects the per-session IAudioMeterInformation QI. */
    IAudioMeterInformation *session_meter;
    ULONGLONG last_resolve_tick;
    IAudioMeterInformation *device_meter;
    BOOL  use_device_fallback;
    int   sessions_seen_no_meter;   /* consecutive resolves: own sessions but no QI */
};

AudioMeter *audio_meter_create(void)
{
    AudioMeter *m = (AudioMeter *)HeapAlloc(GetProcessHeap(),
                                            HEAP_ZERO_MEMORY,
                                            sizeof(AudioMeter));
    if (!m) return NULL;
    return m;
}

void audio_meter_destroy(AudioMeter *m)
{
    if (!m) return;
    if (m->session_meter) { m->session_meter->lpVtbl->Release(m->session_meter);
                            m->session_meter = NULL; }
    if (m->device_meter)  { m->device_meter->lpVtbl->Release(m->device_meter);
                            m->device_meter = NULL; }
    HeapFree(GetProcessHeap(), 0, m);
}

/* Read stereo (or mono-doubled) peaks from a bound meter. Cheap: no
 * enumeration. Returns FALSE only if the meter calls outright fail. */
static BOOL am_read_meter(IAudioMeterInformation *meter,
                          float *peak_l, float *peak_r)
{
    UINT  ch = 0;
    float channels[8] = {0};
    float pv = 0.0f;
    if (!meter) return FALSE;
    if (FAILED(meter->lpVtbl->GetMeteringChannelCount(meter, &ch))) ch = 0;
    if (ch >= 2 && ch <= 8 &&
        SUCCEEDED(meter->lpVtbl->GetChannelsPeakValues(meter, ch, channels))) {
        if (peak_l) *peak_l = channels[0];
        if (peak_r) *peak_r = channels[1];
        return TRUE;
    }
    if (FAILED(meter->lpVtbl->GetPeakValue(meter, &pv))) return FALSE;
    if (peak_l) *peak_l = pv;
    if (peak_r) *peak_r = pv;
    return TRUE;
}

/* Fix Bx: walk EVERY audio session in our process, pick the one with the
 * highest current peak, and return its meter (AddRef'd; caller releases).
 * Robust to stale / expired sessions a previous audio source left behind
 * (e.g. the MF Live TV session that lingers in the enumerator after Stop):
 * the silent session reports ~0 and loses to the active one, so we never
 * latch onto a dead session the way the old cache-once bind did. This runs
 * the cross-process enumeration, so it is called at most ~once a second,
 * not per poll. Caller must hold g_init_cs. *own_seen receives the number
 * of own-process sessions found (>0 with NULL return => sessions exist but
 * none expose IAudioMeterInformation). */
static IAudioMeterInformation *am_resolve_best_own_session(int *own_seen)
{
    IAudioSessionEnumerator *senum = NULL;
    IAudioMeterInformation  *best  = NULL;
    float best_peak = -1.0f;
    int   count = 0, i, seen = 0;
    DWORD self_pid;

    if (own_seen) *own_seen = 0;
    if (!g_mgr) return NULL;
    self_pid = GetCurrentProcessId();

    if (FAILED(g_mgr->lpVtbl->GetSessionEnumerator(g_mgr, &senum)) || !senum)
        return NULL;
    if (FAILED(senum->lpVtbl->GetCount(senum, &count))) count = 0;

    for (i = 0; i < count; i++) {
        IAudioSessionControl   *ctrl  = NULL;
        IAudioSessionControl2  *ctrl2 = NULL;
        IAudioMeterInformation *meter = NULL;
        DWORD pid = 0;
        if (FAILED(senum->lpVtbl->GetSession(senum, i, &ctrl)) || !ctrl) continue;
        if (SUCCEEDED(ctrl->lpVtbl->QueryInterface(ctrl, &IID_IAudioSessionControl2,
                                                   (void **)&ctrl2)) && ctrl2) {
            if (SUCCEEDED(ctrl2->lpVtbl->GetProcessId(ctrl2, &pid)) &&
                pid == self_pid) {
                seen++;
                if (SUCCEEDED(ctrl->lpVtbl->QueryInterface(ctrl,
                        &IID_IAudioMeterInformation, (void **)&meter)) && meter) {
                    float pk = 0.0f;
                    if (FAILED(meter->lpVtbl->GetPeakValue(meter, &pk))) pk = 0.0f;
                    if (pk > best_peak) {
                        if (best) best->lpVtbl->Release(best);
                        best = meter;          /* keep the reference */
                        best_peak = pk;
                    } else {
                        meter->lpVtbl->Release(meter);
                    }
                }
            }
            ctrl2->lpVtbl->Release(ctrl2);
        }
        ctrl->lpVtbl->Release(ctrl);
    }
    senum->lpVtbl->Release(senum);
    if (own_seen) *own_seen = seen;
    return best;
}

/* Device-level fallback: a single IAudioMeterInformation on the whole
 * endpoint. Reports peaks for everything mixed into the endpoint, not
 * just our session. Used only when the per-session QI is rejected on
 * a particular driver, per dispatch 3 / locked decision fallback. */
static IAudioMeterInformation *am_activate_device_meter(void)
{
    IAudioMeterInformation *meter = NULL;
    HRESULT hr;
    if (!g_device) return NULL;
    hr = g_device->lpVtbl->Activate(g_device, &IID_IAudioMeterInformation,
                                    CLSCTX_INPROC_SERVER, NULL, (void **)&meter);
    if (FAILED(hr) || !meter) return NULL;
    return meter;
}

BOOL audio_meter_poll(AudioMeter *m, float *peak_l, float *peak_r)
{
    ULONGLONG now;

    if (!m) return FALSE;

    /* Device-level fallback path: only reached if this driver rejects the
     * per-session IAudioMeterInformation QI (rare; deadlock C). Reports the
     * whole endpoint's mixed peak; the endpoint meter is stable so cached. */
    if (m->use_device_fallback) {
        if (!m->device_meter) {
            EnterCriticalSection(&g_init_cs);
            m->device_meter = am_activate_device_meter();
            LeaveCriticalSection(&g_init_cs);
        }
        return am_read_meter(m->device_meter, peak_l, peak_r);
    }

    /* Periodically re-resolve which own-process session to read (the
     * enumeration is the only expensive call), then read peaks from the
     * cached meter at the caller's full poll rate. The first poll always
     * resolves (cache empty), which is what fixes the regression: by the
     * time the meter is created, the active source's session is loudest,
     * so a stale prior session never wins. */
    now = GetTickCount64();
    if (!m->session_meter ||
        (now - m->last_resolve_tick) >= AM_RESOLVE_INTERVAL_MS) {
        int own_seen = 0;
        IAudioMeterInformation *fresh;
        EnterCriticalSection(&g_init_cs);
        fresh = am_resolve_best_own_session(&own_seen);
        LeaveCriticalSection(&g_init_cs);
        m->last_resolve_tick = now;
        if (fresh) {
            if (m->session_meter) m->session_meter->lpVtbl->Release(m->session_meter);
            m->session_meter = fresh;
            m->sessions_seen_no_meter = 0;
        } else if (own_seen > 0) {
            /* Own sessions exist but none expose a per-session meter:
             * this driver rejects the per-session QI. Switch to the
             * endpoint fallback after a few confirming resolves. */
            if (++m->sessions_seen_no_meter >= 3) m->use_device_fallback = TRUE;
        }
        /* own_seen == 0: no own session registered yet; keep any prior
         * cached meter (if none, the read below returns FALSE). */
    }

    return am_read_meter(m->session_meter, peak_l, peak_r);
}
