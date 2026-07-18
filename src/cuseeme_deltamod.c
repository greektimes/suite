/* cuseeme_deltamod.c - CU-SeeMe DELTAMOD (format 26) 2-bit delta decoder.
 * See cuseeme_deltamod.h for the clean-room derivation and the spec-silent
 * note (Audio-RBK gives the format id but not the step table; the model here
 * was validated against a live reflector capture). */
#include "cuseeme_deltamod.h"

/* Step table indexed by the 2-bit code (see header):
 *   0 -> +small, 1 -> +large, 2 -> -small, 3 -> -large.
 * Magnitudes set playback level/fidelity only (delta-mod is an integrator);
 * the small:large ratio of ~1:3 is the conventional 2-bit DM spread and gives
 * a clean envelope on the captured feed. */
static const int32_t k_step[4] = { +160, +494, -160, -494 };

#define CUSM_DM_LEAK_NUM  255   /* gentle DC leak: acc *= 255/256 each sample */
#define CUSM_DM_LEAK_DEN  256

void cusm_deltamod_init(cusm_deltamod_t *d) {
    d->acc = 0;
}

static int16_t clamp16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

int cusm_deltamod_decode(cusm_deltamod_t *d, const uint8_t *in, int in_len,
                         int16_t *out) {
    int n = 0, i;
    int32_t acc = d->acc;
    for (i = 0; i < in_len; i++) {
        uint8_t b = in[i];
        int s;
        for (s = 0; s < 4; s++) {
            int code = (b >> (6 - 2 * s)) & 0x3;   /* MS pair first */
            acc = (int32_t)((int64_t)acc * CUSM_DM_LEAK_NUM / CUSM_DM_LEAK_DEN);
            acc += k_step[code];
            out[n++] = clamp16(acc);
        }
    }
    d->acc = acc;
    return n;
}
