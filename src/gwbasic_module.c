/*
 * gwbasic_module.c - the GW-BASIC tab: Microsoft's interpreter, in the tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The program this tab runs is GWBASICA.EXE, embedded as RCDATA, built
 * from Microsoft's GW-BASIC source and T.K. Chia's OEM modules (both MIT)
 * by firmware/gwbasic/mgt_build.sh. The machine it runs on is
 * src/gwb_machine.c over libx86emu.
 *
 * ------------------------------------------------------------------
 * SHAPE
 * ------------------------------------------------------------------
 *
 *     worker thread                       UI thread
 *     -------------                       ---------
 *     gwb_run_tick, 18.2 a second         WM_KEYDOWN -> gwb_push_key
 *     gwb_snapshot into g_snap            WM_PAINT: lock, draw g_snap
 *       (lock), invalidate if changed       into the DIB, unlock, blit
 *     feeds pasted text as keys           blink timer, 133 ms
 *
 * The NABU tab's pattern: one thread owns the machine, the UI thread only
 * draws and posts keys, and they meet at one critical section.
 *
 * The machine is NOT stopped when the tab is left, only paused, so a
 * program typed in survives a visit to another tab, the way every other
 * tab keeps its state. Leaving the tab stops the clock; coming back
 * resumes it.
 *
 * ------------------------------------------------------------------
 * THE SCREEN
 * ------------------------------------------------------------------
 *
 * The picture is always 4:3, as a CGA filled a 4:3 monitor and as the
 * RIPscrip tab draws: the largest 4:3 rectangle that fits the tab,
 * centred, letterboxed in the CGA border colour (gb_fit43).
 *
 * Text modes fill that rectangle: the 80x25 or 40x25 grid takes the
 * largest whole-pixel cell that fits, and glyphs are drawn in the Terminal
 * tab's TrueType face (Cascadia Mono, then Consolas) at its own
 * proportions, which renders evenly at any size. Box drawing, shades,
 * blocks and anything the face lacks come from the "Modern DOS" 8x16
 * bitmap (Jayvee Enaguas, CC0; src/gwb_font8x16.h) over the whole cell, so
 * frames join up. See gb_render_text_full.
 *
 * The CGA graphics modes are drawn into a 640x400 DIB and stretched into
 * the same rectangle, nearest neighbour.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suite_shell.h"
#include "suite_res.h"
#include "gwbasic_module.h"
#include "gwb_machine.h"
#include "gwb_font8x16.h"

#define GB_SCREEN_CLASS "MGTGwBasicScreen"
#define GB_STRIP_H      40
#define GB_DIB_W        640
#define GB_DIB_H        400

#define IDC_GB_RESTART  0x7701
#define IDC_GB_PASTE    0x7702
#define IDC_GB_FOLDER   0x7703
#define IDC_GB_LBL      0x7704
#define IDC_GB_SCREEN   0x7705

#define ID_TIMER_BLINK  0x7710
#define WM_GB_STATUS    (WM_APP + 1)
#define WM_GB_EXITED    (WM_APP + 2)

/* controls */
static HWND g_hContent, g_hRestart, g_hPaste, g_hFolderBtn, g_hFolderLbl, g_hScreen;
static int  g_controls_created, g_class_reg;

/* machine and thread */
static CRITICAL_SECTION g_lock;
static int              g_lock_init;
static gwb_machine     *g_m;
static HANDLE           g_thread, g_stop, g_wake;
static volatile LONG    g_paused = 1;
static volatile LONG    g_exited;

/* what the renderer reads (under g_lock) */
static gwb_screen g_snap, g_last;
static int        g_have_snap;

/* the DIB */
static HBITMAP  g_hDib, g_hOldDib;
static HDC      g_hMemDC;
static uint32_t *g_bits;

/* blink: cursor toggles every 133 ms, blinking text every 267 ms, which
 * is what a CGA's 16- and 32-frame counters give at 60 Hz */
static int g_blink_phase;

/* paste (under g_lock) */
static char  *g_paste;
static size_t g_paste_len, g_paste_pos;

static wchar_t g_folder[MAX_PATH];
static char    g_status[200];

static const uint32_t CGA[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

static void gb_status(const char *s)
{
    if (g_lock_init) EnterCriticalSection(&g_lock);
    snprintf(g_status, sizeof g_status, "%s", s);
    if (g_lock_init) LeaveCriticalSection(&g_lock);
    if (g_hScreen) PostMessageA(g_hScreen, WM_GB_STATUS, 0, 0);
}

/* ------------------------------------------------------------------ */
/* The BASIC folder                                                    */
/* ------------------------------------------------------------------ */

static int gb_write_resource_once(const wchar_t *dir, const wchar_t *name, int res_id)
{
    wchar_t path[MAX_PATH];
    HRSRC r;
    HGLOBAL g;
    DWORD n, wrote = 0;
    HANDLE h;

    _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, name);
    path[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 0;   /* never overwrite */
    r = FindResourceA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(res_id), (LPCSTR)RT_RCDATA);
    if (!r || !(g = LoadResource(GetModuleHandleA(NULL), r))) return -1;
    n = SizeofResource(GetModuleHandleA(NULL), r);
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    WriteFile(h, LockResource(g), n, &wrote, NULL);
    CloseHandle(h);
    return wrote == n ? 1 : -1;
}

/* Documents\MGT BASIC, created on first use and seeded with the two
 * listings printed in the Gazette. Documents is asked of the shell rather
 * than built from %USERPROFILE%, so a Documents folder redirected to
 * OneDrive or another drive is still the one used. */
