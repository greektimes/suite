/* cuseeme_mulaw.c - G.711 mu-law expand.
 * Standard ITU-T G.711 mu-law (PCMU): bias 0x84, sign-magnitude
 * with 4-bit exponent (segment) and 4-bit mantissa, biased ones'
 * complement encoding on the wire.
 *
 * Reference: ITU-T G.711 (11/88) section 2 "PCM characteristics".
 * The expand operation is well-known and identical across every
 * reference implementation (the standard one-shot inverse of
 * the compress side). */
#include "cuseeme_mulaw.h"

int cusm_mulaw_decode(const uint8_t *src, int src_len, int16_t *dst) {
    int i;
    for (i = 0; i < src_len; i++) {
        /* G.711 mu-law expand */
        uint8_t mu = ~src[i];            /* invert all bits */
        int sign = (mu & 0x80) ? -1 : 1; /* sign bit */
        int exp  = (mu >> 4) & 0x07;     /* 3-bit exponent */
        int mant = mu & 0x0F;            /* 4-bit mantissa */
        int sample = ((mant << 1) | 0x21) << exp;
        sample -= 33;                    /* remove bias */
        dst[i] = (int16_t)(sign * sample);
    }
    return src_len;
}
