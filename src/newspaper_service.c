/*
 * newspaper_service.c - WinHTTPS-backed Newspaper backend.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * The deployed index.json schema is a small flat object with a
 * "latest" string and an "issues" array. Each issue's manifest
 * lives at /<id>/manifest.json with the same shape Phase 2 lands.
 * Rather than pull a full JSON library, this file implements a
 * pragmatic key-scoped scanner -- works on the project-controlled
 * schema, brittle to surprises (acceptable trade-off).
 *
 * GDI+ flat C API used the same way the rest of the suite uses it
 * (gopher_module / web_module / mode_toggle): typedef the
 * GdiplusStartupInput layout locally, extern-declare the few flat
 * entry points we call. -lgdiplus is in the LDFLAGS already.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <objbase.h>
#include <ole2.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "newspaper_service.h"

#define NEWS_MAX_ISSUES        64
#define NEWS_MANIFEST_CACHE    4
#define NEWS_COVER_CACHE       16
#define NEWS_DEFAULT_HOST      L"newspaper.greektimes.ca"

/* ------------------------------------------------------------------ */
/* GDI+ flat C API (same pattern as gopher_module + web_module).       */
/* ------------------------------------------------------------------ */

typedef struct MgtGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtGdiplusStartupInput;

extern int  WINAPI GdiplusStartup(ULONG_PTR *token,
                                  const MgtGdiplusStartupInput *input,
                                  void *output);
extern void WINAPI GdiplusShutdown(ULONG_PTR token);
extern int  WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int  WINAPI GdipCreateHBITMAPFromBitmap(void *bitmap, HBITMAP *hbm,
                                               DWORD bg);
extern int  WINAPI GdipDisposeImage(void *bitmap);
extern int  WINAPI GdipGetImageWidth(void *bitmap, UINT *w);
extern int  WINAPI GdipGetImageHeight(void *bitmap, UINT *h);
extern int  WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
extern int  WINAPI GdipDeleteGraphics(void *graphics);
extern int  WINAPI GdipDrawImageRectI(void *graphics, void *image,
                                      INT x, INT y, INT w, INT h);
extern int  WINAPI GdipSetInterpolationMode(void *graphics, int mode);
extern int  WINAPI GdipSetSmoothingMode(void *graphics, int mode);
extern int  WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);
extern int  WINAPI GdipGraphicsClear(void *graphics, DWORD color);

#define MGT_INTERPOLATION_HIGHQUALITY_BICUBIC  7
#define MGT_SMOOTHING_HIGHQUALITY              2
#define MGT_PIXEL_OFFSET_HALF                  4

static ULONG_PTR g_news_gdiplus_token = 0;
static BOOL      g_news_gdiplus_inited = FALSE;

static void news_ensure_gdiplus(void)
{
    MgtGdiplusStartupInput in;
    if (g_news_gdiplus_inited) return;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_news_gdiplus_token, &in, NULL) == 0)
        g_news_gdiplus_inited = TRUE;
}

/* ------------------------------------------------------------------ */
/* Cache entries.                                                      */
/* ------------------------------------------------------------------ */

typedef struct CoverEntry {
    char    issue_id[16];
    int     w;
    int     h;
    HBITMAP hbm;
    ULONGLONG last_access;
} CoverEntry;

typedef struct ManifestEntry {
    char              issue_id[16];
    NewspaperManifest m;
    ULONGLONG         last_access;
    BOOL              live;
} ManifestEntry;

struct NewspaperService {
    wchar_t        host[128];
    char           latest[16];
    NewspaperIssue issues[NEWS_MAX_ISSUES];
    int            issue_count;
    char           last_error[256];

    ManifestEntry  mcache[NEWS_MANIFEST_CACHE];
    CoverEntry     ccache[NEWS_COVER_CACHE];