static int gb_prepare_folder(void)
{
    PWSTR docs = NULL;

    if (g_folder[0]) return 1;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs)) || !docs) {
        const wchar_t *up = _wgetenv(L"USERPROFILE");
        if (!up) return 0;
        _snwprintf(g_folder, MAX_PATH, L"%ls\\Documents\\MGT BASIC", up);
    } else {
        _snwprintf(g_folder, MAX_PATH, L"%ls\\MGT BASIC", docs);
        CoTaskMemFree(docs);
    }
    g_folder[MAX_PATH - 1] = 0;
    if (!CreateDirectoryW(g_folder, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        g_folder[0] = 0;
        return 0;
    }
    gb_write_resource_once(g_folder, L"RETRO649.BAS", IDR_GWB_RETRO649);
    gb_write_resource_once(g_folder, L"DIALUP.BAS", IDR_GWB_DIALUP);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The machine and its thread                                          */
/* ------------------------------------------------------------------ */

static void gb_log(void *ctx, const char *line)
{
    (void)ctx;
    OutputDebugStringA("[gwbasic] ");
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}

static gwb_machine *gb_boot(char *err, size_t errn)
{
    HRSRC r = FindResourceA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDR_GWB_EXE), (LPCSTR)RT_RCDATA);
    HGLOBAL g;
    gwb_machine *m;

    if (!r || !(g = LoadResource(GetModuleHandleA(NULL), r))) {
        snprintf(err, errn, "GWBASICA.EXE is missing from this build");
        return NULL;
    }
    m = gwb_create(LockResource(g), SizeofResource(GetModuleHandleA(NULL), r), g_folder, err, errn);
    if (m) gwb_set_log(m, gb_log, NULL);
    return m;
}

/* Turn one pasted character into BIOS keystrokes. Typographic quotes
 * and dashes, which a listing copied from a web page or a PDF of the
 * newspaper is full of, become the ASCII the interpreter expects. */
static int gb_paste_key(unsigned char c, uint16_t *k)
{
    switch (c) {
    case '\r': *k = 0x1C0D; return 1;
    case '\n': *k = 0x1C0D; return 1;
    case '\t': *k = 0x0F09; return 1;
    }
    if (c < 32 || c >= 127) return 0;
    *k = (uint16_t)((MapVirtualKeyA((UINT)(VkKeyScanA((CHAR)c) & 0xFF), MAPVK_VK_TO_VSC) << 8) | c);
    return 1;
}

static void gb_feed_paste(void)
{
    int room = gwb_key_room(g_m), done = 0;

    EnterCriticalSection(&g_lock);
    while (g_paste && g_paste_pos < g_paste_len && room > 8) {
        unsigned char c = (unsigned char)g_paste[g_paste_pos++];
        uint16_t k;
        if (c == '\r' && g_paste_pos < g_paste_len && g_paste[g_paste_pos] == '\n') continue;
        if (gb_paste_key(c, &k)) { gwb_push_key(g_m, k); room--; }
    }
    if (g_paste && g_paste_pos >= g_paste_len) {
        free(g_paste);
        g_paste = NULL;
        g_paste_len = g_paste_pos = 0;
        done = 1;
    }
    LeaveCriticalSection(&g_lock);
    if (done) gb_status("GW-BASIC: paste typed in.");
}

/* ------------------------------------------------------------------ */
/* The PC speaker, through the NABU tab's waveOut pattern               */
/* ------------------------------------------------------------------ */

/*
 * The machine renders each tick's speaker as PCM (gwb_audio); the worker
 * plays it. Same device and pattern as the NABU tab: plain PCM, a ring
 * of headers, one tick per buffer, and DROP when every buffer is still
 * busy, so latency cannot creep. Every waveOut call is made on the
 * worker thread. When the machine pauses, ends or is restarted the
 * device is RESET (gb_audio_stop: a held note stops at once, so the tab
 * is silent when it is not on screen) but stays open; it is closed only
 * when the worker ends. Opening and closing it on every visit made an
 * audible pop on the operator's speakers each time (2026-10-20).
 */
#define GB_NWBUF 8
#define GB_WBUF_SAMP 4096

static HWAVEOUT g_wave;
static WAVEHDR  g_whdr[GB_NWBUF];
static short    g_wbuf[GB_NWBUF][GB_WBUF_SAMP];
static int      g_wave_prime;   /* queue a tick of silence before the next */

static void gb_audio_push(const short *pcm, int nsamp);

/* Test hook (with MGT_GWB_SCREEN_DUMP, as below): device opens and closes,
 * and every non-silent buffer handed to waveOut with its peak and result,
 * appended to "<dump>.audio". */
static void gb_test_audiolog(const char *what, int peak, int rc)
{
    char path[MAX_PATH + 8];
    DWORD n = GetEnvironmentVariableA("MGT_GWB_SCREEN_DUMP", path, MAX_PATH);
    FILE *f;
    if (n == 0 || n >= MAX_PATH) return;
    strcat(path, ".audio");
    if (!(f = fopen(path, "ab"))) return;
    fprintf(f, "%lu %s peak=%d rc=%d\n", (unsigned long)GetTickCount(), what, peak, rc);
    fclose(f);
}

static void gb_audio_open(void)
{
    WAVEFORMATEX wf;

    if (g_wave) return;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = GWB_AUDIO_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 2;
    wf.nAvgBytesPerSec = GWB_AUDIO_RATE * 2;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wave = NULL;
        gb_test_audiolog("open-failed", 0, 1);
        return;
    }
    gb_test_audiolog("open", 0, 0);
    memset(g_whdr, 0, sizeof g_whdr);
    g_wave_prime = 1;
}

