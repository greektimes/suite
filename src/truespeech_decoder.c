/*
 * DSP Group TrueSpeech compatible decoder
 * Copyright (c) 2005 Konstantin Shishkov
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * ===========================================================================
 * PORT NOTE (Montreal Greek Times Unicorn Suite, 2026):
 *
 *   This file is a port of FFmpeg's libavcodec/truespeech.c. The decoding
 *   algorithm (TrueSpeech, reverse-engineered by Konstantin Shishkov) is
 *   unchanged. The FFmpeg framework dependencies have been stripped and
 *   replaced with self-contained equivalents so the decoder can stand
 *   alone inside the Suite:
 *
 *     - libavutil/channel_layout.h, mem_internal.h   -> removed
 *     - avcodec.h, codec_internal.h, decode.h        -> removed
 *     - bswapdsp.h (BswapDSPContext / bswap_buf)      -> inlined ts_bswap_buf()
 *     - get_bits.h (GetBitContext / get_bits*)        -> inlined TSBitReader
 *     - av_clip()                                     -> inlined ts_clip()
 *     - AVCodecContext-style decode_frame entry       -> truespeech_decode()
 *
 *   The TSContext struct, the per-quarter decode pipeline and every
 *   arithmetic step are preserved verbatim from upstream so the output is
 *   bit-identical to FFmpeg's. The codec's lookup tables live in the
 *   sibling port src/truespeech_data.h. The LGPL header above is retained
 *   as required by the license.
 * ===========================================================================
 */

#include "truespeech_decoder.h"

#include <string.h>

#include "truespeech_data.h"

/* ------------------------------------------------------------------ */
/* FFmpeg framework replacements (self-contained).                     */
/* ------------------------------------------------------------------ */

/* av_clip: clamp v into [lo, hi]. */
static int ts_clip(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* bswapdsp's bswap_buf: byte-swap each of `count` 32-bit words. The
 * TrueSpeech bitstream is read big-endian-per-dword; the original used
 * BswapDSPContext::bswap_buf((uint32_t*)dst, (uint32_t*)src, 8) to flip a
 * 32-byte frame into 8 byte-reversed dwords before the MSB-first bit
 * reader walks it. */
static void ts_bswap_buf(uint32_t *dst, const uint32_t *src, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        uint32_t v = src[i];
        dst[i] = ((v & 0x000000FFu) << 24) |
                 ((v & 0x0000FF00u) <<  8) |
                 ((v & 0x00FF0000u) >>  8) |
                 ((v & 0xFF000000u) >> 24);
    }
}

/* Minimal MSB-first bit reader, a drop-in for the handful of get_bits.h
 * entry points this decoder uses (get_bits up to 27 bits, get_bits1).
 * Bit 0 is the MSB of byte 0, matching FFmpeg's default bit order. The
 * frame is only 32 bytes so a simple bit-at-a-time accumulator is more
 * than fast enough. */
typedef struct TSBitReader {
    const uint8_t *buf;
    int            size_bits;
    int            pos;
} TSBitReader;

static void ts_init_get_bits(TSBitReader *br, const uint8_t *buf, int size_bits)
{
    br->buf       = buf;
    br->size_bits = size_bits;
    br->pos       = 0;
}

/* Read `n` (1..27) bits MSB-first, returning them right-aligned. Reads
 * past the end return zero bits (defensive; never happens for a valid
 * 32-byte frame). */
static unsigned ts_get_bits(TSBitReader *br, int n)
{
    unsigned val = 0;
    while (n-- > 0) {
        int bit = 0;
        if (br->pos < br->size_bits) {
            int byte_idx = br->pos >> 3;
            int bit_idx  = 7 - (br->pos & 7);
            bit = (br->buf[byte_idx] >> bit_idx) & 1;
        }
        val = (val << 1) | (unsigned)bit;
        br->pos++;
    }
    return val;
}

static unsigned ts_get_bits1(TSBitReader *br)
{
    return ts_get_bits(br, 1);
}

/* ------------------------------------------------------------------ */
/* TrueSpeech decoder context (verbatim from upstream TSContext, minus  */
/* the FFmpeg-specific BswapDSPContext member and DECLARE_ALIGNED).     */
/* ------------------------------------------------------------------ */

