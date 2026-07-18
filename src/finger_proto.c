/*
 * finger_proto.c - RFC 1288 Finger client. See finger_proto.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
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

#include "finger_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FINGER_RECV_BUF  4096

struct FingerProto {
    SOCKET    sock;
    HANDLE    rx_thread;
    volatile LONG stop_request;
    HWND      notify_hwnd;
    UINT      notify_msg;
    char     *host;
    int       port;
    char     *query;
};

typedef struct WorkerArgs {
    FingerProto *f;
    char         host[256];
    int          port;
    char         query[256];
} WorkerArgs;

static SOCKET connect_to(const char *host, int port)
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
    if (rc != 0 || !res) return INVALID_SOCKET;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

static DWORD WINAPI rx_thread_proc(LPVOID lp)
{
    WorkerArgs  *w = (WorkerArgs *)lp;
    FingerProto *f = w->f;
    SOCKET       s;
    char         qline[300];
    unsigned char buf[FINGER_RECV_BUF];

    s = connect_to(w->host, w->port);
    if (s == INVALID_SOCKET) {
        PostMessageA(f->notify_hwnd, f->notify_msg,
                     TELNET_EVT_ERROR, (LPARAM)"Connection failed");
        PostMessageA(f->notify_hwnd, f->notify_msg, TELNET_EVT_CLOSED, 0);
        free(w);
        return 0;
    }
    f->sock = s;
    PostMessageA(f->notify_hwnd, f->notify_msg, TELNET_EVT_CONNECTED, 0);

    /* Send "<query>\r\n". RFC 1288 also allows an empty query. */
    sprintf(qline, "%s\r\n", w->query);
    send(s, qline, (int)strlen(qline), 0);

    while (!InterlockedCompareExchange(&f->stop_request, 0, 0)) {
        int n = recv(s, (char *)buf, FINGER_RECV_BUF, 0);
        TelnetDataChunk *chunk;
        if (n <= 0) break;
        chunk = (TelnetDataChunk *)malloc(sizeof(TelnetDataChunk));
        if (!chunk) continue;
        chunk->data = (unsigned char *)malloc((size_t)n);
        if (!chunk->data) { free(chunk); continue; }
        memcpy(chunk->data, buf, (size_t)n);
        chunk->len = n;
        PostMessageA(f->notify_hwnd, f->notify_msg,
                     TELNET_EVT_DATA, (LPARAM)chunk);
    }

    if (f->sock != INVALID_SOCKET) {
        closesocket(f->sock);
        f->sock = INVALID_SOCKET;
    }
    PostMessageA(f->notify_hwnd, f->notify_msg, TELNET_EVT_CLOSED, 0);
    free(w);
    return 0;
}

int finger_proto_open(const char *host, int port,
                      const char *query,
                      HWND notify_hwnd, UINT notify_msg,
                      FingerProto **out)
{
    FingerProto *f;
    WorkerArgs  *w;
    DWORD        tid;

    if (!host || !out) return 1;
    *out = NULL;

    f = (FingerProto *)calloc(1, sizeof(*f));
    if (!f) return 2;
    f->sock        = INVALID_SOCKET;
    f->notify_hwnd = notify_hwnd;
    f->notify_msg  = notify_msg;
    f->host        = _strdup(host);
    f->port        = port;
    f->query       = _strdup(query ? query : "");

    w = (WorkerArgs *)calloc(1, sizeof(*w));
    if (!w) { free(f->host); free(f->query); free(f); return 3; }
    w->f = f;
    w->port = port;
    strncpy(w->host,  host,             sizeof(w->host) - 1);
    strncpy(w->query, query ? query : "", sizeof(w->query) - 1);

    f->rx_thread = CreateThread(NULL, 0, rx_thread_proc, w, 0, &tid);
    if (!f->rx_thread) { free(w); free(f->host); free(f->query); free(f); return 4; }

    *out = f;
    return 0;
}

void finger_proto_close(FingerProto *f)
{
    if (!f) return;
    InterlockedExchange(&f->stop_request, 1);
    if (f->sock != INVALID_SOCKET) {
        shutdown(f->sock, SD_BOTH);
        closesocket(f->sock);
        f->sock = INVALID_SOCKET;
    }
    if (f->rx_thread) {
        WaitForSingleObject(f->rx_thread, 3000);
        CloseHandle(f->rx_thread);
        f->rx_thread = NULL;
    }
    free(f->host);
    free(f->query);
    free(f);
}
