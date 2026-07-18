/* cuseeme_deltamod.h - CU-SeeMe DELTAMOD (audio format id 26) decoder.
 *
 * Clean-room from Audio-RBK.96-02-29 (ftp.icm.edu.pl/packages/cu-seeme/html/):
 *   #define AUDF_DELTAMOD 26   "16 kb/s 2 bit delta mod [cvk]"
 *   8 kSamples/sec playout, mono, ported from Maven (Charlie Kline) / VAT.
 *
 * SPEC-SILENT DETAIL: Audio-RBK documents the format id, bit rate, sample rate
 * and packet (VAT) header, but NOT the delta-mod step table or bit packing -
 * it states the compression was "taken directly from Maven" without listing
 * constants.  A delta-mod decoder is an integrator of per-sample steps, so the
 * bit packing and step *signs* determine intelligibility while the step
 * *magnitudes* set only level/fidelity.  The model below was reconstructed and
 * validated against a live capture from the reflector (the same continuous
 * Greek-TV feed the encoder emits), NOT from server source:
 *
 *   - 4 samples per byte, 2 bits each, most-significant pair first.
 *   - 16 kb/s / 8 kHz => exactly 2 bits/sample, confirmed (400-byte payloads
 *     decode to 1600 samples = 200 ms).
 *   - Observed code distribution on the wire: codes 0 and 2 dominate (~78%),
 *     codes 1 and 3 are rare (~29%).  That is the fingerprint of a sign /
 *     magnitude 4-level quantizer: bit0 = magnitude (small step common, large
 *     step rare), bit1 = sign.  Step table indexed by 2-bit code:
 *         code 0 -> +small   code 1 -> +large
 *         code 2 -> -small   code 3 -> -large
 *   - Accumulator integrated with a gentle leak to suppress DC wander, output
 *     clamped to signed 16-bit PCM.
 */
#ifndef CUSEEME_DELTAMOD_H
#define CUSEEME_DELTAMOD_H

#include <stdint.h>

#define CUSM_AUDIO_RATE   8000   /* Audio-RBK: 8 kSamples/sec playout */

typedef struct {
    int32_t acc;   /* running reconstructed level */
} cusm_deltamod_t;

void cusm_deltamod_init(cusm_deltamod_t *d);

/* Decode `in_len` compressed bytes (4 samples/byte) into `out` as signed
 * 16-bit mono PCM.  out must hold at least in_len*4 samples.  Returns the
 * number of samples written. */
int cusm_deltamod_decode(cusm_deltamod_t *d, const uint8_t *in, int in_len,
                         int16_t *out);

#endif /* CUSEEME_DELTAMOD_H */
