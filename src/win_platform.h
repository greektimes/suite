/*
 * win_platform.h - Windows/Winsock compatibility layer for freeWAIS
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

#ifndef WIN_PLATFORM_H
#define WIN_PLATFORM_H

#ifdef _WIN32

/* Winsock2 must come before windows.h */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* Link with Winsock library */
#pragma comment(lib, "ws2_32.lib")

/* Map POSIX types not available on Windows */
#ifndef _SSIZE_T_DEFINED
typedef int ssize_t;
#endif
#ifndef caddr_t
typedef char *caddr_t;
#endif

/* Map POSIX sleep to Windows Sleep (seconds to milliseconds) */
#define sleep(s) Sleep((s) * 1000)

/* Map Strerror to standard strerror */
#define Strerror(e) strerror(e)

/* Suppress the fdopen/fileno approach - we use Winsock send/recv directly */
/* The WAIS_USE_WINSOCK flag tells our patched code to use SOCKET handles */
#define WAIS_USE_WINSOCK 1

/* Winsock initialization helper */
static inline int wais_winsock_init(void) {
    WSADATA wsaData;
    return WSAStartup(MAKEWORD(2, 2), &wsaData);
}

static inline void wais_winsock_cleanup(void) {
    WSACleanup();
}

/* gethostname is available in Winsock */
/* close() on sockets must use closesocket() */
#define close_socket(s) closesocket(s)

#else /* Unix */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define Strerror(e) strerror(e)
#define close_socket(s) close(s)
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

static inline int wais_winsock_init(void) { return 0; }
static inline void wais_winsock_cleanup(void) {}

#endif /* _WIN32 */

#endif /* WIN_PLATFORM_H */
