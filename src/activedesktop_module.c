/*
 * activedesktop_module.c - Unicorn Desktop scene (Phase 6b).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Phase 6b populates the F1 module with the three-zone scene defined
 * in docs/2026-05-19_ACTIVE_CHANNEL_BLUEPRINT_v6 section 2:
 *   1. Backdrop: solid #010180 (RGB 1, 1, 128).
 *   2. Wordmark: "Active Desktop" rendered in a slightly lighter
 *      shade of the backdrop blue (RGB 40, 40, 160). No AlphaBlend;
 *      color contrast alone produces the subdued visual.
 *   3. Channel Bar: docked to the right edge, Win98-style buttons
 *      populated from the items in the served greektimes.cdf.
 *      Placeholder icon rect on each button; no real GIF artwork
 *      (deferred to a later polish item per the v6 blueprint).
 *   4. Ticker band: bottom strip rendered by web_paint_to with this
 *      module's own WebRenderView instance over a parsed ticker.html
 *      WebDoc. Hit-tested via web_hit_test_link for link click
 *      handoff to Retro Web (F2).
 *
 * The module owns: its own WebRenderView (g_b_ad_view), a parsed
 * CdfDoc (g_ad_channel), and a parsed WebDoc (g_ad_ticker_doc). It
 * runs two timer cadences while active: 60 s ticker refetch and
 * 15 min CDF re-pull. State (channel data, ticker doc, view) is
 * preserved across module switches; only the timers are stopped on
 * deactivate and started on activate. The first activate triggers
 * a synchronous initial fetch.
 *
 * Fetch model: synchronous on the GUI thread via web_module.c's
 * Phase 6b additive helpers web_fetch_and_parse_html_sync and
 * web_fetch_raw_bytes_sync. Both defer (return NULL / 0) if a Retro
 * Web fetch is in flight (g_b_fetch != NULL), preserving the
 * single-parser-in-flight contract. The Active Desktop timer just
 * retries on the next tick if a defer happens.
 *
 * Cross-module handoff: a Channel Bar click or a ticker-band link
 * click calls suite_set_active_module(SUITE_ID_BTN_WEB) to switch
 * to F2 (Retro Web) and then web_navigate_from_cdf(href) to drive
 * the navigation. The Retro Web side's web_module_activate has run
 * by the time the navigate fires, so g_b_content is populated; the
 * navigation respects NAV-13 cursor discipline.
 */

#include "suite_shell.h"
#include "render_engine.h"
#include "web_module.h"
#include "web_module_cdf.h"
#include "suite_logo.h"
#include "suite_res.h"

#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Constants.                                                          */
/* ------------------------------------------------------------------ */

#define AD_RENDER_CLASS         "MgtUnicornDesktop"

#define AD_TICKER_TIMER_ID      0xAD20u
#define AD_TICKER_INTERVAL_MS   15000u      /* 15 s (Phase 6c-Client-2) */
#define AD_CDF_TIMER_ID         0xAD21u
#define AD_CDF_INTERVAL_MS      900000u     /* 15 min */

#define AD_CHANNEL_BAR_W        220
#define AD_CHANNEL_BTN_H        56
#define AD_CHANNEL_PAD          8
#define AD_ICON_SIZE            32

/* Floating ticker panel (Phase 6c-Client-2). The ticker is no longer a
 * flush bottom band; it sits as a teal panel with a yellow border,
 * inset horizontally from the main scene's left edge and from the
 * channel bar on the right, and floats AD_TICKER_BOTTOM_MARGIN above
 * the desktop scene's bottom. Phase 6c-Client-5: panel height bumped
 * to 220 so weather (headline + 4-5 wrapped body lines + indicator)
 * fits inside the inset, and AD_TICKER_PAD_PX adds 15 px of guaranteed
 * background between the yellow border and any rendered glyph on all
 * four sides. */
#define AD_TICKER_BAND_H        220         /* panel height */
#define AD_TICKER_BOTTOM_MARGIN 100         /* gap from desktop bottom */
#define AD_TICKER_HMARGIN       60          /* inset from scene_main sides */
#define AD_TICKER_BORDER_W      2           /* yellow frame thickness */
#define AD_TICKER_PAD_PX        15          /* bg gap between border and glyphs */

#define AD_BACKDROP_RGB         RGB(1, 1, 128)        /* #010180 */
#define AD_WORDMARK_RGB         RGB(40, 40, 160)
#define AD_BTN_FILL_RGB         RGB(192, 192, 192)
#define AD_BTN_BORDER_RGB       RGB(0, 0, 0)
#define AD_BTN_TEXT_RGB         RGB(0, 0, 0)
#define AD_BTN_ICON_RGB         RGB(96, 96, 96)
#define AD_BAND_PLACEHOLDER_RGB         RGB(40, 40, 70)
#define AD_BAND_PLACEHOLDER_TEXT_RGB    RGB(220, 220, 220)
#define AD_CHANNELBAR_TITLE_RGB         RGB(255, 255, 255)
/* AD_TICKER_BG_RGB is the safety fallback used only when there is no
 * parsed ticker doc yet; the actual panel bg is read live from the
 * doc's body_bg each paint (Phase 6c-Client-5) so the Amber red flip
 * follows the doc without a second constant. The hover darken is now
 * derived via darken_rgb(doc_bg, 20) at paint time. */
