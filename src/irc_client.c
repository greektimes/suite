/*
 * irc_client.c - Receive-only IRCv3 transport service. See irc_client.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Wire behaviour (verified live against irc.greektimes.ca, UnrealIRCd
 * 6.2.4, 2026-07-17):
 *   1. TCP connect, plaintext 6667.
 *   2. CAP LS 302 -> CAP REQ :server-time batch message-tags -> CAP END.
 *      server-time is mandatory or the +H replay comes back empty.
 *   3. NICK <throwaway> / USER; registration is held behind CAP until
 *      CAP END, then the server sends 001.
 *   4. JOIN <channel>; the +H on-join history replay arrives wrapped in
 *      a `chathistory` BATCH, every line carrying @time= and msgid=,
 *      followed by the live PRIVMSG / NOTICE stream.
 *   5. PING can arrive at ANY time (one lands mid-CAP-negotiation), so
 *      PONG handling is unconditional, not gated on a connected state.
 *
 * The BATCH open/close lines are consumed silently: both backlog and
 * live traffic are ordinary PRIVMSG to the channel, so forwarding every
 * PRIVMSG/NOTICE in arrival order gives backlog-then-live for free.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "irc_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRC_RECV_BUF        4096
/* Line accumulator: an IRCv3 line is <= 512 bytes of message plus up to
 * ~8191 bytes of tags. 16 KiB gives comfortable headroom. */
#define IRC_LINE_MAX        16384
#define IRC_BACKOFF_MIN_MS   2000
#define IRC_BACKOFF_MAX_MS  30000
/* Bounded single-attempt connect budget so a routable-but-dead endpoint
 * cannot hang on the OS default (which can run to ~20s+). Checked in
 * ~200ms slices so a user Disconnect interrupts it promptly. */
#define IRC_CONNECT_TIMEOUT_MS  18000

/* Connect failure reasons (open_socket -> *reason). */
#define IRC_FAIL_NONE     0
#define IRC_FAIL_RESOLVE  1   /* getaddrinfo failed */
#define IRC_FAIL_REFUSED  2   /* connect refused / error */
#define IRC_FAIL_TIMEOUT  3   /* connect exceeded the bounded budget */
#define IRC_FAIL_ABORTED  4   /* stop requested mid-connect (user Disconnect) */
#define IRC_FAIL_GENERIC  5

static const char *fail_reason_text(int reason)
{
    switch (reason) {
    case IRC_FAIL_RESOLVE: return "Could not resolve host";
    case IRC_FAIL_REFUSED: return "Connection refused";
    case IRC_FAIL_TIMEOUT: return "Connection timed out";
    default:               return "Could not connect";
    }
}

struct IrcClient {
    HANDLE           rx_thread;
    volatile LONG    stop_request;
    HWND             notify_hwnd;
    UINT             notify_msg;
    char             host[256];
    int              port;
    char             channel[64];
    char             nick_pattern[64];

    /* Worker-only flags (no lock: only the rx thread reads/writes them).
     * ever_registered: set once any session reaches numeric 001. Before
     * it is set, a connect failure is TERMINAL (post FAILED, stop); after
     * it, a drop reconnects silently. banner_pending: forward the server
     * banner as SYSTEM events until the first MOTD end; cleared there so
     * automatic reconnects stay quiet. */
    int              ever_registered;
    int              banner_pending;

    /* Guards `sock` so irc_client_close() can close the live socket to
     * unblock recv()/connect() without racing the worker's reconnect loop,
     * which repeatedly opens and closes sockets in the same field. */
    CRITICAL_SECTION cs;
    SOCKET           sock;
};

/* Per-connection scratch state, lives on the worker's stack. */
typedef struct Session {
    SOCKET sock;
    char   nick[64];
    int    cap_req_sent;
    int    cap_end_sent;
    int    reached_reg;   /* set once 001 is seen; resets backoff */
    int    banner;        /* forward server banner lines this session */
} Session;

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int stop_requested(IrcClient *c)
{
    return InterlockedCompareExchange(&c->stop_request, 0, 0) != 0;
}