    /* Dispatch 2026-06-15 Phase 2 follow-up bug-1: both the picker UI
     * thread (WM_PAINT calling news_cover_peek) and the picker's
     * cover-fetch worker (calling news_cover_thumb) hit ccache /
     * mcache concurrently. cache_cs serializes lookup + mutation.
     * It does NOT cover the WinHTTPS / GDI+ work itself -- those
     * remain off-lock so a fetch doesn't block a peek. */
    CRITICAL_SECTION cache_cs;
    BOOL             cache_cs_ready;
};

/* ------------------------------------------------------------------ */
/* WinHTTP helpers.                                                    */
/* ------------------------------------------------------------------ */

static void news_set_error(NewspaperService *s, const char *fmt, ...)
{
    va_list ap;
    if (!s) return;
    va_start(ap, fmt);
    _vsnprintf(s->last_error, sizeof(s->last_error) - 1, fmt, ap);
    va_end(ap);
    s->last_error[sizeof(s->last_error) - 1] = '\0';
}

/* Returns a fresh HeapAlloc'd buffer (caller HeapFree) with the
 * response body bytes; *out_len receives the byte count. NUL byte
 * always written at out_data[out_len]. Returns NULL on failure
 * (with s->last_error populated). */
static unsigned char *news_winhttp_get(NewspaperService *s,
                                       const wchar_t *path,
                                       DWORD *out_len)
{
    HINTERNET hSes = NULL, hCon = NULL, hReq = NULL;
    unsigned char *buf = NULL;
    DWORD     buf_cap = 0, buf_used = 0;
    DWORD     status = 0, status_sz = sizeof(status);
    BOOL      ok = FALSE;

    if (out_len) *out_len = 0;

    hSes = WinHttpOpen(L"MGT-Unicorn-Suite/0.2",
                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) { news_set_error(s, "WinHttpOpen failed (err %lu)",
                                 GetLastError()); goto done; }
    hCon = WinHttpConnect(hSes, s->host,
                          INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) { news_set_error(s, "WinHttpConnect failed (err %lu)",
                                 GetLastError()); goto done; }
    hReq = WinHttpOpenRequest(hCon, L"GET", path, NULL,
                              WINHTTP_NO_REFERER,
                              WINHTTP_DEFAULT_ACCEPT_TYPES,
                              WINHTTP_FLAG_SECURE);
    if (!hReq) { news_set_error(s, "WinHttpOpenRequest failed (err %lu)",
                                 GetLastError()); goto done; }
    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        news_set_error(s, "WinHttpSendRequest failed (err %lu)",
                       GetLastError());
        goto done;
    }
    if (!WinHttpReceiveResponse(hReq, NULL)) {
        news_set_error(s, "WinHttpReceiveResponse failed (err %lu)",
                       GetLastError());
        goto done;
    }
    if (!WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            NULL, &status, &status_sz, NULL)) {
        status = 0;
    }
    if (status < 200 || status >= 300) {
        news_set_error(s, "HTTP %lu", (unsigned long)status);
        goto done;
    }
    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            news_set_error(s, "WinHttpQueryDataAvailable failed (err %lu)",
                           GetLastError());
            goto done;
        }
        if (avail == 0) break;
        if (buf_used + avail + 1 > buf_cap) {
            DWORD new_cap = buf_cap ? buf_cap * 2 : 8192;
            unsigned char *nb;
            while (new_cap < buf_used + avail + 1) new_cap *= 2;
            nb = buf ? (unsigned char *)HeapReAlloc(GetProcessHeap(), 0,
                                                    buf, new_cap)
                     : (unsigned char *)HeapAlloc(GetProcessHeap(), 0,
                                                  new_cap);
            if (!nb) { news_set_error(s, "out of memory"); goto done; }
            buf = nb; buf_cap = new_cap;
        }
        if (!WinHttpReadData(hReq, buf + buf_used, avail, &got) || got == 0)
            break;
        buf_used += got;
    }
    if (buf) buf[buf_used] = '\0';
    if (out_len) *out_len = buf_used;
    ok = TRUE;

done:
    if (hReq) WinHttpCloseHandle(hReq);
    if (hCon) WinHttpCloseHandle(hCon);
    if (hSes) WinHttpCloseHandle(hSes);
    if (!ok && buf) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; }
    return buf;
}