typedef struct TSContext {
    /* input data */
    uint8_t buffer[32];
    int16_t vector[8];  /* input vector: 5/5/4/4/4/3/3/3 */
    int offset1[2];     /* 8-bit value, used in one copying offset */
    int offset2[4];     /* 7-bit value, encodes offsets for copying and for two-point filter */
    int pulseoff[4];    /* 4-bit offset of pulse values block */
    int pulsepos[4];    /* 27-bit variable, encodes 7 pulse positions */
    int pulseval[4];    /* 7x2-bit pulse values */
    int flag;           /* 1-bit flag, shows how to choose filters */
    /* temporary data */
    int filtbuf[146];   /* some big vector used for storing filters */
    int prevfilt[8];    /* filter from previous frame */
    int16_t tmp1[8];    /* coefficients for adding to out */
    int16_t tmp2[8];    /* coefficients for adding to out */
    int16_t tmp3[8];    /* coefficients for adding to out */
    int16_t cvector[8]; /* correlated input vector */
    int filtval;        /* gain value for one function */
    int16_t newvec[60]; /* tmp vector */
    int16_t filters[32]; /* filters for every subframe */
} TSContext;

/* ------------------------------------------------------------------ */
/* Decode stages (verbatim port; av_clip -> ts_clip, get_bits* -> ts_*). */
/* ------------------------------------------------------------------ */

static void truespeech_read_frame(TSContext *dec, const uint8_t *input)
{
    TSBitReader gb;

    ts_bswap_buf((uint32_t *) dec->buffer, (const uint32_t *) input, 8);
    ts_init_get_bits(&gb, dec->buffer, 32 * 8);

    dec->vector[7] = ts_codebook[7][ts_get_bits(&gb, 3)];
    dec->vector[6] = ts_codebook[6][ts_get_bits(&gb, 3)];
    dec->vector[5] = ts_codebook[5][ts_get_bits(&gb, 3)];
    dec->vector[4] = ts_codebook[4][ts_get_bits(&gb, 4)];
    dec->vector[3] = ts_codebook[3][ts_get_bits(&gb, 4)];
    dec->vector[2] = ts_codebook[2][ts_get_bits(&gb, 4)];
    dec->vector[1] = ts_codebook[1][ts_get_bits(&gb, 5)];
    dec->vector[0] = ts_codebook[0][ts_get_bits(&gb, 5)];
    dec->flag      = ts_get_bits1(&gb);

    dec->offset1[0] = ts_get_bits(&gb, 4) << 4;
    dec->offset2[3] = ts_get_bits(&gb, 7);
    dec->offset2[2] = ts_get_bits(&gb, 7);
    dec->offset2[1] = ts_get_bits(&gb, 7);
    dec->offset2[0] = ts_get_bits(&gb, 7);

    dec->offset1[1]  = ts_get_bits(&gb, 4);
    dec->pulseval[1] = ts_get_bits(&gb, 14);
    dec->pulseval[0] = ts_get_bits(&gb, 14);

    dec->offset1[1] |= ts_get_bits(&gb, 4) << 4;
    dec->pulseval[3] = ts_get_bits(&gb, 14);
    dec->pulseval[2] = ts_get_bits(&gb, 14);

    dec->offset1[0] |= ts_get_bits1(&gb);
    dec->pulsepos[0] = ts_get_bits(&gb, 27);
    dec->pulseoff[0] = ts_get_bits(&gb, 4);

    dec->offset1[0] |= ts_get_bits1(&gb) << 1;
    dec->pulsepos[1] = ts_get_bits(&gb, 27);
    dec->pulseoff[1] = ts_get_bits(&gb, 4);

    dec->offset1[0] |= ts_get_bits1(&gb) << 2;
    dec->pulsepos[2] = ts_get_bits(&gb, 27);
    dec->pulseoff[2] = ts_get_bits(&gb, 4);

    dec->offset1[0] |= ts_get_bits1(&gb) << 3;
    dec->pulsepos[3] = ts_get_bits(&gb, 27);
    dec->pulseoff[3] = ts_get_bits(&gb, 4);
}