/* Store the in-flight socket under the lock so irc_client_close() can close
 * it to interrupt a pending connect or an active recv. */
static void set_sock(IrcClient *c, SOCKET s)
{
    EnterCriticalSection(&c->cs);
    c->sock = s;
    LeaveCriticalSection(&c->cs);
}

/* Bounded, interruptible TCP connect. Publishes the socket into c->sock so
 * a concurrent irc_client_close() can tear it down. On success returns the
 * connected (blocking) socket with c->sock set. On failure returns
 * INVALID_SOCKET, clears c->sock, and writes *reason. The wait runs in
 * ~200ms slices, checking stop_request each slice, capped at
 * IRC_CONNECT_TIMEOUT_MS. */
static SOCKET open_socket(IrcClient *c, int *reason)
{
    struct addrinfo hints, *res = NULL, *ai;
    char            portbuf[8];
    int             rc;

    *reason = IRC_FAIL_GENERIC;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    sprintf(portbuf, "%d", c->port);

    rc = getaddrinfo(c->host, portbuf, &hints, &res);
    if (rc != 0 || !res) { *reason = IRC_FAIL_RESOLVE; return INVALID_SOCKET; }

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        SOCKET  s;
        u_long  nb = 1;
        int     waited = 0;

        if (stop_requested(c)) { *reason = IRC_FAIL_ABORTED; break; }

        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        /* Publish before connecting so close() can interrupt us. */
        set_sock(c, s);
        ioctlsocket(s, FIONBIO, &nb);

        rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == 0) {
            nb = 0; ioctlsocket(s, FIONBIO, &nb);
            freeaddrinfo(res);
            *reason = IRC_FAIL_NONE;
            return s;                 /* connected immediately (rare) */
        }
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            set_sock(c, INVALID_SOCKET);
            closesocket(s);
            *reason = IRC_FAIL_REFUSED;
            continue;
        }

        /* Wait for writability (connected) or error, in interruptible slices. */
        for (;;) {
            fd_set  wfds, efds;
            struct timeval tv;
            int     sel;

            if (stop_requested(c)) { *reason = IRC_FAIL_ABORTED; break; }
            if (waited >= IRC_CONNECT_TIMEOUT_MS) { *reason = IRC_FAIL_TIMEOUT; break; }

            FD_ZERO(&wfds); FD_SET(s, &wfds);
            FD_ZERO(&efds); FD_SET(s, &efds);
            tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
            sel = select(0, NULL, &wfds, &efds, &tv);
            if (sel == SOCKET_ERROR) { *reason = IRC_FAIL_REFUSED; break; }
            if (sel == 0) { waited += 200; continue; }   /* slice timeout */

            if (FD_ISSET(s, &efds)) { *reason = IRC_FAIL_REFUSED; break; }
            if (FD_ISSET(s, &wfds)) {
                int err = 0, elen = (int)sizeof(err);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &elen) != 0 ||
                    err != 0) {
                    *reason = IRC_FAIL_REFUSED;
                    break;
                }
                nb = 0; ioctlsocket(s, FIONBIO, &nb);
                freeaddrinfo(res);
                *reason = IRC_FAIL_NONE;
                return s;             /* connected */
            }
        }

        set_sock(c, INVALID_SOCKET);
        closesocket(s);
        if (*reason == IRC_FAIL_ABORTED || *reason == IRC_FAIL_TIMEOUT)
            break;                    /* don't churn other addrs on abort/timeout */
    }

    freeaddrinfo(res);
    return INVALID_SOCKET;
}

/* Post one text-only event (SYSTEM banner line or FAILED reason) to the UI
 * thread. Reuses IrcMessage so the caller's single free path applies. */
static void emit_line(IrcClient *c, UINT event, const char *text)
{
    IrcMessage *m = (IrcMessage *)calloc(1, sizeof(*m));
    if (!m) return;
    strncpy(m->text, text ? text : "", sizeof(m->text) - 1);
    GetLocalTime(&m->when_local);
    PostMessageA(c->notify_hwnd, c->notify_msg, event, (LPARAM)m);
}

