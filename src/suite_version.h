/*
 * suite_version.h - THE version of the MGT Unicorn Suite. One place.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ------------------------------------------------------------------
 * TO RELEASE A NEW VERSION, EDIT THE THREE NUMBERS AND THE SUFFIX
 * BELOW AND NOTHING ELSE.
 * ------------------------------------------------------------------
 *
 * Before this header the version lived in four hand-edited places in
 * two files and in three different spellings, and by 2026-08-02 it had
 * drifted: the source said 0.3.1-beta while the repository tag said
 * 0.4.0-beta. Everything now derives from the three integers here, so
 * the spellings cannot disagree again.
 *
 * WHO CONSUMES THIS
 *
 *   src/suite.rc          FILEVERSION / PRODUCTVERSION (numeric) and
 *                         the FileVersion / ProductVersion strings.
 *                         windres runs the C preprocessor, and it
 *                         concatenates adjacent string literals, so the
 *                         composed macros below reach the resource as
 *                         one string. Verified 2026-08-02.
 *   src/suite_shell.h     SUITE_VERSION_STRING, which the title bar and
 *                         the About box already used.
 *   src/suite_update.c    the version the updater compares against the
 *                         manifest, parsed by the same semantic-version
 *                         parser that reads the manifest's string, so
 *                         our own version and theirs take identical
 *                         code paths.
 *   installer/build_msi.bat
 *                         reads SUITE_VERSION_MSI back out through the
 *                         C preprocessor (gcc -E) and hands it to WiX
 *                         as the MSI ProductVersion. Nothing about the
 *                         version is retyped in the WiX authoring.
 *
 * THE TWO FORMS, AND WHY BOTH EXIST
 *
 *   SUITE_VERSION_MSI  "0.4.0"       numeric triple, no suffix.
 *                                    Windows Installer's ProductVersion
 *                                    is strictly major.minor.build, the
 *                                    fields cap at 255.255.65535, and
 *                                    only the first three are compared
 *                                    for upgrade decisions. It cannot
 *                                    carry a pre-release tag.
 *   SUITE_VERSION_STR  "0.4.0-beta"  what a human, the update manifest
 *                                    and the version comparison use.
 *
 * A CONSEQUENCE WORTH KNOWING. Because MSI ignores the suffix, two
 * builds that differ only in their pre-release tag (0.4.0-beta and
 * 0.4.0) carry the SAME ProductVersion and Windows Installer will not
 * treat one as an upgrade of the other. Bump the patch number when a
 * pre-release becomes a release, or ship the release as 0.4.1.
 */

#ifndef SUITE_VERSION_H
#define SUITE_VERSION_H

/* ---- edit these, and only these ---------------------------------- */

#define SUITE_VERSION_MAJOR   0
#define SUITE_VERSION_MINOR   5
#define SUITE_VERSION_PATCH   0

/* Pre-release suffix, including its leading hyphen. Set to "" for a
 * final release. Ordering rule, which the updater's comparator
 * implements: a pre-release sorts BEFORE the release it belongs to, so
 * 0.4.0-beta is older than 0.4.0. */
#define SUITE_VERSION_SUFFIX  "-beta"

/* ---- everything below is derived; do not edit --------------------- */

#define SUITE_VER_STR2_(x)  #x
#define SUITE_VER_STR_(x)   SUITE_VER_STR2_(x)

/* "0.4.0". Numeric triple for the MSI ProductVersion. */
#define SUITE_VERSION_MSI \
    SUITE_VER_STR_(SUITE_VERSION_MAJOR) "." \
    SUITE_VER_STR_(SUITE_VERSION_MINOR) "." \
    SUITE_VER_STR_(SUITE_VERSION_PATCH)

/* "0.4.0-beta". The canonical human and manifest form. */
#define SUITE_VERSION_STR   SUITE_VERSION_MSI SUITE_VERSION_SUFFIX

/* "v0.4.0-beta". The same thing with the leading v, kept for anywhere
 * that still wants it. The title bar and the About box do NOT: they show
 * the bare version after the name. */
#define SUITE_VERSION_DISPLAY  "v" SUITE_VERSION_STR

/* ------------------------------------------------------------------
 * THE TWO NAMES.
 * ------------------------------------------------------------------
 *
 * They live here, next to the version, for the same reason the version
 * lives here: so nothing can retype one of them and drift.
 *
 *   LONG   the full name. The window title, the About box, and every
 *          screen of the setup wizard.
 *   SHORT  what a reader looks for. Both shortcuts, and the entry in
 *          Apps and features.
 *
 * The MSI reads both of these back out through the C preprocessor in
 * installer\build_msi.bat, exactly as it does the version, so the
 * installer and the application cannot disagree about what the product
 * is called.
 *
 * WHY THE SHORT NAME IS THE MSI ProductName. Windows Installer has no
 * ARPDISPLAYNAME property; the Microsoft property reference lists every
 * ARP property there is and DisplayName is not among them. The name in
 * Apps and features is always ProductName. So ProductName is the SHORT
 * name, and the wizard is made to show the LONG name by overriding the
 * WixUI strings, every one of which is declared Overridable="yes" in
 * WiX's own WixUI_en-us.wxl. No registry is written behind Windows
 * Installer's back. See installer\MGTUnicornSuite.wxl.
 */
#define SUITE_APP_NAME_LONG   "The Montreal Greek Times Unicorn Suite"
#define SUITE_APP_NAME_SHORT  "Greek Times"

/* Exactly what the title bar shows, in both Retro and Modern mode.
 * There is no mode word: the mode is visible on the toggle itself, and
 * a title that rewrites itself made the version disappear the moment
 * the first mode change happened. */
#define SUITE_WINDOW_TITLE    SUITE_APP_NAME_LONG " " SUITE_VERSION_STR

#endif /* SUITE_VERSION_H */
