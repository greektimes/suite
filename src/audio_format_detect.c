/*
 * audio_format_detect.c - See audio_format_detect.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "audio_format_detect.h"
#include "wav_parser.h"          /* RIFF WAV / TrueSpeech (tag 0x0022) */

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared helpers.                                                     */
/* ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* Format a sample rate in Hz as a compact kHz string:
 *   8000 -> "8 kHz", 44100 -> "44.1 kHz", 48000 -> "48 kHz",
 *   22050 -> "22.05 kHz". One or two decimals, trailing zeros dropped. */
static void fmt_khz(uint32_t hz, char *out, size_t n)
{
    if (hz % 1000 == 0)
        snprintf(out, n, "%lu kHz", (unsigned long)(hz / 1000));
    else if (hz % 100 == 0)
        snprintf(out, n, "%lu.%lu kHz",
                 (unsigned long)(hz / 1000), (unsigned long)((hz / 100) % 10));
    else
        snprintf(out, n, "%lu.%02lu kHz",
                 (unsigned long)(hz / 1000), (unsigned long)((hz / 10) % 100));
}

static void fmt_channels(uint32_t ch, char *out, size_t n)
{
    if (ch == 1)      snprintf(out, n, "Mono");
    else if (ch == 2) snprintf(out, n, "Stereo");
    else              snprintf(out, n, "%lu channels", (unsigned long)ch);
}

/* ------------------------------------------------------------------ */
/* Sun .au (.snd).                                                     */
/* ------------------------------------------------------------------ */

/* 24-byte header: magic(0) data_offset(4) data_size(8) encoding(12)
 * sample_rate(16) channels(20), all big-endian. */
static bool describe_au(const uint8_t *b, size_t n, char *out, size_t out_len)
{
    uint32_t encoding, rate, channels;
    const char *enc;
    char enc_buf[32], rate_buf[24], chan_buf[24];

    if (n < 24) return false;
    if (!(b[0] == 0x2E && b[1] == 0x73 && b[2] == 0x6E && b[3] == 0x64))
        return false;   /* not ".snd" */

    encoding = be32(b + 12);
    rate     = be32(b + 16);
    channels = be32(b + 20);

    switch (encoding) {
    /* mu-law / A-law are companded 8-bit; the bit depth is not in the
     * codec name, so spell it out as a second token. Linear PCM and float
     * variants already carry their bit depth in the name -- no duplicate. */
    case 1:  enc = "u-law, 8-bit";             break;
    case 2:  enc = "8-bit linear PCM";         break;
    case 3:  enc = "16-bit linear PCM";        break;
    case 4:  enc = "24-bit linear PCM";        break;
    case 5:  enc = "32-bit linear PCM";        break;
    case 6:  enc = "32-bit IEEE float";        break;
    case 7:  enc = "64-bit IEEE float";        break;
    case 27: enc = "A-law, 8-bit";             break;
    default:
        snprintf(enc_buf, sizeof(enc_buf), "encoding %lu",
                 (unsigned long)encoding);
        enc = enc_buf;
        break;
    }

    if (rate == 0 || channels == 0) return false;
    fmt_khz(rate, rate_buf, sizeof(rate_buf));
    fmt_channels(channels, chan_buf, sizeof(chan_buf));

    /* No "(.au)" suffix: the extension now shows in the Filename row. */
    snprintf(out, out_len,
             "Sun Microsystems audio file, %s, %s, %s",
             enc, rate_buf, chan_buf);
    return true;
}

/* ------------------------------------------------------------------ */
/* MPEG audio (.mp3).                                                  */
/* ------------------------------------------------------------------ */

/* Bitrate tables in kbps, indexed [bitrate_index] (0 = free, 15 = bad). */
static const int kBitrateV1L1[16] =
    {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0};
static const int kBitrateV1L2[16] =
    {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
static const int kBitrateV1L3[16] =
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const int kBitrateV2L1[16] =
    {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0};
static const int kBitrateV2L23[16] =
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};

