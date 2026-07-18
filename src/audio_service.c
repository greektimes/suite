/*
 * audio_service.c - Shared Sun .au (mu-law) audio playback service.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Pipeline: HTTP fetch (libwww HTLoadToChunk) -> .au header parse ->
 * mu-law -> PCM16 decode (ITU G.711) -> waveOutOpen / Write / wait
 * for WOM_DONE -> cleanup. A modeless "Now Playing" window shows the
 * URL, current state, elapsed seconds and a Stop button. The window
 * lives on the UI thread that called play_url; the worker thread does
 * the fetch / decode / playback and PostMessages updates back to it.
 */

#include "audio_service.h"
#include "suite_shell.h"
#include "audio_meter_service.h"   /* shared WASAPI own-PID peak meter */
#include "audio_format_detect.h"   /* "Audio Type:" line description */
#include "wav_parser.h"            /* RIFF WAV / TrueSpeech (tag 0x0022) */
#include "truespeech_decoder.h"    /* DSP Group TrueSpeech decoder (LGPL port) */

#include <mmsystem.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

/* minimp3 vendored as a single-header decoder under public-domain CC0.
 * See src/minimp3.h header for the upstream URL and license dedication.
 * Local to this translation unit so the rest of the Suite never sees
 * minimp3's symbol table. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "minimp3.h"
#pragma GCC diagnostic pop

/* Format auto-detect for audio_service_play_bytes: examines the first
 * bytes of an in-memory blob and classifies it. The 6 inline-audio item
 * types the F3 Gopher MVP can hit are .au (mu-law) and .mp3; everything
 * else is dispatched to a save-dialog instead. */
typedef enum {
    AUDIO_FORMAT_UNKNOWN    = 0,
    AUDIO_FORMAT_AU         = 1,
    AUDIO_FORMAT_MP3        = 2,
    AUDIO_FORMAT_TRUESPEECH = 3   /* RIFF WAV, wFormatTag 0x0022 */
} AudioFormat;

/* The URL fetch is delegated to web_module's existing libwww-backed
 * helper. Forward-declared here to keep the audio service free of
 * libwww headers (which would re-trigger wwwsys.h's macro-redef and
 * winsock2-ordering warnings on every compile of this TU). The
 * helper's public declaration lives in web_module.h. */
extern int web_fetch_raw_bytes_sync(const char *url,
                                    char **out_bytes,
                                    int *out_size);

#define AUDIO_STATUS_CLASS   "MGTAudioStatusV1"
#define AUDIO_WIN_W          480
#define AUDIO_WIN_H          207
#define AUDIO_ID_LABEL_FILE  2005    /* "Filename:" line 0 (hidden until known) */
#define AUDIO_ID_LABEL_TYPE  2001    /* "File Type:" line 1 (hidden until parsed) */
#define AUDIO_ID_LABEL_SPEC  2004    /* "Encoding:" line 2 (hidden until parsed) */
#define AUDIO_ID_LABEL_STAT  2002
#define AUDIO_ID_LABEL_TIME  2003
#define AUDIO_ID_BTN_STOP    2010
#define AUDIO_TIMER_ID       3001
#define AUDIO_METER_TIMER_ID 3002    /* ~30 Hz mono VU poll */
#define AUDIO_MSG_STATUS     (WM_USER + 11)   /* wParam: status code */
#define AUDIO_MSG_DONE       (WM_USER + 13)   /* worker says "I'm finished" */
#define AUDIO_MSG_AUDIOTYPE  (WM_USER + 14)   /* lParam: char* desc (window frees) */
#define AUDIO_MSG_FILENAME   (WM_USER + 15)   /* lParam: char* basename (window frees) */

/* Mono VU meter (right of the text, vertically centered). Half the Live
 * Radio scale: 10 LEDs (vs 30/channel), ~half the LED width/height.
 * Same colour zones as Live Radio, in the same 60/20/10/10 proportions. */
#define AUDIO_METER_LEDS       10
#define AUDIO_METER_LED_W       6     /* ~half LR_LED_W (12) */
#define AUDIO_METER_LED_H      20     /* ~half LR_LED_H (40) */
#define AUDIO_METER_LED_GAP     3     /* ~half LR_LED_GAP (4) */
#define AUDIO_METER_TIMER_MS   33     /* ~30 Hz, matches Live Radio */

#define AUDIO_GREEN   RGB(0x33, 0xCC, 0x33)
#define AUDIO_YELLOW  RGB(0xE6, 0xE6, 0x00)
#define AUDIO_ORANGE  RGB(0xFF, 0x99, 0x00)
#define AUDIO_RED     RGB(0xE6, 0x00, 0x00)

enum {
    AUDIO_STAT_FETCHING = 0,
    AUDIO_STAT_DECODING = 1,
    AUDIO_STAT_PLAYING  = 2,
    AUDIO_STAT_STOPPING = 3,
    AUDIO_STAT_ERROR    = 4,
    AUDIO_STAT_DONE     = 5
};

/* Module-global playback state. Single-track; one playback at a time. */
static CRITICAL_SECTION g_lock;
static BOOL             g_lock_inited = FALSE;
static BOOL             g_class_registered = FALSE;
static BOOL             g_playing = FALSE;

static HWND             g_status_hwnd = NULL;
static HANDLE           g_thread      = NULL;
static HANDLE           g_stop_event  = NULL;   /* manual-reset */
static HANDLE           g_done_event  = NULL;   /* auto-reset, set by waveOut callback */
static DWORD            g_started_ms  = 0;

static char            *g_play_url    = NULL;       /* heap copy for the worker */
static char            *g_play_name   = NULL;       /* "Filename:" hint for the bytes path */
static unsigned char   *g_in_buf      = NULL;       /* in-memory .au bytes (worker-owned) */
static int              g_in_len      = 0;
static BOOL             g_in_buf_is_caller_provided = FALSE;

/* Decoded PCM. Held until the worker exits so the WAVEHDR memory is
 * valid for the device's lifetime. */
static short           *g_pcm        = NULL;
static DWORD            g_pcm_bytes  = 0;
static HWAVEOUT         g_hwo        = NULL;
static WAVEHDR          g_whdr;

/* Mu-law -> 16-bit linear PCM table (ITU G.711). Filled on init. */
static short            g_mulaw_table[256];

/* The error-string slot the worker fills when something fails. */
static char             g_err_msg[256];

/* Mono VU meter state. The meter binds to this process's own WASAPI
 * render session (the same waveOut output we feed the speakers), so it
 * needs no separate PCM tap. Lives on the status-window UI thread. */
