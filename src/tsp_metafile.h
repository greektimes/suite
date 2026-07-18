/*
 * tsp_metafile.h - Parser for the 1997-era TrueSpeech ".tsp" metafile.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * A .tsp metafile is a one-line text pointer to the actual TrueSpeech
 * .wav payload, in the form:
 *
 *     TSIP>>host/path/file.wav\n
 *
 * The "TSIP>>" prefix is uppercase and case-sensitive; the host carries
 * NO scheme prefix (the 1997 player implied a transport). Because libwww
 * in this Suite is HTTP-only, we resolve the pointer by prepending
 * "http://", so the example above resolves to
 *
 *     http://host/path/file.wav
 *
 * Standalone and dependency-free so it stays unit-testable in isolation.
 */

#ifndef TSP_METAFILE_H
#define TSP_METAFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the contents of a .tsp metafile (`data`/`len`). On success
 * returns a newly heap-allocated, NUL-terminated "http://..." URL that
 * the caller must free(); on failure (no TSIP>> prefix, empty URL
 * portion, or allocation failure) returns NULL. `len` is the number of
 * bytes in `data` (it need not be NUL-terminated). */
char *tsp_metafile_resolve(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* TSP_METAFILE_H */
