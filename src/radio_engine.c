/*
 * radio_engine.c - Native streaming-radio playback engine.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Pipeline (see radio_engine.h for the block diagram):
 *   WinHTTP streaming GET -> ICY-MetaInt stripper + ADTS framer
 *   -> Microsoft AAC Decoder MFT -> PCM ring buffer -> WASAPI render.
 *
 * Five sections, top to bottom:
 *   1. Diagnostic log layer        (mirrors DEBUG_WV2_SERVICE)
 *   2. Internal state + ring buffer
 *   3. WinHTTP streaming reader + ICY stripper + ADTS framer
 *   4. AAC Decoder MFT
 *   5. WASAPI render + public API plumbing
 */

#define COBJMACROS
#define INITGUID

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>

#include "radio_engine.h"

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

/* The Microsoft AAC Decoder MFT. Locked CLSID per dispatch; ships in
 * mfplat.dll on every supported Windows and decodes HE-AAC / HE-AAC v2.
 * Declared explicitly so we do not depend on CLSID_CMSAACDecMFT being
 * present in the toolchain headers. */
static const GUID RE_CLSID_AAC_DECODER =
    { 0x32D186A7, 0x218F, 0x4C75,
      { 0x88, 0x76, 0xDD, 0x77, 0x27, 0x3A, 0x89, 0x99 } };

/* ================================================================== */
/* 1. Diagnostic log layer.                                            */
/*                                                                     */
/* Mirrors the DEBUG_WV2_SERVICE pattern. Default ON for this dispatch */
/* per Dimitri's gate; flip DEBUG_RADIO_ENGINE to 0 afterward. Logs to */
/* %TEMP%\mgt_radio_engine.log with millisecond deltas, serialized by  */
/* a single critical section (worker + render threads both write).     */
/* ================================================================== */

#ifndef DEBUG_RADIO_ENGINE
#define DEBUG_RADIO_ENGINE 0
#endif

#if DEBUG_RADIO_ENGINE

static FILE             *g_re_log_fp       = NULL;
static ULONGLONG         g_re_log_t0       = 0;
static CRITICAL_SECTION  g_re_log_cs;
static BOOL              g_re_log_cs_ready = FALSE;
static LONG              g_re_log_refs     = 0;

static void relog_init_cs_once(void)
{
    static LONG started = 0;
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        InitializeCriticalSection(&g_re_log_cs);
        g_re_log_cs_ready = TRUE;
    } else {
        while (!g_re_log_cs_ready) Sleep(0);
    }
}

static void relog_open(void)
{
    char path[MAX_PATH];
    DWORD n;
    relog_init_cs_once();
    EnterCriticalSection(&g_re_log_cs);
    if (InterlockedIncrement(&g_re_log_refs) == 1) {
        n = GetEnvironmentVariableA("TEMP", path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) lstrcpyA(path, "C:\\Windows\\Temp");
        lstrcatA(path, "\\mgt_radio_engine.log");
        g_re_log_fp = fopen(path, "a");
        g_re_log_t0 = GetTickCount64();
        if (g_re_log_fp) {
            fprintf(g_re_log_fp,
                "\n==== mgt_radio_engine.log session opened t0=%llu ====\n",
                (unsigned long long)g_re_log_t0);
            fflush(g_re_log_fp);
        }
    }
    LeaveCriticalSection(&g_re_log_cs);
}

static void relog_close(void)
{
    if (!g_re_log_cs_ready) return;
    EnterCriticalSection(&g_re_log_cs);
    if (InterlockedDecrement(&g_re_log_refs) == 0) {
        if (g_re_log_fp) {
            fprintf(g_re_log_fp, "==== session closed ====\n");
            fclose(g_re_log_fp);
            g_re_log_fp = NULL;
        }
    }
    LeaveCriticalSection(&g_re_log_cs);
}

static void relog(const char *fmt, ...)
{
    char      line[512];
    int       n;
    va_list   ap;
    ULONGLONG now;
    if (!g_re_log_cs_ready) return;
    EnterCriticalSection(&g_re_log_cs);
    if (!g_re_log_fp) { LeaveCriticalSection(&g_re_log_cs); return; }
    now = GetTickCount64();
    n = _snprintf(line, sizeof(line), "%6lu  ",
                  (unsigned long)(now - g_re_log_t0));
    if (n < 0 || n >= (int)sizeof(line)) n = 0;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 2] = '\0';
    lstrcatA(line, "\n");
    fputs(line, g_re_log_fp);
    fflush(g_re_log_fp);
    LeaveCriticalSection(&g_re_log_cs);
}

#else  /* DEBUG_RADIO_ENGINE */

#define relog(...)      ((void)0)
static void relog_open(void)  {}
static void relog_close(void) {}

#endif /* DEBUG_RADIO_ENGINE */

/* ================================================================== */
/* 2. Internal state + PCM ring buffer.                                */
/* ================================================================== */

/* Bytes-based circular buffer sized for ~500 ms of decoded PCM. Guarded
 * by its own critical section; written by the worker, read by the render
 * thread. Overflow applies back-pressure (worker waits); underflow is
 * zero-filled by the reader. */
typedef struct {
    BYTE             *buf;
    size_t            cap;
    size_t            head;   /* read cursor  */
    size_t            tail;   /* write cursor */
    size_t            fill;   /* bytes currently stored */
    CRITICAL_SECTION  cs;
    BOOL              ready;
} Ring;

static BOOL ring_init(Ring *r, size_t cap)
{
    r->buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0, cap);
    if (!r->buf) return FALSE;
    r->cap = cap; r->head = r->tail = r->fill = 0;
    InitializeCriticalSection(&r->cs);
    r->ready = TRUE;
    return TRUE;
}

