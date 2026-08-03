/*
 * http_fetch.c - The Suite's one HTTPS GET. See http_fetch.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The request setup here is newspaper_service.c's news_winhttp_get()
 * with three changes, all of them things that function should have had:
 *
 *   1. it takes a whole URL and cracks it with WinHttpCrackUrl, instead
 *      of a path against a host fixed when the service was created;
 *   2. it sets timeouts, which the original did not, so a server that
 *      accepts a connection and then says nothing can no longer hang
 *      the calling thread indefinitely;
 *   3. it can stream to a file instead of buffering the whole body.
 *
 * Behaviour the newspaper module depended on is preserved exactly:
 * automatic proxy detection, a hard failure on any non-2xx status, and
 * a NUL written one byte past the returned length.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "http_fetch.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The user agent carries the real version rather than the frozen "0.2"
 * that newspaper_service.c and radio_engine.c were still sending from a
 * 0.3.1 build.
 *
 * SUITE_VERSION_STR is a narrow literal and WinHTTP wants wide, so it
 * goes through the usual two-level macro that lets the expansion happen
 * before the L is pasted on. */
#include "suite_version.h"

#define HF_WIDE2_(x)  L##x
#define HF_WIDE_(x)   HF_WIDE2_(x)
#define SUITE_VER_WIDE  HF_WIDE_(SUITE_VERSION_STR)

#define HTTP_FETCH_UA  L"MGT-Unicorn-Suite/" SUITE_VER_WIDE

static DWORD g_last_status = 0;

DWORD http_fetch_last_status(void) { return g_last_status; }

static void hf_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf(err, cap - 1, fmt, ap);
    va_end(ap);
    err[cap - 1] = '\0';
}

/* One opened, sent, 2xx-checked request. The caller reads the body off
 * *out_req and closes all three handles through hf_close(). */
typedef struct HfReq {
    HINTERNET ses;
    HINTERNET con;
    HINTERNET req;
} HfReq;

static void hf_close(HfReq *h)
{
    if (h->req) { WinHttpCloseHandle(h->req); h->req = NULL; }
    if (h->con) { WinHttpCloseHandle(h->con); h->con = NULL; }
    if (h->ses) { WinHttpCloseHandle(h->ses); h->ses = NULL; }
}