/* Sample-rate tables [version][srate_index]; version 0=MPEG2.5,2=MPEG2,3=MPEG1. */
static const int kSampleRate[4][3] = {
    {11025, 12000, 8000},   /* 0: MPEG 2.5 */
    {0, 0, 0},              /* 1: reserved */
    {22050, 24000, 16000},  /* 2: MPEG 2   */
    {44100, 48000, 32000}   /* 3: MPEG 1   */
};

/* Try to parse a 4-byte MPEG frame header at b[0..3]. Writes the
 * description on success. */
static bool parse_mpeg_frame(const uint8_t *b, char *out, size_t out_len)
{
    int version, layer, bitrate_idx, srate_idx, chan_mode;
    int bitrate = 0, srate = 0;
    const char *ver_name, *layer_name, *chan_name;
    char rate_buf[24];

    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;   /* sync */

    version     = (b[1] >> 3) & 0x03;   /* 00=2.5 01=res 10=2 11=1 */
    layer       = (b[1] >> 1) & 0x03;   /* 00=res 01=III 10=II 11=I */
    bitrate_idx = (b[2] >> 4) & 0x0F;
    srate_idx   = (b[2] >> 2) & 0x03;
    chan_mode   = (b[3] >> 6) & 0x03;

    if (version == 1) return false;                 /* reserved */
    if (layer == 0)   return false;                 /* reserved */
    if (bitrate_idx == 0 || bitrate_idx == 15) return false; /* free/bad */
    if (srate_idx == 3) return false;               /* reserved */

    /* Bitrate: pick table by version (MPEG-1 vs MPEG-2/2.5) and layer. */
    if (version == 3) {            /* MPEG-1 */
        if (layer == 3)      bitrate = kBitrateV1L1[bitrate_idx];  /* Layer I   */
        else if (layer == 2) bitrate = kBitrateV1L2[bitrate_idx];  /* Layer II  */
        else                 bitrate = kBitrateV1L3[bitrate_idx];  /* Layer III */
    } else {                       /* MPEG-2 / 2.5 */
        if (layer == 3)      bitrate = kBitrateV2L1[bitrate_idx];  /* Layer I   */
        else                 bitrate = kBitrateV2L23[bitrate_idx]; /* Layer II/III */
    }
    if (bitrate <= 0) return false;

    srate = kSampleRate[version][srate_idx];
    if (srate <= 0) return false;

    ver_name   = (version == 3) ? "1" : (version == 2) ? "2" : "2.5";
    layer_name = (layer == 1) ? "III" : (layer == 2) ? "II" : "I";
    switch (chan_mode) {
    case 0:  chan_name = "Stereo";       break;
    case 1:  chan_name = "Joint Stereo"; break;
    case 2:  chan_name = "Dual Channel"; break;
    default: chan_name = "Mono";         break;
    }

    fmt_khz((uint32_t)srate, rate_buf, sizeof(rate_buf));
    snprintf(out, out_len, "MPEG-%s Layer %s, %d kbps, %s, %s",
             ver_name, layer_name, bitrate, rate_buf, chan_name);
    return true;
}

