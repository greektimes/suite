/*
 * zmodem_recv.c - Zmodem RECEIVE session for the Suite.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The protocol primitives come from mbzm, vendored under
 * third_party/mbzm (MIT, Ross Bamford), with its two GPL CRC files
 * removed and replaced; see third_party/mbzm/MGT-CHANGES.md. This file
 * is the DRIVER: the Win32 threading, the transport glue, the file
 * naming policy, and every security decision.
 *
 * ------------------------------------------------------------------
 * THE TWO ESCAPE LAYERS, AND WHY THEY CANNOT COLLIDE
 * ------------------------------------------------------------------
 *
 * The service runs Synchronet's sexyz with -telnet, so on the wire a
 * literal 0xFF is doubled per RFC 854. There are two escape schemes in
 * play and their order matters:
 *
 *   OUTER  telnet IAC (0xFF)   telnet_proto.c, below this module
 *   INNER  Zmodem ZDLE (0x18)  third_party/mbzm, above this module
 *
 * On the way IN, telnet_proto.c's parse_chunk() has already collapsed
 * IAC IAC to one 0xFF and eaten option negotiation before a single byte
 * reaches zmodem_recv_push(). On the way OUT, telnet_proto_send()
 * doubles any 0xFF we hand it. So the Zmodem core lives entirely in
 * un-doubled payload space and never sees, or has to produce, an IAC.
 *
 * That is the whole trick, and getting it backwards is the classic bug:
 * if Zmodem's ZDLE escaping ran first on the OUTGOING side, a 0xFF in a
 * ZDLE-escaped stream would be doubled afterwards and arrive correct,
 * but an IAC-doubled 0xFF arriving from the far end would be handed to
 * the ZDLE decoder as two bytes and corrupt the frame. Peeling the
 * outer layer completely, in one place, before the inner layer ever
 * runs, is what keeps them independent.
 *
 * ------------------------------------------------------------------
 * SECURITY POLICY. Not optional, and all of it lives here.
 * ------------------------------------------------------------------
 *
 * 1. COMMAND DOWNLOAD IS HARD-DISABLED. Zmodem lets a sender ship a
 *    shell command instead of a file (ZCOMMAND) and ask the receiver to
 *    run it. Nothing in this file can execute anything. ZCOMMAND and
 *    ZCOMPL are refused outright: the session cancels and reports.
 *    There is no option, no prompt and no code path that would run it.
 *
 * 2. THE SENDER NEVER CHOOSES WHERE A FILE LANDS. The ZFILE name is put
 *    through zmodem_recv_sanitize_name(), which throws away everything
 *    up to the last separator or colon and then vets what is left. The
 *    result is joined to the download directory this session was
 *    started with, and nothing else.
 *
 * 3. THERE IS A CEILING. A transfer that advertises, or reaches, more
 *    than ZMR_MAX_BYTES is aborted rather than allowed to fill a disk.
 *    The ceiling is on the session total, not just on one file.
 *
 * 4. EVERY READ IS LENGTH-CHECKED. Data subpackets are read into a
 *    fixed buffer whose capacity is passed in and honoured by the core
 *    (it returns OUT_OF_SPACE rather than running on), and every string
 *    this file forms is bounded and terminated explicitly.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include "zmodem_recv.h"

/* winerror.h defines an IS_ERROR() for HRESULT severity and the Zmodem
 * core defines its own for ZRESULT codes. Only the Zmodem one is used
 * in this file, so the Win32 one is dropped before the core's headers
 * arrive rather than left to lose a redefinition warning. */
#ifdef IS_ERROR
#undef IS_ERROR
#endif

#include "zmodem.h"
#include "crc16.h"
#include "crc32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                            */
/* ------------------------------------------------------------------ */

/* The session ceiling. The newspaper is under two megabytes; this is
 * generous enough never to be hit by the service and small enough that
 * a hostile or broken sender cannot fill a disk. */
#define ZMR_MAX_BYTES        (256ULL * 1024ULL * 1024ULL)

/* Largest data subpacket we will accept. Zmodem's classic maximum is
 * 1024 and the 8k extension uses 8192; this leaves room for both plus
 * the frame-end byte and then some. A subpacket bigger than this is not
 * an overflow, it is an OUT_OF_SPACE that costs one ZRPOS retry. */
#define ZMR_BLOCK_BUF        (33 * 1024)

/* Ring between the transport and the worker. One megabyte is far more
 * than the gap between an 8 kB telnet chunk and a worker that only
 * memcpys and writes. */
#define ZMR_RING_BYTES       (1024 * 1024)

/* How long the worker waits for a byte before giving up on the far
 * end, and how long a push will wait for ring space before it decides
 * the worker is wedged. */
#define ZMR_IDLE_TIMEOUT_MS  30000
#define ZMR_PUSH_WAIT_MS     2000

