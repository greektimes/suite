/*
 * gopher_module.h - F3 Gopher client module (plain RFC 1436 over TCP).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The module follows the suite_shell.h activate/deactivate/resize
 * contract used by every other tab. Public symbols beyond that are
 * confined to the shutdown hook called from WinMain's exit path.
 */

#ifndef GOPHER_MODULE_H
#define GOPHER_MODULE_H

#include "suite_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once from WinMain after suite_fonts_init / audio_service_init.
 * No-op-safe on repeat calls. */
void gopher_module_init(void);

/* Called once from WinMain's exit path. Frees any cached resources
 * (history strings, in-flight worker handles, image-viewer windows). */
void gopher_module_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_MODULE_H */
