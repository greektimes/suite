/*
 * suite_branding.c - About dialog and version string.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "suite_shell.h"

void suite_about_show(HWND parent)
{
    MessageBoxA(parent,
        SUITE_APP_TITLE " " SUITE_VERSION_STRING "\r\n"
        "\r\n"
        "Copyright (c) 2026 Dimitri Papadopoulos and\r\n"
        "The Montreal Greek Times.\r\n"
        "GNU Affero General Public License v3.0 or later.\r\n"
        "See COPYING for the full license text.\r\n"
        "\r\n"
        "The Montreal Greek Times\r\n"
        "www.greektimes.ca\r\n"
        "\r\n"
        "Embedded protocol code (full credits in THIRD-PARTY-NOTICES.md):\r\n"
        "  freeWAIS-sf 2.2.14 (CNIDR 1993)\r\n"
        "  libwww 5.4.1 (W3C Software License) [Phase 4+]\r\n",
        "About " SUITE_APP_TITLE,
        /* Plain MB_OK, no icon flag: MB_ICONINFORMATION/MB_ICONASTERISK
         * makes MessageBox play the system asterisk sound. Dropping the
         * icon flag silences the ding on About without any MessageBeep
         * suppression hack. */
        MB_OK);
}