static int send_line(SOCKET s, const char *line)
{
    char buf[IRC_LINE_MAX];
    int  len;
    _snprintf(buf, sizeof(buf), "%s\r\n", line);
    buf[sizeof(buf) - 1] = '\0';
    len = (int)strlen(buf);
    return send(s, buf, len, 0);
}

/* Fill `out` with a nick from `pattern`: each 'X' becomes a random
 * digit; if the pattern has no 'X', four random digits are appended. */
static void gen_nick(const char *pattern, char *out, size_t cap)
{
    size_t i, o = 0;
    int    had_x = 0;
    for (i = 0; pattern[i] && o < cap - 1; i++) {
        if (pattern[i] == 'X') { out[o++] = (char)('0' + rand() % 10); had_x = 1; }
        else                     out[o++] = pattern[i];
    }
    out[o] = '\0';
    if (!had_x) {
        int k;
        for (k = 0; k < 4 && o < cap - 1; k++) out[o++] = (char)('0' + rand() % 10);
        out[o] = '\0';
    }
}

/* Extract tag value for `key` from an IRCv3 tag blob "k1=v1;k2=v2;k3"
 * (no leading '@', no trailing space). Writes "" if absent; writes "" if
 * present-but-valueless (a bare key). */
static void get_tag(const char *blob, const char *key, char *out, size_t cap)
{
    size_t      klen = strlen(key);
    const char *p    = blob;
    out[0] = '\0';
    while (p && *p) {
        const char *semi   = strchr(p, ';');
        size_t      seglen = semi ? (size_t)(semi - p) : strlen(p);
        const char *segend = p + seglen;
        const char *eq     = memchr(p, '=', seglen);
        if (eq) {
            if ((size_t)(eq - p) == klen && strncmp(p, key, klen) == 0) {
                size_t vlen = (size_t)(segend - (eq + 1));
                if (vlen >= cap) vlen = cap - 1;
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
                return;
            }
        } else if (seglen == klen && strncmp(p, key, klen) == 0) {
            return;   /* bare key, no value */
        }
        p = semi ? semi + 1 : NULL;
    }
}

/* Parse an ISO-8601 UTC instant "2026-07-17T22:44:06.859Z" into a
 * SYSTEMTIME (UTC). Milliseconds optional. Returns 1 on success. */
static int parse_iso_utc(const char *iso, SYSTEMTIME *utc)
{
    int y, mo, d, h, mi, s, ms = 0;
    const char *dot;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6)
        return 0;
    dot = strchr(iso, '.');
    if (dot) ms = atoi(dot + 1);
    memset(utc, 0, sizeof(*utc));
    utc->wYear         = (WORD)y;
    utc->wMonth        = (WORD)mo;
    utc->wDay          = (WORD)d;
    utc->wHour         = (WORD)h;
    utc->wMinute       = (WORD)mi;
    utc->wSecond       = (WORD)s;
    utc->wMilliseconds = (WORD)(ms % 1000);
    return 1;
}

/* ------------------------------------------------------------------ */
/* message forwarding                                                  */
/* ------------------------------------------------------------------ */

static void forward_message(IrcClient *c, IrcMsgKind kind,
                            const char *nick, const char *target,
                            const char *msgid, const char *text,
                            const char *time_tag)
{
    IrcMessage *m = (IrcMessage *)calloc(1, sizeof(*m));
    SYSTEMTIME  utc;
    if (!m) return;
    m->kind = kind;
    strncpy(m->nick,   nick   ? nick   : "", sizeof(m->nick)   - 1);
    strncpy(m->target, target ? target : "", sizeof(m->target) - 1);
    strncpy(m->msgid,  msgid  ? msgid  : "", sizeof(m->msgid)  - 1);
    strncpy(m->text,   text   ? text   : "", sizeof(m->text)   - 1);

    if (time_tag && time_tag[0] && parse_iso_utc(time_tag, &utc) &&
        SystemTimeToTzSpecificLocalTime(NULL, &utc, &m->when_local)) {
        m->have_server_time = 1;
    } else {
        GetLocalTime(&m->when_local);
        m->have_server_time = 0;
    }
    PostMessageA(c->notify_hwnd, c->notify_msg, IRC_EVT_MESSAGE, (LPARAM)m);
}

