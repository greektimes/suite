/*
 * suite_res.h - Numeric resource IDs used by suite.rc and the
 *               AddFontMemResourceEx callers in suite_fonts.c.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef SUITE_RES_H
#define SUITE_RES_H

/* Application icon -- referenced in suite.rc as `1 ICON ...`. The
 * numeric literal there stays for back-compat with the existing
 * RegisterClassExA wiring; this constant documents intent. */
#define IDR_ICON_APP                1

/* Embedded fonts (RCDATA). Loaded at startup via AddFontMemResourceEx
 * so they become process-private without touching the system font
 * directory. Used today by the F3 Gopher render area; the F6 Telnet
 * render area uses the OS Cascadia Mono (no embedded font needed). */
#define IDR_UBUNTU_MONO_REGULAR    100
#define IDR_UBUNTU_MONO_BOLD       101

/* Modern-mode logos (RCDATA): the original PNGs are embedded as-is
 * (alpha preserved) and decoded at runtime by suite_logo.c. The Live
 * Radio splash uses IDR_LOGO_RADIO; the Live TV splash uses
 * IDR_LOGO_TV. Source files keep their exact provided names (spaces
 * and all) in src/suite.rc. */
#define IDR_LOGO_RADIO             200
#define IDR_LOGO_TV                201

/* Channel-bar icons (RCDATA): the seven Unicorn Desktop (F1) channel
 * icons, embedded as 32x32 PNGs and decoded at runtime by suite_logo.c.
 * activedesktop_module.c maps each CDF channel item to one of these by
 * title and blits it into the 32x32 icon placeholder, falling back to
 * the gray placeholder square when a resource is missing or fails to
 * decode. Source PNGs live under icons/ at the repo root. */
#define IDR_ICON_NEWS              300
#define IDR_ICON_WEATHER           301
#define IDR_ICON_KAIROS            302
#define IDR_ICON_EISISEIS          303
#define IDR_ICON_HOMEPAGE          304
#define IDR_ICON_NEWSTICKER        305
#define IDR_ICON_LIVERADIO         306

#endif /* SUITE_RES_H */
