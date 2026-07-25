/*
 * ftp_fetch.c - Minimal shared FTP retrieve service (passive, anonymous,
 *               binary). See ftp_fetch.h.
 *
 * Wire flow (RFC 959): connect control (tcp/21); read 220 greeting;
 * USER anonymous / PASS <placeholder>; TYPE I; PASV (parse the data
 * endpoint); open the data connection; RETR <path>; stream the data socket
 * to the local file until EOF; read the 226 completion; QUIT. Passive by
 * construction. The data connection is made to the PASV port at the control
 * connection's own peer address (not the address the server prints in the
 * 227 reply), which is the standard fix for servers behind NAT that report
 * an unroutable PASV address.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "ftp_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FTP_CTRL_TIMEOUT_MS  15000
#define FTP_DATA_TIMEOUT_MS  30000
#define FTP_CONNECT_SECS     12
#define FTP_RECV_CHUNK       32768

static void ftp_err(char *buf, int sz, const char *msg)
{
    if (buf && sz > 0) { strncpy(buf, msg, (size_t)sz - 1); buf[sz - 1] = '\0'; }
}

/* Blocking TCP connect with a bounded timeout. On success *out_peer holds
 * the sockaddr we connected to (so the data connection can reuse the IP). */
static SOCKET ftp_connect(const char *host, int port, int seconds,
                          struct sockaddr_in *out_peer)
{
    SOCKET             s;
    struct sockaddr_in addr;
    unsigned long      a;
    u_long             mode;
    int                rc;
    fd_set             wfds, efds;
    struct timeval     tv;
    int                err = 0, errlen = sizeof(err), sel;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    a = inet_addr(host);
    if (a != INADDR_NONE) {
        addr.sin_addr.s_addr = a;
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return INVALID_SOCKET;
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    if (rc != 0) {
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        tv.tv_sec = seconds; tv.tv_usec = 0;
        sel = select(0, NULL, &wfds, &efds, &tv);
        if (sel <= 0 || FD_ISSET(s, &efds)) { closesocket(s); return INVALID_SOCKET; }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) {
            closesocket(s); return INVALID_SOCKET;
        }
    }
    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);
    if (out_peer) *out_peer = addr;
    return s;
}