/* ------------------------------------------------------------------ */
/* banner forwarding helpers                                           */
/* ------------------------------------------------------------------ */

/* True for a 3-digit numeric reply command ("001".."999"). */
static int is_numeric_cmd(const char *cmd)
{
    return cmd[0] >= '0' && cmd[0] <= '9' &&
           cmd[1] >= '0' && cmd[1] <= '9' &&
           cmd[2] >= '0' && cmd[2] <= '9' && cmd[3] == '\0';
}

/* JOIN-rejection numerics that should always surface as an error line so a
 * bad/forbidden channel gives visible feedback (no such channel, bad mask,
 * +i/+k/+l/+b/+r/+O rejections). */
static int is_join_error(const char *cmd)
{
    static const char *codes[] = {
        "403", "405", "437", "471", "472", "473",
        "474", "475", "476", "477", "479", NULL
    };
    int i;
    for (i = 0; codes[i]; i++)
        if (strcmp(cmd, codes[i]) == 0) return 1;
    return 0;
}

/* Forward a server numeric's human-readable text as a SYSTEM line: drop the
 * leading nick param (the numeric's target) and strip one trailing-param
 * ':' marker. Presentation ("* " prefix) is the viewer module's job. */
static void forward_numeric(IrcClient *c, const char *rest)
{
    const char *p = rest;
    while (*p && *p != ' ') p++;   /* skip the nick token */
    while (*p == ' ') p++;
    if (*p == ':') p++;            /* strip the trailing-param marker */
    emit_line(c, IRC_EVT_SYSTEM, p);
}

/* ------------------------------------------------------------------ */
/* line parsing + protocol state machine                              */
/* ------------------------------------------------------------------ */

/* Parse one complete line (no CR/LF) in place and act on it. The buffer
 * is mutable; the command token is NUL-terminated during dispatch. */