static AudioMeter      *g_meter      = NULL;
static float            g_meter_peak = 0.0f;   /* linear 0..1, max(L,R) */

/* Set by the public entry: which decode branch the worker should take
 * for the bytes in g_in_buf. AU is the existing path; MP3 routes through
 * decode_mp3_to_pcm; UNKNOWN is rejected. */
static AudioFormat      g_format = AUDIO_FORMAT_UNKNOWN;

/* ---------- mu-law decode ---------- */

static short mulaw_to_pcm16(unsigned char u)
{
    /* Standard ITU G.711 mu-law -> linear conversion, BIAS = 0x84. */
    int sign, exponent, mantissa, sample;
    u = (unsigned char)~u;
    sign     = (u & 0x80) ? -1 : 1;
    exponent = (u >> 4) & 0x07;
    mantissa = u & 0x0F;
    sample   = ((mantissa | 0x10) << (exponent + 3)) - 0x84;
    return (short)(sign * sample);
}

static void mulaw_table_init(void)
{
    int i;
    for (i = 0; i < 256; i++)
        g_mulaw_table[i] = mulaw_to_pcm16((unsigned char)i);
}

/* ---------- diagnostic log ---------- */

/* Lightweight file-based logging gated on %MGT_AUDIO_DEBUG%=1. Used
 * during F3 polish to chase the "status window flashes and closes"
 * playback bug. Off by default in production: empty file path -> no
 * I/O. Leave the hook in place because the bytes path is exercised
 * by every Gopher click and a regression here is easy to mask. */
