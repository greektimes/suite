/*
 * gwbasic_module.h - the GW-BASIC tab: Microsoft's interpreter, in the tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The machine is src/gwb_machine.c; this file is the tab around it: a
 * worker thread that runs it 18.2 ticks a second, a GDI renderer for the
 * CGA screen, the keyboard, and the folder in Documents that is its C:
 * drive. See docs/GWBASIC_PHASE_A_2026-10-20.md.
 */

#ifndef GWBASIC_MODULE_H
#define GWBASIC_MODULE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard module entry points (see suite_shell.h). */
void gwbasic_module_activate(HWND content);
void gwbasic_module_deactivate(HWND content);
void gwbasic_module_resize(HWND content, int w, int h);
BOOL gwbasic_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL gwbasic_module_has_unsaved(void);

/* Stop the machine and join its thread; called from the shell's
 * shutdown tail. */
void gwbasic_module_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GWBASIC_MODULE_H */
