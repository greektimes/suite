/*
 * wav_parser.h - Minimal RIFF WAV chunk parser, narrowly scoped to
 *                DSP Group TrueSpeech (wFormatTag 0x0022) payloads.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * This parser deliberately understands ONE thing: a RIFF/WAVE container
 * whose 'fmt ' chunk declares format tag 0x0022 (TrueSpeech). It validates
 * the magic, reads the WAVEFORMATEX fields (including the cbSize extra
 * bytes that non-PCM formats carry), and locates the 'data' chunk. Any
 * other wFormatTag (PCM 0x0001, ADPCM, GSM, ...) is rejected with
 * WAV_ERR_UNSUPPORTED_FORMAT -- per the project decision this stays
 * TrueSpeech-only. Standalone and dependency-free (no Windows headers)
 * so it remains unit-testable in isolation.
 */

#ifndef WAV_PARSER_H
#define WAV_PARSER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The TrueSpeech format tag, the only one this parser accepts. */
#define WAV_FORMAT_TRUESPEECH  0x0022

/* Return codes. */
enum {
    WAV_OK = 0,
    WAV_ERR_ARGS,               /* NULL args / zero length */
    WAV_ERR_NOT_RIFF,           /* missing RIFF/WAVE magic */
    WAV_ERR_TRUNCATED,          /* a chunk runs past the buffer */
    WAV_ERR_NO_FMT,             /* no 'fmt ' chunk found */
    WAV_ERR_NO_DATA,            /* no 'data' chunk found */
    WAV_ERR_UNSUPPORTED_FORMAT  /* wFormatTag is not 0x0022 */
};

/* Parsed view of a TrueSpeech-bearing WAV. All multi-byte fmt fields are
 * little-endian on the wire; this struct holds them in host order. The
 * data_offset/data_length point at the raw codec bitstream (the 'data'
 * chunk payload) inside the original buffer. */
typedef struct {
    uint16_t format_tag;        /* always 0x0022 on a successful parse */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t cb_size;           /* count of extra fmt bytes (non-PCM) */
    uint32_t data_offset;       /* byte offset of the 'data' payload */
    uint32_t data_length;       /* length of the 'data' payload */
} wav_truespeech_info_t;

/* Parse the first `len` bytes of a RIFF WAV blob. On success returns
 * WAV_OK and fills *out; on any failure returns one of the WAV_ERR_*
 * codes and leaves *out unspecified. A non-WAV_OK return (including
 * WAV_ERR_UNSUPPORTED_FORMAT for non-TrueSpeech WAVs) means callers
 * should treat the file as unsupported and hide the format rows. */
int wav_parse_truespeech(const uint8_t *buf, size_t len,
                         wav_truespeech_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WAV_PARSER_H */