static void handle_line(IrcClient *c, Session *ss, char *line)
{
    char  tagblob[IRC_LINE_MAX];
    char  timev[64];
    char  msgidv[64];
    char  nick[64];
    char *p = line;
    char *sp;
    char *cmd;
    char *rest;

    tagblob[0] = timev[0] = msgidv[0] = nick[0] = '\0';

    /* [ '@' tags SPACE ] */
    if (*p == '@') {
        p++;
        sp = strchr(p, ' ');
        if (!sp) return;                 /* malformed: tags with no command */
        {
            size_t tl = (size_t)(sp - p);
            if (tl >= sizeof(tagblob)) tl = sizeof(tagblob) - 1;
            memcpy(tagblob, p, tl);
            tagblob[tl] = '\0';
        }
        p = sp;
        while (*p == ' ') p++;
        get_tag(tagblob, "time",  timev,  sizeof(timev));
        get_tag(tagblob, "msgid", msgidv, sizeof(msgidv));
    }

    /* [ ':' prefix SPACE ] */
    if (*p == ':') {
        char   prefix[256];
        char  *bang;
        size_t pl;
        p++;
        sp = strchr(p, ' ');
        if (!sp) return;
        pl = (size_t)(sp - p);
        if (pl >= sizeof(prefix)) pl = sizeof(prefix) - 1;
        memcpy(prefix, p, pl);
        prefix[pl] = '\0';
        bang = strpbrk(prefix, "!@");     /* nick is up to '!user' / '@host' */
        if (bang) *bang = '\0';
        strncpy(nick, prefix, sizeof(nick) - 1);
        nick[sizeof(nick) - 1] = '\0';
        p = sp;
        while (*p == ' ') p++;
    }

    /* command */
    cmd = p;
    sp  = strchr(p, ' ');
    if (sp) { *sp = '\0'; rest = sp + 1; while (*rest == ' ') rest++; }
    else    { rest = p + strlen(p); }

    /* PING: answer unconditionally, any time. */
    if (strcmp(cmd, "PING") == 0) {
        char pong[600];
        _snprintf(pong, sizeof(pong), "PONG %s", rest[0] ? rest : ":");
        pong[sizeof(pong) - 1] = '\0';
        send_line(ss->sock, pong);
        return;
    }

    /* CAP negotiation. LS may be multi-line under 302: a continuation
     * carries " LS * :"; only the final " LS :" line triggers REQ. */
    if (strcmp(cmd, "CAP") == 0) {
        if (!ss->cap_req_sent &&
            strstr(rest, " LS ") && !strstr(rest, " LS * ")) {
            send_line(ss->sock, "CAP REQ :server-time batch message-tags");
            ss->cap_req_sent = 1;
        } else if (ss->cap_req_sent && !ss->cap_end_sent &&
                   (strstr(rest, " ACK ") || strstr(rest, " NAK "))) {
            send_line(ss->sock, "CAP END");
            ss->cap_end_sent = 1;
        }
        /* Banner: mirror HexChat's "Capabilities supported/acknowledged"
         * lines. The cap list is the trailing param after the first ':'. */
        if (ss->banner) {
            const char *colon = strchr(rest, ':');
            const char *caps  = colon ? colon + 1 : rest;
            char        sline[1200];
            sline[0] = '\0';
            if (strstr(rest, " ACK ") || strstr(rest, " NAK "))
                _snprintf(sline, sizeof(sline), "Capabilities acknowledged: %s", caps);
            else if (strstr(rest, " LS "))
                _snprintf(sline, sizeof(sline), "Capabilities supported: %s", caps);
            if (sline[0]) {
                sline[sizeof(sline) - 1] = '\0';
                emit_line(c, IRC_EVT_SYSTEM, sline);
            }
        }
        return;
    }

    /* 001 welcome: registered -> JOIN the channel. Also marks the whole
     * client as having registered at least once, which flips connect
     * failures from terminal to silent-retry from here on. */
    if (strcmp(cmd, "001") == 0) {
        char join[128];
        ss->reached_reg   = 1;
        c->ever_registered = 1;
        /* Lightweight registration signal so the viewer can replace its
         * "Connecting..." status with a connected one. LPARAM unused. */
        PostMessageA(c->notify_hwnd, c->notify_msg, IRC_EVT_REGISTERED, 0);
        _snprintf(join, sizeof(join), "JOIN %s", c->channel);
        join[sizeof(join) - 1] = '\0';
        send_line(ss->sock, join);
        if (ss->banner) forward_numeric(c, rest);   /* "Welcome to ..." */
        return;
    }

    /* 433 ERR_NICKNAMEINUSE: pick a fresh nick and retry (suppressed from
     * the banner -- we auto-recover, no need to show it). */
    if (strcmp(cmd, "433") == 0) {
        char nl[128];
        gen_nick(c->nick_pattern, ss->nick, sizeof(ss->nick));
        _snprintf(nl, sizeof(nl), "NICK %s", ss->nick);
        nl[sizeof(nl) - 1] = '\0';
        send_line(ss->sock, nl);
        return;
    }

    /* Other numerics: JOIN-rejection errors always surface as feedback;
     * the rest form the registration banner (welcome/host/version/ISUPPORT,
     * LUSERS, MOTD) and are forwarded only during a banner session. The
     * NAMES list (353/366) is channel-join noise -- always skipped. 376/422
     * (End of MOTD / no MOTD) closes the banner. */
    if (is_numeric_cmd(cmd)) {
        if (is_join_error(cmd)) {
            forward_numeric(c, rest);
            return;
        }
        if (strcmp(cmd, "353") == 0 || strcmp(cmd, "366") == 0) return;
        if (ss->banner) {
            forward_numeric(c, rest);
            if (strcmp(cmd, "376") == 0 || strcmp(cmd, "422") == 0) {
                ss->banner        = 0;
                c->banner_pending = 0;   /* reconnects stay quiet */
            }
        }
        return;
    }

    /* The only user-visible traffic: forward it. */
    if (strcmp(cmd, "PRIVMSG") == 0 || strcmp(cmd, "NOTICE") == 0) {
        IrcMsgKind kind = (cmd[0] == 'P') ? IRC_MSG_PRIVMSG : IRC_MSG_NOTICE;
        char       target[64];
        char      *text;
        char      *tsp;
        tsp = strchr(rest, ' ');
        if (!tsp) return;                 /* need "<target> <text>" */
        {
            size_t tl = (size_t)(tsp - rest);
            if (tl >= sizeof(target)) tl = sizeof(target) - 1;
            memcpy(target, rest, tl);
            target[tl] = '\0';
        }
        text = tsp + 1;
        while (*text == ' ') text++;
        if (*text == ':') text++;          /* strip trailing-param marker */
        forward_message(c, kind, nick, target, msgidv, text, timev);
        return;
    }

    /* Everything else (numerics, MOTD, MODE, JOIN/PART/QUIT/NICK, NAMES,
     * BATCH framing, ...) is consumed silently. */
}

