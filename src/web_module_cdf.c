/*
 * web_module_cdf.c - Active Channel (CDF) parser + modeless dialog.
 *
 * Phase 6 of MGT Unicorn Suite v0.1.0-mvp. See web_module_cdf.h
 * for the contract and the locked subset (CHANNEL, ITEM, TITLE,
 * ABSTRACT, LOGO HREF, plus HREF on CHANNEL/ITEM; SCHEDULE
 * tolerated and ignored; everything else silently skipped).
 *
 * Fetch is done by the existing libwww WWW_SOURCE / HTLoadToChunk
 * path in src/web_module.c (the same pattern as the renderer's
 * image fetch). This file owns parsing, the parsed model, and
 * the dialog only.
 *
 * Item-click navigations route back through web_navigate via the
 * forward-declared `web_navigate_from_cdf` (web_module.c), so
 * NAV-13 cursor / commit-time discipline still applies to every
 * page reached from the channel.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <ole2.h>
#include <olectl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* libwww URL resolution: matches the discipline in web_module.c so
 * relative item HREFs resolve against the CDF document base URL
 * and dot-segments are normalized. */
#include "WWWLib.h"

#include "web_module_cdf.h"

/* ------------------------------------------------------------------ */
/* Small string helpers (local; the Suite has no string library).      */
/* ------------------------------------------------------------------ */

static char *cdf_strndup(const char *s, int n)
{
    char *r;
    if (n < 0) n = 0;
    r = (char *)malloc((size_t)n + 1);
    if (!r) return NULL;
    if (n > 0) memcpy(r, s, (size_t)n);
    r[n] = '\0';
    return r;
}

static int cdf_ci_eq_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
    }
    return 1;
}

static int cdf_is_name_end(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n'
        || c == '>' || c == '/' || c == '=';
}

/* Match a tag name at `p` against literal `name` (length `nlen`),
 * case-insensitively. The byte after the name (within the tag
 * range bounded by `end`) must be a tag-name terminator so that
 * "CHANNEL" doesn't match "CHANNELS". */
static int cdf_tag_match(const char *p, const char *end,
                         const char *name, int nlen)
{
    if ((end - p) < nlen) return 0;
    if (!cdf_ci_eq_n(p, name, nlen)) return 0;
    if ((end - p) == nlen) return 1;
    return cdf_is_name_end(p[nlen]);
}

/* Find a literal byte from p to end. */
static const char *cdf_find_byte(const char *p, const char *end, char c)
{
    while (p < end) { if (*p == c) return p; p++; }
    return NULL;
}

/* Extract attribute `name` from a tag range [tag_start, tag_end).
 * Tag content includes attributes after the tag name. Returns
 * a heap copy of the value (caller frees) or NULL if absent.
 * Handles "value", 'value', and bare value forms; entities are
 * decoded in cdf_decode_text, so they stay literal here. */
static char *cdf_attr_get(const char *tag_start, const char *tag_end,
                          const char *attr_name)
{
    int nlen = (int)strlen(attr_name);
    const char *p = tag_start;
    while (p < tag_end) {
        /* Skip until we find an attr-name candidate: an alpha char
         * preceded by whitespace or the very start. */
        while (p < tag_end && !((*p >= 'A' && *p <= 'Z')
                             || (*p >= 'a' && *p <= 'z')
                             || *p == '_'))
            p++;
        if (p >= tag_end) break;
        if ((tag_end - p) >= nlen
         && cdf_ci_eq_n(p, attr_name, nlen)
         && ((tag_end - p) == nlen
             || p[nlen] == '=' || p[nlen] == ' '
             || p[nlen] == '\t' || p[nlen] == '\r'
             || p[nlen] == '\n')) {
            const char *q = p + nlen;
            while (q < tag_end && (*q == ' ' || *q == '\t'
                                || *q == '\r' || *q == '\n')) q++;
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && (*q == ' ' || *q == '\t'
                                    || *q == '\r' || *q == '\n')) q++;
                if (q < tag_end && (*q == '"' || *q == '\'')) {
                    char quote = *q++;
                    const char *val_start = q;
                    while (q < tag_end && *q != quote) q++;
                    return cdf_strndup(val_start, (int)(q - val_start));
                } else {
                    const char *val_start = q;
                    while (q < tag_end && *q != ' ' && *q != '\t'
                                       && *q != '\r' && *q != '\n'
                                       && *q != '/' && *q != '>') q++;
                    return cdf_strndup(val_start, (int)(q - val_start));
                }
            }
            /* attribute without "=" — not what we want */
            p = q;
            continue;
        }
        /* Advance past this token. */
        while (p < tag_end && !(*p == ' ' || *p == '\t'
                             || *p == '\r' || *p == '\n')) p++;
    }
    return NULL;
}