static bool describe_mp3(const uint8_t *b, size_t n, char *out, size_t out_len)
{
    size_t start = 0;
    size_t i;

    /* Skip an ID3v2 tag if present: "ID3" at 0, then a 4-byte syncsafe
     * (7-bit) size at offset 6; frame data follows the 10-byte header. */
    if (n >= 10 && b[0] == 0x49 && b[1] == 0x44 && b[2] == 0x33) {
        uint32_t tag = ((uint32_t)(b[6] & 0x7F) << 21)
                     | ((uint32_t)(b[7] & 0x7F) << 14)
                     | ((uint32_t)(b[8] & 0x7F) << 7)
                     |  (uint32_t)(b[9] & 0x7F);
        start = 10 + (size_t)tag;
    }

    /* Scan from `start` for the first valid MPEG frame header. Bound the
     * search so a huge ID3 tag or junk doesn't run past the buffer. */
    if (start + 4 > n) {
        /* The frame may sit before a (claimed) tag end we cannot see yet,
         * or there was no ID3 tag -- fall back to scanning from 0. */
        start = 0;
    }
    for (i = start; i + 4 <= n; i++) {
        if (b[i] != 0xFF) continue;
        if ((b[i + 1] & 0xE0) != 0xE0) continue;
        if (parse_mpeg_frame(b + i, out, out_len))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* RIFF WAV -- DSP Group TrueSpeech (wFormatTag 0x0022) only.          */
/* ------------------------------------------------------------------ */

/* The WAV path is deliberately narrow: only a RIFF/WAVE container whose
 * 'fmt ' chunk declares TrueSpeech (0x0022) is described. Any other
 * format tag (PCM, ADPCM, GSM, ...) makes wav_parse_truespeech return a
 * non-OK code and we report false, so the dialog hides the format rows,
 * consistent with the .au / .mp3 hide-on-parse-fail convention. */
static bool describe_truespeech(const uint8_t *b, size_t n,
                                char *out, size_t out_len)
{
    wav_truespeech_info_t info;
    char rate_buf[24], chan_buf[24];
    unsigned kbps;

    if (wav_parse_truespeech(b, n, &info) != WAV_OK)
        return false;

    /* kbps from nAvgBytesPerSec * 8 / 1000 (integer division: 1067 * 8 /
     * 1000 = 8, matching the codec's nominal 8 kbps). */
    kbps = (info.avg_bytes_per_sec * 8u) / 1000u;
    if (info.sample_rate == 0 || info.channels == 0) return false;
    fmt_khz(info.sample_rate, rate_buf, sizeof(rate_buf));
    fmt_channels(info.channels, chan_buf, sizeof(chan_buf));

    /* "DSP Group TrueSpeech" is split off as the File Type line; the rest
     * (after the first ", ") becomes the Encoding line. */
    snprintf(out, out_len, "DSP Group TrueSpeech, %u kbps, %s, %s",
             kbps, rate_buf, chan_buf);
    return true;
}

/* ------------------------------------------------------------------ */
/* Public entry.                                                       */
/* ------------------------------------------------------------------ */

bool audio_format_describe(const uint8_t *first_bytes, size_t n,
                           char *out, size_t out_len)
{
    if (!first_bytes || !out || out_len == 0 || n < 4) return false;

    /* AU magic ".snd" at offset 0. */
    if (first_bytes[0] == 0x2E && first_bytes[1] == 0x73 &&
        first_bytes[2] == 0x6E && first_bytes[3] == 0x64)
        return describe_au(first_bytes, n, out, out_len);

    /* RIFF WAV: "RIFF" at 0 and "WAVE" at 8. Only TrueSpeech (tag 0x0022)
     * is described; every other format tag is reported as unrecognized. */
    if (n >= 12 &&
        first_bytes[0] == 'R' && first_bytes[1] == 'I' &&
        first_bytes[2] == 'F' && first_bytes[3] == 'F' &&
        first_bytes[8] == 'W' && first_bytes[9] == 'A' &&
        first_bytes[10] == 'V' && first_bytes[11] == 'E')
        return describe_truespeech(first_bytes, n, out, out_len);

    /* MP3: ID3 tag, or a raw frame sync somewhere near the start. */
    if ((first_bytes[0] == 0x49 && first_bytes[1] == 0x44 &&
         first_bytes[2] == 0x33) ||
        (first_bytes[0] == 0xFF && (first_bytes[1] & 0xE0) == 0xE0))
        return describe_mp3(first_bytes, n, out, out_len);

    /* Some MP3s have leading junk before the first sync; scan a bit. */
    return describe_mp3(first_bytes, n, out, out_len);
}
