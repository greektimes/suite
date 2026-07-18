/*
 * gopher_protocol.c - Plain RFC 1436 over TCP. Synchronous fetch.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The wire is the wire: connect, send "<selector>\r\n", read. For menu
 * and text replies the body ends with ".\r\n" (some servers just close,
 * which we accept). For binary replies (g/9/s) the server closes when
 * done. Dot-stuffing per RFC 1436 is undone in-place for MENU/TEXT.
 *
 * Winsock2 is already started by suite_shell at WinMain; we do not
 * touch WSAStartup/WSACleanup here.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "gopher_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GOPHER_DEFAULT_PORT 70
#define GOPHER_RECV_BUF     65536
#define GOPHER_INIT_CAP     65536

/* ---------- helpers ---------- */

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int starts_with(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

/* ---------- URL parser / builder ---------- */

int gopher_parse_url(const char *url,
                     char **out_host, int *out_port,
                     char *out_type, char **out_selector)
{
    const char *p, *host_start, *host_end, *port_start, *port_end, *path;
    char       *host, *selector;
    int         port;
    size_t      host_len, sel_len;
    char        type;

    if (!url || !out_host || !out_port || !out_type || !out_selector)
        return 1;

    p = url;
    if (starts_with(p, "gopher://")) p += 9;
    else if (starts_with(p, "GOPHER://")) p += 9;

    host_start = p;
    while (*p && *p != ':' && *p != '/') p++;
    host_end = p;

    port = GOPHER_DEFAULT_PORT;
    if (*p == ':') {
        p++;
        port_start = p;
        while (*p && is_digit(*p)) p++;
        port_end = p;
        if (port_end > port_start) {
            char buf[16];
            size_t n = (size_t)(port_end - port_start);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, port_start, n);
            buf[n] = '\0';
            port = atoi(buf);
            if (port <= 0 || port > 65535) port = GOPHER_DEFAULT_PORT;
        }
    }

    /* Skip the leading '/' of the path if present. */
    path = p;
    if (*path == '/') path++;

    /* Type char is the first character after the leading slash, if any.
     * Empty path -> type='1', empty selector (per RFC 1436 root menu). */
    if (*path == '\0') {
        type = '1';
        sel_len = 0;
        selector = (char *)malloc(1);
        if (!selector) return 2;
        selector[0] = '\0';
    } else {
        type = *path;
        path++;
        sel_len = strlen(path);
        selector = (char *)malloc(sel_len + 1);
        if (!selector) return 2;
        memcpy(selector, path, sel_len + 1);
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0) { free(selector); return 3; }
    host = (char *)malloc(host_len + 1);
    if (!host) { free(selector); return 2; }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    *out_host     = host;
    *out_port     = port;
    *out_type     = type;
    *out_selector = selector;
    return 0;
}

char *gopher_build_url(const char *host, int port, char type, const char *selector)
{
    size_t host_len, sel_len, total;
    char  *out;
    char   port_buf[16];
    int    have_port;

    if (!host) return NULL;
    if (!selector) selector = "";

    host_len = strlen(host);
    sel_len  = strlen(selector);

    have_port = (port > 0 && port != GOPHER_DEFAULT_PORT);
    if (have_port)
        _snprintf(port_buf, sizeof(port_buf), ":%d", port);
    else
        port_buf[0] = '\0';

    /* "gopher://" + host + [":port"] + "/" + type + selector + NUL */
    total = 9 + host_len + strlen(port_buf) + 1 + 1 + sel_len + 1;
    out = (char *)malloc(total);
    if (!out) return NULL;
    _snprintf(out, total, "gopher://%s%s/%c%s",
              host, port_buf, type, selector);
    return out;
}

/* ---------- TCP transport ---------- */