static int ftp_send(SOCKET s, const char *cmd)
{
    int len = (int)strlen(cmd), sent = 0, n;
    while (sent < len) {
        n = send(s, cmd + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

/* Read one CRLF-terminated control line into buf (without CR/LF). Returns
 * the length, or -1 on error/close. */
static int ftp_read_line(SOCKET s, char *buf, int sz)
{
    int li = 0;
    for (;;) {
        char c;
        int n = recv(s, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && li < sz - 1) buf[li++] = c;
    }
    buf[li] = '\0';
    return li;
}

/* Read a (possibly multi-line) FTP reply. Returns the 3-digit code, or -1
 * on error. The final line's text is left in last (may be NULL). */
static int ftp_reply(SOCKET s, char *last, int last_sz)
{
    char line[1024];
    int  first = 1, code = 0;
    for (;;) {
        int li = ftp_read_line(s, line, sizeof line);
        if (li < 0) return -1;
        if (last && last_sz > 0) { strncpy(last, line, (size_t)last_sz - 1); last[last_sz - 1] = '\0'; }
        if (first) {
            if (li >= 3 && line[0] >= '0' && line[0] <= '9')
                code = (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0');
            first = 0;
            /* "ddd " => final single line; "ddd-" => multi-line continues. */
            if (li >= 4 && line[3] == ' ') return code;
            if (li < 4) return code;   /* short line, treat as done */
        } else {
            /* Continuation ends at a line "ddd " with the same code. */
            if (li >= 4 && line[3] == ' ' &&
                (line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0') == code)
                return code;
        }
    }
}

/* Send a command and return the reply code (-1 on I/O error). */
static int ftp_cmd(SOCKET s, const char *cmd, char *last, int last_sz)
{
    if (!ftp_send(s, cmd)) return -1;
    return ftp_reply(s, last, last_sz);
}

/* Parse the 6 numbers of a PASV 227 reply. Returns 1 and sets *port on
 * success; the data IP is ignored (we reuse the control peer address). */
static int ftp_parse_pasv(const char *line, int *port)
{
    const char *p;
    int a1, a2, a3, a4, p1, p2;
    /* The address tuple is in parentheses; parse after '(' so the leading
     * reply code ("227") is never mistaken for the first octet. If there is
     * no '(', skip past the code word and on to the first digit. */
    p = strchr(line, '(');
    if (p) {
        p++;
    } else {
        p = line;
        while (*p && *p != ' ') p++;             /* skip the code */
        while (*p && (*p < '0' || *p > '9')) p++; /* to first digit */
    }
    if (sscanf(p, "%d,%d,%d,%d,%d,%d", &a1, &a2, &a3, &a4, &p1, &p2) != 6)
        return 0;
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) return 0;
    *port = p1 * 256 + p2;
    return 1;
}

int ftp_fetch_file(const char *host, int port,
                   const char *remote_path, const char *local_path,
                   ftp_progress_cb progress, void *user,
                   long *out_total, char *errbuf, int errbuf_sz)
{
    SOCKET             ctrl = INVALID_SOCKET, data = INVALID_SOCKET;
    struct sockaddr_in peer, dpeer;
    DWORD              tmo;
    char               line[1024], cmd[1200];
    int                code, dport = 0;
    FILE              *fp = NULL;
    long               total = 0;
    unsigned char     *buf = NULL;
    int                ok = 0;

    if (out_total) *out_total = 0;
    if (!host || !*host || !remote_path || !*remote_path || !local_path || !*local_path) {
        ftp_err(errbuf, errbuf_sz, "Bad download parameters.");
        return 0;
    }
    if (port <= 0 || port > 65535) port = FTP_FETCH_DEFAULT_PORT;

    ctrl = ftp_connect(host, port, FTP_CONNECT_SECS, &peer);
    if (ctrl == INVALID_SOCKET) {
        _snprintf(errbuf, errbuf_sz, "Cannot connect to %s.", host);
        return 0;
    }
    tmo = FTP_CTRL_TIMEOUT_MS;
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);

    if (ftp_reply(ctrl, line, sizeof line) / 100 != 2) {   /* 220 greeting */
        ftp_err(errbuf, errbuf_sz, "No FTP greeting from server.");
        goto done;
    }

    code = ftp_cmd(ctrl, "USER anonymous\r\n", line, sizeof line);
    if (code != 331 && code / 100 != 2) {
        _snprintf(errbuf, errbuf_sz, "USER rejected: %.180s", line);
        goto done;
    }
    if (code == 331) {   /* password wanted */
        code = ftp_cmd(ctrl, "PASS unicorn@greektimes.ca\r\n", line, sizeof line);
        if (code / 100 != 2) {
            _snprintf(errbuf, errbuf_sz, "Anonymous login failed: %.180s", line);
            goto done;
        }
    }

    if (ftp_cmd(ctrl, "TYPE I\r\n", line, sizeof line) / 100 != 2) {
        ftp_err(errbuf, errbuf_sz, "Server refused binary mode.");
        goto done;
    }

    code = ftp_cmd(ctrl, "PASV\r\n", line, sizeof line);
    if (code != 227 || !ftp_parse_pasv(line, &dport)) {
        ftp_err(errbuf, errbuf_sz, "Server refused passive mode.");
        goto done;
    }

    /* Data connection: PASV port at the control peer's IP (NAT-safe). */
    dpeer = peer;
    dpeer.sin_port = htons((unsigned short)dport);
    data = socket(AF_INET, SOCK_STREAM, 0);
    if (data == INVALID_SOCKET ||
        connect(data, (struct sockaddr *)&dpeer, sizeof dpeer) != 0) {
        ftp_err(errbuf, errbuf_sz, "Cannot open data connection.");
        goto done;
    }
    tmo = FTP_DATA_TIMEOUT_MS;
    setsockopt(data, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);

    /* Open the local file only once the transfer is about to start. */
    fp = fopen(local_path, "wb");
    if (!fp) {
        _snprintf(errbuf, errbuf_sz, "Cannot create %s.", local_path);
        goto done;
    }

    _snprintf(cmd, sizeof cmd, "RETR %s\r\n", remote_path);
    code = ftp_cmd(ctrl, cmd, line, sizeof line);
    if (code / 100 != 1) {   /* expect 150 or 125 */
        _snprintf(errbuf, errbuf_sz, "RETR failed: %.180s", line);
        goto done;
    }

    buf = (unsigned char *)malloc(FTP_RECV_CHUNK);
    if (!buf) { ftp_err(errbuf, errbuf_sz, "Out of memory."); goto done; }

    for (;;) {
        int n = recv(data, (char *)buf, FTP_RECV_CHUNK, 0);
        if (n == 0) break;                 /* EOF: transfer complete */
        if (n < 0) { ftp_err(errbuf, errbuf_sz, "Data connection dropped."); goto done; }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            ftp_err(errbuf, errbuf_sz, "Cannot write to local file.");
            goto done;
        }
        total += n;
        if (progress) progress(user, total);
    }

    closesocket(data); data = INVALID_SOCKET;
    if (fclose(fp) != 0) { fp = NULL; ftp_err(errbuf, errbuf_sz, "Cannot flush local file."); goto done; }
    fp = NULL;

    /* Read the transfer-complete reply (226 / 250). */
    code = ftp_reply(ctrl, line, sizeof line);
    if (code / 100 != 2) {
        _snprintf(errbuf, errbuf_sz, "Transfer incomplete: %.180s", line);
        goto done;
    }

    ok = 1;
    if (out_total) *out_total = total;

done:
    if (buf) free(buf);
    if (fp)  fclose(fp);
    if (data != INVALID_SOCKET) closesocket(data);
    if (ctrl != INVALID_SOCKET) {
        ftp_send(ctrl, "QUIT\r\n");
        closesocket(ctrl);
    }
    if (!ok) remove(local_path);   /* never leave a partial file behind */
    return ok;
}