static int hf_open(const wchar_t *url, HfReq *h, char *err, size_t errcap)
{
    URL_COMPONENTS uc;
    wchar_t host[256];
    wchar_t path[2048];
    DWORD   status = 0, status_sz = sizeof(status);
    DWORD   flags = 0;

    memset(h, 0, sizeof(*h));
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize      = sizeof(uc);
    uc.lpszHostName      = host;
    uc.dwHostNameLength  = (DWORD)(sizeof(host) / sizeof(host[0]));
    uc.lpszUrlPath       = path;
    uc.dwUrlPathLength   = (DWORD)(sizeof(path) / sizeof(path[0]));

    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        hf_err(err, errcap, "The address could not be parsed (err %lu).",
               GetLastError());
        return HTTP_FETCH_BADURL;
    }
    /* HTTPS only. Everything the Suite fetches with this is served over
     * TLS, and an updater that would follow a plain-http URL is an
     * updater with a downgrade attack in it. */
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        hf_err(err, errcap, "Only https addresses are allowed.");
        return HTTP_FETCH_BADURL;
    }
    if (path[0] == L'\0') { path[0] = L'/'; path[1] = L'\0'; }

    h->ses = WinHttpOpen(HTTP_FETCH_UA,
                         WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->ses) {
        hf_err(err, errcap, "Could not start the web client (err %lu).",
               GetLastError());
        return HTTP_FETCH_OPEN;
    }
    WinHttpSetTimeouts(h->ses, HTTP_FETCH_RESOLVE_MS, HTTP_FETCH_CONNECT_MS,
                       HTTP_FETCH_SEND_MS, HTTP_FETCH_RECEIVE_MS);

    h->con = WinHttpConnect(h->ses, host, uc.nPort, 0);
    if (!h->con) {
        hf_err(err, errcap, "Could not reach %S (err %lu).",
               host, GetLastError());
        hf_close(h);
        return HTTP_FETCH_CONNECT;
    }

    h->req = WinHttpOpenRequest(h->con, L"GET", path, NULL,
                                WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                WINHTTP_FLAG_SECURE);
    if (!h->req) {
        hf_err(err, errcap, "Could not build the request (err %lu).",
               GetLastError());
        hf_close(h);
        return HTTP_FETCH_REQUEST;
    }

    /* Never serve an update, a manifest or an index out of the WinHTTP
     * cache; always ask the origin. */
    flags = WINHTTP_DISABLE_KEEP_ALIVE;
    WinHttpSetOption(h->req, WINHTTP_OPTION_DISABLE_FEATURE, &flags,
                     sizeof(flags));
    WinHttpAddRequestHeaders(h->req,
        L"Cache-Control: no-cache\r\nPragma: no-cache\r\n",
        (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(h->req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD e = GetLastError();
        hf_err(err, errcap,
               e == ERROR_WINHTTP_TIMEOUT
                 ? "The server did not answer in time."
                 : "The request could not be sent (err %lu).", e);
        hf_close(h);
        return HTTP_FETCH_SEND;
    }
    if (!WinHttpReceiveResponse(h->req, NULL)) {
        DWORD e = GetLastError();
        hf_err(err, errcap,
               e == ERROR_WINHTTP_TIMEOUT
                 ? "The server did not answer in time."
                 : "No response from the server (err %lu).", e);
        hf_close(h);
        return HTTP_FETCH_RESPONSE;
    }
    if (!WinHttpQueryHeaders(h->req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &status_sz, NULL)) {
        status = 0;
    }
    g_last_status = status;
    if (status < 200 || status >= 300) {
        hf_err(err, errcap, "The server answered HTTP %lu.",
               (unsigned long)status);
        hf_close(h);
        return HTTP_FETCH_HTTPSTATUS;
    }
    return HTTP_FETCH_OK;
}

/* Content-Length, or 0 when the server did not send one. */
static unsigned long long hf_content_length(HINTERNET req)
{
    wchar_t buf[64];
    DWORD   sz = sizeof(buf);
    unsigned long long v = 0;
    int i;

    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
                             WINHTTP_HEADER_NAME_BY_INDEX, buf, &sz, NULL))
        return 0;
    buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
    for (i = 0; buf[i] >= L'0' && buf[i] <= L'9'; i++)
        v = v * 10ULL + (unsigned long long)(buf[i] - L'0');
    return v;
}

int http_fetch_to_memory(const wchar_t *url,
                         unsigned char **out_data, DWORD *out_len,
                         unsigned long long max_bytes,
                         char *err, size_t errcap)
{
    HfReq h;
    unsigned char *buf = NULL;
    DWORD buf_cap = 0, buf_used = 0;
    int rc;

    if (out_len)  *out_len = 0;
    if (out_data) *out_data = NULL;
    if (!url || !out_data) return HTTP_FETCH_BADARG;

    rc = hf_open(url, &h, err, errcap);
    if (rc != HTTP_FETCH_OK) return rc;

    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(h.req, &avail)) {
            hf_err(err, errcap, "The transfer was interrupted (err %lu).",
                   GetLastError());
            rc = HTTP_FETCH_READ;
            goto done;
        }
        if (avail == 0) break;

        if (max_bytes && (unsigned long long)buf_used + avail > max_bytes) {
            hf_err(err, errcap, "The reply is larger than the %llu byte limit.",
                   max_bytes);
            rc = HTTP_FETCH_TOOBIG;
            goto done;
        }
        if (buf_used + avail + 1 > buf_cap) {
            DWORD new_cap = buf_cap ? buf_cap * 2 : 8192;
            unsigned char *nb;
            while (new_cap < buf_used + avail + 1) new_cap *= 2;
            nb = buf ? (unsigned char *)HeapReAlloc(GetProcessHeap(), 0,
                                                    buf, new_cap)
                     : (unsigned char *)HeapAlloc(GetProcessHeap(), 0, new_cap);
            if (!nb) {
                hf_err(err, errcap, "Out of memory.");
                rc = HTTP_FETCH_MEMORY;
                goto done;
            }
            buf = nb;
            buf_cap = new_cap;
        }
        if (!WinHttpReadData(h.req, buf + buf_used, avail, &got) || got == 0)
            break;
        buf_used += got;
    }

    if (!buf) {
        /* A legitimate empty body still gets a one-byte NUL block, so
         * callers can treat the result as a string unconditionally. */
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 1);
        if (!buf) { hf_err(err, errcap, "Out of memory."); rc = HTTP_FETCH_MEMORY; goto done; }
    }
    buf[buf_used] = '\0';
    *out_data = buf;
    if (out_len) *out_len = buf_used;
    buf = NULL;                 /* handed to the caller */
    rc = HTTP_FETCH_OK;