static void truespeech_correlate_filter(TSContext *dec)
{
    int16_t tmp[8];
    int i, j;

    for(i = 0; i < 8; i++){
        if(i > 0){
            memcpy(tmp, dec->cvector, i * sizeof(*tmp));
            for(j = 0; j < i; j++)
                dec->cvector[j] += (tmp[i - j - 1] * dec->vector[i] + 0x4000) >> 15;
        }
        dec->cvector[i] = (8 - dec->vector[i]) >> 3;
    }
    for(i = 0; i < 8; i++)
        dec->cvector[i] = (dec->cvector[i] * ts_decay_994_1000[i]) >> 15;

    dec->filtval = dec->vector[0];
}

static void truespeech_filters_merge(TSContext *dec)
{
    int i;

    if(!dec->flag){
        for(i = 0; i < 8; i++){
            dec->filters[i + 0] = dec->prevfilt[i];
            dec->filters[i + 8] = dec->prevfilt[i];
        }
    }else{
        for(i = 0; i < 8; i++){
            dec->filters[i + 0]=(dec->cvector[i] * 21846 + dec->prevfilt[i] * 10923 + 16384) >> 15;
            dec->filters[i + 8]=(dec->cvector[i] * 10923 + dec->prevfilt[i] * 21846 + 16384) >> 15;
        }
    }
    for(i = 0; i < 8; i++){
        dec->filters[i + 16] = dec->cvector[i];
        dec->filters[i + 24] = dec->cvector[i];
    }
}

static void truespeech_apply_twopoint_filter(TSContext *dec, int quart)
{
    int16_t tmp[146 + 60], *ptr0, *ptr1;
    const int16_t *filter;
    int i, t, off;

    t = dec->offset2[quart];
    if(t == 127){
        memset(dec->newvec, 0, 60 * sizeof(*dec->newvec));
        return;
    }
    for(i = 0; i < 146; i++)
        tmp[i] = dec->filtbuf[i];
    off = (t / 25) + dec->offset1[quart >> 1] + 18;
    off = ts_clip(off, 0, 145);
    ptr0 = tmp + 145 - off;
    ptr1 = tmp + 146;
    filter = ts_order2_coeffs + (t % 25) * 2;
    for(i = 0; i < 60; i++){
        t = (ptr0[0] * filter[0] + ptr0[1] * filter[1] + 0x2000) >> 14;
        ptr0++;
        dec->newvec[i] = t;
        ptr1[i] = t;
    }
}

static void truespeech_place_pulses(TSContext *dec, int16_t *out, int quart)
{
    int16_t tmp[7];
    int i, j, t;
    const int16_t *ptr1;
    int16_t *ptr2;
    int coef;

    memset(out, 0, 60 * sizeof(*out));
    for(i = 0; i < 7; i++) {
        t = dec->pulseval[quart] & 3;
        dec->pulseval[quart] >>= 2;
        tmp[6 - i] = ts_pulse_scales[dec->pulseoff[quart] * 4 + t];
    }

    coef = dec->pulsepos[quart] >> 15;
    ptr1 = ts_pulse_values + 30;
    ptr2 = tmp;
    for(i = 0, j = 3; (i < 30) && (j > 0); i++){
        t = *ptr1++;
        if(coef >= t)
            coef -= t;
        else{
            out[i] = *ptr2++;
            ptr1 += 30;
            j--;
        }
    }
    coef = dec->pulsepos[quart] & 0x7FFF;
    ptr1 = ts_pulse_values;
    for(i = 30, j = 4; (i < 60) && (j > 0); i++){
        t = *ptr1++;
        if(coef >= t)
            coef -= t;
        else{
            out[i] = *ptr2++;
            ptr1 += 30;
            j--;
        }
    }

}

static void truespeech_update_filters(TSContext *dec, int16_t *out, int quart)
{
    int i;
    (void)quart;

    memmove(dec->filtbuf, &dec->filtbuf[60], 86 * sizeof(*dec->filtbuf));
    for(i = 0; i < 60; i++){
        dec->filtbuf[i + 86] = out[i] + dec->newvec[i] - (dec->newvec[i] >> 3);
        out[i] += dec->newvec[i];
    }
}

