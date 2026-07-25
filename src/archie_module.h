/*
 * archie_module.h - Archie Search module interface.
 *
 * The WSArchie-style tab: search field, Archie server field, search-type
 * selector, three result panes (Hosts / Directories / Files) and the
 * selected-file detail fields. All protocol work is delegated to the
 * reusable ARDP transport in archie_ardp.[ch]; this module is UI only.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef ARCHIE_MODULE_H
#define ARCHIE_MODULE_H

#include "suite_shell.h"

/* Public entry points (archie_module_activate, _deactivate, _resize,
 * _on_command, _has_unsaved) are declared in suite_shell.h alongside the
 * other module descriptors. This header is reserved for future
 * Archie-module-specific declarations. */

#endif /* ARCHIE_MODULE_H */
