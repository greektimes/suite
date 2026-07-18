/*
 * truespeech_decoder.h - DSP Group TrueSpeech (WAV format tag 0x0022)
 *                        decoder, ported from FFmpeg libavcodec/truespeech.c.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 *
 * The decoder itself is LGPL (see truespeech_decoder.c for the full
 * upstream license header, retained as required). This thin C interface
 * around it is provided so the rest of the Suite can decode a TrueSpeech
 * payload without dragging in the FFmpeg framework.
 *
 * Wire layout: the compressed payload is a sequence of 32-byte frames.
 * Each frame decodes to exactly 240 samples of 16-bit signed mono PCM at
 * 8000 Hz. A trailing partial (< 32 byte) tail is ignored.
 */

#ifndef TRUESPEECH_DECODER_H
#define TRUESPEECH_DECODER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Number of compressed bytes per TrueSpeech frame and the number of
 * PCM samples each frame expands to. */
#define TRUESPEECH_FRAME_BYTES    32
#define TRUESPEECH_FRAME_SAMPLES  240

/* Decode a whole TrueSpeech payload. `src`/`src_len` is the raw codec
 * bitstream (the WAV 'data' chunk). Output 16-bit mono PCM is written to
 * `dst`, which must have room for `dst_max_samples` int16 samples. The
 * decoder consumes src in 32-byte frames (src_len/32 frames); any
 * trailing bytes that do not fill a frame are ignored. Inter-frame
 * filter state is carried across frames within this single call.
 *
 * On return *out_samples holds the number of samples actually written
 * (frames_decoded * 240, clamped so it never exceeds dst_max_samples).
 *
 * Returns 0 on success, non-zero on bad arguments or if the payload is
 * shorter than a single 32-byte frame. */
int truespeech_decode(const uint8_t *src, size_t src_len,
                      int16_t *dst, size_t dst_max_samples,
                      size_t *out_samples);

#ifdef __cplusplus
}
#endif

#endif /* TRUESPEECH_DECODER_H */
