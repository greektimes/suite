/*
 * audio_format_detect.h - Human-readable audio format description from
 *                         the first bytes of a Sun .au or MPEG audio file.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Standalone, dependency-free header parser (no Windows headers, no audio
 * decoder) so it stays unit-testable in isolation. Used by the Retro
 * Audio Player dialog to show the "Audio Type:" line. Format is detected
 * from the magic bytes (".snd" for .au; "ID3" or an MPEG frame sync for
 * .mp3), NOT from any file extension.
 */

#ifndef AUDIO_FORMAT_DETECT_H
#define AUDIO_FORMAT_DETECT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the first `n` bytes of an audio blob and write a one-line,
 * human-readable description into `out` (NUL-terminated, never exceeding
 * out_len). Examples:
 *   "Sun Microsystems audio file (.au), u-law, 8 kHz, Mono"
 *   "MPEG-1 Layer III, 128 kbps, 44.1 kHz, Stereo"
 *
 * Returns true on a successful parse, false otherwise (unrecognized
 * format, header not yet fully available, corrupt/unsupported variant,
 * or invalid arguments). On false, `out` is left untouched and callers
 * should hide the row rather than show stale or "unknown" text. */
bool audio_format_describe(const uint8_t *first_bytes, size_t n,
                           char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_FORMAT_DETECT_H */