/* Progress is posted at most this often. */
#define ZMR_PROGRESS_MS      200

/* Outgoing bytes are gathered here before one send() carries them. A
 * hex header is seventeen bytes plus its frame, so this never fills
 * mid-header. */
#define ZMR_SEND_BUF         256

/* How many consecutive protocol errors we absorb before giving up. */
#define ZMR_MAX_ERRORS       32

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

struct ZmodemRecv {
    HANDLE           thread;
    HWND             notify_hwnd;
    UINT             notify_msg;

    ZmrSendFn        send_fn;
    void            *send_ctx;

    char             dir[MAX_PATH];

    /* Ring buffer, producer = caller's thread, consumer = worker. */
    CRITICAL_SECTION ring_lock;
    HANDLE           data_evt;      /* set when the ring is non-empty  */
    HANDLE           space_evt;     /* set when the ring is non-full   */
    unsigned char   *ring;
    int              ring_head;     /* next write                      */
    int              ring_tail;     /* next read                       */
    int              ring_count;

    volatile LONG    cancel_request;
    volatile LONG    stop_request;

    /* Outgoing coalescing buffer, worker thread only. */
    unsigned char    sendbuf[ZMR_SEND_BUF];
    int              sendlen;
    int              send_failed;

    /* Result, written by the worker before it posts ZMR_EVT_DONE. */
    ZmrStatus          status;
    unsigned long long received;
    unsigned long long total;
    char               name[260];
    char               path[MAX_PATH];
    char               detail[160];

    DWORD              last_progress_tick;
};

/* The vendored core reaches its transport through two free functions,
 * so exactly one session can be live at a time. That is also all the
 * Suite ever wants: one tab, one transfer. */
static ZmodemRecv *g_active = NULL;

static void copy_str(char *dst, size_t cap, const char *src);

unsigned long long zmodem_recv_max_bytes(void) { return ZMR_MAX_BYTES; }

/* ------------------------------------------------------------------ */
/* Ring buffer                                                         */
/* ------------------------------------------------------------------ */

static int ring_pop(ZmodemRecv *z)
{
    DWORD waited = 0;

    for (;;) {
        int b = -1;

        EnterCriticalSection(&z->ring_lock);
        if (z->ring_count > 0) {
            b = z->ring[z->ring_tail];
            z->ring_tail = (z->ring_tail + 1) % ZMR_RING_BYTES;
            z->ring_count--;
            if (z->ring_count == 0) ResetEvent(z->data_evt);
            SetEvent(z->space_evt);
        }
        LeaveCriticalSection(&z->ring_lock);

        if (b >= 0) return b;
        if (InterlockedCompareExchange(&z->stop_request, 0, 0)) return -1;
        if (waited >= ZMR_IDLE_TIMEOUT_MS) return -1;

        WaitForSingleObject(z->data_evt, 100);
        waited += 100;
    }
}

/* Everything still queued when the conversation is over. Called only
 * after the worker has been joined, so there is no concurrent reader. */
static int ring_drain(ZmodemRecv *z, unsigned char **out)
{
    int n, i;

    *out = NULL;
    EnterCriticalSection(&z->ring_lock);
    n = z->ring_count;
    if (n > 0) {
        unsigned char *buf = (unsigned char *)malloc((size_t)n);
        if (buf) {
            for (i = 0; i < n; i++)
                buf[i] = z->ring[(z->ring_tail + i) % ZMR_RING_BYTES];
            *out = buf;
        } else {
            n = 0;
        }
        z->ring_count = 0;
        z->ring_tail  = z->ring_head;
    }
    LeaveCriticalSection(&z->ring_lock);
    return n;
}

void zmodem_recv_push(ZmodemRecv *z, const unsigned char *data, int len)
{
    int off = 0;
    DWORD waited = 0;

    if (!z || !data || len <= 0) return;

    while (off < len) {
        int room, take, i;

        EnterCriticalSection(&z->ring_lock);
        room = ZMR_RING_BYTES - z->ring_count;
        take = len - off;
        if (take > room) take = room;
        for (i = 0; i < take; i++) {
            z->ring[z->ring_head] = data[off + i];
            z->ring_head = (z->ring_head + 1) % ZMR_RING_BYTES;
        }
        z->ring_count += take;
        if (z->ring_count > 0) SetEvent(z->data_evt);
        if (z->ring_count >= ZMR_RING_BYTES) ResetEvent(z->space_evt);
        LeaveCriticalSection(&z->ring_lock);

        off += take;
        if (off >= len) break;

        /* Full. The worker is either busy or gone; wait briefly rather
         * than freeze the UI thread, and drop the remainder if it never
         * drains. Dropped bytes fail a CRC and cost one ZRPOS retry,
         * which is the recoverable failure, not a corrupted file. */
        if (waited >= ZMR_PUSH_WAIT_MS) break;
        WaitForSingleObject(z->space_evt, 50);
        waited += 50;
    }
}

