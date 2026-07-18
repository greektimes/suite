/* cuseeme_mulaw.h - G.711 mu-law audio decoder for CU-SeeMe
 * format ID 0. Standard ITU-T G.711 mu-law (PCMU): 8-bit
 * compressed -> 16-bit linear PCM. 8 kHz mono. Stateless,
 * one byte per sample, no headers, no state seed.
 *
 * Audio-RBK format table lists format 0 as mu-law. The Suite
 * client routes incoming audio packets to this decoder when
 * the VAT header's nsid/format field is 0.
 */
#ifndef CUSEEME_MULAW_H
#define CUSEEME_MULAW_H
#include <stdint.h>

/* Decode N mu-law bytes to N int16 PCM samples.
 * src: mu-law-encoded bytes (the payload after the VAT header)
 * src_len: count of mu-law bytes
 * dst: output buffer, must hold at least src_len * sizeof(int16_t)
 * Returns: number of samples written (== src_len on success). */
int cusm_mulaw_decode(const uint8_t *src, int src_len, int16_t *dst);

#endif
