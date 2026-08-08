/*
 * nabu_native_module.c - the NABU's screen, drawn in the tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ------------------------------------------------------------------
 * THE SHAPE OF IT
 * ------------------------------------------------------------------
 *
 * One thread runs the machine. The UI thread draws whatever the last
 * completed frame was. They meet at exactly one place: a 256x192 32-bit
 * DIB section and the critical section that guards it.
 *
 *   emulation thread            UI thread
 *   ----------------            ---------
 *   nabu_core_run_frame()
 *   nabu_core_copy_frame(dib)  <-- lock -->  StretchBlt from the mem DC
 *   post a repaint
 *
 * The machine has no locks of its own and does not need any: the
 * emulation thread is its only caller, and it is the same thread that
 * converts the frame. The lock exists solely so a paint cannot read the
 * DIB while the converter is halfway through writing it.
 *
 * WHY A DIB SECTION AND NOT A BITMAP. CreateDIBSection hands back a
 * pointer to the actual pixels, so the converter writes straight into
 * the memory GDI will blit from. No intermediate buffer, no SetDIBits
 * per frame. This is the same approach the RIPscrip renderer uses, and
 * the reason both are top-down (a negative biHeight) is that the
 * emulator produces its first scanline first, the way anything with a
 * raster does, and a bottom-up DIB would need every frame flipped.
 *
 * WHY THE UI THREAD NEVER TOUCHES THE MACHINE. It would be easy, and
 * wrong, to have the paint handler call nabu_core_copy_frame directly:
 * the machine would then be read by one thread while another was
 * stepping it. The rule is that only the emulation thread ever calls
 * into nabu_core, and the header says so.
 *
 * PACING IS REAL TIME, ALWAYS, WITH NO EXCEPTIONS. The thread paces
 * itself to 60 emulated frames per second of WALL CLOCK, from the first
 * frame to the last. One emulated frame is 262 scanlines of 228 t-states,
 * so 60 of them a second is 3584160 Hz, which is the NABU's 3.58 MHz
 * clock. The machine therefore runs at 1x and only 1x.
 *
 * There used to be a burst mode here that let the machine run up to eight
 * times real time whenever the firmware was not blocked on an empty wire,
 * to shorten the boot. It worked, and it was still wrong, because the
 * emulated clock is not private: the MGT front page carries a live
 * Montreal clock that the Z80 program ticks itself, and a machine running
 * fast makes that clock run fast. Anything the guest counts in its own
 * cycles is visible to the operator, so the only honest speed is the
 * real one. The burst was removed on 2026-08-07; see
 * docs/2026-08-07_NABU_NATIVE_1X_CLOCK.md.
 *
 * The original constraint that motivated the burst design still holds and
 * is satisfied trivially by running at 1x: Phase 1 proved the firmware
 * breaks if the emulated clock outruns the wall clock, because its
 * timeouts are counted in machine cycles and a machine running twenty
 * times too fast gives up on the channel server long before a network
 * round trip can complete.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
/* WIN32_LEAN_AND_MEAN keeps windows.h from pulling in the multimedia
 * headers, and the PSG output uses waveOut. -lwinmm is already in the
 * link line for the other tabs that play sound. */
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suite_shell.h"
#include "nabu_native_module.h"
#include "nabu_core.h"
#include "nabu_serial.h"

/* ------------------------------------------------------------------ */
/* Control ids. 6601 block, clear of the external NABU tab's 6501.     */
/* ------------------------------------------------------------------ */
#define IDC_NN_CONNECT  6601
#define IDC_NN_DISCONN  6602
/* 6603 was the tab's own status STATIC, retired 2026-08-07 when the
 * status moved to the Suite's status bar. Left unused, not recycled. */
#define IDC_NN_SCREEN   6604
#define IDC_NN_HOST     6605
#define IDC_NN_PORT     6606
#define IDC_NN_CHAN     6607
/* 6608 was the Firmware picker, retired 2026-08-08 when MGT IPL became
 * the only firmware the emulator boots. Left unused, not recycled. */
#define IDC_NN_LBL      6609    /* every static label shares one id */
#define IDC_NN_MODE     6610
#define IDC_NN_COM      6611

#define NN_SCREEN_CLASS "MGTNabuScreenV1"

/*
 * The connection strip is two rows since 2026-08-08. One row held six
 * controls and needed 995 pixels against the 932 a default window gives,
 * and the serial bridge adds a mode picker and a serial port picker on
 * top of that. Two rows fit at the default size with room to spare, and
 * the 34 pixels they cost come out of a picture that is bar-limited
 * anyway at the default window size.
 *
 *   row 1   Mode      Host         Port   Channel
 *   row 2   Firmware  Serial port  Connect  Disconnect
 */
#define NN_ROW_H        24
#define NN_ROW_GAP      6
#define NN_STRIP_H      (8 + NN_ROW_H + NN_ROW_GAP + 30 + 8)

/* The two things this tab can be. */
#define NN_MODE_EMULATOR 0
#define NN_MODE_SERIAL   1

/* The ROM. Phase 2 has no Host, Port or ROM fields (Phase 4 does), so
 * the machine is started on the vendored MIT OpenNabu against the live
 * channel. See third_party/opennabu/MGT-NOTES.md for why this ROM and
 * not the stock one: OpenNabu boots with no keyboard attached, which is
 * the only kind of machine this phase can drive. */
/* No fast-boot tuning lives here any more. The machine runs at 1x, so
 * the boot costs what it costs: the boot image is 25088 bytes and the
 * guest spends about 1800 Z80 cycles per byte, which is roughly 2000
 * bytes a second, so the transfer alone is some 25 seconds. What the
 * operator sees during it is the machine's own boot screen plus the
 * status line below the picture; this tab has no splash of its own. */

/* If the host cannot keep up and the emulation falls this far behind
 * wall clock, the pacer gives up the lost time rather than running fast
 * to reclaim it. Banked credit is exactly the sprint-through-a-timeout
 * failure the 1x rule exists to prevent, so time lost stays lost. */
#define NN_RESYNC_S  0.25

/*
 * THE FIRMWARE, IN PREFERENCE ORDER.
 *
 * MGT IPL is the Suite's own fork of OpenNabu IPL, cut down to load the
 * channel and nothing else: no floppy probe, no Winchester probe, no boot
 * menu, no power-on RAM test. Measured against the live channel it reaches
 * the wire in 0.4 seconds where the stock firmware takes 10.1, which is
 * most of a nine second saving on the whole boot. See
 * firmware/mgtipl/ and docs/2026-08-07_NABU_MGT_IPL_FAST_BOOT.md.
 *
 * Stock OpenNabu IPL stays on the list underneath it, and that is the
 * point of the list. This tab's firmware is now something the Suite
 * builds itself, and a firmware you build yourself is a firmware you can
 * break; if a bad MGT IPL ever ships, the tab finds the stock ROM and
 * still boots. The order is installed-then-source-tree within each
 * firmware, so a built exe run from the working copy behaves the same as
 * an installed one.
 */
typedef struct {
    const char *path;   /* relative to the folder holding the exe */
    const char *name;   /* what to call it if it ever has to be named */
} nn_rom_t;

static const nn_rom_t NN_ROM_CANDIDATES[] = {
    { "NABU\\mgtipl.bin",                   "MGT IPL (fast boot)" },
    { "firmware\\mgtipl\\mgtipl.bin",       "MGT IPL (fast boot)" },
    { "NABU\\opennabu.bin",                 "OpenNabu IPL (stock)" },
    { "third_party\\opennabu\\opennabu.bin","OpenNabu IPL (stock)" },
    /*
     * A real NABU firmware, if one is ever dropped in beside the others.
     * The Suite does not ship it. It is last because it is the last
     * resort, and it needs a keyboard, which the machine has had since
     * Phase 3.
     */
    { "NABU\\nabupc.bin",                   "NABU firmware (preserved)" }
};
#define NN_ROM_COUNT (int)(sizeof NN_ROM_CANDIDATES / sizeof NN_ROM_CANDIDATES[0])

/*
 * The first two entries are MGT IPL, installed and source tree. Anything
 * past them means the preferred firmware was missing and the loader fell
 * through to something else, which is a thing to say out loud.
 */
#define NN_ROM_PREFERRED_COUNT 2

static int nn_rom_is_preferred(int which)
{
    return which >= 0 && which < NN_ROM_PREFERRED_COUNT;
}

static const char *nn_rom_name(int which)
{
    if (which < 0 || which >= NN_ROM_COUNT) return "unknown firmware";
    return NN_ROM_CANDIDATES[which].name;
}

/* The connection, as the fields had it at the moment Connect was pressed.
 * Written by the UI thread before the worker starts and read by the
 * worker only, which is what makes it safe without a lock: the worker
 * does not exist yet when these are written. */
static char g_cfg_host[128] = NABU_CORE_DEFAULT_HOST;
static int  g_cfg_port      = NABU_CORE_DEFAULT_PORT;
static int  g_cfg_channel   = NABU_CORE_DEFAULT_CHANNEL;
static char g_cfg_rom[MAX_PATH];
static int  g_cfg_rom_which = -1;

/* ------------------------------------------------------------------ */
/* Remembering the last connection                                     */
/* ------------------------------------------------------------------ */

/*
 * The Suite already has exactly one settings store: HKCU under
 * "Software\The Montreal Greek Times\MGT Unicorn Suite", which
 * suite_update.c created for the update preferences. This reuses that
 * key with its own value names rather than inventing a second store, so
 * everything the Suite remembers about a user is in one place and
 * uninstalling reaches all of it.
 *
 * suite_update's own helpers are static to that file, so these are a
 * local pair rather than a shared service. Two short functions duplicated
 * is a smaller thing to carry than a settings API invented for its second
 * caller.
 */
#define NN_REG_KEY "Software\\The Montreal Greek Times\\MGT Unicorn Suite"

static void nn_reg_write(const char *name, const char *value)
{
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, NN_REG_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL,
                        &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)value,
                   (DWORD)(strlen(value) + 1));
    RegCloseKey(k);
}

static int nn_reg_read(const char *name, char *out, size_t cap)
{
    HKEY  k;
    DWORD type = 0, sz = (DWORD)cap;
    LONG  rc;
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    if (RegOpenKeyExA(HKEY_CURRENT_USER, NN_REG_KEY, 0, KEY_QUERY_VALUE,
                      &k) != ERROR_SUCCESS)
        return 0;
    rc = RegQueryValueExA(k, name, NULL, &type, (BYTE *)out, &sz);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || type != REG_SZ) { out[0] = '\0'; return 0; }
    out[cap - 1] = '\0';
    return out[0] != '\0';
}

