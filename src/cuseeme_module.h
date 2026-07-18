/* cuseeme_module.h - F7 "CU-SeeMe Live TV" Retro-mode tab.
 *
 * Receive-only CU-SeeMe client: connects to the live reflector on tab
 * activation, renders native 160x120 16-gray video centered in the tab,
 * plays DELTAMOD (format 26) audio via waveOut, and tears the UDP socket
 * down on deactivation. Mirrors the other module entry points in
 * suite_shell.h (telnet/gopher/wais).
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 */
#ifndef CUSEEME_MODULE_H
#define CUSEEME_MODULE_H

#include <windows.h>

/* The public reflector. Resolved via DNS at connect time (never hardcode IP). */
#define CUSEEME_HOST  "cu-seeme.greektv.ca"
#define CUSEEME_PORT  7648

void cuseeme_module_init(void);       /* once at startup */
void cuseeme_module_shutdown(void);   /* once at app exit; clean teardown */

void cuseeme_module_activate(HWND content);
void cuseeme_module_deactivate(HWND content);
void cuseeme_module_resize(HWND content, int w, int h);
BOOL cuseeme_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL cuseeme_module_has_unsaved(void);

#endif /* CUSEEME_MODULE_H */