static void audio_dbg_logf(const char *fmt, ...)
{
    char     dir[MAX_PATH];
    char     path[MAX_PATH];
    DWORD    n;
    FILE    *fp;
    va_list  ap;

    if (!GetEnvironmentVariableA("MGT_AUDIO_DEBUG", path, sizeof(path)) ||
        path[0] != '1') {
        return;
    }
    n = GetEnvironmentVariableA("TEMP", dir, sizeof(dir));
    if (n == 0 || n >= sizeof(dir)) return;
    _snprintf(path, sizeof(path), "%s\\mgt_audio_debug.log", dir);
    fp = fopen(path, "ab");
    if (!fp) return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* ---------- filename from URL ---------- */

static int audio_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Derive a display filename from `url`: the last path segment, with any
 * %XX escapes decoded. Query/fragment are stripped first. Returns TRUE
 * and fills `out` (NUL-terminated) on success; FALSE if no usable
 * basename (no path, trailing slash, or empty). */
static BOOL audio_url_basename(const char *url, char *out, size_t out_len)
{
    const char *p, *seg, *end;
    size_t i = 0;
    if (!url || !out || out_len == 0) return FALSE;
    end = url;
    while (*end && *end != '?' && *end != '#') end++;   /* strip query/fragment */
    seg = url;
    for (p = url; p < end; p++)
        if (*p == '/') seg = p + 1;                      /* last '/' */
    if (seg >= end) return FALSE;                        /* trailing slash / empty */
    for (p = seg; p < end && i + 1 < out_len; ) {
        if (*p == '%' && p + 2 < end &&
            audio_hexval(p[1]) >= 0 && audio_hexval(p[2]) >= 0) {
            out[i++] = (char)((audio_hexval(p[1]) << 4) | audio_hexval(p[2]));
            p += 3;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i > 0;
}

/* ---------- format detect ---------- */

static AudioFormat audio_format_detect(const unsigned char *buf, size_t len)
{
    size_t scan;
    size_t i;
    mp3dec_t          probe_dec;
    mp3dec_frame_info_t probe_info;
    short             probe_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int               probe_samples;

    if (!buf || len < 4) return AUDIO_FORMAT_UNKNOWN;

    /* AU magic ".snd" at offset 0. */
    if (buf[0] == 0x2E && buf[1] == 0x73 && buf[2] == 0x6E && buf[3] == 0x64)
        return AUDIO_FORMAT_AU;

    /* RIFF WAV "RIFF"...."WAVE": accept only if the narrow parser confirms
     * a TrueSpeech (tag 0x0022) fmt chunk; any other format tag stays
     * UNKNOWN (we do not play generic PCM/ADPCM WAVs). */
    if (len >= 12 &&
        buf[0] == 'R' && buf[1] == 'I' && buf[2] == 'F' && buf[3] == 'F' &&
        buf[8] == 'W' && buf[9] == 'A' && buf[10] == 'V' && buf[11] == 'E') {
        wav_truespeech_info_t wi;
        if (wav_parse_truespeech(buf, len, &wi) == WAV_OK)
            return AUDIO_FORMAT_TRUESPEECH;
        return AUDIO_FORMAT_UNKNOWN;
    }

    /* MP3 with ID3 tag: classic prefix "ID3". */
    if (buf[0] == 0x49 && buf[1] == 0x44 && buf[2] == 0x33)
        return AUDIO_FORMAT_MP3;

    /* MP3 without ID3: hunt for a valid frame sync in the first ~4 KB.
     * Per ISO 11172-3, the sync word is 11 ones (0xFFE..0xFFF in the first
     * 12 bits). Heuristic: byte b0 == 0xFF and (b1 & 0xE0) == 0xE0 looks
     * like a frame header. Verify with minimp3 by asking it to decode the
     * frame; mp3dec_decode_frame returns the consumed byte count via
     * info.frame_bytes > 0 when the candidate is a real MP3 frame. */
    scan = len < 4096 ? len : 4096;
    if (scan < 4) return AUDIO_FORMAT_UNKNOWN;
    mp3dec_init(&probe_dec);
    for (i = 0; i + 3 < scan; i++) {
        if (buf[i] != 0xFF) continue;
        if ((buf[i + 1] & 0xE0) != 0xE0) continue;
        probe_samples = mp3dec_decode_frame(
            &probe_dec,
            buf + i,
            (int)(len - i),
            probe_pcm,
            &probe_info);
        if (probe_info.frame_bytes > 0 && probe_samples > 0)
            return AUDIO_FORMAT_MP3;
    }
    return AUDIO_FORMAT_UNKNOWN;
}

/* ---------- MP3 decode (minimp3) ---------- */

/* Decode an in-memory MP3 stream into interleaved int16 PCM. The first
 * decoded frame fixes the sample rate and channel count for the run; if
 * a later frame disagrees minimp3 may emit fewer samples but the wave
 * format header we hand to waveOut stays bound to the first frame's
 * values (acceptable for v0.1.0-mvp: typical Gopher-served MP3s do not
 * change rate mid-stream). */
static int decode_mp3_to_pcm(const unsigned char *buf, size_t len,
                             short **out_pcm, size_t *out_samples,
                             int *out_rate, int *out_channels)
{
    mp3dec_t            dec;
    mp3dec_frame_info_t info;
    short               frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int                 frame_samples;
    size_t              pcm_cap   = 65536 / sizeof(short);
    size_t              pcm_count = 0;
    short              *pcm;
    const unsigned char *p   = buf;
    size_t              left = len;
    int                 first_frame_seen = 0;
    int                 rate = 0;
    int                 ch   = 0;

    if (!buf || len == 0 || !out_pcm || !out_samples
        || !out_rate || !out_channels)
        return 0;

    pcm = (short *)malloc(pcm_cap * sizeof(short));
    if (!pcm) return 0;

    mp3dec_init(&dec);
    while (left > 0) {
        info.frame_bytes = 0;
        frame_samples = mp3dec_decode_frame(&dec, p, (int)left,
                                            frame_pcm, &info);
        if (info.frame_bytes <= 0) {
            /* No frame found at all in the remaining buffer. */
            if (!first_frame_seen) {
                free(pcm);
                return 0;
            }
            break;
        }
        if (frame_samples > 0) {
            int interleaved;
            if (!first_frame_seen) {
                rate = info.hz;
                ch   = info.channels;
                first_frame_seen = 1;
            }
            interleaved = frame_samples * info.channels;
            if (pcm_count + (size_t)interleaved > pcm_cap) {
                size_t new_cap = pcm_cap;
                short *new_pcm;
                while (new_cap < pcm_count + (size_t)interleaved)
                    new_cap *= 2;
                new_pcm = (short *)realloc(pcm, new_cap * sizeof(short));
                if (!new_pcm) { free(pcm); return 0; }
                pcm     = new_pcm;
                pcm_cap = new_cap;
            }
            memcpy(pcm + pcm_count, frame_pcm,
                   (size_t)interleaved * sizeof(short));
            pcm_count += (size_t)interleaved;
        }
        if ((size_t)info.frame_bytes > left) break;
        p    += info.frame_bytes;
        left -= info.frame_bytes;
    }

    if (!first_frame_seen || pcm_count == 0) {
        free(pcm);
        return 0;
    }
    *out_pcm      = pcm;
    *out_samples  = pcm_count;
    *out_rate     = rate;
    *out_channels = ch;
    return 1;
}

/* ---------- .au header ---------- */

typedef struct {
    DWORD data_offset;
    DWORD data_size;
    DWORD encoding;       /* 1 = 8-bit mu-law */
    DWORD sample_rate;
    DWORD channels;
} au_header_t;

static DWORD read_be32(const unsigned char *p)
{
    return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16)
         | ((DWORD)p[2] << 8)  | (DWORD)p[3];
}

static BOOL au_parse_header(const unsigned char *buf, int len, au_header_t *out)
{
    if (!buf || len < 24 || !out) return FALSE;
    /* Magic ".snd" */
    if (!(buf[0] == 0x2E && buf[1] == 0x73 && buf[2] == 0x6E && buf[3] == 0x64))
        return FALSE;
    out->data_offset = read_be32(buf + 4);
    out->data_size   = read_be32(buf + 8);
    out->encoding    = read_be32(buf + 12);
    out->sample_rate = read_be32(buf + 16);
    out->channels    = read_be32(buf + 20);
    if (out->data_offset < 24 || (int)out->data_offset > len) return FALSE;
    return TRUE;
}

/* ---------- URL fetch (delegated to web_module's libwww helper) ---------- */

static BOOL audio_fetch_url(const char *url, unsigned char **out_buf, int *out_len)
{
    char *raw = NULL;
    int   raw_n = 0;
    *out_buf = NULL; *out_len = 0;
    if (!web_fetch_raw_bytes_sync(url, &raw, &raw_n)) return FALSE;
    if (!raw || raw_n <= 0) { if (raw) free(raw); return FALSE; }
    *out_buf = (unsigned char *)raw;   /* same heap, just rebind type */
    *out_len = raw_n;
    return TRUE;
}

/* ---------- waveOut ---------- */

static void CALLBACK audio_waveout_proc(HWAVEOUT hwo, UINT msg,
                                        DWORD_PTR inst,
                                        DWORD_PTR p1, DWORD_PTR p2)
{
    (void)hwo; (void)inst; (void)p1; (void)p2;
    if (msg == WOM_DONE) {
        if (g_done_event) SetEvent(g_done_event);
    }
}

static BOOL audio_play_pcm(short *pcm, DWORD bytes, DWORD sample_rate, DWORD channels)
{
    WAVEFORMATEX wf;
    MMRESULT     mmr;

    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = (WORD)channels;
    wf.nSamplesPerSec  = sample_rate;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (WORD)(wf.nChannels * (wf.wBitsPerSample / 8));
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize          = 0;

    mmr = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf,
                      (DWORD_PTR)audio_waveout_proc, 0,
                      CALLBACK_FUNCTION);
    if (mmr != MMSYSERR_NOERROR) {
        _snprintf(g_err_msg, sizeof(g_err_msg),
                  "waveOutOpen failed (code %u).", (unsigned)mmr);
        g_hwo = NULL;
        return FALSE;
    }

    memset(&g_whdr, 0, sizeof(g_whdr));
    g_whdr.lpData         = (LPSTR)pcm;
    g_whdr.dwBufferLength = bytes;
    g_whdr.dwFlags        = 0;

    mmr = waveOutPrepareHeader(g_hwo, &g_whdr, sizeof(g_whdr));
    if (mmr != MMSYSERR_NOERROR) {
        _snprintf(g_err_msg, sizeof(g_err_msg),
                  "waveOutPrepareHeader failed (code %u).", (unsigned)mmr);
        waveOutClose(g_hwo); g_hwo = NULL;
        return FALSE;
    }
    mmr = waveOutWrite(g_hwo, &g_whdr, sizeof(g_whdr));
    if (mmr != MMSYSERR_NOERROR) {
        _snprintf(g_err_msg, sizeof(g_err_msg),
                  "waveOutWrite failed (code %u).", (unsigned)mmr);
        waveOutUnprepareHeader(g_hwo, &g_whdr, sizeof(g_whdr));
        waveOutClose(g_hwo); g_hwo = NULL;
        return FALSE;
    }
    return TRUE;
}

static void audio_close_waveout(void)
{
    if (!g_hwo) return;
    waveOutReset(g_hwo);
    if (g_whdr.dwFlags & WHDR_PREPARED)
        waveOutUnprepareHeader(g_hwo, &g_whdr, sizeof(g_whdr));
    waveOutClose(g_hwo);
    g_hwo = NULL;
}

/* ---------- mono VU meter (half Live Radio scale) ---------- */

/* Colour zone for LED i, matching Live Radio's green/yellow/orange/red
 * progression scaled to 10 LEDs (0-5 green, 6-7 yellow, 8 orange, 9 red). */
static COLORREF audio_led_color(int i)
{
    if (i < 6) return AUDIO_GREEN;
    if (i < 8) return AUDIO_YELLOW;
    if (i < 9) return AUDIO_ORANGE;
    return AUDIO_RED;
}

static COLORREF audio_dim(COLORREF c, double f)
{
    return RGB((int)(GetRValue(c) * f),
               (int)(GetGValue(c) * f),
               (int)(GetBValue(c) * f));
}

/* The LED block rect (client coords), vertically centered against the
 * three text rows, hugging the right edge. Used by both the painter and
 * the timer's InvalidateRect so they stay in lock-step. */
static void audio_meter_rect(HWND h, RECT *out)
{
    RECT rc;
    int total_w = AUDIO_METER_LEDS * AUDIO_METER_LED_W
                + (AUDIO_METER_LEDS - 1) * AUDIO_METER_LED_GAP;
    int x, y;
    GetClientRect(h, &rc);
    x = (rc.right - rc.left - total_w) / 2;  /* horizontally centered (above Stop button) */
    y = 82 + ((126 - 82) - AUDIO_METER_LED_H) / 2;  /* center on Status+Elapsed rows 82..126 */
    out->left   = x;
    out->top    = y;
    out->right  = x + total_w;
    out->bottom = y + AUDIO_METER_LED_H;
}

static void audio_draw_meter(HWND h, HDC hdc_target)
{
    RECT mr, panel;
    HDC mem_dc;
    HBITMAP mem_bm, old_bm;
    HBRUSH bk;
    int i, n_lit, w, hgt;
    float db;

    audio_meter_rect(h, &mr);
    /* Black sub-panel behind the LEDs (Live Radio aesthetic; the dialog
     * itself is button-face grey). Pad 5 px around the LED block. */
    panel.left = mr.left - 5; panel.top = mr.top - 5;
    panel.right = mr.right + 5; panel.bottom = mr.bottom + 5;
    w = panel.right - panel.left; hgt = panel.bottom - panel.top;
    if (w <= 0 || hgt <= 0) return;

    /* Double-buffer the small panel to avoid 30 Hz flicker (the timer
     * invalidates with bErase=FALSE, so we own every pixel here). */
    mem_dc = CreateCompatibleDC(hdc_target);
    mem_bm = CreateCompatibleBitmap(hdc_target, w, hgt);
    old_bm = (HBITMAP)SelectObject(mem_dc, mem_bm);

    bk = (HBRUSH)GetStockObject(BLACK_BRUSH);
    { RECT fr = {0, 0, w, hgt}; FillRect(mem_dc, &fr, bk); }

    /* Peak -> lit count on a -60..0 dBFS scale (same mapping as Live Radio). */
    db = 20.0f * log10f(g_meter_peak > 1e-7f ? g_meter_peak : 1e-7f);
    if (db < -60.0f) db = -60.0f;
    if (db >   0.0f) db =  0.0f;
    n_lit = (int)((db + 60.0f) / 60.0f * AUDIO_METER_LEDS + 0.5f);
    if (n_lit > AUDIO_METER_LEDS) n_lit = AUDIO_METER_LEDS;
    if (n_lit < 0)                n_lit = 0;

    for (i = 0; i < AUDIO_METER_LEDS; i++) {
        COLORREF c = audio_led_color(i);
        COLORREF paint = (i < n_lit) ? c : audio_dim(c, 0.12);
        int lx = 5 + i * (AUDIO_METER_LED_W + AUDIO_METER_LED_GAP);
        int radius = AUDIO_METER_LED_W;
        HBRUSH br = CreateSolidBrush(paint);
        HPEN   pen = CreatePen(PS_NULL, 0, paint);
        HBRUSH ob = (HBRUSH)SelectObject(mem_dc, br);
        HPEN   op = (HPEN)SelectObject(mem_dc, pen);
        if (radius > AUDIO_METER_LED_H) radius = AUDIO_METER_LED_H;
        RoundRect(mem_dc, lx, 5, lx + AUDIO_METER_LED_W, 5 + AUDIO_METER_LED_H,
                  radius, radius);
        SelectObject(mem_dc, ob); SelectObject(mem_dc, op);
        DeleteObject(br); DeleteObject(pen);
    }

    BitBlt(hdc_target, panel.left, panel.top, w, hgt, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bm);
    DeleteObject(mem_bm);
    DeleteDC(mem_dc);
}

/* ---------- status window ---------- */

static void audio_status_set_label(int code)
{
    HWND lbl;
    if (!g_status_hwnd) return;
    lbl = GetDlgItem(g_status_hwnd, AUDIO_ID_LABEL_STAT);
    if (!lbl) return;
    switch (code) {
    case AUDIO_STAT_FETCHING: SetWindowTextA(lbl, "Status: Fetching..."); break;
    case AUDIO_STAT_DECODING: SetWindowTextA(lbl, "Status: Decoding..."); break;
    case AUDIO_STAT_PLAYING:  SetWindowTextA(lbl, "Status: Playing");     break;
    case AUDIO_STAT_STOPPING: SetWindowTextA(lbl, "Status: Stopping...");  break;
    case AUDIO_STAT_DONE:     SetWindowTextA(lbl, "Status: Done");        break;
    case AUDIO_STAT_ERROR: {
        char buf[320];
        _snprintf(buf, sizeof(buf), "Status: Error - %s", g_err_msg);
        SetWindowTextA(lbl, buf);
        break;
    }
    default: SetWindowTextA(lbl, "Status: ?"); break;
    }
}

static LRESULT CALLBACK audio_status_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(h, GWLP_HINSTANCE);
        HWND lbl_file, lbl_type, lbl_spec, lbl_stat, lbl_time, btn;
        int lbl_w = AUDIO_WIN_W - 24 - 110;
        /* The VU meter is centered horizontally on the Status+Elapsed
         * vertical band. Those two label controls must therefore stop
         * short of the centered meter, or their (grey) control background
         * paints over it and only a thin sliver of the LEDs shows. low_w
         * ends just left of the meter's centered panel. The Filename,
         * File Type and Encoding rows sit ABOVE the meter, so they keep
         * their wider widths. */
        int low_w = AUDIO_WIN_W / 2 - 72;
        /* Three file-info rows start HIDDEN: Filename (line 0), File Type
         * (line 1) and Encoding (line 2). Filename appears on
         * AUDIO_MSG_FILENAME (derived from the URL); File Type + Encoding
         * on AUDIO_MSG_AUDIOTYPE when the header parse succeeds. On
         * missing/unparseable they stay hidden -- no placeholder text. */
        lbl_file = CreateWindowA("STATIC", "",
            WS_CHILD | SS_LEFT,
            12, 12, AUDIO_WIN_W - 24, 20,
            h, (HMENU)(INT_PTR)AUDIO_ID_LABEL_FILE, hInst, NULL);
        lbl_type = CreateWindowA("STATIC", "",
            WS_CHILD | SS_LEFT,
            12, 34, AUDIO_WIN_W - 24, 20,
            h, (HMENU)(INT_PTR)AUDIO_ID_LABEL_TYPE, hInst, NULL);
        lbl_spec = CreateWindowA("STATIC", "",
            WS_CHILD | SS_LEFT,
            12, 56, lbl_w, 20,
            h, (HMENU)(INT_PTR)AUDIO_ID_LABEL_SPEC, hInst, NULL);
        lbl_stat = CreateWindowA("STATIC", "Status: Fetching...",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 82, low_w, 20,
            h, (HMENU)(INT_PTR)AUDIO_ID_LABEL_STAT, hInst, NULL);
        lbl_time = CreateWindowA("STATIC", "Elapsed: 0s",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 106, low_w, 20,
            h, (HMENU)(INT_PTR)AUDIO_ID_LABEL_TIME, hInst, NULL);
        btn = CreateWindowA("BUTTON", "Stop",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            AUDIO_WIN_W / 2 - 45, 138, 90, 28,
            h, (HMENU)(INT_PTR)AUDIO_ID_BTN_STOP, hInst, NULL);
        if (g_hFontUI) {
            SendMessageA(lbl_file, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(lbl_type, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(lbl_spec, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(lbl_stat, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(lbl_time, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(btn,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }
        /* Mono VU meter: bind this process's own WASAPI session (lazy --
         * it latches once waveOut audio flows) and poll at ~30 Hz. */
        audio_meter_init();
        g_meter = audio_meter_create();
        g_meter_peak = 0.0f;
        SetTimer(h, AUDIO_TIMER_ID, 250, NULL);
        SetTimer(h, AUDIO_METER_TIMER_ID, AUDIO_METER_TIMER_MS, NULL);
        SetFocus(btn);
        return 0;
    }
    case WM_TIMER:
        if (w == AUDIO_TIMER_ID) {
            HWND lbl = GetDlgItem(h, AUDIO_ID_LABEL_TIME);
            if (lbl) {
                char buf[64];
                DWORD now = GetTickCount();
                DWORD elapsed_ms = (g_started_ms == 0) ? 0 : (now - g_started_ms);
                _snprintf(buf, sizeof(buf), "Elapsed: %u.%us",
                          (unsigned)(elapsed_ms / 1000),
                          (unsigned)((elapsed_ms / 100) % 10));
                SetWindowTextA(lbl, buf);
            }
        } else if (w == AUDIO_METER_TIMER_ID) {
            float pl = 0.0f, pr = 0.0f;
            RECT mr, panel;
            if (g_meter && audio_meter_poll(g_meter, &pl, &pr))
                g_meter_peak = (pl > pr) ? pl : pr;   /* mono = louder channel */
            else
                g_meter_peak = 0.0f;
            /* Invalidate just the (padded) meter panel, bErase=FALSE: the
             * painter owns every pixel, so no flicker / no logo thrash. */
            audio_meter_rect(h, &mr);
            panel.left = mr.left - 5; panel.top = mr.top - 5;
            panel.right = mr.right + 5; panel.bottom = mr.bottom + 5;
            InvalidateRect(h, &panel, FALSE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == AUDIO_ID_BTN_STOP) {
            audio_status_set_label(AUDIO_STAT_STOPPING);
            EnableWindow(GetDlgItem(h, AUDIO_ID_BTN_STOP), FALSE);
            if (g_stop_event) SetEvent(g_stop_event);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        audio_draw_meter(h, hdc);
        EndPaint(h, &ps);
        return 0;
    }
    case AUDIO_MSG_STATUS:
        audio_status_set_label((int)w);
        return 0;
    case AUDIO_MSG_FILENAME: {
        /* Worker derived the basename from the source URL. Show the row
         * (created hidden). l is a heap copy we now own. */
        HWND lbl_file = GetDlgItem(h, AUDIO_ID_LABEL_FILE);
        char *name = (char *)l;
        if (lbl_file && name && name[0]) {
            char buf[256];
            _snprintf(buf, sizeof(buf), "Filename: %s", name);
            buf[sizeof(buf) - 1] = '\0';
            SetWindowTextA(lbl_file, buf);
            ShowWindow(lbl_file, SW_SHOW);
        }
        if (name) free(name);
        return 0;
    }
    case AUDIO_MSG_AUDIOTYPE: {
        /* Worker parsed the format. Split the description at the first
         * ", " into a file-type line and a specs line, then reveal both
         * (they were created hidden). l is a heap copy we now own.
         *   .au : "Sun Microsystems audio file (.au)" | "u-law, 8 kHz, Mono"
         *   .mp3: "MPEG-1 Layer III"                  | "128 kbps, 44.1 kHz, Stereo"
         */
        HWND lbl_type = GetDlgItem(h, AUDIO_ID_LABEL_TYPE);
        HWND lbl_spec = GetDlgItem(h, AUDIO_ID_LABEL_SPEC);
        char *desc = (char *)l;
        if (lbl_type && desc && desc[0]) {
            char  buf[256];
            char *split = strstr(desc, ", ");
            const char *specs = NULL;
            if (split) { *split = '\0'; specs = split + 2; }
            _snprintf(buf, sizeof(buf), "File Type: %s", desc);
            buf[sizeof(buf) - 1] = '\0';
            SetWindowTextA(lbl_type, buf);
            ShowWindow(lbl_type, SW_SHOW);
            if (lbl_spec && specs && specs[0]) {
                char sbuf[256];
                _snprintf(sbuf, sizeof(sbuf), "Encoding: %s", specs);
                sbuf[sizeof(sbuf) - 1] = '\0';
                SetWindowTextA(lbl_spec, sbuf);
                ShowWindow(lbl_spec, SW_SHOW);
            }
        }
        if (desc) free(desc);
        return 0;
    }
    case AUDIO_MSG_DONE:
        /* Worker says it has stopped. Close ourselves. */
        DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        /* User closed (X or Alt-F4). Same as Stop. */
        if (g_stop_event) SetEvent(g_stop_event);
        return 0;
    case WM_DESTROY:
        KillTimer(h, AUDIO_TIMER_ID);
        KillTimer(h, AUDIO_METER_TIMER_ID);
        if (g_meter) { audio_meter_destroy(g_meter); g_meter = NULL; }
        audio_meter_shutdown();
        g_meter_peak = 0.0f;
        /* The status hwnd pointer is cleared by the worker on its way
         * out so that a stale teardown can't race a fresh start. */
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void audio_register_class_once(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_class_registered) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = audio_status_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = AUDIO_STATUS_CLASS;
    RegisterClassA(&wc);
    g_class_registered = TRUE;
}

static HWND audio_create_status_window(HWND owner)
{
    HINSTANCE hInst = (HINSTANCE)GetModuleHandleA(NULL);
    int x = 0, y = 0;
    audio_register_class_once(hInst);

    /* Center on the Suite's top-level window via the shared helper.
     * Follows the Suite onto a secondary monitor; falls back to the
     * primary-screen center if no Suite window can be found. */
    suite_center_popup(owner, AUDIO_WIN_W, AUDIO_WIN_H, &x, &y);

    return CreateWindowExA(WS_EX_DLGMODALFRAME, AUDIO_STATUS_CLASS,
        "Retro Audio Player",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, AUDIO_WIN_W, AUDIO_WIN_H,
        owner, NULL, hInst, NULL);
}

/* ---------- worker thread ---------- */

static DWORD WINAPI audio_worker(LPVOID arg)
{
    au_header_t hdr;
    DWORD       pcm_samples;
    DWORD       i;
    HANDLE      handles[2];
    DWORD       wait;
    DWORD       play_rate     = 8000;
    DWORD       play_channels = 1;

    (void)arg;

    g_started_ms = GetTickCount();

    audio_dbg_logf("[worker] enter g_in_buf=%p g_in_len=%d g_play_url=%s g_format=%d",
                   (void *)g_in_buf, g_in_len,
                   g_play_url ? g_play_url : "(null)", (int)g_format);

    /* 1. If no bytes were handed to us, fetch via libwww URL. The
     *    previous condition checked g_in_buf_is_caller_provided, but
     *    BOTH the URL path and the bytes path set that flag to FALSE
     *    (the bytes path malloc-copies and "owns" the buffer), so the
     *    worker would try to fetch a NULL URL when called via
     *    audio_service_play_bytes. Decide on the data, not the flag. */
    if (!g_in_buf) {
        if (g_status_hwnd)
            PostMessageA(g_status_hwnd, AUDIO_MSG_STATUS,
                         AUDIO_STAT_FETCHING, 0);
        if (!audio_fetch_url(g_play_url, &g_in_buf, &g_in_len)) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Could not fetch %s.",
                      g_play_url ? g_play_url : "(null URL)");
            audio_dbg_logf("[worker] URL fetch failed");
            goto fail;
        }
        /* Classify the fetched bytes by magic. The .au link route still
         * lands here as AUDIO_FORMAT_AU; the .tsp route hands us the inner
         * .wav URL, whose RIFF/WAVE bytes classify as TrueSpeech. */
        g_format = audio_format_detect(g_in_buf, (size_t)g_in_len);
        audio_dbg_logf("[worker] URL fetched %d bytes, format=%d",
                       g_in_len, (int)g_format);
    } else {
        audio_dbg_logf("[worker] using caller bytes (no fetch)");
    }

    /* Filename row: derived from the last path segment of the source URL
     * (audio_service_play_url) or, for the caller-bytes path, the name
     * hint (e.g. the gopher selector) passed to
     * audio_service_play_bytes_named. If neither is available the row
     * stays hidden. Posted before the Audio Type row. */
    {
    const char *fn_src = g_play_url ? g_play_url : g_play_name;
    if (g_status_hwnd && fn_src) {
        char name[160];
        if (audio_url_basename(fn_src, name, sizeof(name))) {
            size_t nn = strlen(name) + 1;
            char *heap = (char *)malloc(nn);
            if (heap) {
                memcpy(heap, name, nn);
                PostMessageA(g_status_hwnd, AUDIO_MSG_FILENAME,
                             0, (LPARAM)heap);   /* window frees */
            }
            audio_dbg_logf("[worker] filename: %s", name);
        } else {
            audio_dbg_logf("[worker] filename: (none derivable, row hidden)");
        }
    }
    }

    /* Now that the raw bytes are in hand (fetched or caller-provided),
     * derive the "Audio Type:" line from the header and post it. If the
     * parse fails the row stays hidden -- show nothing rather than stale
     * or "unknown" info. */
    if (g_status_hwnd && g_in_buf && g_in_len > 0) {
        char desc[160];
        if (audio_format_describe(g_in_buf, (size_t)g_in_len,
                                  desc, sizeof(desc))) {
            size_t dn = strlen(desc) + 1;
            char *heap = (char *)malloc(dn);
            if (heap) {
                memcpy(heap, desc, dn);
                PostMessageA(g_status_hwnd, AUDIO_MSG_AUDIOTYPE,
                             0, (LPARAM)heap);   /* window frees */
            }
            audio_dbg_logf("[worker] audio type: %s", desc);
        } else {
            audio_dbg_logf("[worker] audio type: (parse failed, row hidden)");
        }
    }

    if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) goto cleanup;

    /* 2. Decode. */
    if (g_status_hwnd)
        PostMessageA(g_status_hwnd, AUDIO_MSG_STATUS, AUDIO_STAT_DECODING, 0);

    if (g_format == AUDIO_FORMAT_AU) {
        if (!au_parse_header(g_in_buf, g_in_len, &hdr)) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Not a valid Sun .au file (.snd magic missing).");
            audio_dbg_logf("[worker] au_parse_header FAILED (first 4 bytes: %02x %02x %02x %02x)",
                           g_in_len > 0 ? g_in_buf[0] : 0,
                           g_in_len > 1 ? g_in_buf[1] : 0,
                           g_in_len > 2 ? g_in_buf[2] : 0,
                           g_in_len > 3 ? g_in_buf[3] : 0);
            goto fail;
        }
        audio_dbg_logf("[worker] AU hdr: data_offset=%u data_size=%u encoding=%u sample_rate=%u channels=%u (buf=%d)",
                       (unsigned)hdr.data_offset, (unsigned)hdr.data_size,
                       (unsigned)hdr.encoding, (unsigned)hdr.sample_rate,
                       (unsigned)hdr.channels, g_in_len);
        if (hdr.encoding != 1) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Unsupported encoding %u (only 8-bit mu-law=1).",
                      (unsigned)hdr.encoding);
            goto fail;
        }
        if (hdr.channels < 1 || hdr.channels > 2) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Unsupported channel count %u.", (unsigned)hdr.channels);
            goto fail;
        }
        if (hdr.sample_rate < 4000 || hdr.sample_rate > 96000) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Unsupported sample rate %u.", (unsigned)hdr.sample_rate);
            goto fail;
        }

        /* 3a. Decode mu-law -> PCM16. data_size == 0 / 0xFFFFFFFF (or
         *     implausibly large) means "until EOF"; clamp to actual file
         *     length. */
        {
            DWORD data_avail = (DWORD)g_in_len - hdr.data_offset;
            DWORD use;
            if (hdr.data_size == 0 || hdr.data_size > data_avail)
                use = data_avail;
            else
                use = hdr.data_size;
            pcm_samples = use;   /* one sample per mu-law byte */
        }
        if (pcm_samples == 0) {
            _snprintf(g_err_msg, sizeof(g_err_msg), "Empty audio payload.");
            goto fail;
        }

        g_pcm_bytes = pcm_samples * sizeof(short);
        g_pcm       = (short *)malloc(g_pcm_bytes);
        if (!g_pcm) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Out of memory for PCM buffer.");
            goto fail;
        }
        {
            const unsigned char *src = g_in_buf + hdr.data_offset;
            for (i = 0; i < pcm_samples; i++)
                g_pcm[i] = g_mulaw_table[src[i]];
        }
        play_rate     = hdr.sample_rate;
        play_channels = hdr.channels;
    } else if (g_format == AUDIO_FORMAT_MP3) {
        /* 3b. Decode MP3 -> PCM16 via vendored minimp3. */
        size_t mp3_samples = 0;
        int    mp3_rate = 0, mp3_channels = 0;
        if (!decode_mp3_to_pcm(g_in_buf, (size_t)g_in_len,
                               &g_pcm, &mp3_samples,
                               &mp3_rate, &mp3_channels)) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "MP3 decode failed (no frames produced).");
            goto fail;
        }
        g_pcm_bytes   = (DWORD)(mp3_samples * sizeof(short));
        play_rate     = (DWORD)mp3_rate;
        play_channels = (DWORD)mp3_channels;
    } else if (g_format == AUDIO_FORMAT_TRUESPEECH) {
        /* 3c. Decode DSP Group TrueSpeech (RIFF WAV tag 0x0022) -> PCM16
         *     via the FFmpeg-ported decoder. The 'data' chunk is the raw
         *     codec bitstream; each 32-byte frame yields 240 mono samples
         *     at 8000 Hz. */
        wav_truespeech_info_t wi;
        size_t frames, cap_samples, got = 0;
        int dec_rc;
        if (wav_parse_truespeech(g_in_buf, (size_t)g_in_len, &wi) != WAV_OK) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Not a valid TrueSpeech WAV (fmt/data parse failed).");
            goto fail;
        }
        audio_dbg_logf("[worker] TrueSpeech WAV: data_offset=%u data_len=%u rate=%u chans=%u",
                       (unsigned)wi.data_offset, (unsigned)wi.data_length,
                       (unsigned)wi.sample_rate, (unsigned)wi.channels);
        frames = wi.data_length / TRUESPEECH_FRAME_BYTES;
        if (frames == 0) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Empty TrueSpeech payload (no 32-byte frames).");
            goto fail;
        }
        cap_samples = frames * TRUESPEECH_FRAME_SAMPLES;
        g_pcm = (short *)malloc(cap_samples * sizeof(short));
        if (!g_pcm) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "Out of memory for PCM buffer.");
            goto fail;
        }
        dec_rc = truespeech_decode(g_in_buf + wi.data_offset,
                                   (size_t)wi.data_length,
                                   g_pcm, cap_samples, &got);
        if (dec_rc != 0 || got == 0) {
            _snprintf(g_err_msg, sizeof(g_err_msg),
                      "TrueSpeech decode failed (rc=%d).", dec_rc);
            goto fail;
        }
        g_pcm_bytes   = (DWORD)(got * sizeof(short));
        /* TrueSpeech is fixed mono 8 kHz; trust the fmt chunk but fall
         * back to the codec's nominal values if the header lied. */
        play_rate     = wi.sample_rate ? wi.sample_rate : 8000;
        play_channels = wi.channels    ? wi.channels    : 1;
    } else {
        _snprintf(g_err_msg, sizeof(g_err_msg),
                  "Unrecognized audio format (not .au, not MP3, not TrueSpeech).");
        goto fail;
    }

    /* The compressed source is no longer needed. */
    if (!g_in_buf_is_caller_provided) {
        free(g_in_buf); g_in_buf = NULL; g_in_len = 0;
    }

    if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) goto cleanup;

    /* 4. Play. */
    if (g_status_hwnd)
        PostMessageA(g_status_hwnd, AUDIO_MSG_STATUS, AUDIO_STAT_PLAYING, 0);
    g_started_ms = GetTickCount();
    audio_dbg_logf("[worker] play_pcm: bytes=%u rate=%u channels=%u",
                   (unsigned)g_pcm_bytes, (unsigned)play_rate, (unsigned)play_channels);
    if (!audio_play_pcm(g_pcm, g_pcm_bytes, play_rate, play_channels)) {
        audio_dbg_logf("[worker] audio_play_pcm FAILED: %s", g_err_msg);
        goto fail;
    }

    /* 5. Wait for completion or stop. */
    handles[0] = g_done_event;
    handles[1] = g_stop_event;
    wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    (void)wait;

