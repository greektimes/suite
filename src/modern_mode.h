/*
 * modern_mode.h - Modern-mode chrome: top tab strip + active-tab content area.
 *
 * Owns four tabs, left-to-right: 0 Website, 1 Newspaper, 2 Live Radio,
 * 3 Live TV (reordered 2026-06-16). Each tab carries a child HWND in the
 * content area below the strip; at create time all four are placeholders,
 * which the shell swaps for the real module HWNDs (keyed by the MM_TAB_*
 * constants, so module-to-tab binding is independent of tab position).
 *
 * The shell creates modern_mode once on first toggle into Modern Mode,
 * then ShowWindow/HideWindow's it as the toggle flips.
 */

#ifndef MODERN_MODE_H
#define MODERN_MODE_H

#include <windows.h>

#define MM_TAB_WEBSITE   0
#define MM_TAB_NEWSPAPER 1
#define MM_TAB_LIVERADIO 2
#define MM_TAB_LIVETV    3
#define MM_TAB_COUNT     4

HWND modern_mode_create(HWND parent, RECT content_rect);
void modern_mode_resize(HWND hwnd, RECT new_content_rect);
void modern_mode_set_active_tab(HWND hwnd, int tab_index);
int  modern_mode_get_active_tab(HWND hwnd);

/* Replace a tab's child HWND. modern_mode takes ownership: it sets the
 * new child as its own child, sizes it to the content area, and shows
 * or hides it based on the active tab. The previous child is destroyed.
 * Used in Phase F to swap tab 0's placeholder for the real livetv pane. */
void modern_mode_set_tab_child(HWND hwnd, int tab_index, HWND new_child);

#endif /* MODERN_MODE_H */