/* ------------------------------------------------------------------ */
/* Minimal JSON scan helpers (shared with index parsing).              */
/* ------------------------------------------------------------------ */

static const char *json_find_key(const char *start, const char *end,
                                 const char *key)
{
    size_t klen = strlen(key);
    const char *p = start;
    while (p && p < end) {
        const char *q;
        p = (const char *)memchr(p, '\"', (size_t)(end - p));
        if (!p) return NULL;
        if ((size_t)(end - p) < klen + 2) return NULL;
        if (p[klen + 1] == '\"' && memcmp(p + 1, key, klen) == 0) {
            q = p + klen + 2;
            while (q < end && (*q == ' ' || *q == '\t' || *q == ':' ||
                               *q == '\r' || *q == '\n')) q++;
            return q < end ? q : NULL;
        }
        p++;
    }
    return NULL;
}

static BOOL json_read_string(const char **pp, const char *end,
                             char *out, size_t out_cap)
{
    const char *p = *pp;
    size_t i = 0;
    if (p >= end || *p != '\"') return FALSE;
    p++;
    while (p < end && *p != '\"' && i + 1 < out_cap) {
        if (*p == '\\' && p + 1 < end) {
            char c = p[1];
            switch (c) {
            case '"':  out[i++] = '\"'; break;
            case '\\': out[i++] = '\\'; break;
            case '/':  out[i++] = '/';  break;
            case 'n':  out[i++] = '\n'; break;
            case 't':  out[i++] = '\t'; break;
            case 'r':  out[i++] = '\r'; break;
            default:   out[i++] = c;    break;
            }
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    while (p < end && *p != '\"') p++;
    if (p >= end || *p != '\"') return FALSE;
    out[i] = '\0';
    *pp = p + 1;
    return TRUE;
}

static int json_read_int(const char *p, const char *end, int *out)
{
    int neg = 0, v = 0, started = 0;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p < end && *p == '-') { neg = 1; p++; }
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        started = 1; p++;
    }
    if (!started) return 0;
    *out = neg ? -v : v;
    return 1;
}

static BOOL obj_pluck_str(const char *obj, const char *obj_end,
                          const char *key, char *out, size_t out_cap)
{
    const char *v = json_find_key(obj, obj_end, key);
    if (!v) { if (out_cap) out[0] = '\0'; return FALSE; }
    return json_read_string(&v, obj_end, out, out_cap);
}

static BOOL obj_pluck_int(const char *obj, const char *obj_end,
                          const char *key, int *out)
{
    const char *v = json_find_key(obj, obj_end, key);
    if (!v) return FALSE;
    return json_read_int(v, obj_end, out) != 0;
}

/* Find the end of the JSON object that starts at *pp (which must
 * point at the opening '{'). On return *pp points just past the
 * closing '}'. Skips strings to avoid braces-in-strings traps. */