cleanup:
    audio_close_waveout();
    if (g_status_hwnd)
        PostMessageA(g_status_hwnd, AUDIO_MSG_STATUS, AUDIO_STAT_DONE, 0);
    goto exit_thread;

fail:
    audio_close_waveout();
    if (g_status_hwnd)
        PostMessageA(g_status_hwnd, AUDIO_MSG_STATUS, AUDIO_STAT_ERROR, 0);

exit_thread:
    /* Hand the window a "go away" message. The window may already be
     * gone if the user X'd it (which sets stop_event and the worker
     * unwinds normally). */
    if (g_status_hwnd) {
        PostMessageA(g_status_hwnd, AUDIO_MSG_DONE, 0, 0);
    }
    /* Free PCM after the device is closed. */
    if (g_pcm)    { free(g_pcm); g_pcm = NULL; g_pcm_bytes = 0; }
    if (g_in_buf && !g_in_buf_is_caller_provided) {
        free(g_in_buf); g_in_buf = NULL;
    }
    if (g_play_url) { free(g_play_url); g_play_url = NULL; }
    if (g_play_name) { free(g_play_name); g_play_name = NULL; }

    EnterCriticalSection(&g_lock);
    g_playing = FALSE;
    g_status_hwnd = NULL;
    LeaveCriticalSection(&g_lock);
    return 0;
}

