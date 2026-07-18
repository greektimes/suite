/*
 * web_module_cdf.h - Active Channel (CDF) parser + dialog.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp,
 * Phase 6. Locked CDF subset: CHANNEL, ITEM, TITLE, ABSTRACT,
 * LOGO HREF, and HREF attributes on CHANNEL/ITEM. SCHEDULE is
 * tolerated and skipped. Everything else is silently ignored.
 * No third-party CDF or XML code; small custom scanner.
 *
 * The fetch lives in src/web_module.c (existing libwww
 * WWW_SOURCE / HTLoadToChunk path used for image bytes). This
 * module only owns: the parser, the parsed in-memory model,
 * the modeless Active Channel dialog, and the item-click route
 * back into web_navigate (NAV-13 cursor discipline preserved).
 */

#ifndef WEB_MODULE_CDF_H
#define WEB_MODULE_CDF_H

#include <windows.h>

typedef struct CdfItem {
    char *title;
    char *abstract;
    char *href;     /* resolved against the CDF document's base URL */
} CdfItem;

typedef struct CdfDoc {
    char    *title;            /* channel title */
    char    *abstract;         /* channel abstract */
    char    *logo_href;        /* resolved */
    char    *channel_href;     /* CHANNEL HREF attr, resolved */
    char    *logo_bytes;       /* raw image bytes (worker-fetched) */
    int      logo_size;
    CdfItem *items;
    int      items_n;
    int      items_cap;
} CdfDoc;

/* Parse `bytes`/`size` of CDF source into a fresh CdfDoc; returns
 * NULL on out-of-memory or completely empty input. Hrefs are
 * resolved against `base_url` (the URL the CDF was fetched from)
 * so relative item HREFs work. The returned doc owns all heap
 * allocations; release with cdf_doc_free. */
CdfDoc *cdf_parse(const char *bytes, int size, const char *base_url);

void    cdf_doc_free(CdfDoc *doc);

/* Show (or replace) the Active Channel dialog. Takes ownership of
 * `doc`. If a previous dialog exists, it is closed first. The
 * dialog frees `doc` on WM_DESTROY. `content_hwnd` is the Retro
 * Web module's content HWND used to route item-click navigations
 * through the normal web_navigate path. Returns the dialog HWND
 * (NULL on failure; `doc` is then freed by this function). */
HWND    cdf_show_dialog(HWND main_hwnd, HWND content_hwnd, CdfDoc *doc);

/* Module-switch teardown discipline (hide/show, do not destroy). */
void    cdf_dialog_hide(void);
void    cdf_dialog_show(void);

/* App-shutdown / forced close: destroys the dialog if open. */
void    cdf_dialog_close(void);

/* Implemented in web_module.c. Routes an Active Channel item
 * click through web_navigate (push_history=1) so the user can
 * Back out of the navigation. The dialog itself stays open. */
void    web_navigate_from_cdf(const char *href);

#endif /* WEB_MODULE_CDF_H */