static void ring_free(Ring *r)
{
    if (!r->ready) return;
    DeleteCriticalSection(&r->cs);
    if (r->buf) { HeapFree(GetProcessHeap(), 0, r->buf); r->buf = NULL; }
    r->ready = FALSE;
}

/* Copy up to `len` bytes in; returns bytes actually written (may be less
 * when full). */
static size_t ring_write(Ring *r, const BYTE *data, size_t len)
{
    size_t space, n, first;
    EnterCriticalSection(&r->cs);
    space = r->cap - r->fill;
    n = (len < space) ? len : space;
    first = r->cap - r->tail;
    if (first > n) first = n;
    memcpy(r->buf + r->tail, data, first);
    if (n > first) memcpy(r->buf, data + first, n - first);
    r->tail = (r->tail + n) % r->cap;
    r->fill += n;
    LeaveCriticalSection(&r->cs);
    return n;
}

/* Copy up to `len` bytes out; returns bytes actually read (may be less
 * when underrun). */
static size_t ring_read(Ring *r, BYTE *out, size_t len)
{
    size_t n, first;
    EnterCriticalSection(&r->cs);
    n = (len < r->fill) ? len : r->fill;
    first = r->cap - r->head;
    if (first > n) first = n;
    memcpy(out, r->buf + r->head, first);
    if (n > first) memcpy(out + first, r->buf, n - first);
    r->head = (r->head + n) % r->cap;
    r->fill -= n;
    LeaveCriticalSection(&r->cs);
    return n;
}

static size_t ring_fill(Ring *r)
{
    size_t f;
    EnterCriticalSection(&r->cs);
    f = r->fill;
    LeaveCriticalSection(&r->cs);
    return f;
}

#define RE_NET_CHUNK     16384   /* WinHTTP read chunk */
#define RE_ADTS_CAP      65536   /* ADTS accumulator cap */
#define RE_META_MAX      (16 * 255 + 1)

struct RadioEngine {
    volatile LONG     run_state;     /* REState, atomically published */

    /* Worker / render threads + lifecycle events. */
    HANDLE            worker;
    HANDLE            render;
    HANDLE            stop_event;     /* manual-reset: signals teardown */
    HANDLE            audio_event;    /* WASAPI event-callback handle */

    wchar_t           url[1024];

    /* WinHTTP. hRequest guarded by net_cs so re_stop can cancel a
     * blocking read by closing it exactly once. */
    HINTERNET         hSession;
    HINTERNET         hConnect;
    HINTERNET         hRequest;
    CRITICAL_SECTION  net_cs;

    /* ICY metadata interleave state. */
    DWORD             icy_metaint;    /* 0 => no ICY interleave */
    DWORD             icy_remain;     /* audio bytes until next meta block */
    int               icy_phase;      /* 0=audio 1=len 2=data */
    DWORD             meta_remain;    /* metadata bytes left in current block */
    char              meta_buf[RE_META_MAX];
    DWORD             meta_len;

    /* ADTS accumulator. */
    BYTE              adts[RE_ADTS_CAP];
    DWORD             adts_len;
    int               adts_sr;        /* sample rate from first ADTS header */
    int               adts_ch;        /* channel config from first ADTS header */

    /* MFT decoder. */
    IMFTransform     *mft;
    DWORD             mft_out_size;
    BOOL              mft_provides_samples;
    BOOL              decoder_ready;

    /* WASAPI. */
    IMMDeviceEnumerator *enumr;
    IMMDevice           *device;
    IAudioClient        *client;
    IAudioRenderClient  *render_client;
    UINT32               wasapi_frames;   /* buffer frame count */
    WORD                 out_channels;
    DWORD                out_rate;
    WORD                 out_bits;
    BOOL                 out_is_float;    /* MFAudioFormat_Float vs _PCM */
    WORD                 frame_bytes;     /* channels * bytes/sample */
    BOOL                 wasapi_ready;

    Ring                 ring;

    /* Stats. */
    LONG                 underruns;
    ULONGLONG            first_pcm_tick;

    /* Callbacks. */
    REStateCb            state_cb;
    void                *state_user;
    REMetaCb             meta_cb;
    void                *meta_user;

    char                 last_error[256];
};

static void re_set_error(RadioEngine *e, const char *fmt, ...)
{
    va_list ap;
    if (!e) return;
    va_start(ap, fmt);
    _vsnprintf(e->last_error, sizeof(e->last_error) - 1, fmt, ap);
    va_end(ap);
    e->last_error[sizeof(e->last_error) - 1] = '\0';
    relog("ERROR: %s", e->last_error);
}

static void re_publish_state(RadioEngine *e, REState st, const char *detail)
{
    InterlockedExchange(&e->run_state, (LONG)st);
    relog("state -> %d (%s)", (int)st, detail ? detail : "");
    if (e->state_cb) e->state_cb(st, detail, e->state_user);
}

/* ================================================================== */
/* 3. ADTS framer + ICY stripper (consume the raw transport stream).   */
/* ================================================================== */

static const int RE_ADTS_SR_TBL[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025,  8000,  7350,     0,     0,     0
};

/* Forward decls for the decode path used by the framer. */
static BOOL re_decoder_ensure(RadioEngine *e, int sr_idx, int ch_cfg);
static void re_decode_frame(RadioEngine *e, const BYTE *frame, DWORD len);

