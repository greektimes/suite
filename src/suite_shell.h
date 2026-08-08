/*
 * suite_shell.h - Public interface shared across the Suite's shell and modules.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef SUITE_SHELL_H
#define SUITE_SHELL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "suite_version.h"

/* The version is single-sourced in suite_version.h, which suite.rc and
 * the MSI build read too. This alias is kept because the title bar and
 * the About box have always spelled it this way. */
#define SUITE_VERSION_STRING SUITE_VERSION_DISPLAY

/* The display name. Both names and the composed window title live in
 * suite_version.h so the application and the MSI read the same source.
 * SUITE_APP_TITLE is the LONG name and is what message-box captions and
 * the About dialog use. */
#define SUITE_APP_TITLE      SUITE_APP_NAME_LONG

/* Module descriptor: one row per protocol module. See BLUEPRINT section 3.3.
 * on_resize and on_command are optional (may be NULL).
 *   on_resize:  called when the content panel changes size; modules with
 *               multiple controls (e.g. WAIS list + view pane) reflow here.
 *   on_command: called when a WM_COMMAND arrives at the content panel
 *               (from buttons, edits, listboxes inside the panel). Returns
 *               TRUE if the module handled the notification. */
typedef struct suite_module {
    const char *button_label;
    int         command_id;
    void      (*on_activate)(HWND content_panel);
    void      (*on_deactivate)(HWND content_panel);
    void      (*on_resize)(HWND content_panel, int w, int h);
    BOOL      (*on_command)(HWND content_panel, WPARAM wParam, LPARAM lParam);
    BOOL      (*has_unsaved_state)(void);
} suite_module_t;

/* Module command IDs (Phase 6a F1-F6 ordering). Exposed here so the
 * Active Desktop module (Phase 6b) can call suite_set_active_module
 * with the right id when a Channel Bar item or a ticker link click
 * needs to hand off to Retro Web (F2). */
#define SUITE_ID_BTN_BASE     100
#define SUITE_ID_BTN_WAIS     (SUITE_ID_BTN_BASE + 0)
#define SUITE_ID_BTN_ARPAMAIL (SUITE_ID_BTN_BASE + 1)
#define SUITE_ID_BTN_WEB      (SUITE_ID_BTN_BASE + 2)
#define SUITE_ID_BTN_DESKTOP  (SUITE_ID_BTN_BASE + 3)  /* retired 2026-08-01 */
#define SUITE_ID_BTN_RIPSCRIP (SUITE_ID_BTN_BASE + 9)
#define SUITE_ID_BTN_GOPHER   (SUITE_ID_BTN_BASE + 4)
#define SUITE_ID_BTN_TELNET   (SUITE_ID_BTN_BASE + 5)
#define SUITE_ID_BTN_CUSEEME  (SUITE_ID_BTN_BASE + 6)
#define SUITE_ID_BTN_IRC      (SUITE_ID_BTN_BASE + 7)
#define SUITE_ID_BTN_ARCHIE   (SUITE_ID_BTN_BASE + 8)
/* SUITE_ID_BTN_BASE + 10 was the external-player NABU tab, retired
 * 2026-08-07. The id is left unused rather than recycled, so an old
 * WM_COMMAND from anywhere cannot land on a different tab. */
#define SUITE_ID_BTN_NABUNAT  (SUITE_ID_BTN_BASE + 11)

/* Amendment 4: drag-resize gate. The shell sets a flag while the user
 * is in a continuous window-edge drag (WM_ENTERSIZEMOVE ..
 * WM_EXITSIZEMOVE) so MF-based modules can suppress per-frame
 * IMFMediaEngineEx::UpdateVideoStream calls that would stall the
 * swap chain. WM_APP_RESIZE_ENDED is broadcast to the active content
 * window on WM_EXITSIZEMOVE so the module can do one explicit
 * swap-chain rebuild at the stabilized size. */
#define WM_APP_RESIZE_ENDED  (WM_APP + 17)
BOOL suite_shell_in_user_resize(void);

/* Status line API exposed to modules. */
void suite_set_status(const char *text);

/* Compute centered (x, y) screen coordinates for a popup of size
 * (w, h) anchored on the Suite's top-level window. The hint HWND can
 * be any HWND owned by the Suite (a child control, the content
 * panel, or the main window itself); GetAncestor(GA_ROOT) climbs to
 * the top-level. If the lookup fails the result falls back to a
 * screen-centered position. Used by the audio status window and the
 * F3 image viewer so popups follow the Suite onto a secondary
 * monitor instead of leaping to the primary. */
void suite_center_popup(HWND hint_hwnd, int w, int h, int *out_x, int *out_y);