/* Stop whatever is playing, keep the device. */
static void gb_audio_stop(void)
{
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (i = 0; i < GB_NWBUF; i++)
        if (g_whdr[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_wave, &g_whdr[i], sizeof(WAVEHDR));
    memset(g_whdr, 0, sizeof g_whdr);
    g_wave_prime = 1;
    gb_test_audiolog("stop", 0, 0);
}

static void gb_audio_close(void)
{
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (i = 0; i < GB_NWBUF; i++)
        if (g_whdr[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_wave, &g_whdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    g_wave = NULL;
    memset(g_whdr, 0, sizeof g_whdr);
    gb_test_audiolog("close", 0, 0);
}

static void gb_audio_push(const short *pcm, int nsamp)
{
    int i;
    if (!g_wave || nsamp <= 0) return;
    if (nsamp > GB_WBUF_SAMP) nsamp = GB_WBUF_SAMP;
    for (i = 0; i < GB_NWBUF; i++) {
        WAVEHDR *h = &g_whdr[i];
        if (h->dwFlags == 0 || (h->dwFlags & WHDR_DONE)) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wave, h, sizeof(WAVEHDR));
            memcpy(g_wbuf[i], pcm, (size_t)nsamp * sizeof(short));
            memset(h, 0, sizeof *h);
            h->lpData = (LPSTR)g_wbuf[i];
            h->dwBufferLength = (DWORD)nsamp * sizeof(short);
            {
                int rc = -1, k, peak = 0;
                if (waveOutPrepareHeader(g_wave, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                    rc = (int)waveOutWrite(g_wave, h, sizeof(WAVEHDR));
                for (k = 0; k < nsamp; k++) {
                    int a = pcm[k] < 0 ? -pcm[k] : pcm[k];
                    if (a > peak) peak = a;
                }
                if (peak) gb_test_audiolog("write", peak, rc);
            }
            return;
        }
    }
    /* every buffer still playing: drop this tick's worth */
}

/* Test hook, off unless MGT_GWB_SCREEN_DUMP names a file: the text
 * screen is written there whenever it changes, so a verification
 * harness can check what the interpreter printed exactly, rather than
 * reading it back out of a screenshot. Nothing else reads it. */
static void gb_test_dump(const gwb_screen *s)
{
    static int checked;
    static char path[MAX_PATH];
    char text[25 * 81 + 1];
    FILE *f;

    if (!checked) {
        DWORD n = GetEnvironmentVariableA("MGT_GWB_SCREEN_DUMP", path, sizeof path);
        if (n == 0 || n >= sizeof path) path[0] = 0;
        checked = 1;
    }
    if (!path[0] || !gwb_screen_text(s, text)) return;
    f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

/* Second test hook, also off unless MGT_GWB_SCREEN_DUMP is set: every
 * key the screen window receives is appended to "<dump>.keys", so a
 * harness can tell a keystroke that never reached the tab from one the
 * machine lost. */
static void gb_test_keylog(int vk, uint16_t k)
{
    char path[MAX_PATH + 8];
    DWORD n = GetEnvironmentVariableA("MGT_GWB_SCREEN_DUMP", path, MAX_PATH);
    FILE *f;

    if (n == 0 || n >= MAX_PATH) return;
    strcat(path, ".keys");
    f = fopen(path, "ab");
    if (!f) return;
    fprintf(f, "%lu vk=%02X key=%04X\n", (unsigned long)GetTickCount(), vk, k);
    fclose(f);
}

static DWORD WINAPI gb_worker(LPVOID param)
{
    LARGE_INTEGER freq, now;
    double next = 0, period;
    (void)param;

    QueryPerformanceFrequency(&freq);
    period = (double)freq.QuadPart / GWB_TICKS_PER_SEC;

    for (;;) {
        HANDLE hs[2] = { g_stop, g_wake };
        unsigned r;
        DWORD wait;

        if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) break;
        if (InterlockedCompareExchange(&g_paused, 0, 0) || InterlockedCompareExchange(&g_exited, 0, 0)) {
            gb_audio_stop();            /* silent while hidden or ended */
            if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0) break;
            next = 0;
            continue;
        }

        gb_feed_paste();
        r = gwb_run_tick(g_m);
        {
            const int16_t *pcm;
            int n = gwb_audio(g_m, &pcm);
            if (!g_wave) gb_audio_open();
            if (g_wave_prime) {
                /* one tick of silence first, so a late tick does not
                 * starve the device and click */
                /* a full buffer: a tick is ~2422 samples, and a shorter
                 * array played what followed it as noise (2026-10-20) */
                static const short silence[GB_WBUF_SAMP];
                gb_audio_push(silence, (int)(GWB_AUDIO_RATE / GWB_TICKS_PER_SEC));
                g_wave_prime = 0;
            }
            gb_audio_push(pcm, n);
        }

        EnterCriticalSection(&g_lock);
        gwb_snapshot(g_m, &g_snap);
        g_have_snap = 1;
        if (memcmp(&g_snap, &g_last, sizeof g_snap)) {
            g_last = g_snap;
            if (g_hScreen) InvalidateRect(g_hScreen, NULL, FALSE);
            gb_test_dump(&g_snap);
        }
        LeaveCriticalSection(&g_lock);

        if (r & GWB_TICK_EXITED) {
            InterlockedExchange(&g_exited, 1);
            if (g_hScreen) PostMessageA(g_hScreen, WM_GB_EXITED, 0, 0);
            continue;
        }

        /* real time: one tick every 54.9 ms, on a deadline so the
         * average is exact even though Sleep is coarse */
        QueryPerformanceCounter(&now);
        if (next == 0 || (double)now.QuadPart > next + period * 10) next = (double)now.QuadPart;
        next += period;
        wait = (double)now.QuadPart < next
             ? (DWORD)((next - (double)now.QuadPart) * 1000.0 / (double)freq.QuadPart) : 0;
        if (wait && WaitForSingleObject(g_stop, wait) == WAIT_OBJECT_0) break;
    }
    gb_audio_close();
    return 0;
}

static void gb_stop_thread(void)
{
    if (!g_thread) return;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    g_thread = NULL;
    ResetEvent(g_stop);
}

static void gb_start(void)
{
    char err[160], msg[300];

    if (!gb_prepare_folder()) {
        gb_status("GW-BASIC: could not create Documents\\MGT BASIC");
        return;
    }
    if (!g_m) {
        g_m = gb_boot(err, sizeof err);
        if (!g_m) {
            snprintf(msg, sizeof msg, "GW-BASIC could not start: %s", err);
            gb_status(msg);
            return;
        }
        InterlockedExchange(&g_exited, 0);
    }
    if (!g_stop) g_stop = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_wake) g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_thread) g_thread = CreateThread(NULL, 0, gb_worker, NULL, 0, NULL);
}

static void gb_restart(void)
{
    gb_stop_thread();
    EnterCriticalSection(&g_lock);
    if (g_m) { gwb_destroy(g_m); g_m = NULL; }
    free(g_paste);
    g_paste = NULL;
    g_paste_len = g_paste_pos = 0;
    g_have_snap = 0;
    memset(&g_last, 0, sizeof g_last);
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_exited, 0);
    gb_start();
    SetEvent(g_wake);
    gb_status("GW-BASIC restarted. Files are in Documents\\MGT BASIC");
    if (g_hScreen) SetFocus(g_hScreen);
}