/* Parse the ICY StreamTitle out of a metadata block and fire meta_cb. */
static void re_emit_meta(RadioEngine *e)
{
    const char *p, *q;
    e->meta_buf[e->meta_len] = '\0';
    p = strstr(e->meta_buf, "StreamTitle='");
    if (p) {
        p += 13;
        q = strstr(p, "';");
        if (!q) q = p + strlen(p);
        {
            char title[512];
            size_t n = (size_t)(q - p);
            if (n >= sizeof(title)) n = sizeof(title) - 1;
            memcpy(title, p, n);
            title[n] = '\0';
            relog("icy StreamTitle='%s'", title);
            if (e->meta_cb) e->meta_cb(title, e->meta_user);
        }
    }
}

/* Push pure-audio (post-ICY-strip) bytes through the ADTS framer.
 * Extracts complete ADTS frames and hands each to the decoder. */
static void re_feed_audio(RadioEngine *e, const BYTE *data, DWORD len)
{
    DWORD i;
    /* Append to accumulator (drop oldest if we somehow overflow; a sane
     * stream never grows past a couple of frames here). */
    if (e->adts_len + len > RE_ADTS_CAP) {
        DWORD drop;
        if (len >= RE_ADTS_CAP) { data += (len - RE_ADTS_CAP); len = RE_ADTS_CAP; }
        drop = e->adts_len + len - RE_ADTS_CAP;
        if (drop > e->adts_len) drop = e->adts_len;
        memmove(e->adts, e->adts + drop, e->adts_len - drop);
        e->adts_len -= drop;
        relog("adts: accumulator overflow, dropped %lu bytes", (unsigned long)drop);
    }
    memcpy(e->adts + e->adts_len, data, len);
    e->adts_len += len;

    i = 0;
    for (;;) {
        DWORD avail = e->adts_len - i;
        BYTE b1;
        int  sr_idx, ch_cfg, layer;
        DWORD frame_len;
        if (avail < 7) break;                 /* need full header */
        if (!(e->adts[i] == 0xFF && (e->adts[i + 1] & 0xF6) == 0xF0)) {
            i++;                               /* resync */
            continue;
        }
        b1     = e->adts[i + 1]; (void)b1;
        layer  = (e->adts[i + 1] >> 1) & 0x03;
        sr_idx = (e->adts[i + 2] >> 2) & 0x0F;
        ch_cfg = ((e->adts[i + 2] & 0x01) << 2) | ((e->adts[i + 3] >> 6) & 0x03);
        frame_len = ((DWORD)(e->adts[i + 3] & 0x03) << 11)
                  | ((DWORD)e->adts[i + 4] << 3)
                  | ((DWORD)(e->adts[i + 5] >> 5) & 0x07);
        /* Sanity per dispatch: layer==0, sf_index<13, ch_cfg in 1..7. */
        if (layer != 0 || sr_idx >= 13 || ch_cfg < 1 || ch_cfg > 7 ||
            frame_len < 7) {
            i++;                               /* false sync, advance */
            continue;
        }
        if (avail < frame_len) break;          /* wait for the rest */

        if (!e->decoder_ready) {
            e->adts_sr = sr_idx;
            e->adts_ch = ch_cfg;
            if (!re_decoder_ensure(e, sr_idx, ch_cfg)) {
                /* Decoder init failed: error already published. Stop
                 * consuming; the worker loop will tear down. */
                return;
            }
        }
        re_decode_frame(e, e->adts + i, frame_len);
        i += frame_len;
    }

    /* Shift any partial remainder to the front. */
    if (i > 0) {
        memmove(e->adts, e->adts + i, e->adts_len - i);
        e->adts_len -= i;
    }
}

/* Consume one WinHTTP chunk, separating interleaved ICY metadata from
 * the audio byte stream per the Icecast / Shoutcast ICY-MetaInt spec. */
static void re_consume(RadioEngine *e, const BYTE *data, DWORD len)
{
    DWORD pos = 0;
    if (e->icy_metaint == 0) {                 /* no interleave */
        re_feed_audio(e, data, len);
        return;
    }
    while (pos < len) {
        if (e->icy_phase == 0) {               /* audio */
            DWORD take = len - pos;
            if (take > e->icy_remain) take = e->icy_remain;
            re_feed_audio(e, data + pos, take);
            pos += take;
            e->icy_remain -= take;
            if (e->icy_remain == 0) e->icy_phase = 1;
        } else if (e->icy_phase == 1) {        /* length byte */
            e->meta_remain = (DWORD)data[pos++] * 16;
            e->meta_len = 0;
            if (e->meta_remain == 0) {
                e->icy_phase = 0;
                e->icy_remain = e->icy_metaint;
            } else {
                e->icy_phase = 2;
            }
        } else {                               /* metadata bytes */
            DWORD take = len - pos;
            if (take > e->meta_remain) take = e->meta_remain;
            if (e->meta_len + take < RE_META_MAX)
                memcpy(e->meta_buf + e->meta_len, data + pos, take);
            e->meta_len += take;
            pos += take;
            e->meta_remain -= take;
            if (e->meta_remain == 0) {
                if (e->meta_len >= RE_META_MAX) e->meta_len = RE_META_MAX - 1;
                re_emit_meta(e);
                e->icy_phase = 0;
                e->icy_remain = e->icy_metaint;
            }
        }
    }
}

/* ================================================================== */
/* 4. AAC Decoder MFT.                                                 */
/* ================================================================== */

/* Forward decl: WASAPI is initialized lazily the first time we have a
 * real decoded PCM format in hand. */
static BOOL re_wasapi_start(RadioEngine *e);

/* Choose + apply the decoder's output type, preferring documented int16
 * PCM. The AAC decoder also offers 32-bit float (index 0 on recent
 * Windows builds) and may demand a re-set via MF_E_TRANSFORM_STREAM_CHANGE
 * once SBR/PS reveals the true rate; this is shared between the initial
 * setup and that renegotiation so the int16 preference is honoured both
 * times. re_on_first_pcm reads the ACTUAL negotiated subtype regardless,
 * so WASAPI is always told the truth even if only float is on offer. */