/* Phase 6b cross-module switch. Activates the module with the given
 * command id (one of SUITE_ID_BTN_*). No-op if the id is unknown or
 * if the module is already active. The standing unsaved-content
 * confirmation discipline in switch_to_module still applies. */
void suite_set_active_module(int command_id);

/* Shared GDI resources, owned by suite_fonts. */
extern HFONT  g_hFontUI;
extern HFONT  g_hFontOutput;
extern HBRUSH g_hBrushBlack;

void suite_fonts_init(void);
void suite_fonts_cleanup(void);

/* About dialog. */
void suite_about_show(HWND parent);

/* Help > Check for Updates. Implemented in suite_update.c. */
#include "suite_update.h"

/* Module entry points, implemented in <module>_module.c. */
void wais_module_activate(HWND content);
void wais_module_deactivate(HWND content);
void wais_module_resize(HWND content, int w, int h);
BOOL wais_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL wais_module_has_unsaved(void);

void arpamail_module_activate(HWND content);
void arpamail_module_deactivate(HWND content);
void arpamail_module_resize(HWND content, int w, int h);
BOOL arpamail_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL arpamail_module_has_unsaved(void);

void web_module_activate(HWND content);
void web_module_deactivate(HWND content);
void web_module_resize(HWND content, int w, int h);
BOOL web_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL web_module_has_unsaved(void);

/* Phase 6a: Active Desktop module skeleton (the Channel Bar / scene
 * / ticker arrive in Phase 6b per the Active Channel blueprint).
 *
 * RETIRED 2026-08-01: the Unicorn Desktop tab was withdrawn and its slot
 * given to the RIPscrip renderer. The translation unit still builds and
 * these entry points still exist, so restoring the tab is a one-row
 * change to g_modules in suite_shell.c; nothing else was removed. */
void activedesktop_module_activate(HWND content);
void activedesktop_module_deactivate(HWND content);
void activedesktop_module_resize(HWND content, int w, int h);
BOOL activedesktop_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL activedesktop_module_has_unsaved(void);

/* Phase 6a: Gopher and Telnet placeholder tabs. Real engines are
 * out of scope for v0.1.0-mvp; each is a future phase with its
 * own blueprint. */
void gopher_module_activate(HWND content);
void gopher_module_deactivate(HWND content);
void gopher_module_resize(HWND content, int w, int h);
BOOL gopher_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL gopher_module_has_unsaved(void);

void telnet_module_activate(HWND content);
void telnet_module_deactivate(HWND content);
void telnet_module_resize(HWND content, int w, int h);
BOOL telnet_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL telnet_module_has_unsaved(void);

/* F7: CU-SeeMe Live TV (receive-only CU-SeeMe client). */
void cuseeme_module_activate(HWND content);
void cuseeme_module_deactivate(HWND content);
void cuseeme_module_resize(HWND content, int w, int h);
BOOL cuseeme_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL cuseeme_module_has_unsaved(void);

/* F8: IRC (receive-only IRCv3 broadcast viewer for #retro). */
void irc_module_activate(HWND content);
void irc_module_deactivate(HWND content);
void irc_module_resize(HWND content, int w, int h);
BOOL irc_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL irc_module_has_unsaved(void);

/* RIPscrip 1.54 graphical terminal (native EGA renderer over the shared
 * telnet transport). Took the retired Unicorn Desktop tab slot. */
void ripscrip_module_activate(HWND content);
void ripscrip_module_deactivate(HWND content);
void ripscrip_module_resize(HWND content, int w, int h);
BOOL ripscrip_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL ripscrip_module_has_unsaved(void);

/* The external-player NABU tab, which launched the bundled NABU-only
 * emulator as a separate process, was retired on 2026-08-07 and its
 * prototypes removed with it. The in-Suite port below took over the name.
 * See docs/2026-08-07_NABU_CONNECTION_PANEL.md. */

/* NABU Native. The in-Suite port of Marduk: the machine runs on a worker
 * thread inside this process and its screen is drawn in the tab. Separate
 * from the NABU tab above, which launches the bundled emulator as its own
 * process and is what ships today; Phase 4 folds the two together. */
void nabu_native_module_activate(HWND content);
void nabu_native_module_deactivate(HWND content);
void nabu_native_module_resize(HWND content, int w, int h);
BOOL nabu_native_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL nabu_native_module_has_unsaved(void);

/* Archie Search (native Prospero/ARDP client, tab immediately after WAIS). */
void archie_module_activate(HWND content);
void archie_module_deactivate(HWND content);
void archie_module_resize(HWND content, int w, int h);
BOOL archie_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL archie_module_has_unsaved(void);

#endif /* SUITE_SHELL_H */
