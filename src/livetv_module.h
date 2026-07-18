/*
 * livetv_module.h - Live TV tab content pane: HLS playback of greektv.
 *
 * Renders one of four visual states (SPLASH, LOADING, PLAYING, ERROR)
 * inside a single child HWND, drives the shared player_service to load
 * and play the live HLS stream, and exposes nothing beyond the create
 * function. The shell hands the resulting HWND to modern_mode_set_tab_child
 * for tab 0 during Phase F integration.
 */

#ifndef LIVETV_MODULE_H
#define LIVETV_MODULE_H

#include <windows.h>

HWND livetv_module_create(HWND parent);

#endif /* LIVETV_MODULE_H */
