/*
 * http_fetch.h - The Suite's one HTTPS GET.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * WinHTTP, synchronous, with timeouts. Lifted out of
 * newspaper_service.c's news_winhttp_get() when the updater needed the
 * same thing, rather than copied: newspaper_service.c now calls in here
 * too, so there is exactly one HTTPS client in the Suite and one place
 * where a proxy, a timeout or a user agent gets fixed.
 *
 * Two shapes, because the two callers want different things:
 *
 *   http_fetch_to_memory   the whole body in one heap block, for a JSON
 *                          index or an update manifest. NUL-terminated
 *                          one byte past the length so it can be parsed
 *                          as a string.
 *   http_fetch_to_file     streamed straight to disk with an optional
 *                          progress callback, for an installer or a PDF
 *                          that has no business being held in RAM.
 *
 * BLOCKING. Both call WinHttp synchronously and must not be called on
 * the UI thread. Every caller in the Suite runs them on a worker.
 *
 * The URL is wide because WinHTTP is wide and both callers already hold
 * wide strings by the time they get here.
 */

#ifndef HTTP_FETCH_H
#define HTTP_FETCH_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timeouts, in milliseconds, as radio_engine.c has always used them.
 * news_winhttp_get had none at all, which meant a dead server hung the
 * calling thread until Winsock gave up on its own. */
#define HTTP_FETCH_RESOLVE_MS   10000
#define HTTP_FETCH_CONNECT_MS   10000
#define HTTP_FETCH_SEND_MS      10000
#define HTTP_FETCH_RECEIVE_MS   30000

/* Called from the fetching thread as bytes arrive. `total` is 0 when
 * the server sent no Content-Length. Return 0 to carry on, non-zero to
 * abort the transfer (http_fetch_to_file then returns
 * HTTP_FETCH_CANCELLED and removes the partial file). Must be
 * thread-safe and must not touch the UI directly. */
typedef int (*HttpProgressFn)(void *ctx,
                              unsigned long long got,
                              unsigned long long total);

/* Return codes. Anything non-zero left *out untouched or cleaned up. */
#define HTTP_FETCH_OK          0
#define HTTP_FETCH_BADARG      1
#define HTTP_FETCH_BADURL      2
#define HTTP_FETCH_OPEN        3
#define HTTP_FETCH_CONNECT     4
#define HTTP_FETCH_REQUEST     5
#define HTTP_FETCH_SEND        6
#define HTTP_FETCH_RESPONSE    7
#define HTTP_FETCH_HTTPSTATUS  8
#define HTTP_FETCH_READ        9
#define HTTP_FETCH_MEMORY     10
#define HTTP_FETCH_FILEIO     11
#define HTTP_FETCH_CANCELLED  12
#define HTTP_FETCH_TOOBIG     13

/* GET `url` into a fresh HeapAlloc'd buffer. On success *out_data holds
 * the body and *out_len its length, and out_data[*out_len] is a NUL the
 * length does not count. Free with HeapFree(GetProcessHeap(), 0, ...).
 *
 * `max_bytes` is a hard ceiling; 0 means no limit. A body that exceeds
 * it fails with HTTP_FETCH_TOOBIG rather than growing the heap.
 *
 * `err` receives a human-readable one-liner and may be NULL. */
int http_fetch_to_memory(const wchar_t *url,
                         unsigned char **out_data, DWORD *out_len,
                         unsigned long long max_bytes,
                         char *err, size_t errcap);

/* GET `url` straight to `out_path`, which is created or truncated. The
 * body is never held whole in memory. On any failure the partial file
 * is deleted, so a caller never finds a half-written result to mistake
 * for a good one. `progress` and `ctx` may be NULL. */
int http_fetch_to_file(const wchar_t *url, const wchar_t *out_path,
                       unsigned long long max_bytes,
                       HttpProgressFn progress, void *ctx,
                       char *err, size_t errcap);

/* The HTTP status code from the most recent call on this thread, or 0.
 * Only meaningful after HTTP_FETCH_HTTPSTATUS. */
DWORD http_fetch_last_status(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_FETCH_H */