done:
    hf_close(&h);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    return rc;
}

int http_fetch_to_file(const wchar_t *url, const wchar_t *out_path,
                       unsigned long long max_bytes,
                       HttpProgressFn progress, void *ctx,
                       char *err, size_t errcap)
{
    HfReq  h;
    HANDLE hf = INVALID_HANDLE_VALUE;
    unsigned char chunk[32768];
    unsigned long long got_total = 0, expect;
    int rc;

    if (!url || !out_path) return HTTP_FETCH_BADARG;

    rc = hf_open(url, &h, err, errcap);
    if (rc != HTTP_FETCH_OK) return rc;

    expect = hf_content_length(h.req);
    if (max_bytes && expect > max_bytes) {
        hf_err(err, errcap,
               "The download is %llu bytes, over the %llu byte limit.",
               expect, max_bytes);
        rc = HTTP_FETCH_TOOBIG;
        goto done;
    }

    hf = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        hf_err(err, errcap, "Could not create the local file (err %lu).",
               GetLastError());
        rc = HTTP_FETCH_FILEIO;
        goto done;
    }

    if (progress && progress(ctx, 0, expect) != 0) {
        rc = HTTP_FETCH_CANCELLED;
        goto done;
    }

    for (;;) {
        DWORD avail = 0, got = 0, written = 0;

        if (!WinHttpQueryDataAvailable(h.req, &avail)) {
            hf_err(err, errcap, "The transfer was interrupted (err %lu).",
                   GetLastError());
            rc = HTTP_FETCH_READ;
            goto done;
        }
        if (avail == 0) break;
        if (avail > sizeof(chunk)) avail = sizeof(chunk);

        if (!WinHttpReadData(h.req, chunk, avail, &got) || got == 0)
            break;

        if (max_bytes && got_total + got > max_bytes) {
            hf_err(err, errcap,
                   "The download ran past the %llu byte limit.", max_bytes);
            rc = HTTP_FETCH_TOOBIG;
            goto done;
        }
        if (!WriteFile(hf, chunk, got, &written, NULL) || written != got) {
            hf_err(err, errcap, "Writing the download failed (err %lu).",
                   GetLastError());
            rc = HTTP_FETCH_FILEIO;
            goto done;
        }
        got_total += got;

        if (progress && progress(ctx, got_total, expect) != 0) {
            rc = HTTP_FETCH_CANCELLED;
            goto done;
        }
    }

    /* A truncated transfer is a failed transfer. Without this a
     * connection dropped at 90 per cent would leave a short file that
     * only the hash check would catch. */
    if (expect && got_total != expect) {
        hf_err(err, errcap,
               "The download stopped short: %llu of %llu bytes.",
               got_total, expect);
        rc = HTTP_FETCH_READ;
        goto done;
    }
    rc = HTTP_FETCH_OK;

done:
    hf_close(&h);
    if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
    /* Never leave a partial file behind for someone to mistake for a
     * finished one. */
    if (rc != HTTP_FETCH_OK) DeleteFileW(out_path);
    return rc;
}