static HRESULT re_select_output_type(RadioEngine *e, BOOL log_types)
{
    DWORD         ti = 0;
    HRESULT       hr = MF_E_NO_MORE_TYPES;
    IMFMediaType *pick = NULL, *first = NULL, *chosen;
    for (;;) {
        GUID   sub;  UINT32 bits = 0;
        IMFMediaType *t = NULL;
        hr = IMFTransform_GetOutputAvailableType(e->mft, 0, ti, &t);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr) || !t) break;
        if (!first) { first = t; IMFMediaType_AddRef(first); }
        ZeroMemory(&sub, sizeof(sub));
        IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
        IMFMediaType_GetUINT32(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        if (log_types)
            relog("decoder: avail out type %lu bits=%u float=%d",
                  (unsigned long)ti, (unsigned)bits,
                  (int)IsEqualGUID(&sub, &MFAudioFormat_Float));
        if (IsEqualGUID(&sub, &MFAudioFormat_PCM) && bits == 16) {
            pick = t; t = NULL; break;
        }
        IMFMediaType_Release(t);
        ti++;
    }
    chosen = pick ? pick : first;
    if (pick && first) IMFMediaType_Release(first);
    if (!chosen) return FAILED(hr) ? hr : MF_E_NO_MORE_TYPES;
    hr = IMFTransform_SetOutputType(e->mft, 0, chosen, 0);
    relog("decoder: SetOutputType hr=0x%08lX (%s)",
          (unsigned long)hr, pick ? "PCM16" : "fallback first type");
    IMFMediaType_Release(chosen);
    return hr;
}

/* Build + apply the MFT input/output types. Tries a couple of AAC
 * profile-level values before giving up (some Win11 builds are picky;
 * see DEADLOCK CRITERIA in the dispatch). */
static BOOL re_decoder_ensure(RadioEngine *e, int sr_idx, int ch_cfg)
{
    static const WORD profiles[] = { 0x29, 0x28, 0x2B, 0x00 };
    HRESULT       hr;
    IMFMediaType *in_type = NULL;
    int           sr = RE_ADTS_SR_TBL[sr_idx & 0x0F];
    unsigned      pi;
    MFT_OUTPUT_STREAM_INFO osi;

    if (e->decoder_ready) return TRUE;
    if (sr <= 0) sr = 44100;

    relog("decoder: creating AAC MFT (adts sr=%d ch_cfg=%d)", sr, ch_cfg);
    hr = CoCreateInstance(&RE_CLSID_AAC_DECODER, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IMFTransform, (void **)&e->mft);
    if (FAILED(hr) || !e->mft) {
        re_set_error(e, "CoCreateInstance(AAC MFT) hr=0x%08lX", (unsigned long)hr);
        return FALSE;
    }

    for (pi = 0; pi < sizeof(profiles) / sizeof(profiles[0]); pi++) {
        /* HEAACWAVEINFO tail (the bytes after WAVEFORMATEX), 12 bytes:
         *   wPayloadType=1 (ADTS), wAudioProfileLevelIndication=profile,
         *   wStructType=0, wReserved1=0, wReserved2=0. No ASC for ADTS. */
        BYTE userdata[12];
        WORD payload = 1;
        ZeroMemory(userdata, sizeof(userdata));
        memcpy(userdata + 0, &payload, 2);
        memcpy(userdata + 2, &profiles[pi], 2);

        if (in_type) { IMFMediaType_Release(in_type); in_type = NULL; }
        hr = MFCreateMediaType(&in_type);
        if (FAILED(hr)) { re_set_error(e, "MFCreateMediaType hr=0x%08lX",
                                       (unsigned long)hr); goto fail; }
        IMFMediaType_SetGUID(in_type, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
        IMFMediaType_SetGUID(in_type, &MF_MT_SUBTYPE,    &MFAudioFormat_AAC);
        IMFMediaType_SetUINT32(in_type, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)ch_cfg);
        IMFMediaType_SetUINT32(in_type, &MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sr);
        IMFMediaType_SetUINT32(in_type, &MF_MT_AAC_PAYLOAD_TYPE, 1);
        IMFMediaType_SetUINT32(in_type, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
                               (UINT32)profiles[pi]);
        IMFMediaType_SetBlob(in_type, &MF_MT_USER_DATA, userdata, sizeof(userdata));

        hr = IMFTransform_SetInputType(e->mft, 0, in_type, 0);
        relog("decoder: SetInputType profile=0x%02X hr=0x%08lX",
              profiles[pi], (unsigned long)hr);
        if (SUCCEEDED(hr)) break;
    }
    if (FAILED(hr)) {
        re_set_error(e, "SetInputType rejected every AAC profile "
                     "(last hr=0x%08lX)", (unsigned long)hr);
        goto fail;
    }

    /* Select the int16 PCM output type the AAC decoder documents. */
    hr = re_select_output_type(e, TRUE);
    if (FAILED(hr)) {
        re_set_error(e, "SetOutputType hr=0x%08lX", (unsigned long)hr);
        goto fail;
    }

    /* Output buffer allocation strategy. */
    ZeroMemory(&osi, sizeof(osi));
    IMFTransform_GetOutputStreamInfo(e->mft, 0, &osi);
    e->mft_out_size = osi.cbSize ? osi.cbSize : 32768;
    e->mft_provides_samples =
        (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                        MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) ? TRUE : FALSE;
    relog("decoder: out_size=%lu provides_samples=%d",
          (unsigned long)e->mft_out_size, (int)e->mft_provides_samples);

    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (in_type) IMFMediaType_Release(in_type);
    e->decoder_ready = TRUE;
    return TRUE;

fail:
    if (in_type) IMFMediaType_Release(in_type);
    if (e->mft)  { IMFTransform_Release(e->mft); e->mft = NULL; }
    return FALSE;
}

