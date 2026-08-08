/*
 * nabu_native_module.h - the NABU running INSIDE the Suite window.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Phase 2 of the native port: video only. The machine boots and its
 * screen is drawn in the tab with GDI. No keyboard, no sound, no splash,
 * and the strip is deliberately the smallest thing that can start and
 * stop a machine, because the real tab is Phase 4.
 *
 * This is a SEPARATE tab from "NABU", which launches the bundled
 * emulator as its own process and is what ships today. Keeping them
 * apart is what lets the shipping tab stay untouched while this one is
 * built. Phase 4 folds them together.
 */

#ifndef NABU_NATIVE_MODULE_H
#define NABU_NATIVE_MODULE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard module entry points (see suite_shell.h). */
void nabu_native_module_activate(HWND content);
void nabu_native_module_deactivate(HWND content);
void nabu_native_module_resize(HWND content, int w, int h);
BOOL nabu_native_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL nabu_native_module_has_unsaved(void);

/* Stop the machine and join its thread. Called from the shell's shutdown
 * tail, so the Suite never exits with an emulation thread still running
 * inside it. */
void nabu_native_module_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NABU_NATIVE_MODULE_H */