/* ------------------------------------------------------------------ */
/* Paste                                                               */
/* ------------------------------------------------------------------ */

static void gb_paste_clipboard(void)
{
    HANDLE h;
    const wchar_t *w;
    char *out;
    size_t n, i, o = 0;

    if (!g_m || !OpenClipboard(g_hScreen)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    w = h ? (const wchar_t *)GlobalLock(h) : NULL;
    if (!w) { CloseClipboard(); return; }
    n = wcslen(w);
    out = malloc(n + 1);
    if (out) {
        for (i = 0; i < n; i++) {
            wchar_t c = w[i];
            if (c == 0x2018 || c == 0x2019 || c == 0x2032) c = '\'';
            else if (c == 0x201C || c == 0x201D || c == 0x2033) c = '"';
            else if (c == 0x2013 || c == 0x2014 || c == 0x2212) c = '-';
            else if (c == 0x00A0) c = ' ';
            if (c < 128) out[o++] = (char)c;
        }
        out[o] = 0;
    }
    GlobalUnlock(h);
    CloseClipboard();
    if (!out) return;

    EnterCriticalSection(&g_lock);
    free(g_paste);
    g_paste = out;
    g_paste_len = o;
    g_paste_pos = 0;
    LeaveCriticalSection(&g_lock);
    gb_status("GW-BASIC: typing the pasted text...");
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

static void gb_update_shift(void)
{
    uint8_t f = 0;
    if (!g_m) return;
    if (GetKeyState(VK_RSHIFT) & 0x8000)  f |= GWB_KF_RSHIFT;
    if (GetKeyState(VK_LSHIFT) & 0x8000)  f |= GWB_KF_LSHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) f |= GWB_KF_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000)    f |= GWB_KF_ALT;
    if (GetKeyState(VK_SCROLL) & 1)       f |= GWB_KF_SCROLL;
    if (GetKeyState(VK_NUMLOCK) & 1)      f |= GWB_KF_NUM;
    if (GetKeyState(VK_CAPITAL) & 1)      f |= GWB_KF_CAPS;
    gwb_set_shift_flags(g_m, f);
}

/* ASCII from the VIRTUAL KEY and the shift state, never from WM_CHAR:
 * WM_CHAR is translated through the active keyboard layout, and on the
 * Greek layout this workstation also runs, T arrives as tau. A PC's
 * BIOS speaks US ASCII. Same rule as the Terminal and NABU tabs. */
static int gb_vk_ascii(int vk, int shift, int ctrl, int caps)
{
    static const char *shiftnums = ")!@#$%^&*(";

    if (vk >= 'A' && vk <= 'Z') {
        if (ctrl) return vk & 0x1F;
        return (shift != caps) ? vk : vk + 32;
    }
    if (vk >= '0' && vk <= '9') {
        if (ctrl) return 0;
        return shift ? shiftnums[vk - '0'] : vk;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_3:      return shift ? '~' : '`';
    case VK_OEM_MINUS:  return ctrl ? 0x1F : (shift ? '_' : '-');
    case VK_OEM_PLUS:   return shift ? '+' : '=';
    case VK_OEM_4:      return ctrl ? 0x1B : (shift ? '{' : '[');
    case VK_OEM_6:      return ctrl ? 0x1D : (shift ? '}' : ']');
    case VK_OEM_5:      return ctrl ? 0x1C : (shift ? '|' : '\\');
    case VK_OEM_1:      return shift ? ':' : ';';
    case VK_OEM_7:      return shift ? '"' : '\'';
    case VK_OEM_COMMA:  return shift ? '<' : ',';
    case VK_OEM_PERIOD: return shift ? '>' : '.';
    case VK_OEM_2:      return shift ? '?' : '/';
    case VK_DIVIDE:     return '/';
    case VK_MULTIPLY:   return '*';
    case VK_SUBTRACT:   return '-';
    case VK_ADD:        return '+';
    case VK_DECIMAL:    return '.';
    }
    return 0;
}

/* A BIOS keystroke word (scan << 8 | ASCII) for a key going down, or 0. */
static uint16_t gb_key_word(int vk, LPARAM lp)
{
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int alt   = (GetKeyState(VK_MENU) & 0x8000) != 0;
    int caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
    unsigned scan = (unsigned)((lp >> 16) & 0xFF);
    int ext = (lp >> 24) & 1, a;

    /* function keys: F1..F10 3Bh..44h, then the shifted banks */
    if (vk >= VK_F1 && vk <= VK_F10) {
        unsigned n = (unsigned)(vk - VK_F1);
        if (alt)   return (uint16_t)((0x68 + n) << 8);
        if (ctrl)  return (uint16_t)((0x5E + n) << 8);
        if (shift) return (uint16_t)((0x54 + n) << 8);
        return (uint16_t)((0x3B + n) << 8);
    }
    switch (vk) {
    case VK_RETURN: return ctrl ? 0x1C0A : 0x1C0D;
    case VK_ESCAPE: return 0x011B;
    case VK_BACK:   return ctrl ? 0x0E7F : 0x0E08;
    case VK_TAB:    return shift ? 0x0F00 : 0x0F09;
    case VK_UP:     return 0x4800;
    case VK_DOWN:   return 0x5000;
    case VK_LEFT:   return ctrl ? 0x7300 : 0x4B00;
    case VK_RIGHT:  return ctrl ? 0x7400 : 0x4D00;
    case VK_HOME:   return ctrl ? 0x7700 : 0x4700;
    case VK_END:    return ctrl ? 0x7500 : 0x4F00;
    case VK_PRIOR:  return ctrl ? 0x8400 : 0x4900;
    case VK_NEXT:   return ctrl ? 0x7600 : 0x5100;
    case VK_INSERT: return 0x5200;
    case VK_DELETE: return 0x5300;
    }
    /* with NumLock off the keypad is a cursor pad, as on a PC */
    if (!ext && vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 && !(GetKeyState(VK_NUMLOCK) & 1))
        return 0;

    a = gb_vk_ascii(vk, shift, ctrl, caps);
    if (alt && vk >= 'A' && vk <= 'Z') return (uint16_t)(scan << 8);  /* Alt+letter: GW-BASIC's keyword keys */
    if (alt && vk >= '0' && vk <= '9') return (uint16_t)((0x78 + (vk == '0' ? 9 : vk - '1')) << 8);
    if (ctrl && vk == '2') return 0x0300;           /* Ctrl-@: NUL */
    if (!a) return 0;
    return (uint16_t)((scan << 8) | (uint8_t)a);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void gb_dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_hDib) return;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = GB_DIB_W;
    bi.bmiHeader.biHeight = -GB_DIB_H;            /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    g_hMemDC = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (g_hDib && g_hMemDC) g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
}