/* Find the next `</NAME>` (case-insensitive) starting at p. */
static const char *cdf_find_close_tag(const char *p, const char *end,
                                      const char *name, int nlen)
{
    while (p < end) {
        p = cdf_find_byte(p, end, '<');
        if (!p) return NULL;
        if ((end - p) >= 2 + nlen && p[1] == '/'
         && cdf_tag_match(p + 2, end, name, nlen))
            return p;
        p++;
    }
    return NULL;
}

/* Decode XML/HTML entities in [src, src+len) into a freshly-malloc'd
 * NUL-terminated string and trim leading/trailing whitespace. */
static char *cdf_decode_text(const char *src, int len)
{
    char *out;
    int   oi = 0;
    int   i  = 0;
    if (len < 0) len = 0;
    out = (char *)malloc((size_t)len + 1);
    if (!out) return NULL;
    while (i < len) {
        if (src[i] == '&') {
            int j = i + 1;
            int max = i + 16 < len ? i + 16 : len;
            const char *semi = NULL;
            while (j < max) { if (src[j] == ';') { semi = src + j; break; } j++; }
            if (semi) {
                int el = (int)(semi - (src + i + 1));
                const char *e = src + i + 1;
                int written = 0;
                if (el == 3 && cdf_ci_eq_n(e, "amp", 3)) {
                    out[oi++] = '&'; written = 1;
                } else if (el == 2 && cdf_ci_eq_n(e, "lt", 2)) {
                    out[oi++] = '<'; written = 1;
                } else if (el == 2 && cdf_ci_eq_n(e, "gt", 2)) {
                    out[oi++] = '>'; written = 1;
                } else if (el == 4 && cdf_ci_eq_n(e, "quot", 4)) {
                    out[oi++] = '"'; written = 1;
                } else if (el == 4 && cdf_ci_eq_n(e, "apos", 4)) {
                    out[oi++] = '\''; written = 1;
                } else if (el >= 2 && e[0] == '#') {
                    int base = 10, k = 1;
                    unsigned long v = 0;
                    if (e[1] == 'x' || e[1] == 'X') { base = 16; k = 2; }
                    while (k < el) {
                        unsigned char c = (unsigned char)e[k];
                        unsigned d;
                        if (c >= '0' && c <= '9') d = c - '0';
                        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
                        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
                        else { v = 0; break; }
                        v = v * (unsigned)base + d;
                        k++;
                    }
                    if (v > 0 && v < 0x80) {
                        out[oi++] = (char)v;
                        written = 1;
                    } else if (v >= 0x80 && v < 0x800) {
                        out[oi++] = (char)(0xC0 | (v >> 6));
                        out[oi++] = (char)(0x80 | (v & 0x3F));
                        written = 1;
                    } else if (v >= 0x800 && v < 0x10000) {
                        out[oi++] = (char)(0xE0 | (v >> 12));
                        out[oi++] = (char)(0x80 | ((v >> 6) & 0x3F));
                        out[oi++] = (char)(0x80 | (v & 0x3F));
                        written = 1;
                    }
                }
                if (written) { i = (int)(semi - src) + 1; continue; }
            }
        }
        out[oi++] = src[i++];
    }
    out[oi] = '\0';
    /* Trim leading whitespace. */
    {
        int s = 0;
        while (s < oi && (out[s] == ' ' || out[s] == '\t'
                       || out[s] == '\r' || out[s] == '\n')) s++;
        if (s > 0) {
            memmove(out, out + s, (size_t)(oi - s + 1));
            oi -= s;
        }
    }
    /* Trim trailing whitespace. */
    while (oi > 0 && (out[oi - 1] == ' ' || out[oi - 1] == '\t'
                   || out[oi - 1] == '\r' || out[oi - 1] == '\n'))
        out[--oi] = '\0';
    /* Collapse internal runs of whitespace to a single space — CDF
     * text is presented in a dialog, not preformatted; multi-line
     * source whitespace becomes one space the way browsers render
     * flowing text. */
    {
        int r = 0, w = 0, last_ws = 0;
        while (r < oi) {
            char c = out[r++];
            int  is_ws = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
            if (is_ws) {
                if (!last_ws) out[w++] = ' ';
                last_ws = 1;
            } else {
                out[w++] = c;
                last_ws = 0;
            }
        }
        out[w] = '\0';
    }
    return out;
}

