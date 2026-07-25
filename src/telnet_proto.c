/*
 * telnet_proto.c - RFC 854 Telnet client with asynchronous I/O.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Winsock2 is already started by suite_shell at WinMain; we do not
 * touch WSAStartup/WSACleanup here.
 *
 * IAC handling matrix:
 *   WILL ECHO        -> reply DO  ECHO
 *   WILL SGA         -> reply DO  SGA
 *   WILL <other>     -> reply DONT <other>
 *   DO TTYPE         -> reply WILL TTYPE; on SB TTYPE SEND reply IS "vt220"
 *   DO NAWS          -> reply WILL NAWS; immediately reply IAC SB NAWS 0 80 0 24 IAC SE
 *   DO SGA           -> reply WILL SGA
 *   DO <other>       -> reply WONT <other>
 *   WONT <opt>       -> reply DONT <opt>
 *   DONT <opt>       -> reply WONT <opt>
 *   SB ... IAC SE    -> parse; reply only to TTYPE SEND
 *   IAC IAC          -> literal 0xFF byte in user data
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "telnet_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IAC_   0xFF
#define DONT_  0xFE
#define DO_    0xFD
#define WONT_  0xFC
#define WILL_  0xFB
#define SB_    0xFA
#define SE_    0xF0

#define OPT_ECHO   1
#define OPT_SGA    3
#define OPT_TTYPE  24
#define OPT_NAWS   31

#define RECV_BUF   8192

typedef enum {
    IS_NORMAL = 0,
    IS_IAC,
    IS_VERB,            /* after WILL/WONT/DO/DONT, awaiting option byte */
    IS_SB,              /* inside subnegotiation, collecting */
    IS_SB_IAC           /* saw IAC inside SB, awaiting SE or doubled IAC */
} IacState;

struct TelnetProto {
    SOCKET    sock;
    HANDLE    rx_thread;
    volatile LONG stop_request;

    HWND      notify_hwnd;
    UINT      notify_msg;

    char     *host;
    int       port;

    /* IAC parser state. */
    IacState  is_state;
    unsigned char verb;     /* the WILL/WONT/DO/DONT byte */
    unsigned char sb_buf[256];
    int           sb_len;

    /* Set when the server negotiates remote echo (WILL ECHO), cleared on
     * WONT ECHO. Written on the rx thread, read on the UI thread; a plain
     * int is fine for this one-bit hint. */
    int           server_echo;

    /* Send-side serialization. */
    CRITICAL_SECTION send_lock;
};

/* ---------------------------------------------------------------------- */
/* Tiny socket helpers.                                                   */
/* ---------------------------------------------------------------------- */

static int send_all(SOCKET s, const unsigned char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)(buf + sent), len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static void send_iac_2(TelnetProto *t, unsigned char b1, unsigned char b2)
{
    unsigned char buf[3];
    buf[0] = IAC_;
    buf[1] = b1;
    buf[2] = b2;
    EnterCriticalSection(&t->send_lock);
    if (t->sock != INVALID_SOCKET) send_all(t->sock, buf, 3);
    LeaveCriticalSection(&t->send_lock);
}

static void send_iac_ttype_is_vt220(TelnetProto *t)
{
    /* IAC SB TTYPE IS "vt220" IAC SE
     *   IAC SB TTYPE 0 v t 2 2 0 IAC SE  (IS == 0)  */
    static const unsigned char prefix[] = { IAC_, SB_, OPT_TTYPE, 0 };
    static const char         name[]   = "vt220";
    static const unsigned char suffix[] = { IAC_, SE_ };
    EnterCriticalSection(&t->send_lock);
    if (t->sock != INVALID_SOCKET) {
        send_all(t->sock, prefix,           (int)sizeof(prefix));
        send_all(t->sock, (const unsigned char *)name, (int)strlen(name));
        send_all(t->sock, suffix,           (int)sizeof(suffix));
    }
    LeaveCriticalSection(&t->send_lock);
}

static void send_iac_naws_80x24(TelnetProto *t)
{
    /* IAC SB NAWS w_hi w_lo h_hi h_lo IAC SE
     *   80 -> 00 50 ; 24 -> 00 18 */
    unsigned char msg[] = { IAC_, SB_, OPT_NAWS, 0, 80, 0, 24, IAC_, SE_ };
    EnterCriticalSection(&t->send_lock);
    if (t->sock != INVALID_SOCKET) send_all(t->sock, msg, (int)sizeof(msg));
    LeaveCriticalSection(&t->send_lock);
}

/* ---------------------------------------------------------------------- */
/* IAC parser. Walks the freshly-read chunk and emits a "clean" payload   */
/* buffer (no IAC noise) of length <= len. Sends option replies inline.   */
/* ---------------------------------------------------------------------- */