#define AD_TICKER_BG_RGB                RGB(0, 128, 128)   /* #008080 teal safety fallback */
#define AD_TICKER_BORDER_RGB            RGB(255, 255, 0)   /* #FFFF00 yellow */

/* Hardcoded resource URLs for v0.1.0-mvp 6b. CDF is the live channel
 * file; the ticker target is the new resource the Phase 6c writer
 * will compose. 6b builds against the same URL and renders the
 * "Ticker unavailable" placeholder until 6c lands. */
#define AD_CDF_URL              "http://retro.greektimes.ca/greektimes.cdf"
#define AD_TICKER_URL           "http://retro.greektimes.ca/ticker.html"

#define AD_MAX_BTNS             32

/* ------------------------------------------------------------------ */
/* State.                                                              */
/* ------------------------------------------------------------------ */

static HWND     g_ad_hwnd               = NULL;
static int      g_ad_class_registered   = 0;
static int      g_ad_first_activate     = 1;
static HFONT    g_ad_wordmark_font      = NULL;

static CdfDoc  *g_ad_channel            = NULL;
static int      g_ad_channel_failed     = 0;

static WebDoc  *g_ad_ticker_doc         = NULL;
static int      g_ad_ticker_failed      = 0;

/* The Active Desktop module's own per-view state. Distinct instance
 * from Retro Web's g_b_view; the shared engine reads and writes
 * through whichever view is passed (Phase 6R contract). */
static WebRenderView g_b_ad_view = {
    .sel_anchor_idx = -1,
    .sel_extent_idx = -1,
};

/* Channel Bar button rectangles, populated each paint and consumed
 * by WM_LBUTTONDOWN for hit testing. */
static RECT     g_ad_btn_rects[AD_MAX_BTNS];
static int      g_ad_btn_count          = 0;
static RECT     g_ad_ticker_rect;       /* current ticker band rect */

/* Phase 6c-Client-3: hover state for the ticker panel. The render
 * darkens the doc body_bg via darken_rgb(_, 20) while
 * g_ad_ticker_hover is non-zero; g_ad_ticker_tracking latches the
 * TrackMouseEvent arm so we only request one WM_MOUSELEAVE per
 * hover entry. */
static int      g_ad_ticker_hover       = 0;
static int      g_ad_ticker_tracking    = 0;

/* Phase 6c-Client-5: click-marker reader state. The Tune-8 server
 * emits an HTML comment of the form `<!-- click=URL -->` after the
 * </table> in ticker.html. scan_ticker_click_marker reads the raw
 * response bytes (the parsed WebDoc does not preserve comments) and
 * populates this buffer once per fetched ticker.html. URLs in the
 * server table are at most a few hundred bytes; 2048 is generous
 * headroom for hostname and path growth. The empty buffer plus the
 * marker-seen flag distinguishes three cases:
 *   url[0] != '\0'                              -> navigate to url
 *   marker_seen && url[0] == '\0'               -> idle slot; no nav
 *   !marker_seen                                -> legacy server; fall
 *                                                  back to the CDF
 *                                                  News Ticker entry */
static char     g_ad_ticker_click_url[2048] = "";
static int      g_ad_ticker_click_marker_seen = 0;

/* ------------------------------------------------------------------ */
/* Wordmark font cache.                                                */
/* ------------------------------------------------------------------ */

static HFONT ad_get_wordmark_font(void)
{
    if (!g_ad_wordmark_font) {
        g_ad_wordmark_font = CreateFontA(
            -64, 0, 0, 0, FW_BOLD,
            FALSE, FALSE, FALSE,
            ANSI_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            VARIABLE_PITCH | FF_SWISS,
            "Tahoma");
    }
    return g_ad_wordmark_font;
}

/* ------------------------------------------------------------------ */
/* Phase 6c-Client-5 helpers.                                          */
/* ------------------------------------------------------------------ */

/* Convert a UTF-8 C string to a malloc'd UTF-16 buffer the caller
 * must free, or NULL. NULL input -> NULL output. CDF text is UTF-8 by
 * spec; this is the render seam that lets the channel bar draw Greek
 * (Ειδήσεις, Καιρός) via DrawTextW instead of mangling it through the
 * ANSI codepage. */