static HWND g_hContent   = NULL;
static HWND g_hStartBtn  = NULL;   /* "Connect"    */
static HWND g_hStopBtn   = NULL;   /* "Disconnect" */
static HWND g_hScreen    = NULL;
static HWND g_hHostLbl   = NULL;
static HWND g_hHostEdit  = NULL;
static HWND g_hPortLbl   = NULL;
static HWND g_hPortEdit  = NULL;
static HWND g_hChanLbl   = NULL;
static HWND g_hChanEdit  = NULL;
static HWND g_hModeLbl   = NULL;
static HWND g_hModeCombo = NULL;
static HWND g_hComLbl    = NULL;
/*
 * GROUPING LABELS, and the model they exist to make obvious.
 *
 * The tab has two directions and they are easy to confuse. Host, Port and
 * Channel are the CONTENT SOURCE, upstream, over TCP. The serial port is
 * the NABU LINK, downstream, over a cable.
 *
 * The distinction matters most in Real NABU mode, where the Suite IS the
 * Internet Adapter: a real NABU has no network of its own and depends on
 * the Suite to fetch content over TCP and relay it down the cable. So
 * Host, Port and Channel are fully live in BOTH modes and keep the Greek
 * Times defaults in both. Greying them in serial mode would leave the
 * adapter with nowhere to fetch from, which is the opposite of what the
 * mode is for. These two labels exist so nobody reaches that conclusion
 * again by looking at the strip.
 */
static HWND g_hGrpSrcLbl  = NULL;
static HWND g_hGrpLinkLbl = NULL;
static HWND g_hComCombo  = NULL;
static int  g_controls_created = 0;
static int  g_class_reg        = 0;

/* The shared frame. */
static CRITICAL_SECTION g_lock;
static int      g_lock_init = 0;
static HBITMAP  g_hDib      = NULL;
static void    *g_dib_bits  = NULL;
static HDC      g_hMemDC    = NULL;
static HBITMAP  g_hOldDib   = NULL;
static uint64_t g_shown_serial = 0;   /* frames converted into the DIB */
static uint32_t g_border    = 0;
static int      g_have_frame = 0;

/* The emulation thread. */
static HANDLE g_thread    = NULL;
static HANDLE g_stopEvent = NULL;
static volatile LONG g_running = 0;

/* Status text, written by the emulation thread and read by the UI. */
static char g_status[256] = "Idle";

/* ------------------------------------------------------------------ */
/* The connection log, for serial mode                                 */
/* ------------------------------------------------------------------ */

/*
 * In serial mode there is no picture to show: the picture is on the
 * NABU's own television, across the room. What the operator needs from
 * the Suite instead is evidence that the cable and the wire are doing
 * something, which is what the separate adapter programs have always
 * shown, so the screen window paints a log in the same rectangle.
 *
 * A fixed ring rather than a growing list. A bridge left running all
 * evening would otherwise accumulate without bound, and nobody scrolls
 * back through hours of a byte counter.
 */
#define NN_LOG_LINES 200
#define NN_LOG_CHARS 160
static char g_log[NN_LOG_LINES][NN_LOG_CHARS];
static int  g_log_head  = 0;    /* next slot to write */
static int  g_log_count = 0;

/* The mode the tab is in, and the serial settings, latched at Connect
 * the same way the others are. */
static int  g_cfg_mode = NN_MODE_EMULATOR;
static char g_cfg_com[16] = "";

/* ------------------------------------------------------------------ */
/* Keystrokes, from the UI thread to the machine                       */
/* ------------------------------------------------------------------ */

/*
 * The recorded rule is that only the emulation thread ever calls into
 * nabu_core, and keystrokes arrive on the UI thread, so they cross here
 * and nowhere else: the window procedure appends NABU key bytes to this
 * ring, and the emulation thread drains it into the machine once a
 * frame. Small on purpose. A human cannot outrun 64 bytes at 60 hertz,
 * and dropping keystrokes is better than growing a queue that plays them
 * back seconds late.
 */
#define NN_KEYQ_SIZE 64
static uint8_t g_keyq[NN_KEYQ_SIZE];
static int     g_keyq_head = 0;   /* next slot to write (UI thread)   */
static int     g_keyq_tail = 0;   /* next slot to read (emu thread)   */

static void nn_key_push(uint8_t code)
{
    int next;
    EnterCriticalSection(&g_lock);
    next = (g_keyq_head + 1) % NN_KEYQ_SIZE;
    if (next != g_keyq_tail) {
        g_keyq[g_keyq_head] = code;
        g_keyq_head = next;
    }
    LeaveCriticalSection(&g_lock);
}

/* Emulation thread. Drains whatever the UI queued into the machine. */
static void nn_key_drain(void)
{
    for (;;) {
        uint8_t code;
        EnterCriticalSection(&g_lock);
        if (g_keyq_tail == g_keyq_head) { LeaveCriticalSection(&g_lock); return; }
        code = g_keyq[g_keyq_tail];
        g_keyq_tail = (g_keyq_tail + 1) % NN_KEYQ_SIZE;
        LeaveCriticalSection(&g_lock);
        nabu_core_key_put(code);
    }
}

/*
 * The NABU keyboard's own codes, taken from Marduk's keyboard_poll and
 * not invented here. The special keys send a MAKE byte on press and a
 * BREAK byte on release; printable keys send their ASCII once, on press.
 *
 *   right E0/F0   left E1/F1   up E2/F2   down E3/F3
 *   PgDn  E4/F4 is the NABU's right guillemet, PgUp E5/F5 the left
 *   Del   E6/F6 is NO, Ins E7/F7 is YES
 *   Alt   E8/F8 is SYM
 *   Pause E9/F9, End EA/FA
 *   Backspace is plain 0x7F and has no break code.
 */
typedef struct { int vk; uint8_t make; uint8_t brk; } nn_special_t;

static const nn_special_t g_specials[] = {
    { VK_RIGHT,  0xE0, 0xF0 },
    { VK_LEFT,   0xE1, 0xF1 },
    { VK_UP,     0xE2, 0xF2 },
    { VK_DOWN,   0xE3, 0xF3 },
    { VK_NEXT,   0xE4, 0xF4 },   /* PageDown, right guillemet */
    { VK_PRIOR,  0xE5, 0xF5 },   /* PageUp, left guillemet    */
    { VK_DELETE, 0xE6, 0xF6 },   /* NO  */
    { VK_INSERT, 0xE7, 0xF7 },   /* YES */
    { VK_MENU,   0xE8, 0xF8 },   /* Alt, SYM */
    { VK_PAUSE,  0xE9, 0xF9 },
    { VK_END,    0xEA, 0xFA },
};

static const nn_special_t *nn_find_special(int vk)
{
    int i;
    for (i = 0; i < (int)(sizeof g_specials / sizeof g_specials[0]); i++)
        if (g_specials[i].vk == vk) return &g_specials[i];
    return NULL;
}

/*
 * Printable keys, derived from the VIRTUAL KEY plus the shift state
 * rather than taken from WM_CHAR.
 *
 * This is not a preference either. WM_CHAR carries the keystroke already
 * translated through the active keyboard layout, so on the Greek layout
 * this workstation also runs, pressing T delivers 0xF4, the codepage
 * byte for tau, and the NABU would receive that instead of 'T'. The
 * Terminal tab hit exactly this and solved it the same way. The NABU
 * keyboard is a US ASCII layout, so the ASCII is derived here.
 */
static int nn_vk_to_ascii(int vk)
{
    BOOL shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    BOOL ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    BOOL caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;
    static const char *shiftnums = ")!@#$%^&*(";
    int ch = 0;

    if (vk >= 'A' && vk <= 'Z') {
        BOOL upper = (shift != caps);
        ch = upper ? vk : (vk + 32);
        if (ctrl) ch &= 0x1F;
        return ch;
    }
    if (vk >= '0' && vk <= '9') {
        if (ctrl) {
            if (vk == '2') return 0xFF;   /* the guest reads this as 0x00 */
            if (vk == '6') return 0x1E;
            return 0;
        }
        return shift ? shiftnums[vk - '0'] : vk;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);

    switch (vk) {
    case VK_SPACE:  return ' ';
    case VK_RETURN: return '\r';
    case VK_TAB:    return '\t';
    case VK_ESCAPE: return 0x1B;
    case VK_BACK:   return 0x7F;   /* the NABU's rubout */
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
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Sound, through the Suite's existing waveOut path                    */
/* ------------------------------------------------------------------ */

/*
 * The same device and the same pattern the CU-SeeMe tab already uses:
 * waveOutOpen with plain PCM, a ring of headers, push a chunk per frame
 * and DROP when every buffer is still busy. Dropping is what keeps the
 * latency from creeping; a queue that never drops ends up playing the
 * machine's sound a second behind its picture.
 *
 * No second device and no new backend: the radio's WASAPI engine is for
 * a decoded network stream and is the wrong shape for a chip that
 * produces samples in lockstep with the CPU.
 */
#define NN_NWBUF     8
#define NN_WBUF_SAMP (NABU_AUDIO_RATE / 60)   /* one emulated frame */

static HWAVEOUT g_wave = NULL;
static WAVEHDR  g_whdr[NN_NWBUF];
static short   *g_wbuf[NN_NWBUF];
static int      g_wbuf_init = 0;

static void nn_audio_open(void)
{
    WAVEFORMATEX wf;
    int i;
    if (g_wave) return;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = NABU_AUDIO_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (wf.wBitsPerSample / 8) * wf.nChannels;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL)
        != MMSYSERR_NOERROR) {
        g_wave = NULL;
        return;
    }
    if (!g_wbuf_init) {
        for (i = 0; i < NN_NWBUF; i++)
            g_wbuf[i] = (short *)malloc(NN_WBUF_SAMP * sizeof(short));
        g_wbuf_init = 1;
    }
    memset(g_whdr, 0, sizeof g_whdr);
}

static void nn_audio_close(void)
{
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);          /* stop and mark every buffer done */
    for (i = 0; i < NN_NWBUF; i++)
        if (g_whdr[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_wave, &g_whdr[i], sizeof(WAVEHDR));
    waveOutClose(g_wave);
    g_wave = NULL;
    memset(g_whdr, 0, sizeof g_whdr);
}

