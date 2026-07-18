/*
 * telnet_proto.h - RFC 854 Telnet client with asynchronous I/O.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Connects to host:port via TCP on a worker thread and pushes events
 * (connected, data chunks, errors, close) back to a UI HWND via
 * PostMessage. The IAC stripping and option negotiation happen inside
 * this TU; callers see only payload bytes via TELNET_EVT_DATA.
 *
 * One TelnetProto handle == one active or pending connection. Calling
 * telnet_proto_close() requests the worker to stop, joins it, and
 * frees the handle. Posting code holds a stable HWND for the lifetime
 * of the handle (the F6 module: its render hwnd).
 */

#ifndef TELNET_PROTO_H
#define TELNET_PROTO_H

#include <stddef.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WPARAM values for the notify message. */
#define TELNET_EVT_CONNECTED  1
#define TELNET_EVT_DATA       2
#define TELNET_EVT_CLOSED     3
#define TELNET_EVT_ERROR      4

/* DataChunk delivered with WPARAM == TELNET_EVT_DATA. The UI thread
 * receives the pointer in LPARAM and is responsible for freeing it
 * with telnet_proto_free_chunk(). */
typedef struct TelnetDataChunk {
    unsigned char *data;     /* malloc'd payload bytes (no IAC) */
    int            len;
} TelnetDataChunk;

typedef struct TelnetProto TelnetProto;

/* Open the connection. Returns 0 on success and writes *out. Caller
 * eventually calls telnet_proto_close() to tear it down.
 *
 * Posts TELNET_EVT_CONNECTED once the TCP handshake completes, then a
 * stream of TELNET_EVT_DATA messages, then TELNET_EVT_CLOSED. If the
 * worker fails to even start (out-of-memory, host lookup) the
 * function returns non-zero and *out is left NULL.
 *
 * If host lookup or connect fails on the worker, TELNET_EVT_ERROR is
 * posted (LPARAM = const char *errmsg, static storage; don't free)
 * followed by TELNET_EVT_CLOSED. */
int  telnet_proto_open(const char *host, int port,
                       HWND notify_hwnd, UINT notify_msg,
                       TelnetProto **out);

/* Send raw user bytes. Returns 0 on success. Doubles any 0xFF bytes in
 * `buf` per RFC 854 IAC escaping. Safe to call from the UI thread; this
 * function does a synchronous send() on the worker's socket. */
int  telnet_proto_send(TelnetProto *t, const void *buf, size_t len);

/* Tear down: signal the worker to stop, close the socket, join the
 * thread, free the handle. After this returns the handle is invalid. */
void telnet_proto_close(TelnetProto *t);

/* Free a TelnetDataChunk delivered via TELNET_EVT_DATA. */
void telnet_proto_free_chunk(TelnetDataChunk *chunk);

#ifdef __cplusplus
}
#endif

#endif /* TELNET_PROTO_H */
