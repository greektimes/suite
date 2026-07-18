/*
 * sockets_win.c - Winsock2 client-side TCP layer for Windows
 *
 * Copyright (c) 2026, Dimitri Papadopoulos and The Montreal Greek Times
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of Dimitri Papadopoulos, The Montreal Greek Times,
 *      nor the names of its contributors may be used to endorse or promote
 *      products derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES HOWEVER CAUSED.
 * See the LICENSE file in the repository root for the full license text.
 *
 * Part of the Greek Times WAIS Search project (v0.9.1-beta2).
 * https://github.com/greektimes/wais
 */

#include "win_platform.h"
#include "sockets.h"
#include "cdialect.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

/* Global socket handle used by transport_message in ui_win.c */
SOCKET g_wais_socket = INVALID_SOCKET;

char host_name[255];
char host_address[255];

#define MAX_RETRYS 10

/*---------------------------------------------------------------------------*/
/* Client functions                                                          */
/*---------------------------------------------------------------------------*/

/*
 * Connect to a WAIS server and return a FILE* for compatibility
 * with the original freeWAIS API. The FILE* is a dummy placeholder;
 * actual I/O goes through g_wais_socket via send()/recv().
 */
FILE *
connect_to_server(hname, port)
    char *hname;
    long port;
{
    struct hostent *host;
    struct sockaddr_in name;
    int i, rc;
    unsigned long addr;

    memset(&name, 0, sizeof(name));

    /* Try numeric address first */
    addr = inet_addr(hname);
    if (addr != INADDR_NONE) {
        name.sin_family = AF_INET;
        name.sin_addr.s_addr = addr;
    } else {
        /* DNS lookup */
        host = gethostbyname(hname);
        if (host == NULL) {
            fprintf(stderr, "Cannot resolve hostname: %s\n", hname);
            return NULL;
        }
        name.sin_family = host->h_addrtype;
        memcpy(&name.sin_addr, host->h_addr_list[0], host->h_length);
    }

    name.sin_port = htons((unsigned short)port);

    g_wais_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_wais_socket == INVALID_SOCKET) {
        fprintf(stderr, "Cannot create socket\n");
        return NULL;
    }

    for (i = 0; i < MAX_RETRYS; i++) {
        rc = connect(g_wais_socket, (struct sockaddr *)&name, sizeof(name));
        if (rc == 0) {
            /*
             * Return a non-NULL FILE* as a sentinel.
             * The actual I/O bypasses stdio entirely and uses
             * g_wais_socket with send()/recv().
             * We use stderr as a harmless non-NULL value.
             */
            return stderr;
        }
#ifdef _WIN32
        {
            int err = WSAGetLastError();
            if (err == WSAEINTR) {
                Sleep(1000);
            } else {
                fprintf(stderr, "Connect failed (WSA error %d)\n", err);
                closesocket(g_wais_socket);
                g_wais_socket = INVALID_SOCKET;
                return NULL;
            }
        }
#else
        if (errno == EINTR) {
            sleep(1);
        } else {
            perror("Connect to socket did not work");
            close(g_wais_socket);
            g_wais_socket = INVALID_SOCKET;
            return NULL;
        }
#endif
    }

    fprintf(stderr, "Connect timed out after %d retries\n", MAX_RETRYS);
#ifdef _WIN32
    closesocket(g_wais_socket);
#else
    close(g_wais_socket);
#endif
    g_wais_socket = INVALID_SOCKET;
    return NULL;
}

/*---------------------------------------------------------------------------*/

void
close_connection_to_server(file)
    FILE *file;
{
    if (g_wais_socket != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(g_wais_socket);
#else
        close(g_wais_socket);
#endif
        g_wais_socket = INVALID_SOCKET;
    }
}

/*---------------------------------------------------------------------------*/

/* gethostname wrapper - works on both Winsock and POSIX */
char *
mygethostname(hostname, len)
    char *hostname;
    long len;
{
    char name[255];
    struct hostent *h;

    gethostname(name, 254);
    strncpy(hostname, name, len - 1);
    hostname[len - 1] = '\0';

    h = gethostbyname(name);
    if (h != NULL) {
        strncpy(hostname, h->h_name, len - 1);
        hostname[len - 1] = '\0';
    }

    return hostname;
}

/*---------------------------------------------------------------------------*/
/* Server functions - stubs only (not needed for client)                      */
/*---------------------------------------------------------------------------*/

void open_server(long port, long *fd, long size)
{
    (void)port; (void)fd; (void)size;
    fprintf(stderr, "open_server: not implemented in client build\n");
}

void accept_client_connection(long socket_fd, FILE **file)
{
    (void)socket_fd; (void)file;
    fprintf(stderr, "accept_client_connection: not implemented\n");
}

void fd_accept_client_connection(long socket_fd, long *fd)
{
    (void)socket_fd; (void)fd;
    fprintf(stderr, "fd_accept_client_connection: not implemented\n");
}

void close_client_connection(FILE *file)
{
    (void)file;
}

void close_server(long socket_fd)
{
    (void)socket_fd;
}
