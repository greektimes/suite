/*
 * irc_module.h - F8 Retro IRC viewer public entry points.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Receive-only broadcast viewer for the #retro channel on
 * irc.greektimes.ca. Mirrors telnet_module's lifecycle: init/shutdown
 * bracket the per-process allocations; activate/deactivate/resize/
 * on_command/has_unsaved are the suite_module_t callbacks (declared in
 * suite_shell.h). Reuses the vt_term core + suite_clipboard for an
 * amber-on-black terminal render surface; the wire lives in irc_client.
 */

#ifndef IRC_MODULE_H
#define IRC_MODULE_H

#include "suite_shell.h"

void irc_module_init(void);
void irc_module_shutdown(void);

#endif /* IRC_MODULE_H */