static void nn_audio_push(const short *pcm, int nsamp)
{
    int i;
    if (!g_wave || nsamp <= 0) return;
    if (nsamp > NN_WBUF_SAMP) nsamp = NN_WBUF_SAMP;
    for (i = 0; i < NN_NWBUF; i++) {
        WAVEHDR *h = &g_whdr[i];
        if (h->dwFlags == 0 || (h->dwFlags & WHDR_DONE)) {
            if (h->dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wave, h, sizeof(WAVEHDR));
            memcpy(g_wbuf[i], pcm, nsamp * sizeof(short));
            memset(h, 0, sizeof *h);
            h->lpData         = (LPSTR)g_wbuf[i];
            h->dwBufferLength = nsamp * sizeof(short);
            if (waveOutPrepareHeader(g_wave, h, sizeof(WAVEHDR))
                == MMSYSERR_NOERROR)
                waveOutWrite(g_wave, h, sizeof(WAVEHDR));
            return;
        }
    }
    /* Every buffer still playing: drop this frame's worth. */
}

static void nn_set_status(const char *s)
{
    if (!s) return;
    EnterCriticalSection(&g_lock);
    snprintf(g_status, sizeof g_status, "%s", s);
    LeaveCriticalSection(&g_lock);
    if (g_hScreen) {
        /* SetWindowText from the emulation thread would block on the UI
         * thread's message queue; post and let the UI do it. */
        PostMessageA(g_hScreen, WM_APP + 1, 0, 0);
    }
}

/*
 * Add one line, stamped with the wall clock.
 *
 * Called from the bridge thread and from the UI thread, so it takes the
 * same lock the frame does. The stamp is local time to the second, which
 * is the resolution anything in this log happens at.
 */
static void nn_log_add(const char *line)
{
    SYSTEMTIME st;
    if (!line) return;
    GetLocalTime(&st);
    EnterCriticalSection(&g_lock);
    snprintf(g_log[g_log_head], NN_LOG_CHARS, "%02d:%02d:%02d  %s",
             st.wHour, st.wMinute, st.wSecond, line);
    g_log_head = (g_log_head + 1) % NN_LOG_LINES;
    if (g_log_count < NN_LOG_LINES) g_log_count++;
    LeaveCriticalSection(&g_lock);
    if (g_hScreen) InvalidateRect(g_hScreen, NULL, FALSE);
}

static void nn_log_clear(void)
{
    EnterCriticalSection(&g_lock);
    g_log_head = g_log_count = 0;
    LeaveCriticalSection(&g_lock);
}

/* The shape nabu_serial wants for its own messages. */
static void nn_log_sink(void *user, const char *line)
{
    (void)user;
    nn_log_add(line);
}

/* ------------------------------------------------------------------ */
/* The idle instructions                                               */
/* ------------------------------------------------------------------ */

/*
 * A BOLD WEIGHT OF THE FONT THE SUITE ALREADY USES.
 *
 * Derived from g_hFontUI rather than named outright: GetObject hands back
 * the LOGFONT the shared font was built from, the weight is changed and
 * nothing else is, so this follows the Suite's UI font wherever it goes.
 * Naming Consolas here would be a second place to edit the day that
 * changes, and would quietly disagree with the rest of the tab if the
 * fallback to Courier New ever fired.
 *
 * Made once and kept, because it is asked for on every repaint of an idle
 * tab. Released in nabu_native_module_shutdown with the other GDI things.
 */
static HFONT g_hFontBold = NULL;

static HFONT nn_font_bold(void)
{
    LOGFONTA lf;
    if (g_hFontBold) return g_hFontBold;
    if (!g_hFontUI)  return NULL;
    if (GetObjectA(g_hFontUI, sizeof lf, &lf) != sizeof lf) return NULL;
    lf.lfWeight = FW_BOLD;
    g_hFontBold = CreateFontIndirectA(&lf);
    return g_hFontBold;
}

/*
 * Two centred lines: a bold heading, then a body line under it.
 *
 * The body wraps, which matters because the serial line runs to ninety
 * odd characters and a narrow tab would otherwise cut it off mid word.
 * Both blocks are measured with DT_CALCRECT first so the pair can be
 * centred vertically as one thing rather than the heading sitting
 * wherever a single-line centre would have put it.
 */
static void nn_draw_two_line(HDC mem, const RECT *area,
                             const char *head, const char *body)
{
    RECT  rh, rb, out;
    HFONT bold = nn_font_bold();
    int   inset = 24, total, y, w;

    if (!head || !body) return;

    w = (area->right - area->left) - 2 * inset;
    if (w < 80) w = area->right - area->left;

    SetBkMode(mem, TRANSPARENT);

    /* Measure the heading. */
    rh = *area;
    rh.left += inset; rh.right = rh.left + w;
    if (bold) SelectObject(mem, bold);
    DrawTextA(mem, head, -1, &rh, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);

    /* Measure the body. */
    rb = *area;
    rb.left += inset; rb.right = rb.left + w;
    if (g_hFontUI) SelectObject(mem, g_hFontUI);
    DrawTextA(mem, body, -1, &rb, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);

    total = (rh.bottom - rh.top) + 6 + (rb.bottom - rb.top);
    y = area->top + ((area->bottom - area->top) - total) / 2;
    if (y < area->top) y = area->top;

    out = *area;
    out.left += inset; out.right = out.left + w;

    out.top = y;
    out.bottom = y + (rh.bottom - rh.top);
    if (bold) SelectObject(mem, bold);
    SetTextColor(mem, RGB(210, 210, 210));
    DrawTextA(mem, head, -1, &out, DT_CENTER | DT_WORDBREAK);

    out.top = out.bottom + 6;
    out.bottom = out.top + (rb.bottom - rb.top);
    if (g_hFontUI) SelectObject(mem, g_hFontUI);
    SetTextColor(mem, RGB(160, 160, 160));
    DrawTextA(mem, body, -1, &out, DT_CENTER | DT_WORDBREAK);
}

/* The exact wording, in one place, so the two paint paths cannot drift
 * apart and a reword is a one-line edit. */
#define NN_MSG_SERIAL_HEAD "Real NABU PC hardware mode"
#define NN_MSG_SERIAL_BODY \
    "Choose the serial port your NABU Personal Computer Appliance is " \
    "cabled to, then press Connect."
#define NN_MSG_EMU_HEAD    "Software Emulation mode"
#define NN_MSG_EMU_BODY \
    "Press Connect to boot a NABU emulator on the Greek Times channel"

/* ------------------------------------------------------------------ */
/* What the machine is doing, in one word                              */
/* ------------------------------------------------------------------ */

/*
 * ONE LINE, ONE FORMAT, WRITTEN ONLY WHEN THE STATE CHANGES.
 *
 * The tab used to have two writers racing each other into the same
 * strip: a once-a-second line of counters, and the emulation core's
 * diagnostic hook, which fired whenever the modem or the adapter had
 * something to say. The line flickered between "Running - 117.0 s, 29090
 * bytes, emu 1.00x" and "NABU: channel 1 selected on the adapter", which
 * is two formats where the operator wanted none.
 *
 * Both are gone. The line is now identity plus state:
 *
 *     nabu.greektimes.ca:5816   ch 1   MGT IPL (fast boot)   Ready
 *
 * and it is rewritten only when the state word changes, which over a
 * whole session is three times.
 *
 * The counters went with them on purpose. The multiplier was always
 * 1.00x once the pacer became unconditional on 2026-08-07, so it was a
 * constant being reported as though it were news; the byte count and the
 * elapsed time answered "is it stuck", which is what a state word answers
 * better.
 */
typedef enum {
    NN_ST_IDLE = 0,
    NN_ST_CONNECTING,
    NN_ST_BOOTING,
    NN_ST_READY,
    NN_ST_NOLINK,
    NN_ST_FAILED,
    /* Serial mode. The cable is open, the server is connected, and bytes
     * are free to move; there is no Ready to reach because what is ready
     * or not is a real machine the Suite cannot see. */
    NN_ST_BRIDGING
} nn_state_t;

static volatile LONG g_state = NN_ST_IDLE;
/* Only read when the state is NN_ST_FAILED, and written before it is
 * set, by the same thread. */
static char g_fail_reason[160];

static void nn_set_state(int st);

/* Say why nothing started, and go to the failure state in one step, so a
 * reason can never be set without the state that displays it. */
static void nn_fail(const char *why)
{
    snprintf(g_fail_reason, sizeof g_fail_reason, "%s", why ? why : "");
    /* Force the state to change even if it was already FAILED, so a
     * second wrong value in a different field still repaints the line. */
    InterlockedExchange(&g_state, NN_ST_IDLE);
    nn_set_state(NN_ST_FAILED);
}

static const char *nn_state_word(int st)
{
    switch (st) {
    case NN_ST_CONNECTING: return "Connecting";
    case NN_ST_BOOTING:    return "Booting";
    case NN_ST_READY:      return "Ready";
    case NN_ST_BRIDGING:   return "Bridging";
    case NN_ST_NOLINK:     return "No channel connection";
    case NN_ST_FAILED:     return g_fail_reason;
    default:               return "Disconnected";
    }
}

static void nn_set_state(int st)
{
    char line[320];

    if (InterlockedExchange(&g_state, (LONG)st) == (LONG)st) return;

    /*
     * Idle is the one state that does NOT carry the identity. The four
     * fields are on screen directly above, they are editable while idle,
     * and repeating them underneath would only give the operator a second
     * copy that goes stale the moment they start typing in the first.
     */
    if (st == NN_ST_IDLE) {
        nn_set_status("Disconnected");
        return;
    }

    /*
     * A failure carries its reason and nothing else. There is no
     * connection to describe, and half of what the identity would report
     * is exactly what the operator has to change, so printing it back at
     * them as though it were a fact would be the wrong shape.
     *
     * This is the only other form the line takes, and it does not
     * alternate with the first: a session is either describing a machine
     * or saying why there is not one.
     */
    if (st == NN_ST_FAILED) {
        nn_set_status(g_fail_reason[0] ? g_fail_reason : "Could not connect");
        return;
    }

    /*
     * The same line in both modes: identity, then the state word. What
     * counts as identity differs, because in serial mode the firmware is
     * the real machine's own and the Suite has no idea what it is, while
     * the cable is the part the operator picked and might have picked
     * wrong.
     */
    if (g_cfg_mode == NN_MODE_SERIAL)
        snprintf(line, sizeof line, "%s -> %s:%d   ch %d   %s",
                 g_cfg_com[0] ? g_cfg_com : "no port",
                 g_cfg_host, g_cfg_port, g_cfg_channel,
                 nn_state_word(st));
    else {
        /*
         * The firmware is named only when it is NOT the one that should
         * have loaded. Normally MGT IPL boots and saying so every time
         * would be a constant reported as though it were news, which is
         * the habit the 2026-08-07 pass took out of this line. But the
         * loader still has a fallback underneath it, and a fallback
         * nobody is told about is the one that wastes an afternoon, so
         * when anything else loads it is named for the whole session.
         */
        char fwnote[96];
        if (nn_rom_is_preferred(g_cfg_rom_which))
            fwnote[0] = '\0';
        else
            snprintf(fwnote, sizeof fwnote, "using %s   ",
                     nn_rom_name(g_cfg_rom_which));
        snprintf(line, sizeof line, "%s:%d   ch %d   %s%s",
                 g_cfg_host, g_cfg_port, g_cfg_channel,
                 fwnote, nn_state_word(st));
    }
    nn_set_status(line);
}