static void gb_dib_destroy(void)
{
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; }
    g_bits = NULL;
}

/* ------------------------------------------------------------------ */
/* Text mode: the Terminal tab's font, filling the tab                  */
/* ------------------------------------------------------------------ */

/*
 * TEXT FILLS THE 4:3 PICTURE (operator, 2026-10-20). A bitmap font can
 * only be scaled cleanly by whole numbers, and on a 1080-line screen that
 * leaves 80x25 at 640x400 in a much larger hole. So text is drawn the way
 * the Terminal tab draws it: in Cascadia Mono (then Consolas, then any
 * fixed pitch), a hinted TrueType face that renders evenly at any size.
 * The 80x25 (or 40x25) grid takes the largest whole-pixel cell that fits
 * the 4:3 rectangle, and the font is the largest whose glyphs fit that
 * cell, at the face's own proportions (never widened or condensed).
 *
 * Two kinds of character are still drawn from the CP437 8x16 bitmap,
 * stretched to the whole cell:
 *
 *   - box drawing, shades and blocks (B0h..DFh). A font's line drawing
 *     stops at its own line height, which is shorter than the cell, so
 *     frames and the reverse-video F-key row would show gaps between
 *     rows. The bitmap fills the cell edge to edge and joins up.
 *   - anything the chosen font lacks, so all 256 code points always show.
 *
 * The graphics modes (SCREEN 1, 2) are pixels, not text, and still go
 * through the 640x400 DIB at a whole-number scale (gb_render_graphics).
 */

/* IBM's pictures for the control range, as tools/font_ttf_to_cp437.py
 * uses them */
static const wchar_t CP437_LOW[32] = {
    0x0020, 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022,
    0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640, 0x266A, 0x266B, 0x263C,
    0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8,
    0x2191, 0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC };

static wchar_t  g_uni[256];             /* CP437 -> Unicode */
static uint8_t  g_from_font[256];       /* 1: draw with the TrueType face */
static HFONT    g_tfont;
static int      g_tfont_cw, g_tfont_ch; /* the cell it was made for */
static int      g_tglyph_w, g_tglyph_h; /* its glyph box, for centring */
static HBITMAP  g_hCli, g_hCliOld;      /* client-sized DIB */
static HDC      g_hCliDC;
static uint32_t *g_cli;
static int      g_cli_w, g_cli_h;

static void gb_cp437_table(void)
{
    static int done;
    char b[256];
    int i;

    if (done) return;
    for (i = 0; i < 256; i++) b[i] = (char)i;
    MultiByteToWideChar(437, 0, b, 256, g_uni, 256);
    for (i = 0; i < 32; i++) g_uni[i] = CP437_LOW[i];
    g_uni[0x7F] = 0x2302;
    g_uni[0xFF] = 0x0020;
    done = 1;
}

static HFONT gb_make_font(int h)
{
    HFONT f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Cascadia Mono");
    if (!f) f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    if (!f) f = CreateFontA(-h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, NULL);
    return f;
}

/* THE PICTURE IS 4:3 (operator, 2026-10-20), as a CGA filled a 4:3
 * monitor and as the RIPscrip tab draws its EGA screen: the largest 4:3
 * rectangle that fits the tab, centred, never stretched to the tab's own
 * shape. Same arithmetic as ripscrip_module.c's fit. */
static void gb_fit43(int cw, int ch, int *x, int *y, int *w, int *h)
{
    int dw = cw, dh = (int)((long)cw * 3 / 4);
    if (dh > ch) { dh = ch; dw = (int)((long)ch * 4 / 3); }
    *x = (cw - dw) / 2;
    *y = (ch - dh) / 2;
    *w = dw;
    *h = dh;
}

/* The largest face whose glyphs fit a cw x ch cell. Rebuilt only when the
 * cell size changes, which is only on a resize. */
static void gb_text_font(HDC dc, int cw, int ch)
{
    int h;
    TEXTMETRICW tm;

    if (g_tfont && g_tfont_cw == cw && g_tfont_ch == ch) return;
    if (g_tfont) { DeleteObject(g_tfont); g_tfont = NULL; }
    gb_cp437_table();

    for (h = ch; h >= 6; h--) {
        HFONT f = gb_make_font(h), old;
        if (!f) continue;
        old = (HFONT)SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        if (tm.tmAveCharWidth <= cw && tm.tmHeight <= ch) {
            WORD idx[256];
            int i;
            /* The face keeps its own proportions: no widening or
             * condensing to the cell (operator, 2026-10-20: do not stretch
             * or distort). It is centred in the cell. */
            g_tfont = f;
            g_tglyph_w = tm.tmAveCharWidth;
            g_tglyph_h = tm.tmHeight;
            old = (HFONT)SelectObject(dc, f);
            GetGlyphIndicesW(dc, g_uni, 256, idx, GGI_MARK_NONEXISTING_GLYPHS);
            SelectObject(dc, old);
            for (i = 0; i < 256; i++)
                g_from_font[i] = idx[i] != 0xFFFF && !(i >= 0xB0 && i <= 0xDF);
            break;
        }
        DeleteObject(f);
    }
    g_tfont_cw = cw;
    g_tfont_ch = ch;
}

