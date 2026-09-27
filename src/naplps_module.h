/*
 * naplps_module.h - NAPLPS (videotex) terminal tab.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The decoder and the raster this tab drives are naplps_decode.[ch] and
 * naplps_surface.[ch], which are clean-room from ANSI X3.110-1983 /
 * CSA T500-1983 and contain no Win32 code. This file is the session, the
 * transport and the paint.
 *
 * NAMING. Since 2026-09-27 the tab is called "Videotex" and carries a
 * Format dropdown, NAPLPS or Minitel (the Minitel terminal is
 * minitel_view.[ch]); the file keeps its naplps_ name so its history
 * stays readable. It was "NAPLPS (videotex)" before that. Alex and Alextel were
 * Bell Canada's service and terminal names for a service withdrawn in
 * 1994; they appear here and in the help prose only as historical context
 * and as the compatibility target, never as branding. The same decoder
 * serves an AT&T Sceptre or any other NAPLPS terminal, so naming the
 * feature for the format is also the more accurate choice.
 */

#ifndef NAPLPS_MODULE_H_INCLUDED
#define NAPLPS_MODULE_H_INCLUDED

#include "suite_shell.h"

/*
 * Defaults.
 *
 * THE HOSTNAME IS videotex.greektimes.ca, port 1983, from 2026-09-26.
 * The operator made videotex.greektimes.ca the umbrella name for every
 * videotex service on the host: NAPLPS on tcp/1983 now, Minitel later on
 * its own port. It replaces naplps.greektimes.ca (decided 2026-09-25),
 * which still resolves and will be retired later, so an older build keeps
 * working until then. Both replaced the alex.greektimes.ca the Phase 1
 * report proposed, a record since DELETED: naming the host for the FORMAT
 * rather than for Bell Canada's withdrawn service is safer and more
 * accurate, since the same decoder serves any NAPLPS terminal. Verified
 * 2026-09-26: videotex.greektimes.ca resolves to the same host as the
 * rest of the estate's per-protocol names.
 *
 * The service has been PUBLIC on port 1983 since 2026-09-26 (Unicorn
 * Phase 3). Opening a local .nap file needs no host at all.
 */
#define NAPLPS_DEFAULT_HOST      "videotex.greektimes.ca"
#define NAPLPS_DEFAULT_PORT      1983
#define NAPLPS_DEFAULT_PORT_STR  "1983"

/* Standard module entry points (see suite_shell.h). */
void naplps_module_activate(HWND content);
void naplps_module_deactivate(HWND content);
void naplps_module_resize(HWND content, int w, int h);
BOOL naplps_module_on_command(HWND content, WPARAM wParam, LPARAM lParam);
BOOL naplps_module_has_unsaved(void);

/* Close any live session. Called from the shell at shutdown. */
void naplps_module_shutdown(void);

#endif /* NAPLPS_MODULE_H_INCLUDED */