/* ------------------------------------------------------------------ */
/* The DIB                                                             */
/* ------------------------------------------------------------------ */

static void nn_dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_hDib) return;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth       = NABU_SCREEN_W;
    bi.bmiHeader.biHeight      = -NABU_SCREEN_H;    /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_dib_bits, NULL, 0);
    if (g_hDib) {
        g_hMemDC  = CreateCompatibleDC(screen);
        g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
    }
    ReleaseDC(NULL, screen);
}

static void nn_dib_destroy(void)
{
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
        g_hOldDib = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; }
    g_dib_bits = NULL;
    g_have_frame = 0;
    g_shown_serial = 0;
}

/* ------------------------------------------------------------------ */
/* Where the screen lands inside the tab                               */
/* ------------------------------------------------------------------ */

/*
 * LARGEST WHOLE-NUMBER SCALE THAT FITS, CENTRED.
 *
 * The source is the TMS9918 active display, NABU_SCREEN_W by
 * NABU_SCREEN_H, which is 256 by 192 and therefore already exactly 4:3.
 * That last fact is worth stating because it makes the arithmetic here
 * much simpler than it looks: scaling 256x192 by a whole number PRESERVES
 * the aspect ratio for free, so there is no separate aspect correction to
 * apply and none is applied.
 *
 * The scale is an integer and never anything else. Nearest neighbour at a
 * fractional scale gives pixel blocks of uneven width, which on the 8x8
 * text a NABU spends most of its life drawing reads as wobbling letters;
 * the alternative, smoothing, turns the same text to mush. A whole number
 * makes every source pixel the same solid block as every other, and that
 * is where the tab's sharpness comes from.
 *
 * The floor is 1x, so a tab too small to hold the whole picture shows the
 * middle of it at full size rather than a blurred shrink. StretchBlt
 * clips the overhang. At the Suite's window sizes this never arises: it
 * would need a screen area under 256x192.
 */