static int parse_chunk(TelnetProto *t,
                       const unsigned char *in, int len,
                       unsigned char *out)
{
    int i;
    int o = 0;

    for (i = 0; i < len; i++) {
        unsigned char b = in[i];

        switch (t->is_state) {
        case IS_NORMAL:
            if (b == IAC_) t->is_state = IS_IAC;
            else out[o++] = b;
            break;

        case IS_IAC:
            if (b == IAC_) {              /* doubled IAC -> literal 0xFF */
                out[o++] = IAC_;
                t->is_state = IS_NORMAL;
            } else if (b == WILL_ || b == WONT_ || b == DO_ || b == DONT_) {
                t->verb = b;
                t->is_state = IS_VERB;
            } else if (b == SB_) {
                t->sb_len = 0;
                t->is_state = IS_SB;
            } else {
                /* NOP, AYT, etc. -- consumed. */
                t->is_state = IS_NORMAL;
            }
            break;

        case IS_VERB: {
            unsigned char opt = b;
            if (t->verb == WILL_) {
                if (opt == OPT_ECHO)      { send_iac_2(t, DO_, OPT_ECHO); t->server_echo = 1; }
                else if (opt == OPT_SGA)  send_iac_2(t, DO_,   OPT_SGA);
                else                      send_iac_2(t, DONT_, opt);
            } else if (t->verb == DO_) {
                if (opt == OPT_TTYPE) {
                    send_iac_2(t, WILL_, OPT_TTYPE);
                    /* TTYPE name is sent later in response to SB SEND. */
                } else if (opt == OPT_NAWS) {
                    send_iac_2(t, WILL_, OPT_NAWS);
                    send_iac_naws_80x24(t);
                } else if (opt == OPT_SGA) {
                    send_iac_2(t, WILL_, OPT_SGA);
                } else {
                    send_iac_2(t, WONT_, opt);
                }
            } else if (t->verb == WONT_) {
                if (opt == OPT_ECHO) t->server_echo = 0;
                send_iac_2(t, DONT_, opt);
            } else if (t->verb == DONT_) {
                send_iac_2(t, WONT_, opt);
            }
            t->is_state = IS_NORMAL;
            break;
        }

        case IS_SB:
            if (b == IAC_) t->is_state = IS_SB_IAC;
            else if (t->sb_len < (int)sizeof(t->sb_buf))
                t->sb_buf[t->sb_len++] = b;
            /* else: overflow, silently drop the byte */
            break;

        case IS_SB_IAC:
            if (b == SE_) {
                /* End of subnegotiation. Dispatch. */
                if (t->sb_len >= 2
                    && t->sb_buf[0] == OPT_TTYPE
                    && t->sb_buf[1] == 1 /* SEND */) {
                    send_iac_ttype_is_vt220(t);
                }
                t->sb_len = 0;
                t->is_state = IS_NORMAL;
            } else if (b == IAC_) {
                /* IAC IAC inside SB -> literal 0xFF in SB body */
                if (t->sb_len < (int)sizeof(t->sb_buf))
                    t->sb_buf[t->sb_len++] = IAC_;
                t->is_state = IS_SB;
            } else {
                /* IAC <other> inside SB -- abandon SB cleanly. */
                t->sb_len = 0;
                t->is_state = IS_NORMAL;
            }
            break;
        }
    }
    return o;
}

/* ---------------------------------------------------------------------- */
/* Worker thread: connect, then read loop.                                */
/* ---------------------------------------------------------------------- */

typedef struct WorkerArgs {
    TelnetProto *t;
    char         host[256];
    int          port;
} WorkerArgs;

static SOCKET connect_to(const char *host, int port, const char **errmsg_out)
{
    struct addrinfo hints, *res = NULL, *ai;
    char            portbuf[8];
    SOCKET          s = INVALID_SOCKET;
    int             rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    sprintf(portbuf, "%d", port);

    rc = getaddrinfo(host, portbuf, &hints, &res);
    if (rc != 0 || !res) {
        *errmsg_out = "Host not found";
        return INVALID_SOCKET;
    }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);

    if (s == INVALID_SOCKET) {
        *errmsg_out = "Connection refused";
    }
    return s;
}