/* ---------- public API ---------- */

void audio_service_init(void)
{
    if (!g_lock_inited) {
        InitializeCriticalSection(&g_lock);
        g_lock_inited = TRUE;
    }
    mulaw_table_init();
    if (!g_stop_event) g_stop_event = CreateEventA(NULL, TRUE,  FALSE, NULL);
    if (!g_done_event) g_done_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    audio_register_class_once((HINSTANCE)GetModuleHandleA(NULL));
}

void audio_service_shutdown(void)
{
    audio_service_stop();
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread); g_thread = NULL;
    }
    if (g_stop_event) { CloseHandle(g_stop_event); g_stop_event = NULL; }
    if (g_done_event) { CloseHandle(g_done_event); g_done_event = NULL; }
    if (g_lock_inited) {
        DeleteCriticalSection(&g_lock);
        g_lock_inited = FALSE;
    }
    if (g_class_registered) {
        UnregisterClassA(AUDIO_STATUS_CLASS, (HINSTANCE)GetModuleHandleA(NULL));
        g_class_registered = FALSE;
    }
}

int audio_service_is_playing(void)
{
    int v;
    if (!g_lock_inited) return 0;
    EnterCriticalSection(&g_lock);
    v = g_playing ? 1 : 0;
    LeaveCriticalSection(&g_lock);
    return v;
}

