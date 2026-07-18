/*
 * placeholder_module.h - Uniform branded splash for not-yet-built tabs.
 *
 * Single parameterized child window: white background, Greek Times icon
 * centered, tab name at 20 pt Segoe UI Semibold, "Coming Soon" at 14 pt
 * Segoe UI. Used for Live Radio, Newspaper, Website in v0.2.0 until
 * their real modules land.
 */

#ifndef PLACEHOLDER_MODULE_H
#define PLACEHOLDER_MODULE_H

#include <windows.h>

HWND placeholder_module_create(HWND parent, const wchar_t *tab_name);

#endif /* PLACEHOLDER_MODULE_H */
