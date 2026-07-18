/*
 * newspaper_service.h - Newspaper module backend: index.json fetch,
 *                       per-issue manifest cache, cover thumbnails,
 *                       PDF save, and raw page-JPEG fetch.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * The reader UX is delegated to BookReader (AGPLv3, served by
 * https://newspaper.greektimes.ca/) via a WebView2 embed; this
 * service is the native side: index + per-issue manifest, cover
 * thumbnails for the back-issues picker, PDF download, and raw
 * page-JPEG fetch for the Print pipeline.
 */
#ifndef NEWSPAPER_SERVICE_H
#define NEWSPAPER_SERVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NewspaperService NewspaperService;

/* One entry from /index.json. */
typedef struct NewspaperIssue {
    char id[16];           /* "gt113"                          */
    char title[256];       /* full title string from index     */
    int  page_count;
    char pdf_path[64];     /* "gt113/gt113.pdf"                */
    char cover_path[64];   /* "gt113/cover.jpg"                */
} NewspaperIssue;

/* Per-page info from /<id>/manifest.json. */
typedef struct NewspaperPage {
    char file[32];         /* "page-001.jpg"                   */
    int  w;
    int  h;
} NewspaperPage;

/* One per-issue manifest. Owned by the service; the returned
 * pointer stays valid until the service is destroyed (or evicted
 * from the LRU). Do not free. */
typedef struct NewspaperManifest {
    char           id[16];
    char           title[256];
    int            page_count;
    NewspaperPage *pages;       /* page_count entries, heap-owned */
} NewspaperManifest;

NewspaperService *news_create(const wchar_t *base_host);
void              news_destroy(NewspaperService *s);

/* Synchronous WinHTTPS GET of /index.json. Returns FALSE on any
 * failure; on success the issue list is populated and
 * news_issue_at / news_latest_id are usable. */
BOOL              news_load_index(NewspaperService *s);

int               news_issue_count(NewspaperService *s);
const NewspaperIssue *news_issue_at(NewspaperService *s, int i);
const char       *news_latest_id(NewspaperService *s);
int               news_issue_page_count(NewspaperService *s, const char *id);

/* Synchronous GET + parse of /<issue_id>/manifest.json. The result
 * is cached (small LRU, cap 4); subsequent calls with the same
 * issue_id return the cached pointer. Returns NULL on any failure. */
const NewspaperManifest *news_fetch_manifest(NewspaperService *s,
                                             const char *issue_id);

/* Return a HBITMAP for /<issue_id>/cover.jpg scaled to
 * (target_w_px, target_h_px) with aspect preserved (the result
 * always EXACTLY target_w x target_h; the cover is centered with
 * black bars filling the inset). HBITMAPs are cached in the
 * service per (issue_id, target_w, target_h). Caller does NOT
 * destroy the HBITMAP; the service owns it.
 *
 * On any fetch / decode failure, returns a synthesized placeholder
 * HBITMAP (gray fill, issue id centered in white). The placeholder
 * is also cached, so repeat calls do not re-attempt the fetch. */
HBITMAP           news_cover_thumb(NewspaperService *s, const char *issue_id,
                                   int target_w_px, int target_h_px);

/* Cache-only peek -- never fetches, never decodes. Returns the
 * cached HBITMAP if it is already in the cache for this
 * (issue_id, w, h) triple; returns NULL otherwise. Safe to call
 * from the UI thread inside WM_PAINT (no blocking I/O). The
 * service holds the cache behind a CRITICAL_SECTION so this
 * peek is thread-safe against a worker thread populating the
 * cache concurrently. */
HBITMAP           news_cover_peek(NewspaperService *s, const char *issue_id,
                                  int target_w_px, int target_h_px);

/* Synchronous WinHTTPS GET of /<id>/<id>.pdf written to out_path.
 * Returns FALSE on any failure; the failure reason is stored in
 * news_last_error() until the next svc call. */
BOOL              news_save_pdf(NewspaperService *s, const char *issue_id,
                                const wchar_t *out_path);

/* Synchronous fetch of /<issue_id>/pages/page-NNN.jpg into a fresh
 * heap allocation. Caller must HeapFree(GetProcessHeap(), 0, *out_data)
 * after use. Returns FALSE on any failure with *out_data NULL and
 * news_last_error() populated. page_index_1_based starts at 1. */
BOOL              news_fetch_page_jpeg(NewspaperService *s,
                                       const char *issue_id,
                                       int page_index_1_based,
                                       unsigned char **out_data,
                                       DWORD *out_len);

/* Most recent error string (NUL-terminated, <= 256 bytes) from any
 * failing call. Never NULL. */
const char       *news_last_error(NewspaperService *s);

#ifdef __cplusplus
}
#endif
#endif /* NEWSPAPER_SERVICE_H */
