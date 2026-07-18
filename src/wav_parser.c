/*
 * wav_parser.c - See wav_parser.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "wav_parser.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Little-endian field readers (RIFF is little-endian).                */
/* ------------------------------------------------------------------ */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]        | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fourcc_eq(const uint8_t *p, const char *cc)
{
    return p[0] == (uint8_t)cc[0] && p[1] == (uint8_t)cc[1]
        && p[2] == (uint8_t)cc[2] && p[3] == (uint8_t)cc[3];
}

/* ------------------------------------------------------------------ */
/* Parse.                                                              */
/* ------------------------------------------------------------------ */

int wav_parse_truespeech(const uint8_t *buf, size_t len,
                         wav_truespeech_info_t *out)
{
    size_t pos;
    int    have_fmt  = 0;
    int    have_data = 0;
    wav_truespeech_info_t info;

    if (!buf || !out || len < 12) return WAV_ERR_ARGS;

    memset(&info, 0, sizeof(info));

    /* "RIFF" <size> "WAVE" */
    if (!fourcc_eq(buf, "RIFF"))     return WAV_ERR_NOT_RIFF;
    if (!fourcc_eq(buf + 8, "WAVE")) return WAV_ERR_NOT_RIFF;

    /* Walk the chunk list starting just after "WAVE". Each chunk is a
     * 4-byte id + 4-byte little-endian size + payload, payload padded to
     * an even byte count. */
    pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = buf + pos;
        uint32_t       sz = le32(buf + pos + 4);
        size_t         payload = pos + 8;
        size_t         advance;

        /* A chunk claiming to extend past the buffer is corrupt. */
        if (payload + sz > len) return WAV_ERR_TRUNCATED;

        if (fourcc_eq(id, "fmt ")) {
            /* WAVEFORMATEX core is 16 bytes; TrueSpeech also carries the
             * 2-byte cbSize plus cbSize extra bytes. Need at least the
             * 16-byte core to read the tag and rate. */
            if (sz < 16) return WAV_ERR_TRUNCATED;
            info.format_tag        = le16(buf + payload + 0);
            info.channels          = le16(buf + payload + 2);
            info.sample_rate       = le32(buf + payload + 4);
            info.avg_bytes_per_sec = le32(buf + payload + 8);
            info.block_align       = le16(buf + payload + 12);
            info.bits_per_sample   = le16(buf + payload + 14);
            if (sz >= 18)
                info.cb_size = le16(buf + payload + 16);
            else
                info.cb_size = 0;
            /* Narrow scope: TrueSpeech only. Anything else is reported as
             * unsupported so the player hides the format rows. */
            if (info.format_tag != WAV_FORMAT_TRUESPEECH)
                return WAV_ERR_UNSUPPORTED_FORMAT;
            have_fmt = 1;
        } else if (fourcc_eq(id, "data")) {
            info.data_offset = (uint32_t)payload;
            info.data_length = sz;
            have_data = 1;
        }

        /* Advance past payload, honoring RIFF's even-byte padding. */
        advance = (size_t)sz + (sz & 1);
        if (payload + advance < payload) return WAV_ERR_TRUNCATED; /* overflow */
        pos = payload + advance;

        if (have_fmt && have_data) break;
    }

    if (!have_fmt)  return WAV_ERR_NO_FMT;
    if (!have_data) return WAV_ERR_NO_DATA;

    *out = info;
    return WAV_OK;
}