static void nn_dest_rect(int cw, int ch, RECT *out)
{
    int scale, w, h;

    scale = cw / NABU_SCREEN_W;
    if (ch / NABU_SCREEN_H < scale) scale = ch / NABU_SCREEN_H;
    if (scale < 1) scale = 1;

    w = NABU_SCREEN_W * scale;
    h = NABU_SCREEN_H * scale;

    /* Centre it. Whatever is left over becomes the bars, and the caller
     * has already filled the whole client area with the surround colour,
     * so there is nothing to draw into them. */
    out->left   = (cw - w) / 2;
    out->top    = (ch - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

/* ------------------------------------------------------------------ */
/* The screen window                                                   */
/* ------------------------------------------------------------------ */

/*
 * Paint the connection log into an already-created memory DC.
 *
 * Bottom up from the newest line, so the most recent thing is always
 * where the eye lands and a window too short to hold the history simply
 * shows less of it. No scrollbar and no scrolling state: the interesting
 * end is the end, and a log the operator has to chase is a log they stop
 * reading.
 */
static void nn_paint_log(HDC mem, const RECT *rc, int cw, int ch)
{
    TEXTMETRICA tm;
    int  line_h, y, i, n, head;
    static char snapshot[NN_LOG_LINES][NN_LOG_CHARS];

    FillRect(mem, rc, g_hBrushBlack);
    SetBkMode(mem, TRANSPARENT);
    if (g_hFontUI) SelectObject(mem, g_hFontUI);
    GetTextMetricsA(mem, &tm);
    line_h = tm.tmHeight + 2;
    if (line_h < 4) line_h = 4;

    /* Copy under the lock and paint outside it. GDI calls can block, and
     * blocking the bridge thread behind a repaint is exactly what a relay
     * must not do. */
    EnterCriticalSection(&g_lock);
    n = g_log_count;
    head = g_log_head;
    if (n > 0) memcpy(snapshot, g_log, sizeof snapshot);
    LeaveCriticalSection(&g_lock);

    if (n == 0) {
        nn_draw_two_line(mem, rc, NN_MSG_SERIAL_HEAD, NN_MSG_SERIAL_BODY);
        return;
    }

    SetTextColor(mem, RGB(220, 220, 220));
    y = ch - line_h - 4;
    for (i = 0; i < n && y > -line_h; i++) {
        /* head is one past the newest, so walk backwards from it. */
        int idx = ((head - 1 - i) % NN_LOG_LINES + NN_LOG_LINES) % NN_LOG_LINES;
        TextOutA(mem, 6, y, snapshot[idx], (int)strlen(snapshot[idx]));
        y -= line_h;
    }
    (void)cw;
}

static void nn_screen_paint(HWND hwnd, HDC hdc)
{
    RECT    rc, dst;
    HDC     mem;
    HBITMAP bmp, oldbmp;
    int     cw, ch;
    char    status[256];
    int     have;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    /* Compose off-screen so the letterbox and the picture reach the
     * screen in one BitBlt. A 60 Hz stream flickers badly otherwise;
     * this is the same lesson the Gopher and RIPscrip renderers carry. */
    mem = CreateCompatibleDC(hdc);
    if (!mem) return;
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    if (!bmp) { DeleteDC(mem); return; }
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    /*
     * SERIAL MODE PAINTS THE LOG INSTEAD OF THE PICTURE.
     *
     * Same window, same rectangle, different content, because in serial
     * mode the picture is on the NABU's own television across the room
     * and what the Suite can usefully show is what it is doing about it.
     */
    if (g_cfg_mode == NN_MODE_SERIAL) {
        nn_paint_log(mem, &rc, cw, ch);
        BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        return;
    }

    EnterCriticalSection(&g_lock);
    have   = g_have_frame;
    snprintf(status, sizeof status, "%s", g_status);

    /*
     * THE BARS ARE THE SAME COLOUR AS THE TAB AROUND THEM.
     *
     * g_hBrushBlack is the shared brush the content window itself is
     * built with, so it is literally what the operator sees in the margin
     * outside this child window. Filling the leftover space with it means
     * the pillarbox and the surround are one continuous field and the
     * picture reads as a picture, not as a picture inside a box inside a
     * box. The brush is reused rather than a colour matched by hand, so
     * if the Suite's surround ever changes, this follows it.
     *
     * This used to fill with the machine's own border colour, which made
     * the bars a different colour from the surround and drew a visible
     * rectangle around the picture. The border colour is still tracked in
     * g_border, it is simply no longer painted here.
     */
    FillRect(mem, &rc, g_hBrushBlack);

    if (have && g_hMemDC) {
        nn_dest_rect(cw, ch, &dst);
        /* COLORONCOLOR is GDI's nearest neighbour. A NABU pixel scaled
         * up must stay a hard-edged block; smoothing 8x8 text turns it
         * to mush. */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, dst.left, dst.top,
                   dst.right - dst.left, dst.bottom - dst.top,
                   g_hMemDC, 0, 0, NABU_SCREEN_W, NABU_SCREEN_H, SRCCOPY);
    }
    LeaveCriticalSection(&g_lock);

    if (!have) {
        /*
         * The placeholder before the first frame arrives. Once the
         * machine is drawing, the picture paints over this and it is not
         * seen again until Disconnect.
         */
        if (InterlockedCompareExchange(&g_running, 0, 0)) {
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, RGB(160, 160, 160));
            if (g_hFontUI) SelectObject(mem, g_hFontUI);
            DrawTextA(mem, "Connecting to the Greek Times channel...", -1,
                      &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            nn_draw_two_line(mem, &rc, NN_MSG_EMU_HEAD, NN_MSG_EMU_BODY);
        }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK NnScreenProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                       /* nn_screen_paint owns the rect */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        nn_screen_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    /*
     * A custom-painted child in this shell MUST claim the keys, or
     * IsDialogMessage in the shell's dispatch loop swallows letters
     * looking for a mnemonic and the guest never sees them. The
     * Terminal tab learned this the hard way and the RIPscrip canvas
     * carries the same declaration.
     */
    case WM_GETDLGCODE:
        return DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTALLKEYS;

    /* Clicking the screen gives it the keyboard, the way clicking a
     * terminal does. */
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const nn_special_t *sp = nn_find_special((int)wParam);
        int ch;
        if (!InterlockedCompareExchange(&g_running, 0, 0)) break;
        if (sp) { nn_key_push(sp->make); return 0; }
        ch = nn_vk_to_ascii((int)wParam);
        if (ch) { nn_key_push((uint8_t)ch); return 0; }
        break;
    }

    case WM_KEYUP:
    case WM_SYSKEYUP: {
        const nn_special_t *sp = nn_find_special((int)wParam);
        if (!InterlockedCompareExchange(&g_running, 0, 0)) break;
        if (sp) { nn_key_push(sp->brk); return 0; }
        /* Printable keys have no break code on this keyboard. */
        break;
    }

    /* The ASCII is derived from the virtual key above, so the
     * layout-translated duplicate is dropped here. */
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;

    /*
     * Status changed, posted by the worker.
     *
     * THE TAB HAS NO STATUS STRIP OF ITS OWN ANY MORE, and that is the
     * fix for the line appearing twice. It used to write the same text
     * into a private STATIC at the bottom of its content AND into the
     * Suite's own status bar immediately below that, so two identical
     * lines sat one above the other and looked like a repaint bug. It was
     * not a repaint bug, it was two controls.
     *
     * The Suite's bar is the one that survives, because that is where
     * Archie, ARPANET, Gopher and IRC all put their status, and because
     * giving up the private strip handed 38 pixels of height back to the
     * picture.
     */
    case WM_APP + 1: {
        char s[320];
        EnterCriticalSection(&g_lock);
        snprintf(s, sizeof s, "%s", g_status);
        LeaveCriticalSection(&g_lock);
        suite_set_status(s);
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void nn_register_class(HINSTANCE hInst)
{
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize        = sizeof wc;
    /* The picture is scaled to the client rect, so every pixel of it
     * depends on the client size and a partial invalidate would leave
     * two scales composited. Same pair the RIPscrip canvas carries. */
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = NnScreenProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = NN_SCREEN_CLASS;
    RegisterClassExA(&wc);
    g_class_reg = 1;
}

/* ------------------------------------------------------------------ */
/* Finding the ROM                                                     */
/* ------------------------------------------------------------------ */

static BOOL nn_file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/*
 * Walk NN_ROM_CANDIDATES and take the first one that is there.
 *
 * The index of the winner comes back through which_out so the caller can
 * say WHICH firmware booted. That is not decoration: the whole point of
 * the fallback is that the tab keeps working when the preferred firmware
 * is missing, and a silent fallback is a fallback nobody notices until
 * they are wondering why the boot got slow again.
 */
/* Resolve candidate i to a full path. Returns FALSE if it is not there. */
static BOOL nn_rom_path(int i, char *out, size_t cap)
{
    char base[MAX_PATH], cand[MAX_PATH], *slash;
    DWORD n;
    if (i < 0 || i >= NN_ROM_COUNT) return FALSE;
    n = GetModuleFileNameA(NULL, base, (DWORD)sizeof base);
    if (n == 0 || n >= sizeof base) return FALSE;
    slash = strrchr(base, '\\');
    if (!slash) return FALSE;
    *slash = '\0';
    snprintf(cand, sizeof cand, "%s\\%s", base, NN_ROM_CANDIDATES[i].path);
    if (!nn_file_exists(cand)) return FALSE;
    snprintf(out, cap, "%s", cand);
    return TRUE;
}

static BOOL nn_find_rom(char *out, size_t cap, int *which_out)
{
    int i;
    if (which_out) *which_out = -1;
    for (i = 0; i < NN_ROM_COUNT; i++) {
        if (nn_rom_path(i, out, cap)) {
            if (which_out) *which_out = i;
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* The connection fields                                               */
/* ------------------------------------------------------------------ */

/*
 * Measure text in the font the controls are actually wearing.
 *
 * The row is laid out from these two, rather than from pixel counts
 * chosen by eye, so it survives a font change and a different DPI. Both
 * take the content window because that is the only HWND the layout
 * function is handed, and a DC from it carries the right device.
 */
static int nn_text_w(HWND ref, const char *s)
{
    HDC   dc;
    SIZE  sz = { 0, 0 };
    HFONT old = NULL;
    if (!ref || !s) return 0;
    dc = GetDC(ref);
    if (!dc) return 0;
    if (g_hFontUI) old = (HFONT)SelectObject(dc, g_hFontUI);
    GetTextExtentPoint32A(dc, s, (int)strlen(s), &sz);
    if (old) SelectObject(dc, old);
    ReleaseDC(ref, dc);
    return (int)sz.cx;
}

static int nn_avg_char_w(HWND ref)
{
    HDC        dc;
    TEXTMETRICA tm;
    HFONT      old = NULL;
    int        w = 7;                       /* a sane floor if this fails */
    if (!ref) return w;
    dc = GetDC(ref);
    if (!dc) return w;
    if (g_hFontUI) old = (HFONT)SelectObject(dc, g_hFontUI);
    if (GetTextMetricsA(dc, &tm) && tm.tmAveCharWidth > 0)
        w = tm.tmAveCharWidth;
    if (old) SelectObject(dc, old);
    ReleaseDC(ref, dc);
    return w;
}

/* Defined below with the rest of the mode handling; needed here because
 * the serial port list has to exist before the saved port can be found in
 * it. */
static void nn_fill_com_list(void);
static int  nn_selected_mode(void);

/*
 * THE TAB OPENS ON THE GREEK TIMES, EVERY TIME.
 *
 * Mode, host, port and channel are NOT remembered between sessions, and
 * that is a deliberate reversal of what this did yesterday. Remembering
 * them turned out to do harm: an operator who had been pointing a serial
 * bridge at a machine on their own desk closed the Suite and reopened it
 * to find the emulator tab still carrying that address, with the Greek
 * Times one gone unless they happened to know it by heart. A default that
 * a previous session can quietly overwrite is not a default.
 *
 * So the flagship values are constants and the tab starts on them:
 * Emulator mode, nabu.greektimes.ca, 5816, channel 1. Anything else is
 * one session's worth of typing and does not outlive it.
 *
 * THE SERIAL PORT IS THE ONE EXCEPTION, and it is the exception because
 * it cannot shadow anything. There is no flagship COM port to lose, the
 * right one is a property of the operator's desk rather than of the
 * Suite, and finding it again means opening Device Manager. It is
 * remembered by name; if that port is not there at launch the first
 * enumerated port is chosen instead.
 */
/*
 * The three content-source fields, back to the Greek Times.
 *
 * Its own function because two things want it now: the tab opening, and
 * Disconnect returning the tab to the state the tab opens in. The serial
 * port and the mode are deliberately NOT here; see the callers.
 */
static void nn_reset_connection_fields(void)
{
    char v[32];
    SetWindowTextA(g_hHostEdit, NABU_CORE_DEFAULT_HOST);
    snprintf(v, sizeof v, "%d", NABU_CORE_DEFAULT_PORT);
    SetWindowTextA(g_hPortEdit, v);
    snprintf(v, sizeof v, "%d", NABU_CORE_DEFAULT_CHANNEL);
    SetWindowTextA(g_hChanEdit, v);
}

static void nn_load_fields(void)
{
    char v[MAX_PATH];

    nn_reset_connection_fields();
    SendMessageA(g_hModeCombo, CB_SETCURSEL, NN_MODE_EMULATOR, 0);

    /* nn_fill_com_list already selects the first port when it has one,
     * so a saved port that has since been unplugged falls back to that
     * without any extra handling here. */
    nn_fill_com_list();
    if (nn_reg_read("NabuSerialPort", v, sizeof v) && v[0]) {
        LRESULT ix = SendMessageA(g_hComCombo, CB_FINDSTRINGEXACT,
                                  (WPARAM)-1, (LPARAM)v);
        if (ix != CB_ERR)
            SendMessageA(g_hComCombo, CB_SETCURSEL, (WPARAM)ix, 0);
    }

    /*
     * Clear out what earlier builds wrote, so a profile that ran the
     * 2026-08-07 or 2026-08-08 Suite does not keep dead values under the
     * Suite's key forever. Deleting a value that is not there fails
     * harmlessly, so this needs no first-run flag.
     */
    {
        HKEY k;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, NN_REG_KEY, 0, KEY_SET_VALUE,
                          &k) == ERROR_SUCCESS) {
            RegDeleteValueA(k, "NabuHost");
            RegDeleteValueA(k, "NabuPort");
            RegDeleteValueA(k, "NabuChannel");
            RegDeleteValueA(k, "NabuMode");
            RegDeleteValueA(k, "NabuFirmware");
            RegCloseKey(k);
        }
    }
}

/* Only the serial port survives the session. See nn_load_fields. */
static void nn_save_fields(void)
{
    char    v[MAX_PATH];
    LRESULT sel;

    sel = SendMessageA(g_hComCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR &&
        SendMessageA(g_hComCombo, CB_GETLBTEXTLEN, (WPARAM)sel, 0)
            < (LRESULT)sizeof v) {
        SendMessageA(g_hComCombo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)v);
        nn_reg_write("NabuSerialPort", v);
    }
}

/*
 * Lock the four fields while a machine is running.
 *
 * Host, port and channel are read once, at connect, and the channel is
 * negotiated with the adapter before the Z80 executes anything, so a
 * field edited mid-session could not take effect and would only lie about
 * what the machine is doing. Locking them says so.
 */
/*
 * Show or hide every control the tab owns.
 *
 * One list, called from both activate and deactivate, because the module
 * state rule is that controls are hidden and shown and never destroyed,
 * and a tab with eleven controls in two hand-maintained lists is a tab
 * where one of them eventually gets left behind on screen.
 */
static void nn_show_controls(int how)
{
    HWND all[] = { g_hStartBtn, g_hStopBtn, g_hScreen,
                   g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit,
                   g_hChanLbl, g_hChanEdit,
                   g_hModeLbl, g_hModeCombo, g_hComLbl, g_hComCombo,
                   g_hGrpSrcLbl, g_hGrpLinkLbl };
    int k;
    for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
        if (all[k]) ShowWindow(all[k], how);
}

/* Which mode the picker is pointing at right now. */
/*
 * The mode is read as an INDEX, never as the label text, which is why the
 * entry could be renamed to "Real NABU PC (serial)" without touching a
 * line of logic. The two entries are added in NN_MODE_EMULATOR then
 * NN_MODE_SERIAL order and the enum is those indices, so the selection is
 * the mode. Keep it that way: a comparison against a display string is a
 * rename waiting to break something.
 */
static int nn_selected_mode(void)
{
    LRESULT sel;
    if (!g_hModeCombo) return NN_MODE_EMULATOR;
    sel = SendMessageA(g_hModeCombo, CB_GETCURSEL, 0, 0);
    return (sel == NN_MODE_SERIAL) ? NN_MODE_SERIAL : NN_MODE_EMULATOR;
}

/* Rebuild the serial port list from whatever is plugged in now. Called
 * on activate and whenever the mode changes, because a USB adapter
 * plugged in after the Suite started is the normal case, not the odd
 * one. */
static void nn_fill_com_list(void)
{
    char names[64][16];
    char keep[16] = "";
    int  n, i;
    LRESULT sel;

    if (!g_hComCombo) return;

    sel = SendMessageA(g_hComCombo, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR &&
        SendMessageA(g_hComCombo, CB_GETLBTEXTLEN, (WPARAM)sel, 0)
            < (LRESULT)sizeof keep)
        SendMessageA(g_hComCombo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)keep);

    SendMessageA(g_hComCombo, CB_RESETCONTENT, 0, 0);
    n = nabu_serial_enumerate(names, 64);
    for (i = 0; i < n; i++)
        SendMessageA(g_hComCombo, CB_ADDSTRING, 0, (LPARAM)names[i]);

    /* Keep the operator's choice across a re-enumeration if it is still
     * there, rather than silently moving them to a different port. */
    if (keep[0]) {
        LRESULT ix = SendMessageA(g_hComCombo, CB_FINDSTRINGEXACT,
                                  (WPARAM)-1, (LPARAM)keep);
        if (ix != CB_ERR) { SendMessageA(g_hComCombo, CB_SETCURSEL, (WPARAM)ix, 0); return; }
    }
    if (n > 0) SendMessageA(g_hComCombo, CB_SETCURSEL, 0, 0);
}

/*
 * Show the controls that belong to the current mode and grey the ones
 * that do not.
 *
 * The firmware picker is disabled in serial mode rather than hidden,
 * because a control that vanishes leaves the operator wondering whether
 * they broke something, while a greyed one with a reason on the status
 * line has answered the question before it is asked.
 */
static void nn_apply_mode(int mode, BOOL announce)
{
    BOOL serial = (mode == NN_MODE_SERIAL);
    BOOL idle   = !InterlockedCompareExchange(&g_running, 0, 0);

    /*
     * THE SERIAL PORT IS THE ONLY MODE-SPECIFIC FIELD, and it is greyed
     * rather than hidden so the strip does not reflow when the mode
     * changes. A row that jumps as you change a dropdown makes you doubt
     * what you just did.
     *
     * HOST, PORT AND CHANNEL ARE DELIBERATELY NOT TOUCHED HERE. They are
     * the content source, and in Real NABU mode the Suite is the Internet
     * Adapter: the real machine has no network and depends on the Suite
     * to fetch over TCP and relay down the cable. Greying them in serial
     * mode would leave the adapter with nowhere to fetch from. They stay
     * live, on the Greek Times defaults, in both modes.
     *
     * The "NABU link" heading follows its field, so in Emulator mode the
     * whole group reads as unavailable together rather than a live
     * heading sitting over a dead control.
     */
    EnableWindow(g_hComCombo,   idle && serial);
    EnableWindow(g_hComLbl,     serial);
    EnableWindow(g_hGrpLinkLbl, serial);

    if (serial) nn_fill_com_list();

    /*
     * INSTRUCTIONS GO IN THE WINDOW, NOT ON THE STATUS LINE.
     *
     * Switching to serial mode used to write a sentence of guidance here
     * while the picture area painted its own, so the operator was told
     * the same thing twice in two places at once. The window keeps its
     * sentence, because that is the surface with room for one and the
     * one they are already looking at. The status line goes back to
     * saying what the tab IS, which while nothing is connected is a
     * single word.
     */
    if (announce && idle) nn_set_state(NN_ST_IDLE);
    if (g_hScreen) InvalidateRect(g_hScreen, NULL, FALSE);
}

static void nn_enable_fields(BOOL on)
{
    EnableWindow(g_hHostEdit,  on);
    EnableWindow(g_hPortEdit,  on);
    EnableWindow(g_hChanEdit,  on);
    EnableWindow(g_hModeCombo, on);
    /* The two pickers are further gated by which mode is selected, so
     * they go through nn_apply_mode rather than being set here. Locking
     * the mode itself matters most: switching mode mid-session would
     * mean the running machine and the panel describing it disagreed. */
    if (on) nn_apply_mode(nn_selected_mode(), FALSE);
    else EnableWindow(g_hComCombo, FALSE);
}

/* ------------------------------------------------------------------ */
/* The emulation thread                                                */
/* ------------------------------------------------------------------ */

/* The diagnostic callback that used to push the core's messages into the
 * status line was removed on 2026-08-07. It was one of the two writers
 * that made the line alternate between formats, and the one thing it
 * carried that the operator needed, a connection that could not be made,
 * comes back through nabu_core_init's err string instead. */

/* ------------------------------------------------------------------ */
/* The bridge thread                                                   */
/* ------------------------------------------------------------------ */

/*
 * THE RELAY, AND WHY IT IS ONE THREAD AND NOT TWO.
 *
 * Both ends are configured to return immediately with whatever they
 * have: the serial handle through COMMTIMEOUTS, the socket through
 * FIONBIO. So one thread can carry both directions in a loop without
 * either ever parking, and there is no second thread to race with at
 * teardown, no shared buffer between them, and no window in which one
 * end is closed while the other is still writing into it.
 *
 * The cost is a poll. At 111860 baud a byte takes about 89
 * microseconds, and the loop sleeps a millisecond only when NOTHING
 * moved in either direction, so a busy link never sleeps and an idle one
 * costs almost nothing. A millisecond of added latency on an idle link
 * is invisible to a machine whose own protocol waits a round trip to
 * Montreal between packets.
 */
static DWORD WINAPI nn_bridge_worker(LPVOID param)
{
    nabu_serial_config_t cfg;
    char err[256] = "";
    uint64_t last_report = 0;
    ULONGLONG next_tick;
    (void)param;

    memset(&cfg, 0, sizeof cfg);
    cfg.com_port = g_cfg_com;
    cfg.host     = g_cfg_host;
    cfg.port     = g_cfg_port;
    cfg.channel  = g_cfg_channel;

    if (nabu_serial_open(&cfg, nn_log_sink, NULL, err, sizeof err) != 0) {
        nn_log_add(err);
        snprintf(g_fail_reason, sizeof g_fail_reason, "%s", err);
        nn_set_state(NN_ST_FAILED);
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    nn_set_state(NN_ST_BRIDGING);
    next_tick = GetTickCount64() + 10000;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        int moved = nabu_serial_pump();
        if (moved < 0) {
            nn_log_add("The link dropped. Disconnecting.");
            break;
        }
        if (moved == 0) Sleep(1);

        /*
         * A byte counter every ten seconds, and only when it has
         * changed. Often enough to show the link is alive, rare enough
         * that the log stays readable over an evening.
         */
        if (GetTickCount64() >= next_tick) {
            uint64_t tn = 0, ts = 0;
            nabu_serial_counts(&tn, &ts);
            if (tn + ts != last_report) {
                char msg[128];
                snprintf(msg, sizeof msg,
                         "%llu bytes to the NABU, %llu bytes to the server.",
                         (unsigned long long)tn, (unsigned long long)ts);
                nn_log_add(msg);
                last_report = tn + ts;
            }
            next_tick = GetTickCount64() + 10000;
        }
    }

    nabu_serial_close();
    nn_log_add("Serial port and connection closed.");
    InterlockedExchange(&g_running, 0);
    nn_set_state(NN_ST_IDLE);
    return 0;
}

static DWORD WINAPI nn_worker(LPVOID param)
{
    nabu_core_config_t cfg;
    char     err[256] = "";
    char     rom[MAX_PATH];
    int      rom_which = -1;
    LARGE_INTEGER qpf, start, now, ref_wall;
    double   ref_emu = 0.0;
    uint64_t frame_no = 0;
    (void)param;

    /* nn_start resolved these on the UI thread and the worker did not
     * exist yet, so reading them here needs no lock. */
    rom_which = g_cfg_rom_which;
    (void)rom_which;
    snprintf(rom, sizeof rom, "%s", g_cfg_rom);
    if (!rom[0]) {
        snprintf(g_fail_reason, sizeof g_fail_reason,
                 "No firmware found");
        nn_set_state(NN_ST_FAILED);
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    /*
     * The core's diagnostics are deliberately NOT routed to the status
     * line any more. They were the second writer that made the line
     * alternate: every modem and adapter message overwrote whatever the
     * state was. What they carried that mattered, a connection that could
     * not be made, arrives here as the err string instead.
     */
    nabu_core_set_diag(NULL, NULL);

    memset(&cfg, 0, sizeof cfg);
    cfg.host     = g_cfg_host;
    cfg.port     = g_cfg_port;
    cfg.channel  = g_cfg_channel;
    cfg.rom_path = rom;

    if (nabu_core_init(&cfg, err, sizeof err) != 0) {
        snprintf(g_fail_reason, sizeof g_fail_reason, "%s",
                 err[0] ? err : "could not start");
        nn_set_state(NN_ST_FAILED);
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    /* The machine is built either way; without a wire it will sit at its
     * firmware's own error path, which is worth naming rather than
     * calling "Booting" forever. */
    nn_set_state(nabu_core_modem_up() ? NN_ST_BOOTING : NN_ST_NOLINK);

    nn_audio_open();

    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&start);
    ref_wall = start;
    ref_emu  = 0.0;

    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        double emu, behind;

        nn_key_drain();
        nabu_core_run_frame();
        frame_no++;

        /* Convert into the DIB under the lock, then ask for a repaint.
         * Converting here rather than in the paint handler is what keeps
         * the machine single-threaded. */
        EnterCriticalSection(&g_lock);
        if (g_dib_bits) {
            nabu_core_copy_frame((uint32_t *)g_dib_bits);
            g_border       = nabu_core_border_rgb();
            g_shown_serial = frame_no;
            g_have_frame   = 1;
        }
        LeaveCriticalSection(&g_lock);

        /* GDI caches DIB writes made outside its own calls; without this
         * a blit can show a stale frame. Cheap, once a frame. */
        GdiFlush();
        if (g_hScreen) InvalidateRect(g_hScreen, NULL, FALSE);

        /*
         * PACING: 1x, ALWAYS.
         *
         * One emulated frame is one sixtieth of a second of the NABU's
         * own time, so the thread simply waits until that much wall
         * clock has passed before running the next one. There is no
         * fast path, no burst, and no state to be in: the emulated
         * clock and the real clock advance together for the whole
         * session.
         *
         * That the firmware's cycle-counted timeouts survive is now a
         * consequence rather than a thing to arrange. The reason to run
         * at 1x is simpler and stricter: the guest program itself keeps
         * time. The MGT front page ticks a Montreal clock off its own
         * cycles, and the operator can see it beside a real one.
         */
        emu = (double)frame_no / 60.0;

        for (;;) {
            double since_ref, ahead;
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
            QueryPerformanceCounter(&now);
            since_ref = (double)(now.QuadPart - ref_wall.QuadPart) /
                        (double)qpf.QuadPart;
            ahead = (emu - ref_emu) - since_ref;
            if (ahead <= 0.0) break;
            /*
             * Sleep for the bulk of the wait and spin only for the last
             * couple of milliseconds, so a frame costs the host almost
             * nothing. Sleep(1) with the default timer resolution can
             * overshoot, which is why the threshold is 2 ms and not the
             * whole remainder: overshooting a frame boundary would show
             * up as a stutter and, worse, as lost emulated time.
             */
            if (ahead > 0.002) Sleep(1);
            else               Sleep(0);
        }

        QueryPerformanceCounter(&now);

        /*
         * If the host could not keep up, forgive the lost time instead
         * of chasing it. Chasing it would mean running faster than real
         * time for a while, which is the very thing this loop is here to
         * refuse. A dropped moment is invisible; a clock that gains is
         * not.
         */
        behind = (double)(now.QuadPart - ref_wall.QuadPart) /
                 (double)qpf.QuadPart - (emu - ref_emu);
        if (behind > NN_RESYNC_S) {
            ref_wall = now;
            ref_emu  = emu;
        }

        /*
         * Sound. One emulated frame is exactly NN_WBUF_SAMP samples at
         * NABU_AUDIO_RATE, and at 1x a frame takes a real sixtieth of a
         * second, so the chip feeds the device at precisely the rate the
         * device drains it. Nothing is gated and nothing is dropped: the
         * PSG is audible from the first frame of the boot.
         */
        if (g_wave) {
            static short pcm[NN_WBUF_SAMP];
            int s;
            for (s = 0; s < NN_WBUF_SAMP; s++)
                pcm[s] = nabu_core_psg_sample();
            nn_audio_push(pcm, NN_WBUF_SAMP);
        }

        /*
         * BOOTING BECOMES READY WHEN THE PROGRAM COUNTER LEAVES THE
         * FIRMWARE.
         *
         * This is the one honest "it is up" signal the machine offers,
         * and it is the same one the headless harness measures boot times
         * with. The firmware lives in the bottom 4 KB and is shadowed
         * there in RAM, the loaded image starts at $140D and is entered
         * at $1410, and the firmware runs with interrupts disabled the
         * whole way, so a PC at or above $1400 means the channel's
         * program is running and nothing else can mean that.
         *
         * It is latched. Once the program is running the PC wanders all
         * over the map, including back down through low memory, and a
         * state word that flickered between Booting and Ready would be
         * exactly the switching this pass exists to remove.
         *
         * Checked once a frame, which costs a struct copy, and only until
         * it fires.
         */
        if (InterlockedCompareExchange(&g_state, 0, 0) == NN_ST_BOOTING) {
            nabu_core_state_t st;
            nabu_core_get_state(&st);
            if (st.pc >= 0x1400 && st.pc < 0xC000)
                nn_set_state(NN_ST_READY);
        }
    }

    nn_audio_close();
    nabu_core_shutdown();
    InterlockedExchange(&g_running, 0);
    /* The worker only ever leaves this loop because the stop event was
     * set, which means Disconnect or leaving the tab, so idle is the
     * truthful state. A failure earlier returned before reaching here
     * and left its own reason on the line. */
    nn_set_state(NN_ST_IDLE);
    return 0;
}

/*
 * Read the four fields, check them, and latch them for the worker.
 * Returns 0 and puts the reason in the status line if anything is wrong,
 * in which case nothing is started and the fields stay editable.
 */
static int nn_capture_fields(void)
{
    char v[MAX_PATH];
    int  which;

    GetWindowTextA(g_hHostEdit, v, sizeof v);
    {   /* Trim: a pasted host with a stray space resolves to nothing and
         * the failure looks like the server is down. */
        char *s = v, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = '\0';
        if (!*s) { nn_fail("No host given."); return 0; }
        snprintf(g_cfg_host, sizeof g_cfg_host, "%s", s);
    }

    GetWindowTextA(g_hPortEdit, v, sizeof v);
    g_cfg_port = atoi(v);
    if (g_cfg_port < 1 || g_cfg_port > 65535) {
        nn_fail("Port must be between 1 and 65535.");
        return 0;
    }

    GetWindowTextA(g_hChanEdit, v, sizeof v);
    g_cfg_channel = atoi(v);
    /* 256 is the adapter's own ceiling, from nabud's image_channel_select. */
    if (g_cfg_channel < 1 || g_cfg_channel > 256) {
        nn_fail("Channel must be between 1 and 256.");
        return 0;
    }

    g_cfg_mode = nn_selected_mode();

    /*
     * In serial mode the firmware is the real machine's own, in a socket
     * on its main board, and nothing the Suite chooses. What has to be
     * right instead is the cable.
     */
    if (g_cfg_mode == NN_MODE_SERIAL) {
        LRESULT sel = SendMessageA(g_hComCombo, CB_GETCURSEL, 0, 0);
        g_cfg_com[0] = '\0';
        g_cfg_rom[0] = '\0';
        g_cfg_rom_which = -1;
        if (sel == CB_ERR ||
            SendMessageA(g_hComCombo, CB_GETLBTEXTLEN, (WPARAM)sel, 0)
                >= (LRESULT)sizeof g_cfg_com) {
            nn_fail("No serial port chosen.");
            return 0;
        }
        SendMessageA(g_hComCombo, CB_GETLBTEXT, (WPARAM)sel,
                     (LPARAM)g_cfg_com);
        if (!g_cfg_com[0]) {
            nn_fail("No serial port chosen.");
            return 0;
        }
        return 1;
    }

    /*
     * THE FIRMWARE IS NOT A CHOICE ANY MORE.
     *
     * MGT IPL is the only firmware the emulator has any reason to boot:
     * it is the Suite's own, it is built from source in this tree, and
     * it reaches the channel in a third of a second where stock OpenNabu
     * takes ten. A picker offering one sensible answer is not a choice,
     * it is a control to get wrong, so it is gone.
     *
     * The search path underneath it stays exactly as it was, and stays
     * for one reason only: it is the thing that means a missing or
     * unreadable mgtipl.bin cannot leave the tab unable to boot at all.
     * It is no longer user-facing, but it is NOT silent. If anything but
     * MGT IPL is what actually loads, the status line says so for the
     * whole session, because a tab quietly running different firmware is
     * how a nine second boot gets mistaken for a slow network.
     */
    if (!nn_find_rom(g_cfg_rom, sizeof g_cfg_rom, &which)) {
        nn_fail("No NABU firmware found. Expected "
                "firmware\\mgtipl\\mgtipl.bin.");
        g_cfg_rom[0] = '\0';
        return 0;
    }
    g_cfg_rom_which = which;
    return 1;
}

static void nn_start(void)
{
    if (InterlockedCompareExchange(&g_running, 0, 0)) return;

    if (!nn_capture_fields()) return;
    nn_save_fields();
    nn_enable_fields(FALSE);

    if (g_cfg_mode == NN_MODE_SERIAL) {
        char msg[200];
        nn_log_clear();
        snprintf(msg, sizeof msg,
                 "Bridging %s to %s port %d, channel %d.",
                 g_cfg_com, g_cfg_host, g_cfg_port, g_cfg_channel);
        nn_log_add(msg);
    } else {
        nn_dib_create();
        EnterCriticalSection(&g_lock);
        g_have_frame = 0;
        LeaveCriticalSection(&g_lock);
    }

    if (!g_stopEvent) g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_stopEvent);
    InterlockedExchange(&g_running, 1);
    nn_set_state(NN_ST_CONNECTING);
    g_thread = CreateThread(NULL, 0,
                            (g_cfg_mode == NN_MODE_SERIAL) ? nn_bridge_worker
                                                           : nn_worker,
                            NULL, 0, NULL);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        snprintf(g_fail_reason, sizeof g_fail_reason,
                 "Could not start the %s thread",
                 (g_cfg_mode == NN_MODE_SERIAL) ? "bridge" : "emulation");
        nn_set_state(NN_ST_FAILED);
        /* Nothing is running, so the fields go back to being editable. */
        nn_enable_fields(TRUE);
        return;
    }
    if (g_hStartBtn) EnableWindow(g_hStartBtn, FALSE);
    if (g_hStopBtn)  EnableWindow(g_hStopBtn,  TRUE);
    if (g_hScreen)   { SetFocus(g_hScreen); InvalidateRect(g_hScreen, NULL, FALSE); }
}