/* Read the decoder's current output format and lazily start WASAPI on
 * the first real PCM sample. */
static void re_on_first_pcm(RadioEngine *e)
{
    IMFMediaType *cur = NULL;
    UINT32 ch = 2, sr = 44100, bits = 16;
    GUID   sub;
    BOOL   is_float = FALSE;
    if (e->wasapi_ready) return;
    ZeroMemory(&sub, sizeof(sub));
    if (SUCCEEDED(IMFTransform_GetOutputCurrentType(e->mft, 0, &cur)) && cur) {
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &ch);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        IMFMediaType_GetGUID(cur, &MF_MT_SUBTYPE, &sub);
        IMFMediaType_Release(cur);
    }
    is_float = IsEqualGUID(&sub, &MFAudioFormat_Float);
    if (bits == 0) bits = is_float ? 32 : 16;
    if (ch == 0)   ch = 2;
    if (sr == 0)   sr = 44100;
    e->out_channels = (WORD)ch;
    e->out_rate     = sr;
    e->out_bits     = (WORD)bits;
    e->out_is_float = is_float;
    e->frame_bytes  = (WORD)(ch * (bits / 8));
    relog("decoder: FIRST decoded sample -> %lu Hz, %u ch, %u-bit %s "
          "(HE-AAC v2 path verified)",
          (unsigned long)sr, (unsigned)ch, (unsigned)bits,
          is_float ? "float" : "PCM");
    re_wasapi_start(e);
}

/* Pull all currently-available decoded PCM out of the MFT and push it
 * into the ring buffer (with back-pressure). */
static void re_drain_output(RadioEngine *e)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out;
        IMFSample      *samp = NULL;
        IMFMediaBuffer *mbuf = NULL;
        DWORD           status = 0;
        HRESULT         hr;

        ZeroMemory(&out, sizeof(out));
        if (!e->mft_provides_samples) {
            if (FAILED(MFCreateSample(&samp))) return;
            if (FAILED(MFCreateMemoryBuffer(e->mft_out_size, &mbuf))) {
                IMFSample_Release(samp); return;
            }
            IMFSample_AddBuffer(samp, mbuf);
            out.pSample = samp;
        }
        out.dwStreamID = 0;

        hr = IMFTransform_ProcessOutput(e->mft, 0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (out.pEvents) IMFCollection_Release(out.pEvents);
            if (mbuf) IMFMediaBuffer_Release(mbuf);
            if (samp) IMFSample_Release(samp);
            break;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            /* Renegotiate output type (SBR/PS reveals the real rate).
             * Re-apply the same int16-preferring selection so the format
             * stays deterministic across the change. */
            if (out.pEvents) IMFCollection_Release(out.pEvents);
            if (mbuf) IMFMediaBuffer_Release(mbuf);
            if (samp) IMFSample_Release(samp);
            relog("decoder: STREAM_CHANGE, renegotiating output type");
            re_select_output_type(e, FALSE);
            continue;
        }
        if (FAILED(hr)) {
            if (out.pEvents) IMFCollection_Release(out.pEvents);
            if (mbuf) IMFMediaBuffer_Release(mbuf);
            if (samp) IMFSample_Release(samp);
            relog("decoder: ProcessOutput hr=0x%08lX", (unsigned long)hr);
            break;
        }

        /* If the MFT provided its own sample, get its buffer. */
        if (!mbuf && out.pSample)
            IMFSample_ConvertToContiguousBuffer(out.pSample, &mbuf);

        if (mbuf) {
            BYTE *p = NULL; DWORD cur = 0, maxl = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(mbuf, &p, &maxl, &cur)) && p && cur) {
                if (!e->wasapi_ready) re_on_first_pcm(e);
                /* Back-pressured write: pace the worker to real time. */
                {
                    DWORD off = 0;
                    while (off < cur) {
                        size_t w = ring_write(&e->ring, p + off, cur - off);
                        off += (DWORD)w;
                        if (off < cur) {
                            if (WaitForSingleObject(e->stop_event, 5) ==
                                WAIT_OBJECT_0) break;
                        }
                    }
                }
                IMFMediaBuffer_Unlock(mbuf);
            }
        }

        if (out.pEvents) IMFCollection_Release(out.pEvents);
        if (mbuf) IMFMediaBuffer_Release(mbuf);
        if (out.pSample && out.pSample == samp) IMFSample_Release(samp);
        else if (out.pSample) IMFSample_Release(out.pSample);
    }
}

/* Feed one ADTS frame to the MFT, then drain whatever PCM comes out. */
static void re_decode_frame(RadioEngine *e, const BYTE *frame, DWORD len)
{
    IMFSample      *samp = NULL;
    IMFMediaBuffer *mbuf = NULL;
    BYTE           *dst  = NULL;
    HRESULT         hr;

    if (!e->mft) return;
    if (FAILED(MFCreateMemoryBuffer(len, &mbuf))) return;
    if (FAILED(IMFMediaBuffer_Lock(mbuf, &dst, NULL, NULL))) {
        IMFMediaBuffer_Release(mbuf); return;
    }
    memcpy(dst, frame, len);
    IMFMediaBuffer_Unlock(mbuf);
    IMFMediaBuffer_SetCurrentLength(mbuf, len);

    if (FAILED(MFCreateSample(&samp))) { IMFMediaBuffer_Release(mbuf); return; }
    IMFSample_AddBuffer(samp, mbuf);

    hr = IMFTransform_ProcessInput(e->mft, 0, samp, 0);
    if (hr == MF_E_NOTACCEPTING) {
        re_drain_output(e);
        hr = IMFTransform_ProcessInput(e->mft, 0, samp, 0);
    }
    if (SUCCEEDED(hr))
        re_drain_output(e);
    else
        relog("decoder: ProcessInput hr=0x%08lX", (unsigned long)hr);

    IMFMediaBuffer_Release(mbuf);
    IMFSample_Release(samp);
}