static wchar_t *ad_utf8_to_wide(const char *s)
{
    int wlen;
    wchar_t *w;
    if (!s) return NULL;
    wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    w = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

/* Darken a COLORREF by pct percent on each channel, clamped at 0.
 * Used to derive the hover tint from the live doc body_bg so the
 * Amber red flip (server sets body_bg=#FF0000 when pinned) produces
 * a darker red on hover without a second constant. pct=20 reproduces
 * the historical teal->#006666 ratio (128 * 0.8 = 102.4 ~= 102). */
static COLORREF darken_rgb(COLORREF c, int pct)
{
    int r = GetRValue(c) * (100 - pct) / 100;
    int g = GetGValue(c) * (100 - pct) / 100;
    int b = GetBValue(c) * (100 - pct) / 100;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return RGB(r, g, b);
}

/* Scan the raw ticker.html response bytes for the Phase 6c-Tune-8
 * click marker `<!-- click=URL -->`. URLs do not contain "--" so the
 * terminator is unambiguous. Always clears `out` to "" before the
 * search so the caller can distinguish missing-marker from
 * present-but-empty by combining the empty buffer with the return
 * value. Returns 1 if both literals were found, else 0. */
static int scan_ticker_click_marker(const char *bytes, size_t len,
                                    char *out, size_t outsz)
{
    const char *open_lit  = "<!-- click=";
    const size_t open_len = 11;
    const char *close_lit = " -->";
    const size_t close_len = 4;
    const char *p;
    const char *url_end;
    size_t url_len;
    if (outsz == 0) return 0;
    out[0] = '\0';
    if (!bytes || len < open_len + close_len) return 0;
    {
        size_t i;
        p = NULL;
        for (i = 0; i + open_len <= len; i++) {
            if (memcmp(bytes + i, open_lit, open_len) == 0) {
                p = bytes + i + open_len;
                break;
            }
        }
    }
    if (!p) return 0;
    {
        const char *limit = bytes + len;
        const char *q = p;
        url_end = NULL;
        while (q + close_len <= limit) {
            if (memcmp(q, close_lit, close_len) == 0) {
                url_end = q;
                break;
            }
            q++;
        }
    }
    if (!url_end) return 0;
    url_len = (size_t)(url_end - p);
    if (url_len >= outsz) url_len = outsz - 1;
    if (url_len > 0) memcpy(out, p, url_len);
    out[url_len] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Initial and timer-driven fetches.                                   */
/* ------------------------------------------------------------------ */

static void ad_refresh_channel(HWND hwnd)
{
    char *bytes = NULL;
    int   size  = 0;
    if (web_fetch_raw_bytes_sync(AD_CDF_URL, &bytes, &size)) {
        CdfDoc *ch = cdf_parse(bytes, size, AD_CDF_URL);
        if (ch) {
            if (g_ad_channel) cdf_doc_free(g_ad_channel);
            g_ad_channel = ch;
            g_ad_channel_failed = 0;
            if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
        }
    } else if (!g_ad_channel) {
        g_ad_channel_failed = 1;
        if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
    }
    free(bytes);
}

static void ad_refresh_ticker(HWND hwnd)
{
    WebDoc *new_doc;
    /* Phase 6c-Client-5: do a raw-bytes fetch first so the Tune-8
     * `<!-- click=URL -->` comment marker can be read out of the
     * verbatim response body; the HText parse path drops HTML
     * comments and cannot expose them through the WebDoc surface.
     * The parsed-doc fetch follows for rendering. Two HTTP requests
     * per 15 s tick; the back-to-back window is sub-100 ms while the
     * server-side rotation cadence is 15 s, so a straddle of a
     * server-write boundary is sub-1 %. On a straddle the displayed
     * text and the click URL mismatch for one slot; self-corrects on
     * the next refresh. */
    {
        char *raw = NULL;
        int   raw_size = 0;
        if (web_fetch_raw_bytes_sync(AD_TICKER_URL, &raw, &raw_size)
            && raw && raw_size > 0) {
            g_ad_ticker_click_marker_seen =
                scan_ticker_click_marker(raw, (size_t)raw_size,
                                         g_ad_ticker_click_url,
                                         sizeof g_ad_ticker_click_url);
        }
        /* If the raw fetch failed we keep the prior marker state so
         * a transient failure does not silently lose click routing. */
        free(raw);
    }
    new_doc = web_fetch_and_parse_html_sync(AD_TICKER_URL);
    if (new_doc) {
        if (g_ad_ticker_doc) webdoc_free(g_ad_ticker_doc);
        g_ad_ticker_doc = new_doc;
        g_ad_ticker_failed = 0;
        /* The new doc has its own token map; the previous selection
         * extent is no longer meaningful. */
        g_b_ad_view.sel_anchor_idx = -1;
        g_b_ad_view.sel_extent_idx = -1;
        g_b_ad_view.sel_active     = 0;
        if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
    } else if (!g_ad_ticker_doc) {
        g_ad_ticker_failed = 1;
        if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* Channel-item invocation helper. Single navigation entry point so    */
/* the channel-bar button click and the Phase 6c-Client-3 ticker-panel */
/* click cannot diverge: both resolve to one CdfItem index in the      */
/* parsed channel doc and run the same hand-off (switch to Retro Web   */
/* + web_navigate_from_cdf on the item's href).                        */
/* ------------------------------------------------------------------ */

static void ad_invoke_channel_item(int idx)
{
    const char *href;
    if (!g_ad_channel || idx < 0 || idx >= g_ad_channel->items_n) return;
    href = g_ad_channel->items[idx].href;
    if (!href || !*href) return;
    /* External-handler dispatch (2026-06-26): .ram (and any future
     * stream-descriptor extension rwb_external_handler_try recognizes)
     * launches RealPlayer directly from the Unicorn Desktop. No module
     * switch, no empty Retro Web page -- the artifact this removes. The
     * CDF href is UTF-8, which is exactly what the entry point expects,
     * so no wide conversion is needed here. TRUE means it took the URL
     * over (even if the handoff failed); we must not fall through to the
     * F2 navigation in that case. */
    if (rwb_external_handler_try(href, NULL)) return;
    suite_set_active_module(SUITE_ID_BTN_WEB);
    web_navigate_from_cdf(href);
}

/* The ticker panel always lands the user on the channel item titled
 * "News Ticker" (case-insensitive). The CDF served by the Phase 6c
 * pipeline assigns USAGE VALUE="DesktopComponent" to that exact item
 * and routes its HREF at the day's latest Daily News (English) page,
 * so this lookup keeps the panel click and the channel-bar entry in
 * lockstep without duplicating the href. */
static int ad_find_news_ticker_index(void)
{
    int i;
    if (!g_ad_channel) return -1;
    for (i = 0; i < g_ad_channel->items_n; i++) {
        const char *t = g_ad_channel->items[i].title;
        if (t && _stricmp(t, "News Ticker") == 0) return i;
    }
    return -1;
}

/* Map a CDF channel item's title to the RCDATA resource ID of its
 * 32x32 icon (see suite_res.h / suite.rc). ASCII titles match
 * case-insensitively; the two Greek titles match on their exact UTF-8
 * bytes (this .c file is UTF-8, as are it->title strings parsed from the
 * served CDF). Returns 0 when no icon is defined for the title, in which
 * case the caller falls back to the gray placeholder square. */
static int ad_icon_res_for_title(const char *title)
{
    if (!title) return 0;
    if (_stricmp(title, "News")        == 0) return IDR_ICON_NEWS;
    if (_stricmp(title, "Weather")     == 0) return IDR_ICON_WEATHER;
    if (_stricmp(title, "Home Page")   == 0) return IDR_ICON_HOMEPAGE;
    if (_stricmp(title, "News Ticker") == 0) return IDR_ICON_NEWSTICKER;
    if (_stricmp(title, "Live Radio")  == 0) return IDR_ICON_LIVERADIO;
    if (strcmp(title, "\xCE\x9A\xCE\xB1\xCE\xB9\xCF\x81\xCF\x8C\xCF\x82") == 0)
        return IDR_ICON_KAIROS;    /* Καιρός */
    if (strcmp(title, "\xCE\x95\xCE\xB9\xCE\xB4\xCE\xAE\xCF\x83\xCE\xB5\xCE\xB9\xCF\x82") == 0)
        return IDR_ICON_EISISEIS;  /* Ειδήσεις */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scene compositor.                                                   */
/* ------------------------------------------------------------------ */

static void ad_paint_scene(HDC hdc, const RECT *client)
{
    int w = client->right - client->left;
    int h = client->bottom - client->top;
    int channel_left = w - AD_CHANNEL_BAR_W;
    /* Channel-bar bottom: preserved at the historical h - 80 position
     * from the pre-Phase-6c-Client-2 layout. The ticker is no longer
     * a flush band at this y, so this is now a stand-alone channel-bar
     * inset rather than a derived "ticker top". */
    int channel_bottom = h - 80;
    /* scene_main is the desktop scene drawable area excluding the
     * Channel Bar on the right (there is no top tab bar at this
     * level; the suite tab bar lives in the parent shell). */
    int scene_main_right = channel_left;
    HBRUSH brBackdrop;
    HFONT  old_font  = NULL;
    int    i;

    if (channel_left     < 0) channel_left     = 0;
    if (channel_bottom   < 0) channel_bottom   = 0;
    if (scene_main_right < 0) scene_main_right = 0;

    /* Backdrop. */
    brBackdrop = CreateSolidBrush(AD_BACKDROP_RGB);
    if (brBackdrop) {
        FillRect(hdc, (RECT *)client, brBackdrop);
        DeleteObject(brBackdrop);
    }

    /* Wordmark. */
    {
        RECT wmrc;
        wmrc.left   = 24;
        wmrc.top    = h / 3;
        wmrc.right  = channel_left - 16;
        wmrc.bottom = wmrc.top + 96;
        if (wmrc.right > wmrc.left) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, AD_WORDMARK_RGB);
            old_font = (HFONT)SelectObject(hdc, ad_get_wordmark_font());
            DrawTextA(hdc, "Unicorn Desktop", -1, &wmrc,
                      DT_LEFT | DT_TOP | DT_NOPREFIX | DT_NOCLIP);
            SelectObject(hdc, old_font);
        }
    }

    /* Channel Bar. */
    g_ad_btn_count = 0;
    {
        RECT cb;
        cb.left   = channel_left;
        cb.top    = 0;
        cb.right  = w;
        cb.bottom = channel_bottom;

        SetBkMode(hdc, TRANSPARENT);
        if (g_hFontUI) old_font = (HFONT)SelectObject(hdc, g_hFontUI);

        if (!g_ad_channel || g_ad_channel->items_n <= 0) {
            const char *msg = g_ad_channel_failed
                              ? "Channel unavailable"
                              : "Loading channel...";
            SetTextColor(hdc, AD_CHANNELBAR_TITLE_RGB);
            DrawTextA(hdc, msg, -1, &cb,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            HPEN   penBorder = CreatePen(PS_SOLID, 1, AD_BTN_BORDER_RGB);
            HBRUSH brFill    = CreateSolidBrush(AD_BTN_FILL_RGB);
            HPEN   oldpen    = penBorder ? (HPEN)SelectObject(hdc, penBorder) : NULL;
            HBRUSH oldbr     = brFill   ? (HBRUSH)SelectObject(hdc, brFill)   : NULL;
            int    y         = cb.top + AD_CHANNEL_PAD;

            /* Channel title strip at top (read from the parsed
             * CdfDoc; defaults to "Channel" if absent). */
            {
                RECT titlerc;
                titlerc.left   = cb.left + AD_CHANNEL_PAD;
                titlerc.top    = y;
                titlerc.right  = cb.right - AD_CHANNEL_PAD;
                titlerc.bottom = y + 24;
                SetTextColor(hdc, AD_CHANNELBAR_TITLE_RGB);
                {
                    wchar_t *wtitle = ad_utf8_to_wide(g_ad_channel->title);
                    DrawTextW(hdc,
                              wtitle ? wtitle : L"Channel",
                              -1, &titlerc,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE
                              | DT_END_ELLIPSIS);
                    free(wtitle);
                }
                y = titlerc.bottom + AD_CHANNEL_PAD;
            }

            for (i = 0;
                 i < g_ad_channel->items_n && g_ad_btn_count < AD_MAX_BTNS;
                 i++) {
                CdfItem *it = &g_ad_channel->items[i];
                RECT     btn;
                RECT     icon;
                RECT     text_rc;
                btn.left   = cb.left + AD_CHANNEL_PAD;
                btn.top    = y;
                btn.right  = cb.right - AD_CHANNEL_PAD;
                btn.bottom = y + AD_CHANNEL_BTN_H;
                if (btn.bottom > cb.bottom - AD_CHANNEL_PAD) break;

                Rectangle(hdc, btn.left, btn.top, btn.right, btn.bottom);

                icon.left   = btn.left + AD_CHANNEL_PAD;
                icon.top    = btn.top + (AD_CHANNEL_BTN_H - AD_ICON_SIZE) / 2;
                icon.right  = icon.left + AD_ICON_SIZE;
                icon.bottom = icon.top  + AD_ICON_SIZE;
                {
                    int icon_res = ad_icon_res_for_title(it->title);
                    if (!icon_res
                        || !suite_logo_draw(hdc, icon_res,
                                            icon.left, icon.top,
                                            AD_ICON_SIZE)) {
                        /* No icon mapped for this title, or the PNG
                         * failed to decode/draw: fall back to the gray
                         * placeholder square (pre-icons behaviour). */
                        HBRUSH brIcon = CreateSolidBrush(AD_BTN_ICON_RGB);
                        if (brIcon) {
                            FillRect(hdc, &icon, brIcon);
                            DeleteObject(brIcon);
                        }
                    }
                }

                text_rc.left   = icon.right + AD_CHANNEL_PAD;
                text_rc.top    = btn.top;
                text_rc.right  = btn.right - AD_CHANNEL_PAD;
                text_rc.bottom = btn.bottom;
                SetTextColor(hdc, AD_BTN_TEXT_RGB);
                {
                    wchar_t *wtitle = ad_utf8_to_wide(it->title);
                    DrawTextW(hdc,
                              wtitle ? wtitle : L"(untitled)",
                              -1, &text_rc,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE
                              | DT_END_ELLIPSIS);
                    free(wtitle);
                }

                g_ad_btn_rects[g_ad_btn_count++] = btn;
                y += AD_CHANNEL_BTN_H + AD_CHANNEL_PAD;
            }

            if (oldpen) SelectObject(hdc, oldpen);
            if (oldbr)  SelectObject(hdc, oldbr);
            if (penBorder) DeleteObject(penBorder);
            if (brFill)    DeleteObject(brFill);
        }

        if (g_hFontUI && old_font) SelectObject(hdc, old_font);
    }

    /* Ticker band (Phase 6c-Client-2 geometry, Phase 6c-Client-3 polish:
     * clipping to the inset rect so long articles cannot leak past the
     * yellow border, vertical centering for content shorter than the
     * panel, and a hover-state darken derived from the doc body_bg via
     * darken_rgb. Paint order: panel bg pre-fill (hover-aware), measure
     * pass to compute the rendered content height, set a clip region
     * around the inset, render ticker.html into the inset (centered if
     * it fits), release the clip, then draw the yellow border last on
     * the outer rect so the frame is never overwritten. */
    g_ad_ticker_rect.left   = AD_TICKER_HMARGIN;
    g_ad_ticker_rect.right  = scene_main_right - AD_TICKER_HMARGIN;
    g_ad_ticker_rect.bottom = h - AD_TICKER_BOTTOM_MARGIN;
    g_ad_ticker_rect.top    = g_ad_ticker_rect.bottom - AD_TICKER_BAND_H;
    if (g_ad_ticker_rect.right > g_ad_ticker_rect.left
     && g_ad_ticker_rect.bottom > g_ad_ticker_rect.top) {
        /* Phase 6c-Client-5: source the panel bg live from the parsed
         * doc's body_bg so the Tune-8 Amber red flip (body_bg flips
         * to #FF0000 when Amber is pinned) follows the server without
         * a second constant. AD_TICKER_BG_RGB is the safety fallback
         * for the brief pre-fetch window before the first ticker doc
         * arrives. Hover darkens the same source via darken_rgb. */
        COLORREF doc_bg = g_ad_ticker_doc ? g_ad_ticker_doc->body_bg
                                          : AD_TICKER_BG_RGB;
        COLORREF bg_rgb = g_ad_ticker_hover ? darken_rgb(doc_bg, 20)
                                            : doc_bg;
        HBRUSH   brBg   = CreateSolidBrush(bg_rgb);
        if (brBg) {
            FillRect(hdc, &g_ad_ticker_rect, brBg);
            DeleteObject(brBg);
        }
        if (g_ad_ticker_doc) {
            POINT    old_org;
            RECT     band_local;
            RECT     inset = g_ad_ticker_rect;
            HRGN     hClip;
            HDC      mdc      = NULL;
            HBITMAP  mbmp     = NULL;
            HBITMAP  mbmp_old = NULL;
            int      vis_first_y = 0;
            int      vis_last_y  = 0;
            int      visible_h   = 0;
            int      have_visible = 0;
            int      dy = 0;
            int      ki;
            COLORREF saved_bg;
            /* Phase 6c-Client-5: inset past the border AND past
             * AD_TICKER_PAD_PX so 15 px of background sits between
             * the yellow frame and any rendered glyph on all four
             * sides. Client-4's visible-ink centering still operates
             * on the resulting (now smaller) band_local rect. */
            InflateRect(&inset,
                        -(AD_TICKER_BORDER_W + AD_TICKER_PAD_PX),
                        -(AD_TICKER_BORDER_W + AD_TICKER_PAD_PX));
            band_local.left   = 0;
            band_local.top    = 0;
            band_local.right  = inset.right  - inset.left;
            band_local.bottom = inset.bottom - inset.top;
            g_b_ad_view.view_doc = g_ad_ticker_doc;
            /* Temporarily override the doc's body_bg so web_paint_to's
             * built-in FillRect(client, body_bg) matches the panel's
             * current (possibly hovered) tint and we get a single
             * coherent surface instead of a darker outer ring around a
             * lighter content-area fill. Restored unconditionally. */
            saved_bg = g_ad_ticker_doc->body_bg;
            g_ad_ticker_doc->body_bg = bg_rgb;
            /* Phase 6c-Client-4: pre-paint to a discardable memory DC
             * to populate view->sel_map with the laid-out token rects
             * for THIS canvas width. We then walk sel_map to find the
             * actual VISIBLE-ink extent (first token y to last
             * token y+h) and center that, instead of centering the
             * full layout box reported by web_paint_to's measure mode.
             * The layout box includes a doc-structure lead-in
             * (leading empty <br>-driven line + table top_gap + cell
             * pad) that is larger than the trailing tail, so centering
             * the layout box leaves the visible ink positioned lower
             * than the visual centre. Engine source is untouched;
             * sel_map is an existing public WebRenderView field. */
            mdc = CreateCompatibleDC(hdc);
            if (mdc && band_local.right > 0 && band_local.bottom > 0) {
                mbmp = CreateCompatibleBitmap(hdc,
                                              band_local.right,
                                              band_local.bottom);
            }
            if (mdc && mbmp) {
                mbmp_old = (HBITMAP)SelectObject(mdc, mbmp);
                web_paint_to(&g_b_ad_view, mdc, &band_local, FALSE, NULL);
                for (ki = 0; ki < g_b_ad_view.sel_map_n; ki++) {
                    int ty = g_b_ad_view.sel_map[ki].y;
                    int tb = ty + g_b_ad_view.sel_map[ki].h;
                    if (!have_visible || ty < vis_first_y) vis_first_y = ty;
                    if (!have_visible || tb > vis_last_y)  vis_last_y  = tb;
                    have_visible = 1;
                }
                SelectObject(mdc, mbmp_old);
            }
            if (mbmp) DeleteObject(mbmp);
            if (mdc)  DeleteDC(mdc);
            if (have_visible) {
                visible_h = vis_last_y - vis_first_y;
                if (visible_h > 0 && visible_h < band_local.bottom) {
                    /* Map doc y = vis_first_y to device y =
                     * inset.top + half_whitespace, so the visible ink
                     * is symmetric inside the panel. */
                    dy = ((band_local.bottom - visible_h) / 2)
                         - vis_first_y;
                }
            }
            /* Clip to the inset so long articles are cut at the border
             * rather than spilling into the desktop scene below. */
            hClip = CreateRectRgn(inset.left, inset.top,
                                  inset.right, inset.bottom);
            SelectClipRgn(hdc, hClip);
            SetViewportOrgEx(hdc, inset.left, inset.top + dy, &old_org);
            web_paint_to(&g_b_ad_view, hdc, &band_local, FALSE, NULL);
            SetViewportOrgEx(hdc, old_org.x, old_org.y, NULL);
            SelectClipRgn(hdc, NULL);
            if (hClip) DeleteObject(hClip);
            g_ad_ticker_doc->body_bg = saved_bg;
        } else {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, AD_BAND_PLACEHOLDER_TEXT_RGB);
            if (g_hFontUI) old_font = (HFONT)SelectObject(hdc, g_hFontUI);
            DrawTextA(hdc,
                      g_ad_ticker_failed ? "Ticker unavailable"
                                         : "Loading ticker...",
                      -1, &g_ad_ticker_rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (g_hFontUI && old_font) SelectObject(hdc, old_font);
        }
        {
            HBRUSH brBorder = CreateSolidBrush(AD_TICKER_BORDER_RGB);
            if (brBorder) {
                RECT outer = g_ad_ticker_rect;
                RECT inner = g_ad_ticker_rect;
                int  k;
                FrameRect(hdc, &outer, brBorder);
                for (k = 1; k < AD_TICKER_BORDER_W; k++) {
                    InflateRect(&inner, -1, -1);
                    FrameRect(hdc, &inner, brBorder);
                }
                DeleteObject(brBorder);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Window procedure for the AD render child window.                    */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ad_wndproc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        /* Double-buffered to honor UI-07 (atomic repaint, no
         * intermediate flash). Same pattern as the Retro Web view's
         * WM_PAINT in src/web_module.c. */
        PAINTSTRUCT ps;
        HDC         hdc;
        HDC         mdc  = NULL;
        HBITMAP     mbmp = NULL, oldbmp = NULL;
        RECT        client;
        int         cw, ch;
        hdc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &client);
        cw = client.right - client.left;
        ch = client.bottom - client.top;
        if (cw > 0 && ch > 0) {
            mdc  = CreateCompatibleDC(hdc);
            mbmp = mdc ? CreateCompatibleBitmap(hdc, cw, ch) : NULL;
        }
        if (mdc && mbmp) {
            oldbmp = (HBITMAP)SelectObject(mdc, mbmp);
            ad_paint_scene(mdc, &client);
            BitBlt(hdc, 0, 0, cw, ch, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, oldbmp);
        } else {
            ad_paint_scene(hdc, &client);
        }
        if (mbmp) DeleteObject(mbmp);
        if (mdc)  DeleteDC(mdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT pt;
        int   i;
        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);

        /* Channel Bar hit test (delegates to the shared invocation
         * helper so the panel click below cannot diverge). */
        for (i = 0; i < g_ad_btn_count; i++) {
            if (PtInRect(&g_ad_btn_rects[i], pt)) {
                ad_invoke_channel_item(i);
                return 0;
            }
        }

        /* Phase 6c-Client-3 / Client-5: ticker panel is a single
         * click target. Three-way decision driven by the Tune-8
         * marker:
         *   url[0] != '\0'                     -> navigate to that
         *                                         per-rotation URL
         *                                         (news / weather /
         *                                          alert detail).
         *   marker seen && url empty           -> idle slot; no nav.
         *   marker not seen (legacy server)    -> CDF News Ticker
         *                                         entry fallback. */
        if (PtInRect(&g_ad_ticker_rect, pt)) {
            if (g_ad_ticker_click_url[0] != '\0') {
                suite_set_active_module(SUITE_ID_BTN_WEB);
                web_navigate_from_cdf(g_ad_ticker_click_url);
            } else if (!g_ad_ticker_click_marker_seen) {
                ad_invoke_channel_item(ad_find_news_ticker_index());
            }
            return 0;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt;
        int   in_ticker;
        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);
        in_ticker = PtInRect(&g_ad_ticker_rect, pt) ? 1 : 0;
        if (in_ticker && !g_ad_ticker_hover) {
            g_ad_ticker_hover = 1;
            if (!g_ad_ticker_tracking) {
                TRACKMOUSEEVENT tme;
                memset(&tme, 0, sizeof(tme));
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = hwnd;
                tme.dwHoverTime = HOVER_DEFAULT;
                if (TrackMouseEvent(&tme)) g_ad_ticker_tracking = 1;
            }
            InvalidateRect(hwnd, &g_ad_ticker_rect, FALSE);
        } else if (!in_ticker && g_ad_ticker_hover) {
            g_ad_ticker_hover = 0;
            InvalidateRect(hwnd, &g_ad_ticker_rect, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        g_ad_ticker_hover    = 0;
        g_ad_ticker_tracking = 0;
        InvalidateRect(hwnd, &g_ad_ticker_rect, FALSE);
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (PtInRect(&g_ad_ticker_rect, pt)) {
                /* Phase 6c-Client-5: hand cursor only when the panel
                 * is actually clickable: either a per-rotation URL is
                 * loaded, or no marker was seen so the legacy CDF
                 * News Ticker fallback applies. An empty URL with the
                 * marker present is an idle slot; leave the default
                 * arrow so the user reads the no-click affordance. */
                if (g_ad_ticker_click_url[0] != '\0'
                 || !g_ad_ticker_click_marker_seen) {
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
            }
        }
        return DefWindowProcA(hwnd, msg, w, l);
    }

    case WM_TIMER: {
        UINT id = (UINT)w;
        if (id == AD_TICKER_TIMER_ID) {
            ad_refresh_ticker(hwnd);
            return 0;
        }
        if (id == AD_CDF_TIMER_ID) {
            ad_refresh_channel(hwnd);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

static void ad_register_class(HINSTANCE hInst)
{
    WNDCLASSA wc;
    if (g_ad_class_registered) return;
    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ad_wndproc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   /* WM_ERASEBKGND returns 1 */
    wc.lpszClassName = AD_RENDER_CLASS;
    RegisterClassA(&wc);
    g_ad_class_registered = 1;
}

/* ------------------------------------------------------------------ */
/* Module entry points.                                                */
/* ------------------------------------------------------------------ */

void activedesktop_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    GetClientRect(content, &rc);

    if (g_ad_hwnd) {
        MoveWindow(g_ad_hwnd, 0, 0,
                   rc.right - rc.left,
                   rc.bottom - rc.top, TRUE);
        ShowWindow(g_ad_hwnd, SW_SHOW);
        SetTimer(g_ad_hwnd, AD_TICKER_TIMER_ID, AD_TICKER_INTERVAL_MS, NULL);
        SetTimer(g_ad_hwnd, AD_CDF_TIMER_ID,    AD_CDF_INTERVAL_MS,    NULL);
        SetFocus(g_ad_hwnd);
        return;
    }

    ad_register_class(hInst);
    g_ad_hwnd = CreateWindowExA(0, AD_RENDER_CLASS, "",
        WS_CHILD | WS_VISIBLE,
        0, 0,
        rc.right - rc.left,
        rc.bottom - rc.top,
        content, NULL, hInst, NULL);

    if (g_ad_first_activate) {
        g_ad_first_activate = 0;
        ad_refresh_channel(g_ad_hwnd);
        ad_refresh_ticker(g_ad_hwnd);
    }
    SetTimer(g_ad_hwnd, AD_TICKER_TIMER_ID, AD_TICKER_INTERVAL_MS, NULL);
    SetTimer(g_ad_hwnd, AD_CDF_TIMER_ID,    AD_CDF_INTERVAL_MS,    NULL);
    SetFocus(g_ad_hwnd);
}

void activedesktop_module_deactivate(HWND content)
{
    (void)content;
    if (g_ad_hwnd) {
        KillTimer(g_ad_hwnd, AD_TICKER_TIMER_ID);
        KillTimer(g_ad_hwnd, AD_CDF_TIMER_ID);
        ShowWindow(g_ad_hwnd, SW_HIDE);
    }
}

void activedesktop_module_resize(HWND content, int w, int h)
{
    (void)content;
    if (g_ad_hwnd) MoveWindow(g_ad_hwnd, 0, 0, w, h, TRUE);
}

BOOL activedesktop_module_on_command(HWND content,
                                     WPARAM wParam, LPARAM lParam)
{
    (void)content; (void)wParam; (void)lParam;
    return FALSE;
}

BOOL activedesktop_module_has_unsaved(void)
{
    return FALSE;
}
