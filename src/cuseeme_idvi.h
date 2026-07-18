/* cuseeme_idvi.h - CU-SeeMe IDVI (Intel DVI / IMA ADPCM, audio format id 30) decoder.
 *
 * Derived from the operator root-cause + investigation docs
 *   temp/2026-06-30_CUSeeMe_Audio_Click_Root_Cause_And_Codec_Fix.md
 *   temp/2026-06-30_CUSeeMe_Audio_Click_Investigation_Journey.md
 * which pinned the Cornell CU-SeeMe 0.92b2 client's exact IDVI decode
 * conventions from a read-only disassembly of cu-seeme.exe (wire facts only,
 * no server code copied):
 *
 *   - 4-bit IMA/DVI ADPCM, 8 kHz mono, ~32 kb/s.
 *   - PER-PACKET re-seed: each block carries its own predictor + step-index
 *     seed; NO state is carried across packets (unlike DELTAMOD's integrator).
 *   - Block layout (the 204-byte payload the reflector places at packet 0x26,
 *     i.e. cusm_packet_t.aud_data / aud_len):
 *       [0..1] initial predictor, 16-bit BIG-ENDIAN (byte-swapped vs IMA-WAV)
 *       [2]    initial step index (0..88)
 *       [3]    reserved
 *       [4..]  200 nibble bytes = 400 samples
 *     The seed predictor is NOT emitted ("seed-only"); all 400 samples come
 *     from the nibble stream (the VAT timestamp advances +400 per block).
 *   - HIGH nibble = the earlier sample ((byte>>4) first, then (byte & 0x0F)).
 *   - Reconstruction delta = (step*mag)>>2 + (step>>3), where mag = nibble & 7
 *     and the sign bit is nibble & 8 (subtract when set). The step index adapts
 *     through the standard IMA index table, clamped 0..88; the predictor is
 *     clamped to signed 16-bit.
 *
 * The step-size (89) and index-adjustment (16) tables are the standard IMA/DVI
 * ADPCM tables: the source docs enumerate the client's deviations from standard
 * IMA-WAV and list ONLY four axes (nibble order, quantizer, reconstruction
 * delta, predictor endianness); the step/index tables are not among the
 * deviations and are cited at fixed offsets in the client binary
 * (0x7d220 / 0x7d1e0) as the reference IMA tables.
 */
#ifndef CUSEEME_IDVI_H
#define CUSEEME_IDVI_H

#include <stdint.h>

/* Decode one IDVI block (the audio payload after the VAT header) into signed
 * 16-bit mono PCM at 8 kHz. `in` points at the block's 4-byte seed header;
 * `in_len` is the block length in bytes. Writes 2*(in_len-4) samples; `out`
 * must hold at least that many. Returns the number of samples written (0 when
 * in_len < 4). Stateless across calls: every block re-seeds from its header. */
int cusm_idvi_decode(const uint8_t *in, int in_len, int16_t *out);

#endif /* CUSEEME_IDVI_H */