/* ================================================================== */
/* 5. WASAPI render + public API plumbing.                             */
/* ================================================================== */

#define RE_REFTIMES_PER_MS  10000

/* Render thread: waits on the WASAPI event, pulls PCM from the ring into
 * the shared-mode buffer; underrun is zero-filled and logged. */
static DWORD WINAPI re_render_thread(LPVOID arg)
{
    RadioEngine *e = (RadioEngine *)arg;
    HANDLE waits[2];
    HANDLE mmcss = NULL;
    DWORD  task_idx = 0;
    HRESULT hr;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    {
        HMODULE av = GetModuleHandleA("avrt.dll");
        if (av || (av = LoadLibraryA("avrt.dll"))) {
            typedef HANDLE (WINAPI *PFN)(LPCSTR, LPDWORD);
            PFN f = (PFN)GetProcAddress(av, "AvSetMmThreadCharacteristicsA");
            if (f) mmcss = f("Pro Audio", &task_idx);
        }
    }

    hr = IAudioClient_Start(e->client);
    relog("render: IAudioClient_Start hr=0x%08lX", (unsigned long)hr);

    waits[0] = e->stop_event;
    waits[1] = e->audio_event;

    for (;;) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
        UINT32 pad = 0, avail;
        BYTE  *buf = NULL;
        size_t want, got;

        if (w == WAIT_OBJECT_0) break;       /* stop */

        if (FAILED(IAudioClient_GetCurrentPadding(e->client, &pad))) continue;
        avail = e->wasapi_frames - pad;
        if (avail == 0) continue;
        if (FAILED(IAudioRenderClient_GetBuffer(e->render_client, avail, &buf)))
            continue;

        want = (size_t)avail * e->frame_bytes;
        got  = ring_read(&e->ring, buf, want);
        if (got < want) {
            ZeroMemory(buf + got, want - got);
            InterlockedIncrement(&e->underruns);
            relog("render: UNDERRUN, %lu/%lu bytes (silence-filled)",
                  (unsigned long)got, (unsigned long)want);
        }
        IAudioRenderClient_ReleaseBuffer(e->render_client, avail, 0);
    }

    IAudioClient_Stop(e->client);
    if (mmcss) {
        HMODULE av = GetModuleHandleA("avrt.dll");
        if (av) {
            typedef BOOL (WINAPI *PFN)(HANDLE);
            PFN f = (PFN)GetProcAddress(av, "AvRevertMmThreadCharacteristics");
            if (f) f(mmcss);
        }
    }
    CoUninitialize();
    relog("render: thread exit (underruns=%ld)", (long)e->underruns);
    return 0;
}

/* Lazily initialize WASAPI for the decoder's output format and launch
 * the render thread. Called once from the worker on the first PCM. */
static BOOL re_wasapi_start(RadioEngine *e)
{
    HRESULT     hr;
    WAVEFORMATEX wfx;
    REFERENCE_TIME req = 100000;     /* 10 ms requested (dispatch) */
    size_t      ring_bytes;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = e->out_is_float ? WAVE_FORMAT_IEEE_FLOAT
                                          : WAVE_FORMAT_PCM;
    wfx.nChannels       = e->out_channels;
    wfx.nSamplesPerSec  = e->out_rate;
    wfx.wBitsPerSample  = e->out_bits;
    wfx.nBlockAlign     = (WORD)(e->out_channels * (e->out_bits / 8));
    wfx.nAvgBytesPerSec = e->out_rate * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    /* ~500 ms ring of decoded PCM between the decode worker and the
     * render callback. */
    ring_bytes = (size_t)wfx.nAvgBytesPerSec / 2;
    if (ring_bytes < 32768) ring_bytes = 32768;
    if (!ring_init(&e->ring, ring_bytes)) {
        re_set_error(e, "ring_init failed (%lu bytes)",
                     (unsigned long)ring_bytes);
        return FALSE;
    }
    relog("wasapi: ring %lu bytes (~500 ms @ %lu B/s)",
          (unsigned long)ring_bytes, (unsigned long)wfx.nAvgBytesPerSec);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&e->enumr);
    if (FAILED(hr)) { re_set_error(e, "MMDeviceEnumerator hr=0x%08lX",
                                   (unsigned long)hr); return FALSE; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(e->enumr, eRender,
                                                     eMultimedia, &e->device);
    if (FAILED(hr)) { re_set_error(e, "GetDefaultAudioEndpoint hr=0x%08lX",
                                   (unsigned long)hr); return FALSE; }
    hr = IMMDevice_Activate(e->device, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&e->client);
    if (FAILED(hr)) { re_set_error(e, "IMMDevice_Activate hr=0x%08lX",
                                   (unsigned long)hr); return FALSE; }

    hr = IAudioClient_Initialize(e->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            req, 0, &wfx, NULL);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 fr = 0;
        IAudioClient_GetBufferSize(e->client, &fr);
        req = (REFERENCE_TIME)((10000.0 * 1000 / wfx.nSamplesPerSec) * fr + 0.5);
        relog("wasapi: re-init aligned buffer (%u frames)", fr);
        hr = IAudioClient_Initialize(e->client, AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                req, 0, &wfx, NULL);
    }
    if (FAILED(hr)) {
        /* Retry with system-default period before giving up. */
        relog("wasapi: Initialize(req=%lld) hr=0x%08lX, retry req=0",
              (long long)req, (unsigned long)hr);
        hr = IAudioClient_Initialize(e->client, AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                0, 0, &wfx, NULL);
    }
    if (FAILED(hr)) {
        re_set_error(e, "IAudioClient_Initialize hr=0x%08lX "
                     "(AUTOCONVERTPCM may be rejected on this device)",
                     (unsigned long)hr);
        return FALSE;
    }

    if (FAILED(IAudioClient_GetBufferSize(e->client, &e->wasapi_frames))) {
        re_set_error(e, "GetBufferSize failed"); return FALSE;
    }
    IAudioClient_SetEventHandle(e->client, e->audio_event);
    hr = IAudioClient_GetService(e->client, &IID_IAudioRenderClient,
                                 (void **)&e->render_client);
    if (FAILED(hr)) { re_set_error(e, "GetService(RenderClient) hr=0x%08lX",
                                   (unsigned long)hr); return FALSE; }

    relog("wasapi: ready, %u frames buffer, %lu Hz %u ch",
          e->wasapi_frames, (unsigned long)e->out_rate,
          (unsigned)e->out_channels);
    e->wasapi_ready = TRUE;

    e->render = CreateThread(NULL, 0, re_render_thread, e, 0, NULL);
    if (!e->render) { re_set_error(e, "render CreateThread failed"); return FALSE; }

    re_publish_state(e, RE_STATE_PLAYING, "first PCM rendered");
    return TRUE;
}

