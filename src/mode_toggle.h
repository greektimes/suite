/*
 * mode_toggle.h - iOS-style Modern / Retro toggle widget.
 *
 * Visual: 60 x 28 base (DPI-aware), 14 px corner radius (stadium track),
 * 24 px white thumb with 2 px inset. Modern (thumb left): #34C759.
 * Retro (thumb right): #C0C0C0. 150 ms ease-out animation.
 *
 * Click, Space, or Enter toggles. On state change the toggle posts
 * WM_USER_MODE_CHANGED to its parent with wParam = new mode
 * (0 = Modern, 1 = Retro), lParam = 0.
 */

#ifndef MODE_TOGGLE_H
#define MODE_TOGGLE_H

#include <windows.h>

#define MODE_MODERN  0
#define MODE_RETRO   1

#define WM_USER_MODE_CHANGED  (WM_USER + 0x40)

HWND mode_toggle_create(HWND parent, int x, int y, int id);
int  mode_toggle_get_mode(HWND hwnd);
void mode_toggle_set_mode(HWND hwnd, int mode, BOOL animate);

#endif /* MODE_TOGGLE_H */