/* Run one connected session to completion. Returns 1 if registration
 * (001) was reached, so the caller can reset its reconnect backoff. */
static int run_session(IrcClient *c, SOCKET s)
{
    Session       ss;
    char          acc[IRC_LINE_MAX];
    int           acclen = 0;
    unsigned char rbuf[IRC_RECV_BUF];
    char          nl[128];
    char          userline[192];

    ss.sock         = s;
    ss.cap_req_sent = 0;
    ss.cap_end_sent = 0;
    ss.reached_reg  = 0;
    ss.banner       = c->banner_pending;   /* show banner on the manual session */
    gen_nick(c->nick_pattern, ss.nick, sizeof(ss.nick));

    /* Kick off negotiation + registration. Registration is deferred by
     * the server until CAP END, which we send after the LS/ACK exchange. */
    send_line(s, "CAP LS 302");
    _snprintf(nl, sizeof(nl), "NICK %s", ss.nick);
    nl[sizeof(nl) - 1] = '\0';
    send_line(s, nl);
    _snprintf(userline, sizeof(userline),
              "USER %s 0 * :MGT Unicorn Viewer", ss.nick);
    userline[sizeof(userline) - 1] = '\0';
    send_line(s, userline);

    while (!stop_requested(c)) {
        int n = recv(s, (char *)rbuf, sizeof(rbuf), 0);
        int i;
        if (n <= 0) break;                /* peer closed or error */
        for (i = 0; i < n; i++) {
            char ch = (char)rbuf[i];
            if (ch == '\n') {
                if (acclen > 0 && acc[acclen - 1] == '\r') acclen--;
                acc[acclen] = '\0';
                if (acc[0]) handle_line(c, &ss, acc);
                acclen = 0;
            } else if (acclen < (int)sizeof(acc) - 1) {
                acc[acclen++] = ch;
            } else {
                acclen = 0;               /* oversized line: drop it */
            }
        }
    }
    return ss.reached_reg;
}

/* Interruptible backoff wait. Returns 0 if stop was requested during the
 * wait (caller should exit), 1 otherwise (and doubles *backoff_ms up to
 * the cap for next time). */
static int wait_backoff(IrcClient *c, int *backoff_ms)
{
    int waited = 0;
    int target = *backoff_ms;
    while (waited < target) {
        if (stop_requested(c)) return 0;
        Sleep(200);
        waited += 200;
    }
    *backoff_ms *= 2;
    if (*backoff_ms > IRC_BACKOFF_MAX_MS) *backoff_ms = IRC_BACKOFF_MAX_MS;
    return 1;
}