/* Resolve a (possibly relative) href against base_url using libwww
 * (HTParse + HTSimplify), mirroring web_navigate's resolution. */
static char *cdf_resolve(const char *href, const char *base_url)
{
    char *resolved;
    char *result;
    if (!href || !*href) return NULL;
    resolved = HTParse((char *)href,
                       base_url && *base_url ? (char *)base_url : (char *)href,
                       PARSE_ALL);
    if (!resolved) return NULL;
    HTSimplify(&resolved);
    result = cdf_strndup(resolved, (int)strlen(resolved));
    HT_FREE(resolved);
    return result;
}

/* ------------------------------------------------------------------ */
/* Model.                                                              */
/* ------------------------------------------------------------------ */

static CdfItem *cdf_doc_append_item(CdfDoc *doc)
{
    if (doc->items_n >= doc->items_cap) {
        int new_cap = doc->items_cap ? doc->items_cap * 2 : 8;
        CdfItem *na = (CdfItem *)realloc(doc->items,
                                         (size_t)new_cap * sizeof(*na));
        if (!na) return NULL;
        memset(na + doc->items_n, 0,
               (size_t)(new_cap - doc->items_n) * sizeof(*na));
        doc->items = na;
        doc->items_cap = new_cap;
    }
    return &doc->items[doc->items_n++];
}

void cdf_doc_free(CdfDoc *doc)
{
    int i;
    if (!doc) return;
    free(doc->title);
    free(doc->abstract);
    free(doc->logo_href);
    free(doc->channel_href);
    free(doc->logo_bytes);
    for (i = 0; i < doc->items_n; i++) {
        free(doc->items[i].title);
        free(doc->items[i].abstract);
        free(doc->items[i].href);
    }
    free(doc->items);
    free(doc);
}

/* ------------------------------------------------------------------ */
/* Parser.                                                             */
/* ------------------------------------------------------------------ */