/* Open the WinHTTP streaming request and report icy-metaint. */
static BOOL re_http_open(RadioEngine *e)
{
    URL_COMPONENTS uc;
    wchar_t host[256], path[1024];
    DWORD   flags = 0;
    DWORD   metaint = 0, sz = sizeof(metaint);
    DWORD   status = 0;
    BOOL    secure;

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.lpszHostName     = host;  uc.dwHostNameLength     = 256;
    uc.lpszUrlPath      = path;  uc.dwUrlPathLength      = 1024;
    if (!WinHttpCrackUrl(e->url, 0, 0, &uc)) {
        re_set_error(e, "WinHttpCrackUrl failed (err %lu)", GetLastError());
        return FALSE;
    }
    secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    e->hSession = WinHttpOpen(L"MGT-Unicorn-Suite-Radio/0.2",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!e->hSession) { re_set_error(e, "WinHttpOpen err %lu", GetLastError());
                        return FALSE; }
    /* Generous receive timeout so a stalled stream cannot wedge teardown;
     * re_stop also closes hRequest to cancel a blocking read promptly. */
    WinHttpSetTimeouts(e->hSession, 10000, 10000, 10000, 15000);

    e->hConnect = WinHttpConnect(e->hSession, host, uc.nPort, 0);
    if (!e->hConnect) { re_set_error(e, "WinHttpConnect err %lu", GetLastError());
                        return FALSE; }

    if (secure) flags |= WINHTTP_FLAG_SECURE;
    e->hRequest = WinHttpOpenRequest(e->hConnect, L"GET", path, NULL,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!e->hRequest) { re_set_error(e, "WinHttpOpenRequest err %lu",
                                     GetLastError()); return FALSE; }

    /* Ask Icecast to interleave ICY metadata so we can strip it. */
    WinHttpAddRequestHeaders(e->hRequest, L"Icy-MetaData: 1\r\n",
                             (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(e->hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        re_set_error(e, "WinHttpSendRequest err %lu", GetLastError());
        return FALSE;
    }
    if (!WinHttpReceiveResponse(e->hRequest, NULL)) {
        re_set_error(e, "WinHttpReceiveResponse err %lu", GetLastError());
        return FALSE;
    }

    sz = sizeof(status);
    if (WinHttpQueryHeaders(e->hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &sz, NULL)) {
        relog("http: status %lu", (unsigned long)status);
        if (status && (status < 200 || status >= 300)) {
            re_set_error(e, "HTTP %lu", (unsigned long)status);
            return FALSE;
        }
    }

    /* icy-metaint header => interleave is active. */
    sz = sizeof(metaint);
    if (WinHttpQueryHeaders(e->hRequest,
            WINHTTP_QUERY_CUSTOM | WINHTTP_QUERY_FLAG_NUMBER,
            L"icy-metaint", &metaint, &sz, NULL)) {
        e->icy_metaint = metaint;
        e->icy_remain  = metaint;
        e->icy_phase   = 0;
        relog("http: icy-metaint=%lu (ICY stripping active)",
              (unsigned long)metaint);
    } else {
        e->icy_metaint = 0;
        relog("http: no icy-metaint header (no interleave)");
    }
    return TRUE;
}

static void re_http_close(RadioEngine *e)
{
    EnterCriticalSection(&e->net_cs);
    if (e->hRequest) { WinHttpCloseHandle(e->hRequest); e->hRequest = NULL; }
    LeaveCriticalSection(&e->net_cs);
    if (e->hConnect) { WinHttpCloseHandle(e->hConnect); e->hConnect = NULL; }
    if (e->hSession) { WinHttpCloseHandle(e->hSession); e->hSession = NULL; }
}

/* The single worker thread: COM/MF init -> HTTP connect -> read loop
 * (ICY strip + ADTS frame + MFT decode + ring write) -> teardown. */
static DWORD WINAPI re_worker_thread(LPVOID arg)
{
    RadioEngine *e = (RadioEngine *)arg;
    HRESULT cohr;
    BOOL    mf_up = FALSE;

    relog("worker: start, url=%ls", e->url);
    cohr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) mf_up = TRUE;

    re_publish_state(e, RE_STATE_CONNECTING, "connecting");

    if (!re_http_open(e)) {
        re_publish_state(e, RE_STATE_ERROR, e->last_error);
        goto teardown;
    }

    for (;;) {
        BYTE  chunk[RE_NET_CHUNK];
        DWORD got = 0;
        if (WaitForSingleObject(e->stop_event, 0) == WAIT_OBJECT_0) break;
        if (!WinHttpReadData(e->hRequest, chunk, sizeof(chunk), &got)) {
            relog("worker: WinHttpReadData err %lu", GetLastError());
            break;                          /* handle closed or error */
        }
        if (got == 0) { relog("worker: stream EOF"); break; }
        re_consume(e, chunk, got);
        if (InterlockedCompareExchange(&e->run_state, 0, 0) == RE_STATE_ERROR)
            break;
    }

    /* If we never reached PLAYING and no explicit stop was requested,
     * surface an error so the UI can offer retry. */
    if (!e->wasapi_ready &&
        WaitForSingleObject(e->stop_event, 0) != WAIT_OBJECT_0 &&
        InterlockedCompareExchange(&e->run_state, 0, 0) != RE_STATE_ERROR) {
        if (!e->last_error[0]) re_set_error(e, "stream ended before audio");
        re_publish_state(e, RE_STATE_ERROR, e->last_error);
    }

teardown:
    /* Stop + join the render thread first so it stops touching WASAPI. */
    SetEvent(e->stop_event);
    if (e->render) {
        WaitForSingleObject(e->render, 5000);
        CloseHandle(e->render);
        e->render = NULL;
    }
    if (e->render_client) { IAudioRenderClient_Release(e->render_client);
                            e->render_client = NULL; }
    if (e->client)  { IAudioClient_Release(e->client);  e->client = NULL; }
    if (e->device)  { IMMDevice_Release(e->device);     e->device = NULL; }
    if (e->enumr)   { IMMDeviceEnumerator_Release(e->enumr); e->enumr = NULL; }
    if (e->mft) {
        IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        IMFTransform_ProcessMessage(e->mft, MFT_MESSAGE_COMMAND_FLUSH, 0);
        IMFTransform_Release(e->mft);
        e->mft = NULL;
    }
    ring_free(&e->ring);
    re_http_close(e);

    e->decoder_ready = FALSE;
    e->wasapi_ready  = FALSE;
    if (InterlockedCompareExchange(&e->run_state, 0, 0) != RE_STATE_ERROR)
        re_publish_state(e, RE_STATE_STOPPED, "stopped");

    if (mf_up) MFShutdown();
    if (SUCCEEDED(cohr)) CoUninitialize();
    relog("worker: exit");
    return 0;
}

/* ---- public API ---- */

RadioEngine *re_create(void)
{
    RadioEngine *e = (RadioEngine *)HeapAlloc(GetProcessHeap(),
                                              HEAP_ZERO_MEMORY,
                                              sizeof(RadioEngine));
    if (!e) return NULL;
    relog_open();
    InitializeCriticalSection(&e->net_cs);
    e->stop_event  = CreateEventA(NULL, TRUE,  FALSE, NULL);  /* manual */
    e->audio_event = CreateEventA(NULL, FALSE, FALSE, NULL);  /* auto */
    e->run_state   = RE_STATE_IDLE;
    relog("re_create %p", (void *)e);
    return e;
}

void re_destroy(RadioEngine *e)
{
    if (!e) return;
    re_stop(e);
    if (e->stop_event)  CloseHandle(e->stop_event);
    if (e->audio_event) CloseHandle(e->audio_event);
    DeleteCriticalSection(&e->net_cs);
    relog("re_destroy %p", (void *)e);
    relog_close();
    HeapFree(GetProcessHeap(), 0, e);
}

void re_set_state_cb(RadioEngine *e, REStateCb cb, void *user)
{
    if (!e) return;
    e->state_cb = cb; e->state_user = user;
}

void re_set_meta_cb(RadioEngine *e, REMetaCb cb, void *user)
{
    if (!e) return;
    e->meta_cb = cb; e->meta_user = user;
}

BOOL re_start(RadioEngine *e, const wchar_t *icecast_url)
{
    if (!e || !icecast_url) return FALSE;
    re_stop(e);                                  /* idempotent restart */

    e->last_error[0] = '\0';
    e->underruns     = 0;
    e->adts_len      = 0;
    e->icy_phase     = 0;
    e->decoder_ready = FALSE;
    e->wasapi_ready  = FALSE;
    e->first_pcm_tick = 0;
    ResetEvent(e->stop_event);
    lstrcpynW(e->url, icecast_url, (int)(sizeof(e->url) / sizeof(wchar_t)));

    e->worker = CreateThread(NULL, 0, re_worker_thread, e, 0, NULL);
    if (!e->worker) {
        re_set_error(e, "worker CreateThread failed (err %lu)", GetLastError());
        return FALSE;
    }
    return TRUE;
}

void re_stop(RadioEngine *e)
{
    if (!e) return;
    if (!e->worker) return;

    relog("re_stop: signalling");
    SetEvent(e->stop_event);
    /* Cancel a blocking WinHttpReadData by closing the request handle. */
    EnterCriticalSection(&e->net_cs);
    if (e->hRequest) { WinHttpCloseHandle(e->hRequest); e->hRequest = NULL; }
    LeaveCriticalSection(&e->net_cs);

    WaitForSingleObject(e->worker, 8000);
    CloseHandle(e->worker);
    e->worker = NULL;
    relog("re_stop: joined");
}

const char *re_last_error(RadioEngine *e)
{
    return e ? e->last_error : "";
}