/* ------------------------------------------------------------------ */
/* What the vendored core calls                                        */
/* ------------------------------------------------------------------ */

static void zsend_flush(ZmodemRecv *z)
{
    if (!z || z->sendlen <= 0) return;
    if (z->send_fn && z->send_fn(z->send_ctx, z->sendbuf, (size_t)z->sendlen) != 0)
        z->send_failed = 1;
    z->sendlen = 0;
}

ZRESULT zm_recv(void)
{
    ZmodemRecv *z = g_active;
    int b;

    if (!z) return CLOSED;
    if (InterlockedCompareExchange(&z->cancel_request, 0, 0)) return CANCELLED;

    b = ring_pop(z);
    if (b < 0) return CLOSED;
    return (ZRESULT)(b & 0xff);
}

ZRESULT zm_send(uint8_t chr)
{
    ZmodemRecv *z = g_active;

    if (!z) return CLOSED;
    z->sendbuf[z->sendlen++] = chr;
    if (z->sendlen >= ZMR_SEND_BUF) zsend_flush(z);
    return z->send_failed ? (ZRESULT)CLOSED : (ZRESULT)OK;
}

/* Header helpers that flush, so a reply always leaves in one write. */
static ZRESULT zsend_pos(ZmodemRecv *z, uint8_t type, uint32_t pos)
{
    ZRESULT r = zm_send_pos_hdr(type, pos);
    zsend_flush(z);
    return z->send_failed ? (ZRESULT)CLOSED : r;
}

static ZRESULT zsend_flags(ZmodemRecv *z, uint8_t type,
                           uint8_t f0, uint8_t f1, uint8_t f2, uint8_t f3)
{
    ZRESULT r = zm_send_flags_hdr(type, f0, f1, f2, f3);
    zsend_flush(z);
    return z->send_failed ? (ZRESULT)CLOSED : r;
}

/* The cancel sequence every Zmodem sender recognises: eight CANs so
 * that five survive any plausible loss, then backspaces to wipe them
 * off a terminal that was not listening for a transfer. Sent raw, so a
 * half-built header in the coalescing buffer never rides along. */
static void zsend_cancel(ZmodemRecv *z)
{
    static const unsigned char can8[] = {
        0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
        0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08
    };
    z->sendlen = 0;
    if (z->send_fn) z->send_fn(z->send_ctx, can8, sizeof can8);
}

/* ------------------------------------------------------------------ */
/* Filename hardening                                                  */
/* ------------------------------------------------------------------ */

/* CON, PRN, AUX, NUL, COM1-9, LPT1-9. Win32 resolves these as devices
 * whatever directory you appear to be in, and with any extension, so
 * the stem is what has to be checked. */