CdfDoc *cdf_parse(const char *bytes, int size, const char *base_url)
{
    CdfDoc *doc;
    const char *p, *end;
    int scope = 0;   /* 0=outside, 1=channel, 2=item */
    CdfItem *cur_item = NULL;

    if (!bytes || size <= 0) return NULL;
    doc = (CdfDoc *)calloc(1, sizeof(*doc));
    if (!doc) return NULL;

    p   = bytes;
    end = bytes + size;

    while (p < end) {
        const char *tag_start, *tag_end;
        int is_close;
        const char *name_start;

        if (*p != '<') { p++; continue; }
        tag_start = p + 1;
        tag_end   = cdf_find_byte(tag_start, end, '>');
        if (!tag_end) break;

        if (tag_start < end && (*tag_start == '!' || *tag_start == '?')) {
            p = tag_end + 1;
            continue;
        }
        is_close = (tag_start < end && *tag_start == '/');
        name_start = tag_start + (is_close ? 1 : 0);

        if (cdf_tag_match(name_start, tag_end, "CHANNEL", 7)) {
            if (is_close) {
                scope = 0;
            } else {
                char *href;
                scope = 1;
                href = cdf_attr_get(name_start, tag_end, "HREF");
                if (href && !doc->channel_href)
                    doc->channel_href = cdf_resolve(href, base_url);
                free(href);
            }
            p = tag_end + 1;
            continue;
        }
        if (cdf_tag_match(name_start, tag_end, "ITEM", 4)) {
            if (is_close) {
                cur_item = NULL;
                if (scope == 2) scope = 1;
            } else {
                cur_item = cdf_doc_append_item(doc);
                if (cur_item) {
                    char *href = cdf_attr_get(name_start, tag_end, "HREF");
                    if (href) cur_item->href = cdf_resolve(href, base_url);
                    free(href);
                }
                scope = 2;
            }
            p = tag_end + 1;
            continue;
        }
        if (cdf_tag_match(name_start, tag_end, "LOGO", 4)) {
            if (!is_close) {
                char *href = cdf_attr_get(name_start, tag_end, "HREF");
                if (href && !doc->logo_href)
                    doc->logo_href = cdf_resolve(href, base_url);
                free(href);
            }
            p = tag_end + 1;
            continue;
        }
        /* TITLE / ABSTRACT: the deployed format (incl. the served
         * greektimes.cdf) uses element content (<TITLE>text</TITLE>),
         * but the 1997-03-09 W3C submission DTD declares both as
         * EMPTY with a required VALUE attribute. Per the authority
         * order, prefer what is actually served (element content),
         * and tolerate the 970309 VALUE-attribute form as a fallback
         * when element content is absent or empty (self-closing tag,
         * missing close tag, or whitespace-only body). */
        if (!is_close && cdf_tag_match(name_start, tag_end, "TITLE", 5)) {
            char       *val_attr  = cdf_attr_get(name_start, tag_end, "VALUE");
            int         self_close;
            const char *close;
            char       *text      = NULL;
            {
                const char *q = tag_end;
                while (q > name_start && (q[-1] == ' ' || q[-1] == '\t'
                                       || q[-1] == '\r' || q[-1] == '\n')) q--;
                self_close = (q > name_start && q[-1] == '/');
            }
            if (!self_close) {
                close = cdf_find_close_tag(tag_end + 1, end, "TITLE", 5);
                if (close) {
                    text = cdf_decode_text(tag_end + 1,
                                           (int)(close - (tag_end + 1)));
                }
                if ((!text || !*text) && val_attr) {
                    free(text);
                    text = cdf_decode_text(val_attr, (int)strlen(val_attr));
                }
                if (text) {
                    if (scope == 2 && cur_item && !cur_item->title)
                        cur_item->title = text;
                    else if (scope == 1 && !doc->title)
                        doc->title = text;
                    else
                        free(text);
                }
                free(val_attr);
                if (close) {
                    p = cdf_find_byte(close, end, '>');
                    if (!p) break;
                    p++;
                } else {
                    p = tag_end + 1;
                }
                continue;
            }
            if (val_attr) {
                text = cdf_decode_text(val_attr, (int)strlen(val_attr));
                if (text) {
                    if (scope == 2 && cur_item && !cur_item->title)
                        cur_item->title = text;
                    else if (scope == 1 && !doc->title)
                        doc->title = text;
                    else
                        free(text);
                }
                free(val_attr);
            }
            p = tag_end + 1;
            continue;
        }
        if (!is_close && cdf_tag_match(name_start, tag_end, "ABSTRACT", 8)) {
            char       *val_attr  = cdf_attr_get(name_start, tag_end, "VALUE");
            int         self_close;
            const char *close;
            char       *text      = NULL;
            {
                const char *q = tag_end;
                while (q > name_start && (q[-1] == ' ' || q[-1] == '\t'
                                       || q[-1] == '\r' || q[-1] == '\n')) q--;
                self_close = (q > name_start && q[-1] == '/');
            }
            if (!self_close) {
                close = cdf_find_close_tag(tag_end + 1, end, "ABSTRACT", 8);
                if (close) {
                    text = cdf_decode_text(tag_end + 1,
                                           (int)(close - (tag_end + 1)));
                }
                if ((!text || !*text) && val_attr) {
                    free(text);
                    text = cdf_decode_text(val_attr, (int)strlen(val_attr));
                }
                if (text) {
                    if (scope == 2 && cur_item && !cur_item->abstract)
                        cur_item->abstract = text;
                    else if (scope == 1 && !doc->abstract)
                        doc->abstract = text;
                    else
                        free(text);
                }
                free(val_attr);
                if (close) {
                    p = cdf_find_byte(close, end, '>');
                    if (!p) break;
                    p++;
                } else {
                    p = tag_end + 1;
                }
                continue;
            }
            if (val_attr) {
                text = cdf_decode_text(val_attr, (int)strlen(val_attr));
                if (text) {
                    if (scope == 2 && cur_item && !cur_item->abstract)
                        cur_item->abstract = text;
                    else if (scope == 1 && !doc->abstract)
                        doc->abstract = text;
                    else
                        free(text);
                }
                free(val_attr);
            }
            p = tag_end + 1;
            continue;
        }
        /* SCHEDULE and anything else: silently skip. */
        p = tag_end + 1;
    }

    return doc;
}