static const char *json_skip_object(const char *p, const char *end)
{
    int depth = 1;
    if (p >= end || *p != '{') return p;
    p++;
    while (p < end && depth > 0) {
        if (*p == '\"') {
            p++;
            while (p < end && *p != '\"') {
                if (*p == '\\' && p + 1 < end) p++;
                p++;
            }
            if (p < end) p++;
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        p++;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Service create / destroy / shared accessors.                        */
/* ------------------------------------------------------------------ */

NewspaperService *news_create(const wchar_t *base_host)
{
    NewspaperService *s = (NewspaperService *)HeapAlloc(GetProcessHeap(),
        HEAP_ZERO_MEMORY, sizeof(*s));
    if (!s) return NULL;
    if (base_host && base_host[0])
        lstrcpynW(s->host, base_host,
                  (int)(sizeof(s->host) / sizeof(s->host[0])));
    else
        lstrcpyW(s->host, NEWS_DEFAULT_HOST);
    InitializeCriticalSection(&s->cache_cs);
    s->cache_cs_ready = TRUE;
    return s;
}

void news_destroy(NewspaperService *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < NEWS_MANIFEST_CACHE; i++)
        if (s->mcache[i].m.pages)
            HeapFree(GetProcessHeap(), 0, s->mcache[i].m.pages);
    for (i = 0; i < NEWS_COVER_CACHE; i++)
        if (s->ccache[i].hbm) DeleteObject(s->ccache[i].hbm);
    if (s->cache_cs_ready) {
        DeleteCriticalSection(&s->cache_cs);
        s->cache_cs_ready = FALSE;
    }
    HeapFree(GetProcessHeap(), 0, s);
}

const char *news_last_error(NewspaperService *s)
{
    return (s && s->last_error[0]) ? s->last_error : "";
}

/* ------------------------------------------------------------------ */
/* news_load_index.                                                    */
/* ------------------------------------------------------------------ */

BOOL news_load_index(NewspaperService *s)
{
    unsigned char *body;
    DWORD          len = 0;
    const char    *arr_start, *arr_end, *p;
    const char    *body_c;

    if (!s) return FALSE;
    body = news_winhttp_get(s, L"/index.json", &len);
    if (!body) return FALSE;
    body_c = (const char *)body;

    {
        const char *v = json_find_key(body_c, body_c + len, "latest");
        if (v) json_read_string(&v, body_c + len,
                                s->latest, sizeof(s->latest));
    }

    arr_start = json_find_key(body_c, body_c + len, "issues");
    if (!arr_start) goto done_ok;
    while (arr_start < body_c + len && *arr_start != '[') arr_start++;
    if (arr_start >= body_c + len) goto done_ok;
    arr_start++;
    arr_end = body_c + len;

    s->issue_count = 0;
    p = arr_start;
    while (p < arr_end && s->issue_count < NEWS_MAX_ISSUES) {
        const char *obj_start, *obj_end;
        while (p < arr_end && *p != '{' && *p != ']') p++;
        if (p >= arr_end || *p == ']') break;
        obj_start = p;
        obj_end = json_skip_object(p, arr_end);
        p = obj_end;
        {
            NewspaperIssue *it = &s->issues[s->issue_count];
            obj_pluck_str(obj_start, obj_end, "id",
                          it->id, sizeof(it->id));
            obj_pluck_str(obj_start, obj_end, "title",
                          it->title, sizeof(it->title));
            obj_pluck_int(obj_start, obj_end, "page_count",
                          &it->page_count);
            obj_pluck_str(obj_start, obj_end, "pdf",
                          it->pdf_path, sizeof(it->pdf_path));
            obj_pluck_str(obj_start, obj_end, "cover",
                          it->cover_path, sizeof(it->cover_path));
            if (it->id[0]) s->issue_count++;
        }
    }

done_ok:
    HeapFree(GetProcessHeap(), 0, body);
    return TRUE;
}

int news_issue_count(NewspaperService *s)
{
    return s ? s->issue_count : 0;
}

const NewspaperIssue *news_issue_at(NewspaperService *s, int i)
{
    if (!s || i < 0 || i >= s->issue_count) return NULL;
    return &s->issues[i];
}

const char *news_latest_id(NewspaperService *s)
{
    if (!s || !s->latest[0]) return NULL;
    return s->latest;
}

int news_issue_page_count(NewspaperService *s, const char *id)
{
    int i;
    if (!s || !id) return 0;
    for (i = 0; i < s->issue_count; i++)
        if (strcmp(s->issues[i].id, id) == 0)
            return s->issues[i].page_count;
    return 0;
}

/* ------------------------------------------------------------------ */
/* news_fetch_manifest with 4-LRU cache.                               */
/* ------------------------------------------------------------------ */

static ManifestEntry *manifest_cache_lookup(NewspaperService *s,
                                            const char *issue_id)
{
    int i;
    for (i = 0; i < NEWS_MANIFEST_CACHE; i++) {
        if (s->mcache[i].live &&
            strcmp(s->mcache[i].issue_id, issue_id) == 0) {
            s->mcache[i].last_access = GetTickCount64();
            return &s->mcache[i];
        }
    }
    return NULL;
}

static ManifestEntry *manifest_cache_evict_slot(NewspaperService *s)
{
    int i, oldest = 0;
    ULONGLONG oldest_t = (ULONGLONG)-1;
    for (i = 0; i < NEWS_MANIFEST_CACHE; i++) {
        if (!s->mcache[i].live) { oldest = i; oldest_t = 0; break; }
        if (s->mcache[i].last_access < oldest_t) {
            oldest_t = s->mcache[i].last_access;
            oldest = i;
        }
    }
    if (s->mcache[oldest].live && s->mcache[oldest].m.pages) {
        HeapFree(GetProcessHeap(), 0, s->mcache[oldest].m.pages);
        s->mcache[oldest].m.pages = NULL;
    }
    ZeroMemory(&s->mcache[oldest], sizeof(s->mcache[oldest]));
    return &s->mcache[oldest];
}

const NewspaperManifest *news_fetch_manifest(NewspaperService *s,
                                             const char *issue_id)
{
    ManifestEntry *e;
    unsigned char *body;
    DWORD          len = 0;
    wchar_t        path[64];
    const char    *body_c;
    const char    *arr_start, *arr_end, *p;
    NewspaperPage *pages = NULL;
    int            cap = 0, used = 0;
    char           id_tmp[16] = {0};
    char           title_tmp[256] = {0};
    int            page_count_hint = 0;

    if (!s || !issue_id || !*issue_id) return NULL;
    if (s->cache_cs_ready) {
        EnterCriticalSection(&s->cache_cs);
        e = manifest_cache_lookup(s, issue_id);
        LeaveCriticalSection(&s->cache_cs);
        if (e) return &e->m;
    } else {
        e = manifest_cache_lookup(s, issue_id);
        if (e) return &e->m;
    }

    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"/%S/manifest.json", issue_id);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    body = news_winhttp_get(s, path, &len);
    if (!body) return NULL;
    body_c = (const char *)body;

    /* Pluck the top-level scalars. */
    {
        const char *v = json_find_key(body_c, body_c + len, "id");
        if (v) json_read_string(&v, body_c + len, id_tmp, sizeof(id_tmp));
    }
    {
        const char *v = json_find_key(body_c, body_c + len, "title");
        if (v) json_read_string(&v, body_c + len, title_tmp, sizeof(title_tmp));
    }
    {
        const char *v = json_find_key(body_c, body_c + len, "page_count");
        if (v) json_read_int(v, body_c + len, &page_count_hint);
    }

    /* Pages array. */
    arr_start = json_find_key(body_c, body_c + len, "pages");
    if (arr_start) {
        while (arr_start < body_c + len && *arr_start != '[') arr_start++;
        if (arr_start < body_c + len) {
            arr_start++;
            arr_end = body_c + len;
            p = arr_start;
            cap = page_count_hint > 0 ? page_count_hint : 32;
            pages = (NewspaperPage *)HeapAlloc(GetProcessHeap(), 0,
                        sizeof(NewspaperPage) * cap);
            if (!pages) {
                news_set_error(s, "out of memory");
                HeapFree(GetProcessHeap(), 0, body);
                return NULL;
            }
            while (p < arr_end) {
                const char *obj_start, *obj_end;
                while (p < arr_end && *p != '{' && *p != ']') p++;
                if (p >= arr_end || *p == ']') break;
                obj_start = p;
                obj_end = json_skip_object(p, arr_end);
                p = obj_end;
                if (used >= cap) {
                    int new_cap = cap * 2;
                    NewspaperPage *np = (NewspaperPage *)HeapReAlloc(
                        GetProcessHeap(), 0, pages,
                        sizeof(NewspaperPage) * new_cap);
                    if (!np) break;
                    pages = np; cap = new_cap;
                }
                ZeroMemory(&pages[used], sizeof(pages[used]));
                obj_pluck_str(obj_start, obj_end, "file",
                              pages[used].file, sizeof(pages[used].file));
                obj_pluck_int(obj_start, obj_end, "w", &pages[used].w);
                obj_pluck_int(obj_start, obj_end, "h", &pages[used].h);
                if (pages[used].file[0]) used++;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, body);

    if (s->cache_cs_ready) EnterCriticalSection(&s->cache_cs);
    /* Re-check the cache under the lock in case another caller
     * finished first; if so discard our work. */
    {
        ManifestEntry *existing = manifest_cache_lookup(s, issue_id);
        if (existing) {
            if (pages) HeapFree(GetProcessHeap(), 0, pages);
            if (s->cache_cs_ready) LeaveCriticalSection(&s->cache_cs);
            return &existing->m;
        }
    }
    e = manifest_cache_evict_slot(s);
    lstrcpynA(e->issue_id, issue_id, sizeof(e->issue_id));
    lstrcpynA(e->m.id, id_tmp[0] ? id_tmp : issue_id, sizeof(e->m.id));
    lstrcpynA(e->m.title, title_tmp, sizeof(e->m.title));
    e->m.page_count = used;
    e->m.pages      = pages;
    e->last_access  = GetTickCount64();
    e->live         = TRUE;
    if (s->cache_cs_ready) LeaveCriticalSection(&s->cache_cs);
    return &e->m;
}

/* ------------------------------------------------------------------ */
/* news_cover_thumb with cache + placeholder fallback.                 */
/* ------------------------------------------------------------------ */

static CoverEntry *cover_cache_lookup(NewspaperService *s,
                                      const char *id, int w, int h)
{
    int i;
    for (i = 0; i < NEWS_COVER_CACHE; i++) {
        if (s->ccache[i].hbm &&
            s->ccache[i].w == w && s->ccache[i].h == h &&
            strcmp(s->ccache[i].issue_id, id) == 0) {
            s->ccache[i].last_access = GetTickCount64();
            return &s->ccache[i];
        }
    }
    return NULL;
}

static CoverEntry *cover_cache_evict_slot(NewspaperService *s)
{
    int i, oldest = 0;
    ULONGLONG oldest_t = (ULONGLONG)-1;
    for (i = 0; i < NEWS_COVER_CACHE; i++) {
        if (!s->ccache[i].hbm) { oldest = i; oldest_t = 0; break; }
        if (s->ccache[i].last_access < oldest_t) {
            oldest_t = s->ccache[i].last_access;
            oldest = i;
        }
    }
    if (s->ccache[oldest].hbm) {
        DeleteObject(s->ccache[oldest].hbm);
        s->ccache[oldest].hbm = NULL;
    }
    ZeroMemory(&s->ccache[oldest], sizeof(s->ccache[oldest]));
    return &s->ccache[oldest];
}

/* Make a 32bpp top-down DIB section at (w, h). Returns the HBITMAP. */
static HBITMAP make_dib(int w, int h)
{
    BITMAPINFO bmi;
    void *bits = NULL;
    HBITMAP hbm;
    if (w <= 0 || h <= 0) return NULL;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    hbm = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    return hbm;
}

static void cover_fill_placeholder(HBITMAP hbm, int w, int h,
                                   const char *issue_id)
{
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP old;
    HBRUSH gray;
    RECT rc = { 0, 0, w, h };
    HFONT  hf, hf_old;
    LOGFONTA lf;
    if (!dc) return;
    old = (HBITMAP)SelectObject(dc, hbm);
    gray = CreateSolidBrush(RGB(60, 60, 64));
    FillRect(dc, &rc, gray);
    DeleteObject(gray);

    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = -MulDiv(16, 96, 72);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    hf = CreateFontIndirectA(&lf);
    if (hf) {
        hf_old = (HFONT)SelectObject(dc, hf);
        SetTextColor(dc, RGB(220, 220, 224));
        SetBkMode(dc, TRANSPARENT);
        DrawTextA(dc, issue_id, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, hf_old);
        DeleteObject(hf);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
}

/* Render the decoded source GpBitmap into the target HBITMAP at
 * (target_w, target_h), preserving aspect, black bars filling
 * empty space. */
static BOOL render_cover_into(void *gp_bitmap, HBITMAP target,
                              int target_w, int target_h)
{
    HDC dc;
    HBITMAP old;
    UINT src_w = 0, src_h = 0;
    void *graphics = NULL;
    HBRUSH black;
    RECT rc_clear = { 0, 0, target_w, target_h };
    int draw_w, draw_h, draw_x, draw_y;
    BOOL ok = FALSE;

    GdipGetImageWidth (gp_bitmap, &src_w);
    GdipGetImageHeight(gp_bitmap, &src_h);
    if (src_w == 0 || src_h == 0) return FALSE;

    dc = CreateCompatibleDC(NULL);
    if (!dc) return FALSE;
    old = (HBITMAP)SelectObject(dc, target);

    black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc_clear, black);
    DeleteObject(black);

    if ((double)src_w / (double)src_h > (double)target_w / (double)target_h) {
        draw_w = target_w;
        draw_h = (int)((double)target_w * (double)src_h / (double)src_w + 0.5);
    } else {
        draw_h = target_h;
        draw_w = (int)((double)target_h * (double)src_w / (double)src_h + 0.5);
    }
    draw_x = (target_w - draw_w) / 2;
    draw_y = (target_h - draw_h) / 2;

    if (GdipCreateFromHDC(dc, &graphics) == 0 && graphics) {
        GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
        GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
        GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HALF);
        if (GdipDrawImageRectI(graphics, gp_bitmap,
                               draw_x, draw_y, draw_w, draw_h) == 0)
            ok = TRUE;
        GdipDeleteGraphics(graphics);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    return ok;
}

HBITMAP news_cover_peek(NewspaperService *s, const char *issue_id,
                        int target_w_px, int target_h_px)
{
    CoverEntry *ce;
    HBITMAP     hbm = NULL;
    if (!s || !issue_id || !*issue_id ||
        target_w_px <= 0 || target_h_px <= 0)
        return NULL;
    if (!s->cache_cs_ready) return NULL;
    EnterCriticalSection(&s->cache_cs);
    ce = cover_cache_lookup(s, issue_id, target_w_px, target_h_px);
    if (ce) hbm = ce->hbm;
    LeaveCriticalSection(&s->cache_cs);
    return hbm;
}

HBITMAP news_cover_thumb(NewspaperService *s, const char *issue_id,
                        int target_w_px, int target_h_px)
{
    CoverEntry *ce;
    unsigned char *body = NULL;
    DWORD          len = 0;
    wchar_t        path[64];
    IStream       *stream = NULL;
    HGLOBAL        hg = NULL;
    void          *gp_bitmap = NULL;
    HBITMAP        hbm = NULL;
    BOOL           rendered = FALSE;

    if (!s || !issue_id || !*issue_id ||
        target_w_px <= 0 || target_h_px <= 0)
        return NULL;

    /* Fast path: already cached. The lock is short -- a peek + pointer
     * read -- so the worker thread does not block the UI thread for
     * any meaningful time. */
    if (s->cache_cs_ready) {
        EnterCriticalSection(&s->cache_cs);
        ce = cover_cache_lookup(s, issue_id, target_w_px, target_h_px);
        if (ce) hbm = ce->hbm;
        LeaveCriticalSection(&s->cache_cs);
        if (hbm) return hbm;
    }

    /* Slow path: fetch + decode happens off-lock so the UI thread's
     * peek path is not stalled by network or GDI+ work. The downside
     * is we may race two worker threads and decode the same cover
     * twice; whoever wins inserts first and the loser's HBITMAP is
     * leaked into the void. The picker only spawns one worker per
     * instance so in practice this race is benign. */
    news_ensure_gdiplus();

    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"/%S/cover.jpg", issue_id);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    body = news_winhttp_get(s, path, &len);

    hbm = make_dib(target_w_px, target_h_px);
    if (!hbm) {
        if (body) HeapFree(GetProcessHeap(), 0, body);
        return NULL;
    }

    if (body && len > 0 && g_news_gdiplus_inited) {
        hg = GlobalAlloc(GMEM_MOVEABLE, len);
        if (hg) {
            void *dst = GlobalLock(hg);
            if (dst) {
                memcpy(dst, body, len);
                GlobalUnlock(hg);
                if (CreateStreamOnHGlobal(hg, TRUE, &stream) == S_OK && stream) {
                    if (GdipCreateBitmapFromStream(stream, &gp_bitmap) == 0
                        && gp_bitmap) {
                        rendered = render_cover_into(gp_bitmap, hbm,
                                                     target_w_px, target_h_px);
                        GdipDisposeImage(gp_bitmap);
                    }
                    stream->lpVtbl->Release(stream);
                    hg = NULL;
                }
            }
            if (hg) GlobalFree(hg);
        }
    }
    if (body) HeapFree(GetProcessHeap(), 0, body);

    if (!rendered)
        cover_fill_placeholder(hbm, target_w_px, target_h_px, issue_id);

    /* Insert under the lock; race-loser leaks its HBITMAP but does
     * not crash. */
    if (s->cache_cs_ready) {
        HBITMAP final_hbm = hbm;
        EnterCriticalSection(&s->cache_cs);
        ce = cover_cache_lookup(s, issue_id, target_w_px, target_h_px);
        if (ce && ce->hbm) {
            /* Lost the race -- discard ours, return the winner's. */
            DeleteObject(hbm);
            final_hbm = ce->hbm;
        } else {
            ce = cover_cache_evict_slot(s);
            lstrcpynA(ce->issue_id, issue_id, sizeof(ce->issue_id));
            ce->w           = target_w_px;
            ce->h           = target_h_px;
            ce->hbm         = hbm;
            ce->last_access = GetTickCount64();
        }
        LeaveCriticalSection(&s->cache_cs);
        return final_hbm;
    }
    return hbm;
}

/* ------------------------------------------------------------------ */
/* news_save_pdf.                                                      */
/* ------------------------------------------------------------------ */

BOOL news_save_pdf(NewspaperService *s, const char *issue_id,
                   const wchar_t *out_path)
{
    unsigned char *body;
    DWORD          len = 0;
    wchar_t        path[64];
    HANDLE         hFile;
    DWORD          written = 0;
    BOOL           ok = FALSE;

    if (!s || !issue_id || !out_path) return FALSE;
    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"/%S/%S.pdf", issue_id, issue_id);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    body = news_winhttp_get(s, path, &len);
    if (!body) return FALSE;

    hFile = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        news_set_error(s, "CreateFileW failed (err %lu)", GetLastError());
        HeapFree(GetProcessHeap(), 0, body);
        return FALSE;
    }
    if (WriteFile(hFile, body, len, &written, NULL) && written == len)
        ok = TRUE;
    else
        news_set_error(s, "WriteFile incomplete (%lu / %lu, err %lu)",
                       (unsigned long)written, (unsigned long)len,
                       GetLastError());
    CloseHandle(hFile);
    HeapFree(GetProcessHeap(), 0, body);
    return ok;
}

/* ------------------------------------------------------------------ */
/* news_fetch_page_jpeg (raw bytes).                                   */
/* ------------------------------------------------------------------ */

BOOL news_fetch_page_jpeg(NewspaperService *s, const char *issue_id,
                          int page_index_1_based,
                          unsigned char **out_data, DWORD *out_len)
{
    wchar_t        path[80];
    unsigned char *body;
    DWORD          len = 0;

    if (out_data) *out_data = NULL;
    if (out_len)  *out_len  = 0;

    if (!s || !issue_id || !out_data || !out_len ||
        page_index_1_based <= 0) {
        if (s) news_set_error(s, "bad args");
        return FALSE;
    }
    _snwprintf(path, sizeof(path) / sizeof(path[0]),
               L"/%S/pages/page-%03d.jpg", issue_id, page_index_1_based);
    path[sizeof(path) / sizeof(path[0]) - 1] = L'\0';
    body = news_winhttp_get(s, path, &len);
    if (!body) return FALSE;
    *out_data = body;
    *out_len  = len;
    return TRUE;
}