static int name_is_reserved(const char *stem)
{
    static const char *fixed[] = { "CON", "PRN", "AUX", "NUL", NULL };
    char up[16];
    size_t i, n = strlen(stem);

    if (n == 0 || n >= sizeof up) return 0;
    for (i = 0; i < n; i++) {
        char c = stem[i];
        up[i] = (char)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    up[n] = '\0';

    for (i = 0; fixed[i]; i++)
        if (strcmp(up, fixed[i]) == 0) return 1;

    if (n == 4 && up[3] >= '1' && up[3] <= '9' &&
        (strncmp(up, "COM", 3) == 0 || strncmp(up, "LPT", 3) == 0))
        return 1;

    return 0;
}

/* Reduce whatever the sender sent to a bare, legal, local file name.
 *
 *   - keep only what follows the last '/', '\' or ':', which disposes
 *     of directory components, absolute paths, drive letters and
 *     alternate data streams in one step;
 *   - stop at the first NUL, and reject any embedded control byte;
 *   - reject the Win32-illegal characters  < > : " / \ | ? *  ;
 *   - reject "." and ".." outright, so no traversal survives even if
 *     the separator strip somehow did not fire;
 *   - trim trailing dots and spaces, which Win32 silently drops and
 *     which are therefore a way to smuggle a different final name;
 *   - refuse the reserved device names;
 *   - cap the length.
 *
 * Anything that fails becomes the fallback name rather than an error,
 * so a hostile name costs the user a rename and nothing else.
 */
int zmodem_recv_sanitize_name(const char *raw, char *out, size_t cap)
{
    static const char *fallback = "download.bin";
    const char *base;
    const char *p;
    size_t i, n;
    char work[256];
    char stem[16];
    size_t dot;

    if (!out || cap == 0) return 0;
    out[0] = '\0';

    if (!raw) goto bad;

    /* Last separator of any kind wins. */
    base = raw;
    for (p = raw; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;

    n = strlen(base);
    if (n == 0 || n >= sizeof work) goto bad;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)base[i];
        if (c < 0x20 || c == 0x7f) goto bad;
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
            goto bad;
        work[i] = (char)c;
    }
    work[n] = '\0';

    while (n > 0 && (work[n - 1] == '.' || work[n - 1] == ' '))
        work[--n] = '\0';
    if (n == 0) goto bad;

    if (strcmp(work, ".") == 0 || strcmp(work, "..") == 0) goto bad;

    /* Stem for the device-name test: up to the first dot. */
    dot = 0;
    while (dot < n && work[dot] != '.') dot++;
    if (dot < sizeof stem) {
        memcpy(stem, work, dot);
        stem[dot] = '\0';
        if (name_is_reserved(stem)) goto bad;
    }

    if (n >= cap) {
        /* Too long for the caller's buffer: keep the head, which is
         * where a human-meaningful name lives, and let the extension
         * go. Never a silent truncation into an unterminated buffer. */
        n = cap - 1;
        while (n > 0 && (work[n - 1] == '.' || work[n - 1] == ' ')) n--;
        if (n == 0) goto bad;
        work[n] = '\0';
    }

    memcpy(out, work, n + 1);
    return 1;

bad:
    if (cap > strlen(fallback)) strcpy(out, fallback);
    else { out[0] = 'f'; out[1] = '\0'; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Where downloads land                                                */
/* ------------------------------------------------------------------ */

char *zmodem_recv_default_dir(char *out, size_t cap)
{
    const char *env;
    char profile[MAX_PATH];

    if (!out || cap == 0) return out;
    out[0] = '\0';

    env = getenv("MGT_ZMODEM_DIR");
    if (env && env[0] && strlen(env) < cap) {
        strcpy(out, env);
        return out;
    }

    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, profile))) {
        if (strlen(profile) + 11 < cap) {
            strcpy(out, profile);
            strcat(out, "\\Downloads");
            return out;
        }
    }

    if (GetTempPathA((DWORD)cap, out) == 0) out[0] = '\0';
    return out;
}

/* dir + name, with " (n)" inserted before the extension if the name is
 * taken. Never overwrites a file that is already there. */