static SOCKET gopher_connect(const char *host, int port, char *err, size_t err_n)
{
    struct addrinfo  hints;
    struct addrinfo *ai = NULL, *p;
    char             port_str[16];
    SOCKET           s = INVALID_SOCKET;
    int              rc;
    DWORD            tmo_ms = GOPHER_TIMEOUT_SEC * 1000;

    if (err && err_n) err[0] = '\0';

    _snprintf(port_str, sizeof(port_str), "%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;          /* dual-stack: v4 + v6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    rc = getaddrinfo(host, port_str, &hints, &ai);
    if (rc != 0 || !ai) {
        if (err && err_n)
            _snprintf(err, err_n, "getaddrinfo(%s:%d) failed (%d).",
                      host, port, rc);
        return INVALID_SOCKET;
    }

    for (p = ai; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&tmo_ms, sizeof(tmo_ms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                   (const char *)&tmo_ms, sizeof(tmo_ms));
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(ai);

    if (s == INVALID_SOCKET && err && err_n)
        _snprintf(err, err_n, "connect(%s:%d) failed (WSA %d).",
                  host, port, WSAGetLastError());
    return s;
}

/* ---------- dot-stuffing / terminator stripping ---------- */

/* Walk the buffer line by line. Lines starting with ".\r\n" mark the
 * terminator (and everything after is dropped). Lines starting with
 * ".." have one dot dropped. The transform is in-place: 'len' shrinks. */
static size_t gopher_undot_inplace(char *buf, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        /* Find end of current line: scan until \n or end-of-buffer. */
        size_t line_start = r;
        size_t line_end;
        while (r < len && buf[r] != '\n') r++;
        if (r < len) r++;   /* include the trailing \n */
        line_end = r;
        {
            size_t line_len = line_end - line_start;
            char  *line     = buf + line_start;

            /* Lone "." (with optional \r\n) means end-of-body. */
            if ((line_len == 2 && line[0] == '.' && line[1] == '\n') ||
                (line_len == 3 && line[0] == '.' && line[1] == '\r' && line[2] == '\n') ||
                (line_len == 1 && line[0] == '.')) {
                break;
            }

            if (line_len >= 2 && line[0] == '.' && line[1] == '.') {
                memmove(buf + w, line + 1, line_len - 1);
                w += line_len - 1;
            } else {
                if (w != line_start)
                    memmove(buf + w, line, line_len);
                w += line_len;
            }
        }
    }
    return w;
}

/* ---------- public fetch ---------- */

int gopher_fetch_sync(const char *host, int port,
                      const char *selector,
                      GopherFetchKind kind,
                      char **out_bytes, size_t *out_size)
{
    SOCKET  s;
    char    err[256];
    char   *buf = NULL;
    size_t  cap = GOPHER_INIT_CAP;
    size_t  len = 0;
    char    recv_buf[GOPHER_RECV_BUF];
    int     n;
    char   *send_line;
    size_t  sel_len, req_len;
    int     sent;

    if (!host || port <= 0 || !out_bytes || !out_size) return 1;
    if (!selector) selector = "";

    s = gopher_connect(host, port, err, sizeof(err));
    if (s == INVALID_SOCKET) return 2;

    sel_len  = strlen(selector);
    req_len  = sel_len + 2;     /* selector + "\r\n" */
    send_line = (char *)malloc(req_len + 1);
    if (!send_line) { closesocket(s); return 3; }
    memcpy(send_line, selector, sel_len);
    send_line[sel_len]     = '\r';
    send_line[sel_len + 1] = '\n';
    send_line[sel_len + 2] = '\0';

    sent = send(s, send_line, (int)req_len, 0);
    free(send_line);
    if (sent < 0) {
        closesocket(s);
        return 4;
    }

    buf = (char *)malloc(cap);
    if (!buf) { closesocket(s); return 3; }

    for (;;) {
        n = recv(s, recv_buf, (int)sizeof(recv_buf), 0);
        if (n <= 0) break;
        if (len + (size_t)n > cap) {
            size_t new_cap = cap;
            char  *new_buf;
            while (new_cap < len + (size_t)n) new_cap *= 2;
            new_buf = (char *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                closesocket(s);
                return 3;
            }
            buf = new_buf;
            cap = new_cap;
        }
        memcpy(buf + len, recv_buf, (size_t)n);
        len += (size_t)n;
    }
    closesocket(s);

    if (kind == GOPHER_KIND_MENU || kind == GOPHER_KIND_TEXT) {
        len = gopher_undot_inplace(buf, len);
    }

    /* Always NUL-terminate so callers can treat MENU/TEXT as a C string
     * without a separate copy step. Binary callers ignore the trailing
     * NUL (they read up to out_size). */
    if (len + 1 > cap) {
        char *new_buf = (char *)realloc(buf, len + 1);
        if (!new_buf) { free(buf); return 3; }
        buf = new_buf;
    }
    buf[len] = '\0';

    *out_bytes = buf;
    *out_size  = len;
    return 0;
}