/* ------------------------------------------------------------------ */
/* Active Channel dialog.                                              */
/* ------------------------------------------------------------------ */

#define CDF_DIALOG_CLASS "MgtActiveChannel"

enum {
    IDC_CDF_TITLE   = 0xC101,
    IDC_CDF_ABSTRACT,
    IDC_CDF_LOGO,
    IDC_CDF_LIST,
    IDC_CDF_CLOSE,
    IDC_CDF_OPEN
};

static HWND     g_cdf_dialog       = NULL;
static CdfDoc  *g_cdf_doc          = NULL;
static HBITMAP  g_cdf_logo_hbm     = NULL;
static int      g_cdf_logo_w       = 0;
static int      g_cdf_logo_h       = 0;
static HWND     g_cdf_owner_content = NULL;
static int      g_cdf_class_registered = 0;
static HFONT    g_cdf_font         = NULL;

/* Decode logo bytes into HBITMAP via OleLoadPicture (same fallback
 * the renderer uses; works for GIF/JPEG/PNG/BMP). Returns NULL on
 * failure; updates *out_w, *out_h on success. */
static HBITMAP cdf_decode_logo(const char *bytes, int size,
                               int *out_w, int *out_h)
{
    HGLOBAL    hg;
    void      *p;
    IStream   *stream = NULL;
    IPicture  *pic    = NULL;
    HRESULT    hr;
    HBITMAP    hbm    = NULL;
    HBITMAP    result = NULL;

    *out_w = 0;
    *out_h = 0;
    if (!bytes || size <= 0) return NULL;
    hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)size);
    if (!hg) return NULL;
    p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return NULL; }
    memcpy(p, bytes, (size_t)size);
    GlobalUnlock(hg);
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK) {
        GlobalFree(hg);
        return NULL;
    }
    hr = OleLoadPicture(stream, (LONG)size, FALSE,
                        &IID_IPicture, (LPVOID *)&pic);
    if (SUCCEEDED(hr) && pic) {
        OLE_HANDLE handle = 0;
        long       hmW = 0, hmH = 0;
        HDC        sdc;
        int        dpiX, dpiY;
        int        w, h;
        pic->lpVtbl->get_Handle(pic, &handle);
        pic->lpVtbl->get_Width (pic, &hmW);
        pic->lpVtbl->get_Height(pic, &hmH);
        sdc  = GetDC(NULL);
        dpiX = GetDeviceCaps(sdc, LOGPIXELSX);
        dpiY = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(NULL, sdc);
        w = (int)((hmW * dpiX) / 2540);
        h = (int)((hmH * dpiY) / 2540);
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
        if (handle) {
            hbm = (HBITMAP)CopyImage((HBITMAP)(UINT_PTR)handle,
                                     IMAGE_BITMAP, 0, 0,
                                     LR_COPYRETURNORG);
            if (hbm) {
                result = hbm;
                *out_w = w;
                *out_h = h;
            }
        }
        pic->lpVtbl->Release(pic);
    }
    stream->lpVtbl->Release(stream);
    return result;
}