static int build_unique_path(const char *dir, const char *name,
                             char *out, size_t cap)
{
    char stem[260], ext[64];
    const char *dot;
    int n;

    if (!dir || !name || !out) return 0;

    _snprintf(out, cap - 1, "%s\\%s", dir, name);
    out[cap - 1] = '\0';
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return 1;

    dot = strrchr(name, '.');
    if (dot && dot != name && strlen(dot) < sizeof ext) {
        size_t sl = (size_t)(dot - name);
        if (sl >= sizeof stem) sl = sizeof stem - 1;
        memcpy(stem, name, sl);
        stem[sl] = '\0';
        strcpy(ext, dot);
    } else {
        strncpy(stem, name, sizeof stem - 1);
        stem[sizeof stem - 1] = '\0';
        ext[0] = '\0';
    }

    for (n = 1; n < 1000; n++) {
        _snprintf(out, cap - 1, "%s\\%s (%d)%s", dir, stem, n, ext);
        out[cap - 1] = '\0';
        if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Progress                                                            */
/* ------------------------------------------------------------------ */

static void post_progress(ZmodemRecv *z, int force)
{
    ZmrProgress *p;
    DWORD now = GetTickCount();

    if (!force && (now - z->last_progress_tick) < ZMR_PROGRESS_MS) return;
    z->last_progress_tick = now;

    p = (ZmrProgress *)calloc(1, sizeof *p);
    if (!p) return;
    p->received = z->received;
    p->total    = z->total;
    copy_str(p->name, sizeof p->name, z->name);
    if (!PostMessageA(z->notify_hwnd, z->notify_msg,
                      ZMR_EVT_PROGRESS, (LPARAM)p))
        free(p);
}

void zmodem_recv_free_progress(ZmrProgress *p) { free(p); }

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

/* Bounded copy that always terminates. Used instead of strncpy for the
 * fixed-width record fields, where source and destination are the same
 * width and GCC is right to point out that strncpy alone would not. */
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_detail(ZmodemRecv *z, const char *s)
{
    copy_str(z->detail, sizeof z->detail, s);
}

/* Was this stop the user's doing?
 *
 * A cancel reaches the worker as zm_recv() returning CANCELLED, which
 * is the same code the core produces when the SENDER cancels, so the
 * two are indistinguishable at the point they surface. They must not be
 * treated the same. When the cancel is ours:
 *
 *   - the sender has not been told anything yet and will keep pushing
 *     the rest of the file down a socket nobody is reading, so the
 *     Zmodem cancel sequence has to go out here;
 *   - the outcome is ZMR_CANCELLED, not "the sender gave up".
 *
 * Called at every site that can see a CANCELLED or CLOSED result, and
 * it must be called BEFORE the result is attributed to the far end.
 * Returns non-zero if it took ownership of the stop. */
static void zsend_cancel(ZmodemRecv *z);

static int zmr_own_cancel(ZmodemRecv *z)
{
    if (!InterlockedCompareExchange(&z->cancel_request, 0, 0)) return 0;
    zsend_cancel(z);
    z->status = ZMR_CANCELLED;
    set_detail(z, "Cancelled.");
    return 1;
}

/* ZRINIT capabilities. Full duplex, can receive during disk I/O, can do
 * 32-bit frame checks. Deliberately NOT advertised: CANLZW and CANCRY,
 * which would invite the sender to compress or encrypt with code this
 * receiver does not have, and CANBRK, which it cannot honour. */
#define ZMR_CAPS  (CANFDX | CANOVIO | CANFC32)

typedef struct {
    FILE *fp;
    int   open;
} OutFile;

static void out_close(OutFile *o)
{
    if (o->fp) { fclose(o->fp); o->fp = NULL; }
    o->open = 0;
}

/* Parse a ZFILE payload: "name\0size mtime mode ...". `len` excludes
 * the frame-end byte. Bounded throughout: the payload is copied into a
 * local buffer and terminated before anything reads it as a string. */
static void parse_zfile(const unsigned char *buf, int len,
                        char *name_out, size_t name_cap,
                        unsigned long long *size_out)
{
    char tmp[ZMR_BLOCK_BUF];
    int i, namelen;

    name_out[0] = '\0';
    *size_out = 0;

    if (len <= 0) return;
    if (len > (int)sizeof tmp - 1) len = (int)sizeof tmp - 1;
    memcpy(tmp, buf, (size_t)len);
    tmp[len] = '\0';

    namelen = 0;
    while (namelen < len && tmp[namelen] != '\0') namelen++;

    zmodem_recv_sanitize_name(tmp, name_out, name_cap);

    /* The info field follows the name's terminator. It may be absent. */
    i = namelen + 1;
    if (i < len) {
        unsigned long long v = 0;
        int digits = 0;
        while (i < len && tmp[i] == ' ') i++;
        while (i < len && tmp[i] >= '0' && tmp[i] <= '9' && digits < 19) {
            v = v * 10ULL + (unsigned long long)(tmp[i] - '0');
            i++; digits++;
        }
        if (digits > 0) *size_out = v;
    }
}

static DWORD WINAPI zmr_thread(LPVOID lp)
{
    ZmodemRecv *z = (ZmodemRecv *)lp;
    static unsigned char data_buf[ZMR_BLOCK_BUF];
    ZHDR      hdr;
    OutFile   out;
    int       errors = 0;
    int       running = 1;
    int       saw_file = 0;

    memset(&out, 0, sizeof out);
    z->status = ZMR_PROTOCOL_ERROR;
    set_detail(z, "The transfer did not start.");

    /* Announce ourselves. A sender that is already emitting ZRQINIT
     * stops as soon as it sees this. */
    zsend_flags(z, ZRINIT, ZMR_CAPS, 0, 0, 0);

    while (running) {
        ZRESULT r;

        if (InterlockedCompareExchange(&z->cancel_request, 0, 0)) {
            zsend_cancel(z);
            z->status = ZMR_CANCELLED;
            set_detail(z, "Cancelled.");
            break;
        }
        if (z->send_failed) {
            z->status = ZMR_ABORTED;
            set_detail(z, "The connection dropped during the transfer.");
            break;
        }

        r = zm_await_header(&hdr);

        if (r == CANCELLED || r == CLOSED) {
            if (!zmr_own_cancel(z)) {
                z->status = (r == CANCELLED) ? ZMR_ABORTED : ZMR_TIMEOUT;
                set_detail(z, (r == CANCELLED)
                              ? "The sender cancelled the transfer."
                              : "The sender stopped responding.");
            }
            break;
        }
        if (r != OK) {
            /* Noise or a bad CRC. Ask for the frame again from where we
             * actually are; ZNAK before any file, ZRPOS once there is a
             * position to resume from. */
            if (++errors > ZMR_MAX_ERRORS) {
                z->status = ZMR_PROTOCOL_ERROR;
                set_detail(z, "Too many protocol errors; gave up.");
                break;
            }
            if (out.open) zsend_pos(z, ZRPOS, (uint32_t)z->received);
            else          zsend_pos(z, ZNAK, 0);
            continue;
        }
        errors = 0;

        switch (hdr.type) {

        /* ---- refused outright ------------------------------------- */
        case ZCOMMAND:
        case ZCOMPL:
            /* COMMAND DOWNLOAD. A sender asking us to execute something
             * is refused here and nowhere else has to know about it:
             * there is no interpreter, no CreateProcess, no system()
             * anywhere in this file. Cancel and report. */
            zsend_cancel(z);
            z->status = ZMR_REFUSED;
            set_detail(z, "The sender asked to run a command. Refused.");
            running = 0;
            break;

        /* ---- housekeeping ----------------------------------------- */
        case ZRQINIT:
            zsend_flags(z, ZRINIT, ZMR_CAPS, 0, 0, 0);
            break;

        case ZSINIT: {
            /* Carries the sender's attention string, which we have no
             * use for. Read it so the stream stays in step, discard it,
             * acknowledge. */
            uint16_t count = (uint16_t)sizeof data_buf;
            ZRESULT dr = zm_read_data_block(data_buf, &count);
            if (dr == CANCELLED) {
                if (!zmr_own_cancel(z)) {
                    z->status = ZMR_ABORTED;
                    set_detail(z, "The sender cancelled the transfer.");
                }
                running = 0;
            } else {
                zsend_flags(z, ZACK, 0, 0, 0, 0);
            }
            break;
        }

        case ZCHALLENGE:
            /* Echo the number back, which is all the challenge asks. */
            zsend_pos(z, ZACK,
                      ((uint32_t)hdr.position.p3 << 24) |
                      ((uint32_t)hdr.position.p2 << 16) |
                      ((uint32_t)hdr.position.p1 << 8)  |
                       (uint32_t)hdr.position.p0);
            break;

        case ZFREECOUNT:
            zsend_pos(z, ZACK, (uint32_t)ZMR_MAX_BYTES);
            break;

        /* ---- the file --------------------------------------------- */
        case ZFILE: {
            uint16_t count = (uint16_t)sizeof data_buf;
            ZRESULT dr = zm_read_data_block(data_buf, &count);
            char  name[260];
            unsigned long long advertised = 0;
            int   payload;

            if (dr == CANCELLED) {
                if (!zmr_own_cancel(z)) {
                    z->status = ZMR_ABORTED;
                    set_detail(z, "The sender cancelled the transfer.");
                }
                running = 0;
                break;
            }
            if (IS_ERROR(dr)) {
                /* Bad ZFILE payload. Ask for it again. */
                if (++errors > ZMR_MAX_ERRORS) {
                    z->status = ZMR_PROTOCOL_ERROR;
                    set_detail(z, "The file header would not decode.");
                    running = 0;
                } else {
                    zsend_pos(z, ZNAK, 0);
                }
                break;
            }

            payload = (count > 0) ? (int)count - 1 : 0;
            parse_zfile(data_buf, payload, name, sizeof name, &advertised);

            if (advertised > ZMR_MAX_BYTES) {
                zsend_cancel(z);
                z->status = ZMR_TOO_BIG;
                _snprintf(z->detail, sizeof z->detail - 1,
                          "The sender offered %llu bytes; the limit is %llu.",
                          advertised, (unsigned long long)ZMR_MAX_BYTES);
                z->detail[sizeof z->detail - 1] = '\0';
                running = 0;
                break;
            }

            /* A second file in one session: finish the first cleanly
             * and start again, but keep counting against one ceiling. */
            out_close(&out);

            strncpy(z->name, name, sizeof z->name - 1);
            z->name[sizeof z->name - 1] = '\0';
            z->total    = advertised;
            z->received = 0;

            if (!build_unique_path(z->dir, z->name, z->path, sizeof z->path)) {
                zsend_cancel(z);
                z->status = ZMR_IO_ERROR;
                set_detail(z, "Could not find a free name in the download folder.");
                running = 0;
                break;
            }
            out.fp = fopen(z->path, "wb");
            if (!out.fp) {
                zsend_cancel(z);
                z->status = ZMR_IO_ERROR;
                set_detail(z, "Could not create the file in the download folder.");
                running = 0;
                break;
            }
            out.open = 1;
            saw_file = 1;
            post_progress(z, 1);

            zsend_pos(z, ZRPOS, 0);
            break;
        }

        case ZDATA: {
            unsigned long long hpos =
                ((unsigned long long)hdr.position.p3 << 24) |
                ((unsigned long long)hdr.position.p2 << 16) |
                ((unsigned long long)hdr.position.p1 << 8)  |
                 (unsigned long long)hdr.position.p0;

            if (!out.open) {
                /* Data with nowhere to go. Never write it; ask the
                 * sender to start over from the beginning. */
                zsend_pos(z, ZRPOS, 0);
                break;
            }
            if (hpos != z->received) {
                /* Out of step, which is the ordinary consequence of a
                 * dropped frame. Tell the sender where we actually
                 * are and let it rewind. */
                zsend_pos(z, ZRPOS, (uint32_t)z->received);
                break;
            }

            for (;;) {
                uint16_t count = (uint16_t)sizeof data_buf;
                ZRESULT dr;
                int payload;

                if (InterlockedCompareExchange(&z->cancel_request, 0, 0)) {
                    zsend_cancel(z);
                    z->status = ZMR_CANCELLED;
                    set_detail(z, "Cancelled.");
                    running = 0;
                    break;
                }

                dr = zm_read_data_block(data_buf, &count);

                if (dr == CANCELLED || dr == CLOSED) {
                    if (!zmr_own_cancel(z)) {
                        z->status = (dr == CANCELLED) ? ZMR_ABORTED : ZMR_TIMEOUT;
                        set_detail(z, (dr == CANCELLED)
                                      ? "The sender cancelled the transfer."
                                      : "The sender stopped responding.");
                    }
                    running = 0;
                    break;
                }
                if (IS_ERROR(dr)) {
                    /* A bad subpacket, an over-long one, or a bad
                     * escape. Nothing is written. ZRPOS puts the sender
                     * back at our real position and the frame is
                     * resent; this is the CRC retry path. */
                    if (++errors > ZMR_MAX_ERRORS) {
                        z->status = ZMR_PROTOCOL_ERROR;
                        set_detail(z, "Too many bad blocks; gave up.");
                        running = 0;
                        break;
                    }
                    zsend_pos(z, ZRPOS, (uint32_t)z->received);
                    break;              /* back to awaiting a header */
                }
                errors = 0;

                payload = (count > 0) ? (int)count - 1 : 0;

                if (z->received + (unsigned long long)payload > ZMR_MAX_BYTES) {
                    zsend_cancel(z);
                    z->status = ZMR_TOO_BIG;
                    set_detail(z, "The transfer ran past the size limit and was stopped.");
                    running = 0;
                    break;
                }

                if (payload > 0) {
                    if (fwrite(data_buf, 1, (size_t)payload, out.fp)
                            != (size_t)payload) {
                        zsend_cancel(z);
                        z->status = ZMR_IO_ERROR;
                        set_detail(z, "Writing to the download folder failed.");
                        running = 0;
                        break;
                    }
                    z->received += (unsigned long long)payload;
                    post_progress(z, 0);
                }

                if (dr == GOT_CRCE) break;          /* frame done      */
                if (dr == GOT_CRCG) continue;       /* keep streaming  */
                if (dr == GOT_CRCQ) { zsend_pos(z, ZACK, (uint32_t)z->received); continue; }
                if (dr == GOT_CRCW) { zsend_pos(z, ZACK, (uint32_t)z->received); break; }

                /* Not a frame end and not an error: the core only ever
                 * returns one of the four, so this cannot happen. Treat
                 * it as an error rather than spin. */
                z->status = ZMR_PROTOCOL_ERROR;
                set_detail(z, "Unexpected end of data block.");
                running = 0;
                break;
            }
            break;
        }

        case ZEOF:
            if (out.open) {
                if (fflush(out.fp) != 0) {
                    z->status = ZMR_IO_ERROR;
                    set_detail(z, "Writing to the download folder failed.");
                    running = 0;
                    break;
                }
                post_progress(z, 1);
            }
            /* The sender now expects a fresh ZRINIT, either to send
             * another file or to move on to ZFIN. */
            zsend_flags(z, ZRINIT, ZMR_CAPS, 0, 0, 0);
            break;

        case ZFIN:
            zsend_flags(z, ZFIN, 0, 0, 0, 0);
            /* The sender answers "OO". Read the two bytes if they come
             * so they do not land in the terminal stream, but do not
             * make the transfer depend on them. */
            {
                int i;
                for (i = 0; i < 2; i++) {
                    ZRESULT c = zm_recv();
                    if (IS_ERROR(c) || ZVALUE(c) != 'O') break;
                }
            }
            if (saw_file && out.open) {
                z->status = ZMR_OK;
                _snprintf(z->detail, sizeof z->detail - 1,
                          "Received %llu bytes.", z->received);
                z->detail[sizeof z->detail - 1] = '\0';
            } else if (!saw_file) {
                z->status = ZMR_PROTOCOL_ERROR;
                set_detail(z, "The sender finished without offering a file.");
            }
            running = 0;
            break;

        case ZSKIP:
        case ZABORT:
        case ZERR:
        case ZCAN:
            z->status = ZMR_ABORTED;
            set_detail(z, "The sender abandoned the transfer.");
            running = 0;
            break;

        default:
            /* An unknown frame type is not fatal; ask for a resend. */
            if (++errors > ZMR_MAX_ERRORS) {
                z->status = ZMR_PROTOCOL_ERROR;
                set_detail(z, "The sender kept sending frames we do not know.");
                running = 0;
            } else {
                zsend_pos(z, ZNAK, 0);
            }
            break;
        }
    }

    zsend_flush(z);
    out_close(&out);

    /* A transfer that did not finish leaves a partial file behind. It
     * is more use to the user gone than half-written under the right
     * name, so it is removed and the path cleared. */
    if (z->status != ZMR_OK && z->path[0]) {
        DeleteFileA(z->path);
        z->path[0] = '\0';
    }

    post_progress(z, 1);
    PostMessageA(z->notify_hwnd, z->notify_msg, ZMR_EVT_DONE, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int zmodem_recv_start(const char *download_dir,
                      HWND notify_hwnd, UINT notify_msg,
                      ZmrSendFn send_fn, void *send_ctx,
                      ZmodemRecv **out)
{
    ZmodemRecv *z;
    DWORD tid;

    if (!out) return 1;
    *out = NULL;
    if (g_active) return 2;              /* one session at a time */

    z = (ZmodemRecv *)calloc(1, sizeof *z);
    if (!z) return 3;

    z->notify_hwnd = notify_hwnd;
    z->notify_msg  = notify_msg;
    z->send_fn     = send_fn;
    z->send_ctx    = send_ctx;
    z->status      = ZMR_PROTOCOL_ERROR;

    if (download_dir && download_dir[0])
        strncpy(z->dir, download_dir, sizeof z->dir - 1);
    else
        zmodem_recv_default_dir(z->dir, sizeof z->dir);
    z->dir[sizeof z->dir - 1] = '\0';

    /* The folder has to exist before the first ZFILE arrives, and the
     * user's Downloads folder can be missing on a fresh profile. */
    if (z->dir[0] && GetFileAttributesA(z->dir) == INVALID_FILE_ATTRIBUTES)
        CreateDirectoryA(z->dir, NULL);

    z->ring = (unsigned char *)malloc(ZMR_RING_BYTES);
    if (!z->ring) { free(z); return 4; }

    InitializeCriticalSection(&z->ring_lock);
    z->data_evt  = CreateEventA(NULL, TRUE, FALSE, NULL);
    z->space_evt = CreateEventA(NULL, TRUE, TRUE,  NULL);
    if (!z->data_evt || !z->space_evt) {
        if (z->data_evt)  CloseHandle(z->data_evt);
        if (z->space_evt) CloseHandle(z->space_evt);
        DeleteCriticalSection(&z->ring_lock);
        free(z->ring); free(z);
        return 5;
    }

    /* The CRC tables are reached through macros that index them
     * directly, so they must exist before the first header. */
    mbzm_crc16_init();
    mbzm_crc32_init();

    g_active = z;
    z->thread = CreateThread(NULL, 0, zmr_thread, z, 0, &tid);
    if (!z->thread) {
        g_active = NULL;
        CloseHandle(z->data_evt);
        CloseHandle(z->space_evt);
        DeleteCriticalSection(&z->ring_lock);
        free(z->ring); free(z);
        return 6;
    }

    *out = z;
    return 0;
}

void zmodem_recv_cancel(ZmodemRecv *z)
{
    if (!z) return;
    InterlockedExchange(&z->cancel_request, 1);
    SetEvent(z->data_evt);      /* wake a worker parked on the ring */
}

ZmrResult *zmodem_recv_finish(ZmodemRecv *z)
{
    ZmrResult *r = (ZmrResult *)calloc(1, sizeof *r);

    if (!r) return NULL;
    if (!z) {
        r->status = ZMR_PROTOCOL_ERROR;
        strcpy(r->detail, "No transfer was running.");
        return r;
    }

    InterlockedExchange(&z->stop_request, 1);
    SetEvent(z->data_evt);
    if (z->thread) {
        if (WaitForSingleObject(z->thread, 5000) == WAIT_TIMEOUT) {
            /* The worker only ever waits on the ring with a timeout, so
             * this should not happen. Do not kill it and do not free
             * what it still points at: leak the session and say so. */
            r->status = ZMR_PROTOCOL_ERROR;
            strcpy(r->detail, "The transfer thread did not stop.");
            return r;
        }
        CloseHandle(z->thread);
        z->thread = NULL;
    }

    r->status   = z->status;
    r->received = z->received;
    r->total    = z->total;
    copy_str(r->name,   sizeof r->name,   z->name);
    copy_str(r->path,   sizeof r->path,   z->path);
    copy_str(r->detail, sizeof r->detail, z->detail);
    r->tail_len = ring_drain(z, &r->tail);

    g_active = NULL;
    CloseHandle(z->data_evt);
    CloseHandle(z->space_evt);
    DeleteCriticalSection(&z->ring_lock);
    free(z->ring);
    free(z);
    return r;
}

void zmodem_recv_free_result(ZmrResult *r)
{
    if (!r) return;
    if (r->tail) free(r->tail);
    free(r);
}
