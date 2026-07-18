/* cuseeme_idvi.c - CU-SeeMe IDVI (Intel DVI / IMA ADPCM, format id 30) decoder.
 * See cuseeme_idvi.h for the derivation and the Cornell-exact conventions
 * (per-packet reseed, big-endian seed predictor, HIGH-nibble-first,
 * reconstruction delta = (step*mag)>>2 + (step>>3)). Clean-room from the
 * operator root-cause / investigation docs; cu-seeme.exe was used as a
 * read-only wire-fact reference only (no code copied). */
#include "cuseeme_idvi.h"

/* Standard IMA/DVI ADPCM step-size table (89 entries, 7..32767). */
static const int32_t k_step[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/* Standard IMA/DVI index-adjustment table (indexed by the full 4-bit code). */
static const int k_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int cusm_idvi_decode(const uint8_t *in, int in_len, int16_t *out) {
    int32_t pred;
    int index, n = 0, i;

    if (in_len < 4) return 0;

    /* Seed header: predictor is 16-bit BIG-ENDIAN (byte-swapped vs IMA-WAV),
     * then the initial step index. The seed predictor is not emitted. */
    pred  = (int16_t)((in[0] << 8) | in[1]);   /* sign-extend the 16-bit BE value */
    index = in[2];
    if (index < 0)  index = 0;
    if (index > 88) index = 88;

    for (i = 4; i < in_len; i++) {
        uint8_t b = in[i];
        int half;
        for (half = 0; half < 2; half++) {
            /* HIGH nibble carries the earlier sample. */
            int nib   = (half == 0) ? ((b >> 4) & 0x0F) : (b & 0x0F);
            int32_t step = k_step[index];
            int mag   = nib & 0x07;
            int32_t delta = ((step * mag) >> 2) + (step >> 3);
            if (nib & 0x08) pred -= delta; else pred += delta;
            pred = clamp16(pred);
            out[n++] = (int16_t)pred;
            index += k_index[nib];
            if (index < 0)  index = 0;
            if (index > 88) index = 88;
        }
    }
    return n;
}