static void cdf_layout(HWND hwnd)
{
    RECT rc;
    HWND h_title, h_abs, h_logo, h_list, h_close, h_open;
    int  pad = 10;
    int  title_h = 24;
    int  abs_h   = 60;
    int  logo_h  = g_cdf_logo_hbm ? (g_cdf_logo_h > 96 ? 96 : g_cdf_logo_h) : 0;
    int  logo_w  = g_cdf_logo_hbm ? (g_cdf_logo_w > 96 ? 96 : g_cdf_logo_w) : 0;
    int  btn_h   = 28;
    int  y;
    int  w, h;

    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    h_title = GetDlgItem(hwnd, IDC_CDF_TITLE);
    h_abs   = GetDlgItem(hwnd, IDC_CDF_ABSTRACT);
    h_logo  = GetDlgItem(hwnd, IDC_CDF_LOGO);
    h_list  = GetDlgItem(hwnd, IDC_CDF_LIST);
    h_close = GetDlgItem(hwnd, IDC_CDF_CLOSE);
    h_open  = GetDlgItem(hwnd, IDC_CDF_OPEN);

    y = pad;
    if (logo_h > 0) {
        MoveWindow(h_logo, w - pad - logo_w, y, logo_w, logo_h, TRUE);
        MoveWindow(h_title, pad, y,
                   (w - logo_w - 3 * pad), title_h, TRUE);
        y += title_h + pad / 2;
        MoveWindow(h_abs, pad, y,
                   (w - logo_w - 3 * pad), abs_h, TRUE);
        y = pad + (logo_h > (title_h + pad / 2 + abs_h)
                 ? logo_h : (title_h + pad / 2 + abs_h));
    } else {
        MoveWindow(h_title, pad, y, w - 2 * pad, title_h, TRUE);
        y += title_h + pad / 2;
        MoveWindow(h_abs, pad, y, w - 2 * pad, abs_h, TRUE);
        y += abs_h;
    }
    y += pad;

    MoveWindow(h_list, pad, y, w - 2 * pad,
               h - y - btn_h - 2 * pad, TRUE);

    MoveWindow(h_open,  pad,             h - pad - btn_h, 120, btn_h, TRUE);
    MoveWindow(h_close, w - pad - 100,   h - pad - btn_h, 100, btn_h, TRUE);
}

static void cdf_open_selected(HWND hwnd)
{
    HWND lb;
    LRESULT sel;
    if (!g_cdf_doc) return;
    lb = GetDlgItem(hwnd, IDC_CDF_LIST);
    sel = SendMessageA(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    if (sel >= 0 && sel < g_cdf_doc->items_n) {
        const char *href = g_cdf_doc->items[(int)sel].href;
        if (href && *href) web_navigate_from_cdf(href);
    }
}

static LRESULT CALLBACK CdfDlgProc(HWND hwnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(hwnd, GWLP_HINSTANCE);
        HWND h;
        int  i;

        if (!g_cdf_font) {
            NONCLIENTMETRICSA ncm;
            ncm.cbSize = sizeof(ncm);
            if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS,
                                      sizeof(ncm), &ncm, 0))
                g_cdf_font = CreateFontIndirectA(&ncm.lfMessageFont);
        }

        h = CreateWindowExA(0, "STATIC",
                            g_cdf_doc && g_cdf_doc->title
                              ? g_cdf_doc->title
                              : "Active Channel",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_CDF_TITLE, hInst, NULL);
        if (h && g_cdf_font) SendMessageA(h, WM_SETFONT, (WPARAM)g_cdf_font, TRUE);

        h = CreateWindowExA(0, "STATIC",
                            g_cdf_doc && g_cdf_doc->abstract
                              ? g_cdf_doc->abstract
                              : "",
                            WS_CHILD | WS_VISIBLE | SS_LEFT,
                            0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_CDF_ABSTRACT, hInst, NULL);
        if (h && g_cdf_font) SendMessageA(h, WM_SETFONT, (WPARAM)g_cdf_font, TRUE);

        h = CreateWindowExA(0, "STATIC", "",
                            WS_CHILD | (g_cdf_logo_hbm ? WS_VISIBLE : 0)
                              | SS_BITMAP,
                            0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_CDF_LOGO, hInst, NULL);
        if (h && g_cdf_logo_hbm)
            SendMessageA(h, STM_SETIMAGE, IMAGE_BITMAP,
                         (LPARAM)g_cdf_logo_hbm);

        h = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP
                              | LBS_NOTIFY,
                            0, 0, 0, 0, hwnd,
                            (HMENU)(INT_PTR)IDC_CDF_LIST, hInst, NULL);
        if (h && g_cdf_font) SendMessageA(h, WM_SETFONT, (WPARAM)g_cdf_font, TRUE);
        if (h && g_cdf_doc) {
            for (i = 0; i < g_cdf_doc->items_n; i++) {
                const char *t = g_cdf_doc->items[i].title;
                SendMessageA(h, LB_ADDSTRING, 0,
                             (LPARAM)(t && *t ? t : "(untitled)"));
            }
            if (g_cdf_doc->items_n > 0)
                SendMessageA(h, LB_SETCURSEL, 0, 0);
        }

        h = CreateWindowA("BUTTON", "Open",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                          0, 0, 0, 0, hwnd,
                          (HMENU)(INT_PTR)IDC_CDF_OPEN, hInst, NULL);
        if (h && g_cdf_font) SendMessageA(h, WM_SETFONT, (WPARAM)g_cdf_font, TRUE);

        h = CreateWindowA("BUTTON", "Close",
                          WS_CHILD | WS_VISIBLE | WS_TABSTOP
                            | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                          0, 0, 0, 0, hwnd,
                          (HMENU)(INT_PTR)IDC_CDF_CLOSE, hInst, NULL);
        if (h && g_cdf_font) SendMessageA(h, WM_SETFONT, (WPARAM)g_cdf_font, TRUE);

        cdf_layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        cdf_layout(hwnd);
        return 0;

    case WM_COMMAND: {
        WORD id     = LOWORD(wParam);
        WORD notify = HIWORD(wParam);
        (void)lParam;
        if (id == IDC_CDF_CLOSE && notify == BN_CLICKED) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == IDC_CDF_OPEN && notify == BN_CLICKED) {
            cdf_open_selected(hwnd);
            return 0;
        }
        if (id == IDC_CDF_LIST && notify == LBN_DBLCLK) {
            cdf_open_selected(hwnd);
            return 0;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_cdf_logo_hbm) {
            DeleteObject(g_cdf_logo_hbm);
            g_cdf_logo_hbm = NULL;
        }
        g_cdf_logo_w = g_cdf_logo_h = 0;
        if (g_cdf_doc) {
            cdf_doc_free(g_cdf_doc);
            g_cdf_doc = NULL;
        }
        if (hwnd == g_cdf_dialog) g_cdf_dialog = NULL;
        g_cdf_owner_content = NULL;
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void cdf_register_class(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_cdf_class_registered) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = CdfDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CDF_DIALOG_CLASS;
    RegisterClassA(&wc);
    g_cdf_class_registered = 1;
}