static DWORD WINAPI rx_thread_proc(LPVOID lp)
{
    IrcClient *c          = (IrcClient *)lp;
    int        backoff_ms = IRC_BACKOFF_MIN_MS;

    while (!stop_requested(c)) {
        SOCKET s;
        int    reached;
        int    reason = IRC_FAIL_NONE;

        /* Banner session (the initial manual connect) announces the attempt
         * HexChat-style before dialing. */
        if (c->banner_pending && !c->ever_registered) {
            char line[400];
            _snprintf(line, sizeof(line), "Connecting to %s:%d %s...",
                      c->host, c->port, c->channel);
            line[sizeof(line) - 1] = '\0';
            emit_line(c, IRC_EVT_SYSTEM, line);
        }

        s = open_socket(c, &reason);   /* bounded + interruptible; sets c->sock */
        if (s == INVALID_SOCKET) {
            if (stop_requested(c)) break;          /* user Disconnect: silent */
            if (!c->ever_registered) {
                /* Initial manual connect never registered -> TERMINAL.
                 * Report the reason and stop; no silent-retry freeze. */
                emit_line(c, IRC_EVT_FAILED, fail_reason_text(reason));
                break;
            }
            /* Established session dropped: keep reconnecting silently. */
            if (!wait_backoff(c, &backoff_ms)) break;
            continue;
        }

        if (c->banner_pending && !c->ever_registered)
            emit_line(c, IRC_EVT_SYSTEM, "Connected");

        reached = run_session(c, s);

        EnterCriticalSection(&c->cs);
        if (c->sock != INVALID_SOCKET) {
            closesocket(c->sock);
            c->sock = INVALID_SOCKET;
        }
        LeaveCriticalSection(&c->cs);

        if (stop_requested(c)) break;              /* user Disconnect: silent */

        if (!reached && !c->ever_registered) {
            /* Connected but the peer closed before registration on the
             * initial manual attempt (e.g. not an IRC server). TERMINAL. */
            emit_line(c, IRC_EVT_FAILED, "Connection closed before registration");
            break;
        }

        if (reached) backoff_ms = IRC_BACKOFF_MIN_MS;  /* good run: reset */
        if (!wait_backoff(c, &backoff_ms)) break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int irc_client_open(const char *host, int port,
                    const char *channel, const char *nick_pattern,
                    int show_banner,
                    HWND notify_hwnd, UINT notify_msg,
                    IrcClient **out)
{
    IrcClient *c;
    DWORD      tid;

    if (!host || !channel || !out) return 1;
    *out = NULL;

    c = (IrcClient *)calloc(1, sizeof(*c));
    if (!c) return 2;
    c->sock           = INVALID_SOCKET;
    c->notify_hwnd    = notify_hwnd;
    c->notify_msg     = notify_msg;
    c->port           = port;
    c->ever_registered = 0;
    c->banner_pending  = show_banner ? 1 : 0;
    strncpy(c->host,    host,    sizeof(c->host)    - 1);
    strncpy(c->channel, channel, sizeof(c->channel) - 1);
    strncpy(c->nick_pattern,
            (nick_pattern && nick_pattern[0]) ? nick_pattern : "mgtview-XXXX",
            sizeof(c->nick_pattern) - 1);

    InitializeCriticalSection(&c->cs);
    /* Seed the nick RNG per handle; the address adds entropy so two
     * viewers opened in the same tick still diverge. */
    srand(GetTickCount() ^ (unsigned)(ULONG_PTR)c);

    c->rx_thread = CreateThread(NULL, 0, rx_thread_proc, c, 0, &tid);
    if (!c->rx_thread) {
        DeleteCriticalSection(&c->cs);
        free(c);
        return 3;
    }

    *out = c;
    return 0;
}

void irc_client_close(IrcClient *c)
{
    if (!c) return;
    InterlockedExchange(&c->stop_request, 1);

    EnterCriticalSection(&c->cs);
    if (c->sock != INVALID_SOCKET) {
        shutdown(c->sock, SD_BOTH);       /* unblock recv() in the worker */
        closesocket(c->sock);
        c->sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&c->cs);

    if (c->rx_thread) {
        WaitForSingleObject(c->rx_thread, 4000);
        CloseHandle(c->rx_thread);
        c->rx_thread = NULL;
    }
    DeleteCriticalSection(&c->cs);
    free(c);
}

void irc_client_free_message(IrcMessage *m)
{
    if (m) free(m);
}
