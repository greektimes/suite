/*
 * gopher_protocol.h - Plain RFC 1436 over TCP. Synchronous fetch.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Real Gopher, not an HTML emulation, not a protocol bridge: the client
 * opens a TCP connection to host:port, sends the selector + CRLF, reads
 * until either a "." terminator line (text/menu) or socket close
 * (binary), and hands the bytes back.
 *
 * Callers that must not block the UI thread (the F3 gopher_module
 * worker) spawn a thread around gopher_fetch_sync; no async I/O lives
 * inside this TU.
 */

#ifndef GOPHER_PROTOCOL_H
#define GOPHER_PROTOCOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GOPHER_KIND_MENU   = 0,   /* gophermap; ".\r\n" terminator, dot-stuffed */
    GOPHER_KIND_TEXT   = 1,   /* item type '0'; same framing as MENU         */
    GOPHER_KIND_BINARY = 2    /* image / audio / file; raw bytes, server EOF */
} GopherFetchKind;

#define GOPHER_TIMEOUT_SEC 15

/* Synchronous fetch. Blocks the calling thread until the transfer is
 * complete, the timeout fires, or the connection drops.
 *
 * Returns 0 on success and writes a freshly-malloc'd buffer + length to
 * *out_bytes / *out_size; caller frees with free(). On any failure the
 * out parameters are not touched and the return value is non-zero.
 *
 * For MENU and TEXT kinds dot-stuffing is undone (leading ".." becomes
 * ".") and the terminating ".\r\n" line is stripped. For BINARY the
 * bytes pass through verbatim.
 *
 * Note: per RFC 1436 the server may simply close the socket on a menu
 * if it forgets the lone "." terminator; we accept either. */
int gopher_fetch_sync(const char *host, int port,
                      const char *selector,
                      GopherFetchKind kind,
                      char **out_bytes, size_t *out_size);

/* Parse a gopher:// URL. Accepted forms:
 *   gopher://host[:port]
 *   gopher://host[:port]/
 *   gopher://host[:port]/<type><selector>
 *   host[:port]                (scheme implied)
 *   host[:port]/<type><selector>
 * Default port = 70. Empty path / selector imply type '1'.
 * Allocates *out_host (caller frees) and *out_selector (caller frees).
 * Returns 0 on success. */
int gopher_parse_url(const char *url,
                     char **out_host, int *out_port,
                     char *out_type, char **out_selector);

/* Build "gopher://host[:port]/<type><selector>". Caller frees. */
char *gopher_build_url(const char *host, int port,
                       char type, const char *selector);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_PROTOCOL_H */