static int gb_cli_dib(HDC ref, int w, int h)
{
    BITMAPINFO bi;

    if (g_hCli && g_cli_w == w && g_cli_h == h) return 1;
    if (g_hCliDC) {
        if (g_hCliOld) SelectObject(g_hCliDC, g_hCliOld);
        DeleteDC(g_hCliDC);
        g_hCliDC = NULL;
    }
    if (g_hCli) { DeleteObject(g_hCli); g_hCli = NULL; }
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_hCli = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, (void **)&g_cli, NULL, 0);
    if (!g_hCli) return 0;
    g_hCliDC = CreateCompatibleDC(ref);
    g_hCliOld = (HBITMAP)SelectObject(g_hCliDC, g_hCli);
    g_cli_w = w;
    g_cli_h = h;
    return 1;
}

static void gb_fill(int x0, int y0, int w, int h, uint32_t c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++) {
        uint32_t *p = g_cli + y * g_cli_w + x0;
        for (x = 0; x < w; x++) p[x] = c;
    }
}

/* the 8x16 bitmap glyph stretched over a whole cell, nearest neighbour */
static void gb_bitmap_cell(int x0, int y0, int cw, int ch, uint8_t c, uint32_t fg)
{
    int x, y;
    for (y = 0; y < ch; y++) {
        uint8_t bits = gwb_font8x16[c][y * 16 / ch];
        uint32_t *p = g_cli + (y0 + y) * g_cli_w + x0;
        if (!bits) continue;
        for (x = 0; x < cw; x++)
            if ((bits >> (7 - x * 8 / cw)) & 1) p[x] = fg;
    }
}