static void nn_stop(void)
{
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        /* The worker can be inside a socket read; give it room, then
         * stop waiting rather than hanging the UI forever. */
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    InterlockedExchange(&g_running, 0);

    /*
     * DROP THE LAST FRAME.
     *
     * Without this the picture area kept showing whatever the machine had
     * drawn when it stopped, because the paint path only falls through to
     * the idle message when there is no frame to show. A still picture of
     * a machine that is no longer running is the worst of both readings:
     * it looks like the tab is still connected, and it is not.
     *
     * Under the lock, because the emulation thread writes this same flag.
     * The worker has already been waited for by this point, so there is
     * nothing left to race with, but taking the lock is what makes that
     * true rather than merely likely.
     */
    if (g_lock_init) {
        EnterCriticalSection(&g_lock);
        g_have_frame = 0;
        LeaveCriticalSection(&g_lock);
    }

    if (g_hStartBtn) EnableWindow(g_hStartBtn, TRUE);
    if (g_hStopBtn)  EnableWindow(g_hStopBtn,  FALSE);
    /* Back to idle: the connection is gone, so the fields describe the
     * NEXT connection again and are editable. */
    if (g_controls_created) nn_enable_fields(TRUE);
    if (g_hScreen)   InvalidateRect(g_hScreen, NULL, FALSE);
}

