/*
 * suite_sha256.h - SHA-256 over a file, via Windows CNG.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The updater verifies a downloaded installer against the hash in the
 * update manifest, and that check is the entire security model of the
 * update path: the manifest and the MSI both come over TLS from
 * greektimes.ca, and the hash is what ties the bytes on disk to the
 * bytes the manifest vouched for.
 *
 * NOT the Zmodem CRCs. third_party/mbzm/crc16.c and crc32.c are 16- and
 * 32-bit error-detection checksums for framing, trivially forgeable,
 * and using one here would look like verification while providing none.
 *
 * CNG (bcrypt.dll) is in-box on every supported Windows, so this brings
 * in no vendored crypto and nothing to license: one more -lbcrypt on
 * the link line.
 */

#ifndef SUITE_SHA256_H
#define SUITE_SHA256_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lower-case hex, 64 characters plus the terminator. */
#define SUITE_SHA256_HEXLEN  65

/* Hash the file at `path`. On success writes SUITE_SHA256_HEXLEN bytes
 * of lower-case hex into `out_hex` and returns 0. Non-zero on any
 * failure, with `out_hex` set to the empty string. `err` may be NULL.
 *
 * Reads the file in chunks; a multi-megabyte installer is never held in
 * memory. Blocking: run it on a worker thread. */
int suite_sha256_file(const wchar_t *path, char *out_hex,
                      char *err, size_t errcap);

/* Constant-time-ish comparison of two hex digests, case-insensitive.
 * Returns non-zero when they match. The comparison does not stop at the
 * first differing character; a hash check is not a secret comparison,
 * but there is no reason to write the leaky version either. */
int suite_sha256_hex_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* SUITE_SHA256_H */
