/*
 * suite_update.h - Help > Update: check, download, verify.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ------------------------------------------------------------------
 * THE UPDATE MANIFEST. This is the operator-facing contract.
 * ------------------------------------------------------------------
 *
 *   URL:  https://apps.greektimes.ca/unicorn-suite/manifest.json
 *
 *   {
 *     "version":       "0.4.1-beta",
 *     "msi_url":       "https://apps.greektimes.ca/unicorn-suite/MGT_Unicorn_Suite_0.4.1.msi",
 *     "msi_sha256":    "3f6a...64 lower-case hex characters...c1",
 *     "msi_bytes":     5012345,
 *     "notes":         "What changed. May be several lines.",
 *     "min_supported": "0.0.0"
 *   }
 *
 *   version        REQUIRED. major.minor.patch with an optional
 *                  pre-release suffix, exactly the form
 *                  SUITE_VERSION_STR carries. Compared against the
 *                  running app to decide whether anything is offered.
 *   msi_url        REQUIRED. Absolute https URL whose host MUST be
 *                  apps.greektimes.ca exactly. Anything else is refused
 *                  before a byte is fetched.
 *   msi_sha256     REQUIRED. 64 hex characters, either case. The
 *                  downloaded file must hash to exactly this or it is
 *                  deleted and never run.
 *   msi_bytes      REQUIRED. Size in bytes, shown to the user. A
 *                  manifest advertising 0 bytes is treated as "nothing
 *                  published" and is never downloaded.
 *   notes          Optional. Plain text release notes, shown in the
 *                  scrollable box on the update dialog. JSON escapes
 *                  line breaks; the dialog converts them for the edit
 *                  control, which needs CRLF.
 *   min_supported  Optional. The oldest version that may still be
 *                  carried forward. When the running version is BELOW
 *                  it, the update is presented as required and the Skip
 *                  button is withheld. "0.0.0" means no floor.
 *
 * Unknown keys are ignored, so the schema can grow.
 *
 * THE PLACEHOLDER. Until a release is published the endpoint answers
 * with a well-formed manifest carrying version "0.0.0-placeholder", a
 * zeroed hash and msi_bytes 0. Nothing downloads: that version compares
 * older than anything shipped, and the zero size and zero hash are
 * checked independently of the version so neither alone can be trusted.
 *
 * ------------------------------------------------------------------
 * WHAT SECURES THIS
 * ------------------------------------------------------------------
 *
 * Three things, in order:
 *   1. both fetches are HTTPS, enforced in http_fetch.c, so there is no
 *      plain-http path to downgrade to, and the system certificate
 *      store authenticates the server;
 *   2. msi_url is restricted to apps.greektimes.ca, matched exactly, so
 *      a manifest that was somehow tampered with still cannot point the
 *      download at another machine;
 *   3. the downloaded file is hashed with SHA-256 and must equal the
 *      manifest's value. A mismatch deletes the file and stops. This
 *      check is not optional and there is no way to skip it.
 *
 * That is HTTPS plus a published hash, which is the standard guarantee.
 * There is deliberately no bespoke challenge-response and no hand-rolled
 * crypto here: inventing one would be strictly worse than the transport
 * security the platform already provides.
 *
 * THE NEXT LAYER, out of scope for now: Authenticode signing the MSI and
 * the exe. That would let Windows itself vouch for the publisher before
 * anything runs, and would survive a compromise of the update host,
 * which the hash alone does not. It needs a code-signing certificate.
 *
 * ------------------------------------------------------------------
 * SETTINGS
 * ------------------------------------------------------------------
 *
 * The MSI writes the installed version to
 *   HKLM\SOFTWARE\The Montreal Greek Times\MGT Unicorn Suite
 * which is READ-ONLY to the running app, because a per-machine install
 * means a standard user cannot write under HKLM.
 *
 * The app's own mutable settings therefore live in the matching
 *   HKCU\Software\The Montreal Greek Times\MGT Unicorn Suite
 * which this module creates on first use. It is the Suite's first
 * settings store of any kind. Values:
 *   startup_check_enabled  REG_DWORD, 1 or 0. Absent means 1: a fresh
 *                          profile checks for updates without anyone
 *                          having to opt in.
 *   last_checked           REG_SZ, "YYYY-MM-DDTHH:MM:SSZ" (UTC). Drives
 *                          the 24 hour throttle on the startup check.
 *   skipped_version        REG_SZ. Set by Skip This Version. Suppresses
 *                          the STARTUP prompt for that version and any
 *                          older one; a manual check ignores it.
 */

#ifndef SUITE_UPDATE_H
#define SUITE_UPDATE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Posted to the main window by the update worker threads. Handled by
 * suite_update_on_message, which the shell's window procedure calls. */
#define WM_APP_UPDATE_STATUS  (WM_APP + 30)  /* LPARAM = char *, freed by the handler */
#define WM_APP_UPDATE_RESULT  (WM_APP + 31)  /* the manifest check finished */
#define WM_APP_UPDATE_DLDONE  (WM_APP + 32)  /* the download and hash finished */

/* Help > Check for Updates. Starts the check on a worker thread and
 * returns at once; the UI thread is never blocked on the network. Safe
 * to call again while one is running (it says so and does nothing).
 *
 * A MANUAL check always reports something, always shows a version the
 * user previously skipped, and ignores the 24 hour throttle. The user
 * asked. */
void suite_update_check(HWND main_hwnd);

/* The automatic check, called once shortly after the window is up.
 * Silent unless there is genuinely something to offer, and it does
 * nothing at all when the user has turned startup checks off or when
 * one has already run inside the throttle window. Never blocks. */
void suite_update_startup_check(HWND main_hwnd);

/* The startup-check setting, exposed so anything else that wants to
 * surface it can. The checkbox on the update dialog is the user-facing
 * control today. */
int  suite_update_startup_enabled(void);
void suite_update_set_startup_enabled(int on);

/* Called from the shell's window procedure. Returns TRUE if the message
 * belonged to the updater and was handled. */
BOOL suite_update_on_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* Join any worker still running, at shutdown. */
void suite_update_shutdown(void);

/* Semantic version comparison, exposed because it is the part most
 * worth testing on its own.
 *
 * Returns <0 if a is older than b, 0 if they are the same, >0 if a is
 * newer. Accepts "1.2.3" and "1.2.3-beta" and "1.2.3-rc.2"; a leading
 * "v" is tolerated. Per semver, a pre-release is OLDER than the release
 * it belongs to, so 0.4.0-beta < 0.4.0. Missing numeric fields read as
 * zero, so "0.4" equals "0.4.0". */
int suite_update_compare_versions(const char *a, const char *b);

/* The manifest URL, so the report and the release runbook cannot drift
 * from the code. */
const char *suite_update_manifest_url(void);

#ifdef __cplusplus
}
#endif

#endif /* SUITE_UPDATE_H */