/*
 * THE DISCONNECT BUTTON, which is more than a teardown.
 *
 * nn_stop is the shared teardown and runs on three paths: this button,
 * leaving the tab, and the Suite shutting down. Only the button means
 * "put this tab back the way it opens", so the resetting lives here and
 * not in nn_stop. Switching to another tab and back keeps whatever the
 * operator typed, which is what the last two passes established.
 *
 * The mode is deliberately NOT reset. Disconnecting from serial mode
 * leaves you in serial mode, showing the serial idle instruction, because
 * the mode is a statement about which machine is on the desk and pressing
 * Disconnect does not change what is on the desk.
 */
static void nn_disconnect_to_idle(void)
{
    nn_stop();
    nn_reset_connection_fields();
    /* The bridge log belongs to the session that just ended. Clearing it
     * is what lets the serial idle instruction show through, the same way
     * dropping the frame does for the emulator. */
    nn_log_clear();
    if (g_hScreen) InvalidateRect(g_hScreen, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

void nabu_native_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int btn_h  = 30;
    int screen_h;
    (void)content;
    if (!g_controls_created) return;

    /*
     * TWO ROWS, GROUPED BY DIRECTION.
     *
     *   row 1   Mode: [picker]        Content source  Host: [....................]
     *   row 2   Port: [..] Channel: [.]  NABU link  Serial port: [COM3]  Connect  Disconnect
     *
     * The two grouping labels are what the arrangement is for. Host, Port
     * and Channel are the content source, upstream over TCP, and they are
     * live in both modes because in Real NABU mode the Suite is the
     * adapter and has to fetch from somewhere. The serial port is the
     * NABU link, downstream over a cable, and it is the only field that
     * is mode-specific.
     *
     * "Content source" leads the group on row 1 and is NOT repeated over
     * Port and Channel on row 2. Repeating it would be the more literal
     * reading of the grouping, and it is the more cluttered one: the
     * strip would then carry two identical headings and the eye would
     * have to work out whether they meant different things. One heading
     * on the row where the group starts, and the group simply continuing
     * onto the next row, is how a form reads.
     *
     * EVERY WIDTH IS MEASURED. The labels were hand sized once and every
     * one of them was too narrow for the Suite's font, which is how the
     * strip came to read "ort:" and "hannel:". Measured with Consolas 16
     * at the default 980x770 window, which gives 932 usable pixels: row 1
     * spends 417 on everything but the host box and hands the remaining
     * 515 to it, which is 62 characters; row 2 needs 731 and has 201 to
     * spare.
     */
    {
        const int lbl_h = 22, fld_h = NN_ROW_H, gap = 8;
        const int w_btn = 95;
        int w_modelbl, w_hostlbl, w_portlbl, w_chanlbl, w_comlbl;
        int w_grpsrc, w_grplink;
        int w_mode, w_host, w_port, w_chan, w_com;
        int avg, x;
        int y1     = 8;
        int y2     = 8 + NN_ROW_H + NN_ROW_GAP;
        int y1_lbl = y1 + (fld_h - lbl_h) / 2;
        int y2_lbl = y2 + (30 - lbl_h) / 2;
        int y2_fld = y2 + (30 - fld_h) / 2;

        avg       = nn_avg_char_w(content);
        w_grpsrc  = nn_text_w(content, "Content source") + 8;
        w_grplink = nn_text_w(content, "NABU link")      + 8;
        w_modelbl = nn_text_w(content, "Mode:")          + 8;
        w_hostlbl = nn_text_w(content, "Host:")          + 8;
        w_portlbl = nn_text_w(content, "Port:")          + 8;
        w_chanlbl = nn_text_w(content, "Channel:")       + 8;
        w_comlbl  = nn_text_w(content, "Serial port:")   + 8;

        w_port = avg *  6 + 12;
        w_chan = avg *  4 + 12;
        w_mode = nn_text_w(content, "Real NABU PC (serial)") +
                 GetSystemMetrics(SM_CXVSCROLL) + 16;
        /* Wide enough for the longest port name anyone plausibly has,
         * which on Windows runs to COM256. */
        w_com  = nn_text_w(content, "COM256") +
                 GetSystemMetrics(SM_CXVSCROLL) + 16;

        /*
         * THE HOST BOX IS A FIXED 30 CHARACTERS, and does not grow to
         * fill the row any more.
         *
         * A box that took the whole row was a box sized by how much space
         * happened to be left rather than by what goes in it, and at the
         * default window that was 62 characters for a name that is
         * eighteen. Thirty is generous for a host name and leaves row 1
         * looking like a row of fields rather than one field and some
         * offcuts.
         *
         * IT IS A VISIBLE WIDTH, NOT A LIMIT. No EM_LIMITTEXT is set, so
         * a longer name types in perfectly well and the edit control
         * scrolls horizontally the way every Win32 edit does; thirty
         * characters is simply how many are on show at once.
         */
        w_host = avg * 30 + 12;

        /* Row 1: the mode, then the content source, starting with Host. */
        x = margin;
        MoveWindow(g_hModeLbl,    x, y1_lbl, w_modelbl, lbl_h, TRUE);
        x += w_modelbl + 4;
        MoveWindow(g_hModeCombo,  x, y1, w_mode, fld_h + 120, TRUE);
        x += w_mode + gap;
        MoveWindow(g_hGrpSrcLbl,  x, y1_lbl, w_grpsrc, lbl_h, TRUE);
        x += w_grpsrc + gap;
        MoveWindow(g_hHostLbl,    x, y1_lbl, w_hostlbl, lbl_h, TRUE);
        x += w_hostlbl + 4;
        MoveWindow(g_hHostEdit,   x, y1, w_host, fld_h, TRUE);

        /* Row 2: the rest of the content source, then the NABU link, then
         * the buttons. */
        x = margin;
        MoveWindow(g_hPortLbl,    x, y2_lbl, w_portlbl, lbl_h, TRUE);
        x += w_portlbl + 4;
        MoveWindow(g_hPortEdit,   x, y2_fld, w_port, fld_h, TRUE);
        x += w_port + gap;
        MoveWindow(g_hChanLbl,    x, y2_lbl, w_chanlbl, lbl_h, TRUE);
        x += w_chanlbl + 4;
        MoveWindow(g_hChanEdit,   x, y2_fld, w_chan, fld_h, TRUE);
        x += w_chan + gap;
        MoveWindow(g_hGrpLinkLbl, x, y2_lbl, w_grplink, lbl_h, TRUE);
        x += w_grplink + gap;
        MoveWindow(g_hComLbl,     x, y2_lbl, w_comlbl, lbl_h, TRUE);
        x += w_comlbl + 4;
        MoveWindow(g_hComCombo,   x, y2_fld, w_com, fld_h + 200, TRUE);
        x += w_com + gap;
        MoveWindow(g_hStartBtn,   x, y2, w_btn, btn_h, TRUE);
        x += w_btn + gap;
        MoveWindow(g_hStopBtn,    x, y2, w_btn, btn_h, TRUE);
    }

    /* The picture runs from under the connection row to the bottom of the
     * content, less one margin. The 22 pixel status strip that used to
     * live down here went to the Suite's own status bar on 2026-08-07 and
     * the height came back to the picture. */
    screen_h = h - NN_STRIP_H - margin;
    if (screen_h < 0) screen_h = 0;
    MoveWindow(g_hScreen, margin, NN_STRIP_H, w - 2 * margin, screen_h, TRUE);
}

void nabu_native_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }

    g_hContent = content;
    nn_register_class(hInst);

    if (g_controls_created) {
        nn_show_controls(SW_SHOW);
    } else {
        g_hModeLbl = CreateWindowA("STATIC", "Mode:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hModeCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 10, 120, content, (HMENU)(INT_PTR)IDC_NN_MODE, hInst, NULL);
        SendMessageA(g_hModeCombo, CB_ADDSTRING, 0, (LPARAM)"Emulator");
        SendMessageA(g_hModeCombo, CB_ADDSTRING, 0,
                     (LPARAM)"Real NABU PC (serial)");
        SendMessageA(g_hModeCombo, CB_SETCURSEL, NN_MODE_EMULATOR, 0);

        /* The two grouping labels. No trailing colon, which is what marks
         * them as headings for a group rather than labels for a field:
         * every field label in this strip ends in one and neither of
         * these does. */
        g_hGrpSrcLbl = CreateWindowA("STATIC", "Content source",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hGrpLinkLbl = CreateWindowA("STATIC", "NABU link",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);

        g_hComLbl = CreateWindowA("STATIC", "Serial port:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hComCombo = CreateWindowA("COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 10, 200, content, (HMENU)(INT_PTR)IDC_NN_COM, hInst, NULL);

        g_hHostLbl = CreateWindowA("STATIC", "Host:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hHostEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_HOST, hInst, NULL);
        g_hPortLbl = CreateWindowA("STATIC", "Port:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_NUMBER,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_PORT, hInst, NULL);
        g_hChanLbl = CreateWindowA("STATIC", "Channel:",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_LBL, hInst, NULL);
        g_hChanEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_NUMBER,
            0, 0, 10, 10, content, (HMENU)(INT_PTR)IDC_NN_CHAN, hInst, NULL);
        g_hStartBtn = CreateWindowA("BUTTON", "Connect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 95, 30, content,
            (HMENU)(INT_PTR)IDC_NN_CONNECT, hInst, NULL);
        g_hStopBtn = CreateWindowA("BUTTON", "Disconnect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | WS_DISABLED,
            0, 0, 95, 30, content,
            (HMENU)(INT_PTR)IDC_NN_DISCONN, hInst, NULL);
        /* WS_TABSTOP so the screen can take the keyboard; the guest
         * needs it and the key handling above depends on focus. */
        g_hScreen = CreateWindowExA(WS_EX_CLIENTEDGE, NN_SCREEN_CLASS, "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 100, 100, content,
            (HMENU)(INT_PTR)IDC_NN_SCREEN, hInst, NULL);
        /* No status STATIC here. The tab writes to the Suite's own status
         * bar, which is the only one now. See the WM_APP+1 handler. */

        if (g_hFontUI) {
            HWND all[] = { g_hStartBtn, g_hStopBtn,
                           g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit,
                           g_hChanLbl, g_hChanEdit,
                           g_hModeLbl, g_hModeCombo, g_hComLbl, g_hComCombo,
                           g_hGrpSrcLbl, g_hGrpLinkLbl };
            int k;
            for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
                SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }
        nn_load_fields();
        /* The tab always opens in Emulator mode, so this is really just
         * getting the serial port picker into the greyed state that goes
         * with it. Silently: a tab opening is not a decision being made. */
        g_cfg_mode = nn_selected_mode();
        nn_apply_mode(g_cfg_mode, FALSE);
        g_controls_created = 1;
    }

    /* Opening the tab claims the status line from whichever tab had it
     * last, so it describes what the operator is now looking at. */
    if (!InterlockedCompareExchange(&g_running, 0, 0))
        nn_set_status("Disconnected");

    GetClientRect(content, &rc);
    nabu_native_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
}

void nabu_native_module_deactivate(HWND content)
{
    (void)content;
    /* Leaving the tab stops the machine. It holds a live socket to the
     * channel server and burns a core pacing itself; both are things to
     * stop when nobody is looking at them. Same call the shipping tab's
     * deactivate makes about its external process. */
    if (InterlockedCompareExchange(&g_running, 0, 0)) nn_stop();

    if (!g_controls_created) return;
    nn_show_controls(SW_HIDE);
}

BOOL nabu_native_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id   = LOWORD(wParam);
    int code = HIWORD(wParam);
    (void)content; (void)lParam;

    if (code == BN_CLICKED) {
        if (id == IDC_NN_CONNECT) { nn_start(); return TRUE; }
        if (id == IDC_NN_DISCONN) { nn_disconnect_to_idle(); return TRUE; }
    }
    /*
     * Changing mode repaints the picture area, because the two modes put
     * different things in it, and re-enumerates the serial ports, because
     * the adapter was very likely plugged in after the Suite started.
     * g_cfg_mode follows the picker while idle so the screen shows the
     * mode the operator is looking at, not the one they last ran.
     */
    if (code == CBN_SELCHANGE && id == IDC_NN_MODE) {
        if (!InterlockedCompareExchange(&g_running, 0, 0)) {
            g_cfg_mode = nn_selected_mode();
            nn_apply_mode(g_cfg_mode, TRUE);
        }
        return TRUE;
    }
    return FALSE;
}

BOOL nabu_native_module_has_unsaved(void)
{
    return FALSE;
}

void nabu_native_module_shutdown(void)
{
    if (!g_lock_init) return;
    nn_stop();
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = NULL; }
    nn_audio_close();
    nn_dib_destroy();
    /* The bold weight this tab derived for its idle headings. The shared
     * fonts belong to suite_fonts and are not ours to free; this one is. */
    if (g_hFontBold) { DeleteObject(g_hFontBold); g_hFontBold = NULL; }
}