void audio_service_stop(void)
{
    HANDLE thr;
    HWND   hwnd;
    if (!g_lock_inited) return;
    EnterCriticalSection(&g_lock);
    if (!g_playing) { LeaveCriticalSection(&g_lock); return; }
    if (g_stop_event) SetEvent(g_stop_event);
    thr  = g_thread;
    hwnd = g_status_hwnd;
    LeaveCriticalSection(&g_lock);
    /* If the worker is mid-waveOut, give it a nudge: reset cancels the
     * playback, the callback fires WOM_DONE, and the worker unblocks. */
    if (g_hwo) waveOutReset(g_hwo);
    if (thr) {
        if (WaitForSingleObject(thr, 5000) == WAIT_TIMEOUT) {
            /* Worker is wedged; let it leak rather than corrupt state. */
        } else {
            CloseHandle(thr);
            EnterCriticalSection(&g_lock);
            g_thread = NULL;
            LeaveCriticalSection(&g_lock);
        }
    }
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
}

static int audio_start_common(HWND owner)
{
    /* Caller has already populated g_play_url or g_in_buf. */
    if (!g_lock_inited) audio_service_init();

    EnterCriticalSection(&g_lock);
    if (g_playing) {
        LeaveCriticalSection(&g_lock);
        /* Refuse: one playback at a time. The caller can ask the user
         * to press Stop and try again. */
        return 1;
    }
    g_playing = TRUE;
    LeaveCriticalSection(&g_lock);

    /* Reset events for the new run. */
    if (g_stop_event) ResetEvent(g_stop_event);
    if (g_done_event) ResetEvent(g_done_event);

    g_status_hwnd = audio_create_status_window(owner);
    g_thread = CreateThread(NULL, 0, audio_worker, NULL, 0, NULL);
    if (!g_thread) {
        if (g_status_hwnd) { DestroyWindow(g_status_hwnd); g_status_hwnd = NULL; }
        EnterCriticalSection(&g_lock);
        g_playing = FALSE;
        LeaveCriticalSection(&g_lock);
        return 2;
    }
    return 0;
}