static void truespeech_synth(TSContext *dec, int16_t *out, int quart)
{
    int i,k;
    int t[8];
    int16_t *ptr0, *ptr1;

    ptr0 = dec->tmp1;
    ptr1 = dec->filters + quart * 8;
    for(i = 0; i < 60; i++){
        int sum = 0;
        for(k = 0; k < 8; k++)
            sum += ptr0[k] * (unsigned)ptr1[k];
        sum = out[i] + ((int)(sum + 0x800U) >> 12);
        out[i] = ts_clip(sum, -0x7FFE, 0x7FFE);
        for(k = 7; k > 0; k--)
            ptr0[k] = ptr0[k - 1];
        ptr0[0] = out[i];
    }

    for(i = 0; i < 8; i++)
        t[i] = (ts_decay_35_64[i] * ptr1[i]) >> 15;

    ptr0 = dec->tmp2;
    for(i = 0; i < 60; i++){
        int sum = 0;
        for(k = 0; k < 8; k++)
            sum += ptr0[k] * t[k];
        for(k = 7; k > 0; k--)
            ptr0[k] = ptr0[k - 1];
        ptr0[0] = out[i];
        out[i] += (- sum) >> 12;
    }

    for(i = 0; i < 8; i++)
        t[i] = (ts_decay_3_4[i] * ptr1[i]) >> 15;

    ptr0 = dec->tmp3;
    for(i = 0; i < 60; i++){
        int sum = out[i] * (1 << 12);
        for(k = 0; k < 8; k++)
            sum += ptr0[k] * t[k];
        for(k = 7; k > 0; k--)
            ptr0[k] = ptr0[k - 1];
        ptr0[0] = ts_clip((sum + 0x800) >> 12, -0x7FFE, 0x7FFE);

        sum = ((ptr0[1] * (dec->filtval - (dec->filtval >> 2))) >> 4) + sum;
        sum = sum - (sum >> 3);
        out[i] = ts_clip((sum + 0x800) >> 12, -0x7FFE, 0x7FFE);
    }
}

static void truespeech_save_prevvec(TSContext *c)
{
    int i;

    for(i = 0; i < 8; i++)
        c->prevfilt[i] = c->cvector[i];
}

/* ------------------------------------------------------------------ */
/* Public entry. Replaces FFmpeg's truespeech_decode_frame, decoding    */
/* the whole payload in one pass with a fresh (zero) context so the      */
/* inter-frame filter state evolves across frames exactly as it does in  */
/* a continuous stream.                                                  */
/* ------------------------------------------------------------------ */

int truespeech_decode(const uint8_t *src, size_t src_len,
                      int16_t *dst, size_t dst_max_samples,
                      size_t *out_samples)
{
    TSContext c;
    const uint8_t *buf = src;
    size_t iterations;
    size_t j;
    int    i;
    int16_t *samples = dst;
    size_t  produced = 0;

    if (out_samples) *out_samples = 0;
    if (!src || !dst || src_len < TRUESPEECH_FRAME_BYTES) return 1;

    iterations = src_len / TRUESPEECH_FRAME_BYTES;
    /* Clamp to the caller's output capacity (240 samples per frame). */
    if (iterations * TRUESPEECH_FRAME_SAMPLES > dst_max_samples)
        iterations = dst_max_samples / TRUESPEECH_FRAME_SAMPLES;
    if (iterations == 0) return 1;

    memset(&c, 0, sizeof(c));

    for(j = 0; j < iterations; j++) {
        truespeech_read_frame(&c, buf);
        buf += 32;

        truespeech_correlate_filter(&c);
        truespeech_filters_merge(&c);

        for(i = 0; i < 4; i++) {
            truespeech_apply_twopoint_filter(&c, i);
            truespeech_place_pulses  (&c, samples, i);
            truespeech_update_filters(&c, samples, i);
            truespeech_synth         (&c, samples, i);
            samples += 60;
        }

        truespeech_save_prevvec(&c);
        produced += TRUESPEECH_FRAME_SAMPLES;
    }

    if (out_samples) *out_samples = produced;
    return 0;
}
