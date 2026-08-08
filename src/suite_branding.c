/*
 * suite_branding.c - About dialog and version string.
 *
 * THE NAME SPELLINGS HERE ARE DELIBERATELY PLAIN ASCII. This is a
 * MessageBoxA, so the text is interpreted in the system ANSI code page,
 * and a character outside it arrives as a question mark or worse. Marcin
 * Woloszczuk's name carries an l-with-stroke in the licence text, which
 * CP1252 has no room for, so it is written unaccented here. CREDITS.md
 * and THIRD-PARTY-NOTICES.md are UTF-8 and carry it correctly; they are
 * the surfaces that spell the names in full.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "suite_shell.h"

void suite_about_show(HWND parent)
{
    MessageBoxA(parent,
        SUITE_WINDOW_TITLE "\r\n"
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
        "  libwww 5.4.1 (W3C Software License)\r\n"
        "\r\n"
        "NABU emulation compiled into this program:\r\n"
        "  Marduk, by S. V. Nickolas and Marcin Woloszczuk. MIT.\r\n"
        "  Its Z80 core by Nicolas Allemand, its TMS9918 core by\r\n"
        "  Troy Schrapel, its AY-3-8910 core by Mitsutaka Okazaki.\r\n"
        "  MGT IPL, this Suite's fork of OpenNabu IPL by\r\n"
        "  S. V. Nickolas. MIT. Changes listed in MGT-CHANGES.md.\r\n"
        "\r\n"
        "Font data used by the RIPscrip renderer:\r\n"
        "  Hershey Fonts, created by Dr. A. V. Hershey while working\r\n"
        "  at the U. S. National Bureau of Standards. Distribution\r\n"
        "  format originally created by James Hurt, Cognition Inc.\r\n"
        "  Public domain.\r\n"
        "  font8x8 by Daniel Hepper, based on the public-domain 8x8\r\n"
        "  VGA font by Marcel Sondaas. Public domain.\r\n"
        "\r\n"
        "RIPscrip is a trademark of TeleGrafix Communications, Inc.\r\n"
        "\r\n"
        "Dedicated to the loving memory of\r\n"
        "Basile Papadopoulos\r\n"
        "and\r\n"
        "Despina Kavalou-Papadopoulos\r\n",
        "About " SUITE_APP_TITLE,
        /* Plain MB_OK, no icon flag: MB_ICONINFORMATION/MB_ICONASTERISK
         * makes MessageBox play the system asterisk sound. Dropping the
         * icon flag silences the ding on About without any MessageBeep
         * suppression hack. */
        MB_OK);
}