HWND cdf_show_dialog(HWND main_hwnd, HWND content_hwnd, CdfDoc *doc)
{
    HINSTANCE hInst;
    HWND      hwnd;
    RECT      mrc;
    int       x, y, w, h;

    if (!doc) return NULL;

    /* Replace any existing dialog (single-instance discipline). */
    if (g_cdf_dialog) {
        DestroyWindow(g_cdf_dialog);
        /* WM_DESTROY clears globals and frees the prior doc. */
    }

    hInst = (HINSTANCE)GetWindowLongPtrA(main_hwnd, GWLP_HINSTANCE);
    cdf_register_class(hInst);

    g_cdf_doc          = doc;
    g_cdf_owner_content = content_hwnd;
    if (doc->logo_bytes && doc->logo_size > 0) {
        g_cdf_logo_hbm = cdf_decode_logo(doc->logo_bytes, doc->logo_size,
                                         &g_cdf_logo_w, &g_cdf_logo_h);
    }

    if (main_hwnd && GetWindowRect(main_hwnd, &mrc)) {
        w = 520;
        h = 480;
        x = mrc.left + ((mrc.right - mrc.left) - w) / 2;
        y = mrc.top  + ((mrc.bottom - mrc.top) - h) / 2;
    } else {
        x = CW_USEDEFAULT; y = CW_USEDEFAULT;
        w = 520; h = 480;
    }

    hwnd = CreateWindowExA(0, CDF_DIALOG_CLASS, "Active Channel",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU
                             | WS_THICKFRAME | WS_MINIMIZEBOX,
                           x, y, w, h,
                           main_hwnd, NULL, hInst, NULL);
    if (!hwnd) {
        /* Owner of the doc reverts to us on failure; free it. */
        g_cdf_doc = NULL;
        if (g_cdf_logo_hbm) {
            DeleteObject(g_cdf_logo_hbm);
            g_cdf_logo_hbm = NULL;
        }
        cdf_doc_free(doc);
        return NULL;
    }
    g_cdf_dialog = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

void cdf_dialog_hide(void)
{
    if (g_cdf_dialog) ShowWindow(g_cdf_dialog, SW_HIDE);
}

void cdf_dialog_show(void)
{
    if (g_cdf_dialog) ShowWindow(g_cdf_dialog, SW_SHOW);
}

void cdf_dialog_close(void)
{
    if (g_cdf_dialog) {
        HWND h = g_cdf_dialog;
        DestroyWindow(h);
    }
}