static DWORD WINAPI rx_thread_proc(LPVOID lp)
{
    WorkerArgs   *w = (WorkerArgs *)lp;
    TelnetProto  *t = w->t;
    const char   *errmsg = NULL;
    SOCKET        s;
    unsigned char in[RECV_BUF];
    unsigned char outbuf[RECV_BUF];
    int           closed_normally = 1;

    s = connect_to(w->host, w->port, &errmsg);
    if (s == INVALID_SOCKET) {
        PostMessageA(t->notify_hwnd, t->notify_msg,
                     TELNET_EVT_ERROR,
                     (LPARAM)(errmsg ? errmsg : "Connection failed"));
        PostMessageA(t->notify_hwnd, t->notify_msg, TELNET_EVT_CLOSED, 0);
        free(w);
        return 0;
    }
    t->sock = s;
    PostMessageA(t->notify_hwnd, t->notify_msg, TELNET_EVT_CONNECTED, 0);

    while (!InterlockedCompareExchange(&t->stop_request, 0, 0)) {
        int n = recv(s, (char *)in, RECV_BUF, 0);
        int payload_len;
        TelnetDataChunk *chunk;

        if (n == 0) break;                 /* server closed */
        if (n < 0) {
            if (!InterlockedCompareExchange(&t->stop_request, 0, 0)) {
                closed_normally = 0;
            }
            break;
        }
        payload_len = parse_chunk(t, in, n, outbuf);
        if (payload_len <= 0) continue;

        chunk = (TelnetDataChunk *)malloc(sizeof(TelnetDataChunk));
        if (!chunk) continue;
        chunk->data = (unsigned char *)malloc((size_t)payload_len);
        if (!chunk->data) { free(chunk); continue; }
        memcpy(chunk->data, outbuf, (size_t)payload_len);
        chunk->len = payload_len;
        PostMessageA(t->notify_hwnd, t->notify_msg,
                     TELNET_EVT_DATA, (LPARAM)chunk);
    }

    /* Close the socket if still open, and post CLOSED. */
    EnterCriticalSection(&t->send_lock);
    if (t->sock != INVALID_SOCKET) {
        closesocket(t->sock);
        t->sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&t->send_lock);

    PostMessageA(t->notify_hwnd, t->notify_msg,
                 TELNET_EVT_CLOSED, closed_normally ? 0 : 1);
    free(w);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Public API.                                                            */
/* ---------------------------------------------------------------------- */

int telnet_proto_open(const char *host, int port,
                      HWND notify_hwnd, UINT notify_msg,
                      TelnetProto **out)
{
    TelnetProto *t;
    WorkerArgs  *w;
    DWORD        tid;

    if (!host || !out) return 1;
    *out = NULL;

    t = (TelnetProto *)calloc(1, sizeof(*t));
    if (!t) return 2;
    t->sock        = INVALID_SOCKET;
    t->notify_hwnd = notify_hwnd;
    t->notify_msg  = notify_msg;
    t->port        = port;
    t->host        = _strdup(host);
    t->is_state    = IS_NORMAL;
    InitializeCriticalSection(&t->send_lock);

    w = (WorkerArgs *)calloc(1, sizeof(*w));
    if (!w) { free(t->host); DeleteCriticalSection(&t->send_lock); free(t); return 3; }
    w->t = t;
    w->port = port;
    strncpy(w->host, host, sizeof(w->host) - 1);

    t->rx_thread = CreateThread(NULL, 0, rx_thread_proc, w, 0, &tid);
    if (!t->rx_thread) {
        free(w);
        free(t->host);
        DeleteCriticalSection(&t->send_lock);
        free(t);
        return 4;
    }

    *out = t;
    return 0;
}

int telnet_proto_send(TelnetProto *t, const void *buf, size_t len)
{
    const unsigned char *p;
    unsigned char *esc;
    size_t i, j;
    int rc = 0;

    if (!t || !buf || len == 0) return 1;
    p = (const unsigned char *)buf;

    /* Worst case: every byte is 0xFF, doubled. */
    esc = (unsigned char *)malloc(len * 2);
    if (!esc) return 2;
    for (i = 0, j = 0; i < len; i++) {
        if (p[i] == 0xFF) { esc[j++] = 0xFF; esc[j++] = 0xFF; }
        else              esc[j++] = p[i];
    }

    EnterCriticalSection(&t->send_lock);
    if (t->sock == INVALID_SOCKET) {
        rc = 3;
    } else if (send_all(t->sock, esc, (int)j) != 0) {
        rc = 4;
    }
    LeaveCriticalSection(&t->send_lock);

    free(esc);
    return rc;
}

int telnet_proto_server_echo(TelnetProto *t)
{
    return t ? t->server_echo : 0;
}

void telnet_proto_close(TelnetProto *t)
{
    if (!t) return;
    InterlockedExchange(&t->stop_request, 1);

    EnterCriticalSection(&t->send_lock);
    if (t->sock != INVALID_SOCKET) {
        shutdown(t->sock, SD_BOTH);
        closesocket(t->sock);
        t->sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&t->send_lock);

    if (t->rx_thread) {
        WaitForSingleObject(t->rx_thread, 3000);
        CloseHandle(t->rx_thread);
        t->rx_thread = NULL;
    }
    DeleteCriticalSection(&t->send_lock);
    free(t->host);
    free(t);
}

void telnet_proto_free_chunk(TelnetDataChunk *chunk)
{
    if (!chunk) return;
    if (chunk->data) free(chunk->data);
    free(chunk);
}
