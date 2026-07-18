/*
 * qotd_proto.c - Raw TCP read helper. See qotd_proto.h.
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

#include "qotd_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QOTD_RECV_BUF  4096

struct QotdProto {
    SOCKET    sock;
    HANDLE    rx_thread;
    volatile LONG stop_request;
    HWND      notify_hwnd;
    UINT      notify_msg;
    char     *host;
    int       port;
};

typedef struct WorkerArgs {
    QotdProto *q;
    char       host[256];
    int        port;
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
    WorkerArgs *w = (WorkerArgs *)lp;
    QotdProto  *q = w->q;
    SOCKET      s;
    unsigned char buf[QOTD_RECV_BUF];

    s = connect_to(w->host, w->port);
    if (s == INVALID_SOCKET) {
        PostMessageA(q->notify_hwnd, q->notify_msg,
                     TELNET_EVT_ERROR, (LPARAM)"Connection failed");
        PostMessageA(q->notify_hwnd, q->notify_msg, TELNET_EVT_CLOSED, 0);
        free(w);
        return 0;
    }
    q->sock = s;
    PostMessageA(q->notify_hwnd, q->notify_msg, TELNET_EVT_CONNECTED, 0);

    while (!InterlockedCompareExchange(&q->stop_request, 0, 0)) {
        int n = recv(s, (char *)buf, QOTD_RECV_BUF, 0);
        TelnetDataChunk *chunk;
        if (n <= 0) break;
        chunk = (TelnetDataChunk *)malloc(sizeof(TelnetDataChunk));
        if (!chunk) continue;
        chunk->data = (unsigned char *)malloc((size_t)n);
        if (!chunk->data) { free(chunk); continue; }
        memcpy(chunk->data, buf, (size_t)n);
        chunk->len = n;
        PostMessageA(q->notify_hwnd, q->notify_msg,
                     TELNET_EVT_DATA, (LPARAM)chunk);
    }

    if (q->sock != INVALID_SOCKET) {
        closesocket(q->sock);
        q->sock = INVALID_SOCKET;
    }
    PostMessageA(q->notify_hwnd, q->notify_msg, TELNET_EVT_CLOSED, 0);
    free(w);
    return 0;
}

int qotd_proto_open(const char *host, int port,
                    HWND notify_hwnd, UINT notify_msg,
                    QotdProto **out)
{
    QotdProto  *q;
    WorkerArgs *w;
    DWORD       tid;

    if (!host || !out) return 1;
    *out = NULL;

    q = (QotdProto *)calloc(1, sizeof(*q));
    if (!q) return 2;
    q->sock        = INVALID_SOCKET;
    q->notify_hwnd = notify_hwnd;
    q->notify_msg  = notify_msg;
    q->host        = _strdup(host);
    q->port        = port;

    w = (WorkerArgs *)calloc(1, sizeof(*w));
    if (!w) { free(q->host); free(q); return 3; }
    w->q = q;
    w->port = port;
    strncpy(w->host, host, sizeof(w->host) - 1);

    q->rx_thread = CreateThread(NULL, 0, rx_thread_proc, w, 0, &tid);
    if (!q->rx_thread) { free(w); free(q->host); free(q); return 4; }

    *out = q;
    return 0;
}

void qotd_proto_close(QotdProto *q)
{
    if (!q) return;
    InterlockedExchange(&q->stop_request, 1);
    if (q->sock != INVALID_SOCKET) {
        shutdown(q->sock, SD_BOTH);
        closesocket(q->sock);
        q->sock = INVALID_SOCKET;
    }
    if (q->rx_thread) {
        WaitForSingleObject(q->rx_thread, 3000);
        CloseHandle(q->rx_thread);
        q->rx_thread = NULL;
    }
    free(q->host);
    free(q);
}