static COLORREF gb_cref(uint32_t c)
{
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

/* Draw the text screen into the client DIB. Returns 0 if it could not.
 *
 * The grid IS the 4:3 rectangle: column c starts at bx + c * bw / cols and
 * row r at by + r * bh / 25, so cells differ by at most one pixel and the
 * grid is exactly 4:3 and fills the rectangle at every size. (Uniform
 * whole-pixel cells cannot do both: at a 7-pixel cell, flooring loses a
 * tenth of the width.) Each glyph is still drawn whole by the TrueType
 * face, so a one-pixel difference in a cell's width is a one-pixel
 * difference in spacing, never in a stroke. */
static int gb_render_text_full(HDC ref, const gwb_screen *s, int w, int h)
{
    unsigned cols = s->cols == 40 ? 40 : 80, r, c;
    int bx, by, bw, bh, mincw, minch;
    int xs[81], ys[26];
    int blink_enabled = (s->cga_mode & 0x20) != 0;
    int text_on = (g_blink_phase & 2) == 0, cursor_on = (g_blink_phase & 1) == 0;
    INT dx[80];
    HFONT oldf;

    if (!gb_cli_dib(ref, w, h)) return 0;
    gb_fit43(w, h, &bx, &by, &bw, &bh);
    for (c = 0; c <= cols; c++) xs[c] = bx + (int)((long)c * bw / (long)cols);
    for (r = 0; r <= 25; r++) ys[r] = by + (int)((long)r * bh / 25);
    mincw = bw / (int)cols;
    minch = bh / 25;
    if (mincw < 4 || minch < 8) return 0;

    gb_fill(0, 0, w, h, CGA[s->cga_color & 15]);        /* border */
    gb_text_font(g_hCliDC, mincw, minch);
    if (!g_tfont) return 0;

    /* backgrounds, bitmap glyphs and the cursor go straight into the bits */
    for (r = 0; r < 25; r++) {
        for (c = 0; c < cols; c++) {
            unsigned off = (s->page_start + (r * cols + c) * 2) & (GWB_VRAM_BYTES - 1);
            uint8_t chr = s->vram[off], at = s->vram[off + 1];
            uint32_t bg = CGA[blink_enabled ? (at >> 4) & 7 : at >> 4];
            int cw = xs[c + 1] - xs[c], ch = ys[r + 1] - ys[r];
            int hide = blink_enabled && (at & 0x80) && !text_on;
            gb_fill(xs[c], ys[r], cw, ch, bg);
            if (!hide && !g_from_font[chr]) gb_bitmap_cell(xs[c], ys[r], cw, ch, chr, CGA[at & 15]);
        }
    }

    /* the TrueType glyphs, one ExtTextOutW per run of one colour; each
     * glyph is centred in its own cell through the per-character advances */
    GdiFlush();
    oldf = (HFONT)SelectObject(g_hCliDC, g_tfont);
    SetBkMode(g_hCliDC, TRANSPARENT);
    SetTextAlign(g_hCliDC, TA_TOP | TA_LEFT);
    for (r = 0; r < 25; r++) {
        unsigned start = 0;
        int ty = ys[r] + (ys[r + 1] - ys[r] - g_tglyph_h) / 2;
        while (start < cols) {
            wchar_t run[80];
            unsigned n = 0, k;
            uint32_t fg = 0;
            int first = 1;
            for (k = start; k < cols; k++) {
                unsigned off = (s->page_start + (r * cols + k) * 2) & (GWB_VRAM_BYTES - 1);
                uint8_t chr = s->vram[off], at = s->vram[off + 1];
                int hide = blink_enabled && (at & 0x80) && !text_on;
                uint32_t f = CGA[at & 15];
                if (!first && f != fg) break;
                fg = f;
                first = 0;
                run[n++] = (hide || !g_from_font[chr]) ? L' ' : g_uni[chr];
            }
            /* advance from the centre of one cell to the centre of the next */
            for (k = 0; k < n; k++) {
                unsigned a = start + k;
                int c0 = (xs[a] + xs[a + 1]) / 2;
                int c1 = a + 1 < cols ? (xs[a + 1] + xs[a + 2]) / 2 : c0 + mincw;
                dx[k] = c1 - c0;
            }
            SetTextColor(g_hCliDC, gb_cref(fg));
            ExtTextOutW(g_hCliDC, (xs[start] + xs[start + 1]) / 2 - g_tglyph_w / 2, ty,
                        0, NULL, run, n, dx);
            start += n;
        }
    }
    SelectObject(g_hCliDC, oldf);
    GdiFlush();

    /* the cursor: CGA scan lines 0..7 mapped onto the cell's height */
    if (cursor_on && !(s->cur_start & 0x20) && s->cur_row < 25 && s->cur_col < cols) {
        unsigned off = (s->page_start + (s->cur_row * cols + s->cur_col) * 2) & (GWB_VRAM_BYTES - 1);
        uint32_t fg = CGA[s->vram[off + 1] & 15];
        int x0 = xs[s->cur_col], y0 = ys[s->cur_row];
        int cw = xs[s->cur_col + 1] - x0, ch = ys[s->cur_row + 1] - y0;
        int a = (s->cur_start & 7) * ch / 8, b = ((s->cur_end & 7) + 1) * ch / 8;
        if (a < b) gb_fill(x0, y0 + a, cw, b - a, fg);
        else { gb_fill(x0, y0 + a, cw, ch - a, fg); gb_fill(x0, y0, cw, b, fg); }
    }
    return 1;
}


static void gb_text_destroy(void)
{
    if (g_tfont) { DeleteObject(g_tfont); g_tfont = NULL; }
    if (g_hCliDC) {
        if (g_hCliOld) SelectObject(g_hCliDC, g_hCliOld);
        DeleteDC(g_hCliDC);
        g_hCliDC = NULL;
    }
    if (g_hCli) { DeleteObject(g_hCli); g_hCli = NULL; }
    g_cli = NULL;
}


static void gb_render_graphics(const gwb_screen *s)
{
    unsigned x, y;

    if (s->mode == 6) {
        uint32_t fg = CGA[s->cga_color & 15], bg = 0;
        for (y = 0; y < 200; y++) {
            const uint8_t *line = s->vram + (y & 1) * 0x2000 + (y >> 1) * 80;
            uint32_t *d0 = g_bits + (y * 2) * GB_DIB_W, *d1 = d0 + GB_DIB_W;
            for (x = 0; x < 640; x++) d0[x] = d1[x] = ((line[x >> 3] >> (7 - (x & 7))) & 1) ? fg : bg;
        }
    } else {
        uint32_t pal[4];
        int hi = (s->cga_color & 0x10) ? 8 : 0;
        pal[0] = CGA[s->cga_color & 15];
        if (s->mode == 5)             { pal[1] = CGA[3 + hi]; pal[2] = CGA[4 + hi]; pal[3] = CGA[7 + hi]; }
        else if (s->cga_color & 0x20) { pal[1] = CGA[3 + hi]; pal[2] = CGA[5 + hi]; pal[3] = CGA[7 + hi]; }
        else                          { pal[1] = CGA[2 + hi]; pal[2] = CGA[4 + hi]; pal[3] = CGA[6 + hi]; }
        for (y = 0; y < 200; y++) {
            const uint8_t *line = s->vram + (y & 1) * 0x2000 + (y >> 1) * 80;
            uint32_t *d0 = g_bits + (y * 2) * GB_DIB_W, *d1 = d0 + GB_DIB_W;
            for (x = 0; x < 320; x++) {
                uint32_t v = pal[(line[x >> 2] >> ((3 - (x & 3)) * 2)) & 3];
                d0[x * 2] = d0[x * 2 + 1] = d1[x * 2] = d1[x * 2 + 1] = v;
            }
        }
    }
}

static void gb_paint(HWND hwnd, HDC hdc)
{
    RECT rc;
    int cw, ch, dw, dh, dx, dy;
    uint32_t border = 0;
    HDC mem;
    HBITMAP bmp, old;
    HBRUSH br;

    GetClientRect(hwnd, &rc);
    cw = rc.right; ch = rc.bottom;
    if (cw <= 0 || ch <= 0) return;

    /* text modes fill the whole tab with the TrueType face */
    EnterCriticalSection(&g_lock);
    if (g_have_snap && g_snap.mode <= 3 && gb_render_text_full(hdc, &g_snap, cw, ch)) {
        LeaveCriticalSection(&g_lock);
        BitBlt(hdc, 0, 0, cw, ch, g_hCliDC, 0, 0, SRCCOPY);
        return;
    }
    LeaveCriticalSection(&g_lock);

    gb_dib_create();
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    old = (HBITMAP)SelectObject(mem, bmp);

    EnterCriticalSection(&g_lock);
    if (g_have_snap && g_bits && g_snap.mode > 3) {
        gb_render_graphics(&g_snap);
        if (g_snap.mode != 6) border = CGA[g_snap.cga_color & 15];
        GdiFlush();
    }
    LeaveCriticalSection(&g_lock);

    /* the CGA border colour around the picture */
    br = CreateSolidBrush(RGB((border >> 16) & 0xFF, (border >> 8) & 0xFF, border & 0xFF));
    FillRect(mem, &rc, br);
    DeleteObject(br);

    if (g_have_snap && g_hMemDC && g_snap.mode > 3) {
        /* GRAPHICS MODES: the CGA's 640x200 / 320x200 raster filled a 4:3
         * monitor, so it goes into the same largest-4:3 rectangle as the
         * text and as RIPscrip's EGA screen, nearest neighbour (never
         * smoothed), letterboxed in the border colour. (Text modes do not
         * come here; see gb_render_text_full.) */
        gb_fit43(cw, ch, &dx, &dy, &dw, &dh);
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, dx, dy, dw, dh, g_hMemDC, 0, 0, GB_DIB_W, GB_DIB_H, SRCCOPY);
    }
    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

/* ------------------------------------------------------------------ */
/* The screen window                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK GbScreenProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        gb_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    /* A custom-painted child MUST claim every key, or the shell's
     * IsDialogMessage eats letters looking for mnemonics. */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS | DLGC_WANTTAB;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == ID_TIMER_BLINK) {
            g_blink_phase = (g_blink_phase + 1) & 3;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        uint16_t k;
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (!g_m) break;
        gb_update_shift();
        if (InterlockedCompareExchange(&g_exited, 0, 0)) {
            gb_restart();                   /* any key after SYSTEM boots again */
            return 0;
        }
        /* Ctrl+Break (Windows reports VK_CANCEL) and Ctrl+ScrollLock */
        if (wp == VK_CANCEL || (ctrl && wp == VK_SCROLL) || (ctrl && wp == VK_PAUSE)) {
            gwb_ctrl_break(g_m);
            return 0;
        }
        if ((ctrl && wp == 'V') || (shift && wp == VK_INSERT)) {
            gb_paste_clipboard();
            return 0;
        }
        if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_CAPITAL ||
            wp == VK_NUMLOCK || wp == VK_SCROLL || wp == VK_LWIN || wp == VK_RWIN)
            return 0;
        /* Alt+F4 still closes the Suite */
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        k = gb_key_word((int)wp, lp);
        if (k) gwb_push_key(g_m, k);
        gb_test_keylog((int)wp, k);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        gb_update_shift();
        if (msg == WM_SYSKEYUP && wp == VK_MENU) return 0;   /* no menu bar on lone Alt */
        break;
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_SYSDEADCHAR:
    case WM_DEADCHAR:
        return 0;                           /* ASCII came from the virtual key */
    case WM_GB_STATUS: {
        char s[220];
        EnterCriticalSection(&g_lock);
        snprintf(s, sizeof s, "%s", g_status);
        LeaveCriticalSection(&g_lock);
        suite_set_status(s);
        return 0;
    }
    case WM_GB_EXITED:
        gb_status("GW-BASIC ended (SYSTEM). Press any key in the screen to start it again.");
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static void gb_register_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = GbScreenProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_IBEAM);
    wc.lpszClassName = GB_SCREEN_CLASS;
    RegisterClassExA(&wc);
    g_class_reg = 1;
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