int audio_service_play_url(HWND owner, const char *url)
{
    size_t n;
    if (!url || !url[0]) return 3;
    if (!g_lock_inited) audio_service_init();

    EnterCriticalSection(&g_lock);
    if (g_playing) { LeaveCriticalSection(&g_lock); return 1; }
    LeaveCriticalSection(&g_lock);

    n = strlen(url);
    g_play_url = (char *)malloc(n + 1);
    if (!g_play_url) return 4;
    memcpy(g_play_url, url, n + 1);

    g_play_name = NULL;
    g_in_buf = NULL; g_in_len = 0;
    g_in_buf_is_caller_provided = FALSE;
    return audio_start_common(owner);
}

int audio_service_play_bytes_named(HWND owner, const void *buf, size_t len,
                                   const char *name)
{
    AudioFormat fmt;

    if (!buf || len == 0) return 3;
    if (!g_lock_inited) audio_service_init();

    fmt = audio_format_detect((const unsigned char *)buf, len);
    if (fmt == AUDIO_FORMAT_UNKNOWN) {
        _snprintf(g_err_msg, sizeof(g_err_msg),
                  "Audio buffer is neither Sun .au nor MP3.");
        return 5;
    }

    EnterCriticalSection(&g_lock);
    if (g_playing) { LeaveCriticalSection(&g_lock); return 1; }
    LeaveCriticalSection(&g_lock);

    g_play_url = NULL;
    /* Optional "Filename:" hint (e.g. the gopher selector). Heap-copied so
     * the worker can derive the basename; freed at worker exit. */
    g_play_name = NULL;
    if (name && name[0]) {
        size_t nn = strlen(name) + 1;
        g_play_name = (char *)malloc(nn);
        if (g_play_name) memcpy(g_play_name, name, nn);
    }
    g_in_buf   = (unsigned char *)malloc(len);
    if (!g_in_buf) { if (g_play_name) { free(g_play_name); g_play_name = NULL; } return 4; }
    memcpy(g_in_buf, buf, len);
    g_in_len   = (int)len;
    g_in_buf_is_caller_provided = FALSE;   /* we copied; we own it now */
    g_format   = fmt;
    return audio_start_common(owner);
}

int audio_service_play_bytes(HWND owner, const void *buf, size_t len)
{
    return audio_service_play_bytes_named(owner, buf, len, NULL);
}

/* Back-compat thin wrapper: the .au-only entry point kept for any peer
 * that still calls it. New code should call audio_service_play_bytes
 * directly so the format-detect path handles MP3 too. */
int audio_service_play_au_bytes(HWND owner, const void *buf, size_t len)
{
    return audio_service_play_bytes(owner, buf, len);
}
