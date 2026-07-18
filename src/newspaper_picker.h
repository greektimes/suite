/*
 * newspaper_picker.h - Back-issues overlay picker for the Newspaper tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Picker is a child window of the Newspaper module's content area,
 * sized to the same rect as the WebView2 host. When shown it sits
 * z-above the WebView2 (so right-click and clicks land here, not in
 * BookReader). When hidden, the WebView2 is fully interactive.
 *
 * Layout: scrollable 4-column grid of issue cells, each cell a
 * cover thumbnail (lazy-fetched via news_cover_thumb) plus issue id
 * and one-line title. Click a cell -> on_pick callback fires with
 * the chosen issue id; the picker auto-hides immediately. Close (X)
 * top-right or VK_ESCAPE dismisses without picking.
 */
#ifndef NEWSPAPER_PICKER_H
#define NEWSPAPER_PICKER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "newspaper_service.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NewspaperPicker NewspaperPicker;
typedef void (*NsPickerOnPick)(const char *issue_id, void *user);
typedef void (*NsPickerOnDismiss)(void *user);

NewspaperPicker *picker_create(HWND parent_module, NewspaperService *svc);
void             picker_destroy(NewspaperPicker *p);
HWND             picker_hwnd(NewspaperPicker *p);
void             picker_show(NewspaperPicker *p);
void             picker_hide(NewspaperPicker *p);
void             picker_set_on_pick(NewspaperPicker *p,
                                    NsPickerOnPick cb, void *user);
/* Fires from every dismiss path: close-button click, Esc key, and
 * cell-click (right BEFORE the on_pick callback so the underlying
 * WebView2 can be made visible again before the consumer navigates
 * it to the new issue). */
void             picker_set_on_dismiss(NewspaperPicker *p,
                                       NsPickerOnDismiss cb, void *user);

/* Picker reports how many cells it would render (for the smoke handback). */
int              picker_cell_count(NewspaperPicker *p);

#ifdef __cplusplus
}
#endif
#endif /* NEWSPAPER_PICKER_H */