static void gb_show(int how)
{
    HWND all[] = { g_hRestart, g_hPaste, g_hFolderBtn, g_hFolderLbl, g_hScreen };
    int i;
    for (i = 0; i < (int)(sizeof all / sizeof all[0]); i++) if (all[i]) ShowWindow(all[i], how);
}

void gwbasic_module_resize(HWND content, int w, int h)
{
    int margin = 6, bw = 96, bh = 28, y = (GB_STRIP_H - bh) / 2, x = margin;
    (void)content;
    if (!g_controls_created) return;
    MoveWindow(g_hRestart, x, y, bw, bh, TRUE);        x += bw + margin;
    MoveWindow(g_hPaste, x, y, bw, bh, TRUE);          x += bw + margin;
    MoveWindow(g_hFolderBtn, x, y, bw + 64, bh, TRUE); x += bw + 64 + margin * 2;
    MoveWindow(g_hFolderLbl, x, y, w - x - margin > 0 ? w - x - margin : 0, bh, TRUE);
    MoveWindow(g_hScreen, margin, GB_STRIP_H, w - 2 * margin,
               h - GB_STRIP_H - margin > 0 ? h - GB_STRIP_H - margin : 0, TRUE);
}

void gwbasic_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    wchar_t lbl[MAX_PATH + 32];

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
    g_hContent = content;
    gb_register_class(hInst);
    gb_prepare_folder();

    if (g_controls_created) {
        gb_show(SW_SHOW);
    } else {
        g_hRestart = CreateWindowA("BUTTON", "Restart", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_GB_RESTART, hInst, NULL);
        g_hPaste = CreateWindowA("BUTTON", "Paste", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_GB_PASTE, hInst, NULL);
        g_hFolderBtn = CreateWindowA("BUTTON", "Open BASIC folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_GB_FOLDER, hInst, NULL);
        g_hFolderLbl = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_GB_LBL, hInst, NULL);
        /* wide, because Documents may sit under a name outside the ANSI
         * code page */
        _snwprintf(lbl, MAX_PATH + 32, L"LOAD and SAVE use %ls", g_folder[0] ? g_folder : L"(folder unavailable)");
        lbl[MAX_PATH + 31] = 0;
        SetWindowTextW(g_hFolderLbl, lbl);
        g_hScreen = CreateWindowExA(WS_EX_CLIENTEDGE, GB_SCREEN_CLASS, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            0, 0, 100, 100, content, (HMENU)(INT_PTR)IDC_GB_SCREEN, hInst, NULL);
        if (g_hFontUI) {
            HWND all[] = { g_hRestart, g_hPaste, g_hFolderBtn, g_hFolderLbl };
            int i;
            for (i = 0; i < 4; i++) SendMessageA(all[i], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }
        g_controls_created = 1;
    }

    GetClientRect(content, &rc);
    gwbasic_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    gb_start();
    InterlockedExchange(&g_paused, 0);
    if (g_wake) SetEvent(g_wake);
    SetTimer(g_hScreen, ID_TIMER_BLINK, 133, NULL);
    if (InterlockedCompareExchange(&g_exited, 0, 0))
        gb_status("GW-BASIC ended (SYSTEM). Press any key in the screen to start it again.");
    else if (g_m)
        gb_status("GW-BASIC. LOAD and SAVE use Documents\\MGT BASIC. Ctrl+V pastes a listing.");
    SetFocus(g_hScreen);
}

void gwbasic_module_deactivate(HWND content)
{
    (void)content;
    /* Paused, not stopped: the program in memory stays for when the
     * reader comes back. */
    InterlockedExchange(&g_paused, 1);
    if (g_hScreen) KillTimer(g_hScreen, ID_TIMER_BLINK);
    if (g_controls_created) gb_show(SW_HIDE);
}

BOOL gwbasic_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam), code = HIWORD(wParam);
    (void)content; (void)lParam;

    if (code != BN_CLICKED) return FALSE;
    switch (id) {
    case IDC_GB_RESTART:
        gb_restart();
        return TRUE;
    case IDC_GB_PASTE:
        gb_paste_clipboard();
        if (g_hScreen) SetFocus(g_hScreen);
        return TRUE;
    case IDC_GB_FOLDER:
        if (gb_prepare_folder())
            ShellExecuteW(NULL, L"open", g_folder, NULL, NULL, SW_SHOWNORMAL);
        if (g_hScreen) SetFocus(g_hScreen);
        return TRUE;
    }
    return FALSE;
}

BOOL gwbasic_module_has_unsaved(void)
{
    return FALSE;
}

void gwbasic_module_shutdown(void)
{
    if (!g_lock_init) return;
    gb_stop_thread();
    if (g_m) { gwb_destroy(g_m); g_m = NULL; }
    if (g_stop) { CloseHandle(g_stop); g_stop = NULL; }
    if (g_wake) { CloseHandle(g_wake); g_wake = NULL; }
    free(g_paste);
    g_paste = NULL;
    gb_dib_destroy();
    gb_text_destroy();
}
