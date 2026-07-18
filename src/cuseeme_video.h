/* cuseeme_video.h - CU-SeeMe 16-level grayscale 8x8-square video decoder.
 *
 * Clean-room from VidCodec-TD.96-04-03 ("Video Encoding", Tim Dorcey) and
 * video-SE.96-02-29, ftp.icm.edu.pl/packages/cu-seeme/html/.
 *
 * Image: 160x120, 4-bit gray, 0 = white .. 15 = black (VidCodec-TD overview).
 * These dimensions are the native wire/decode resolution and stay 160x120; the
 * F7 module renders the frame doubled to 320x240 (2x nearest-pixel) for display
 * only - see cuseeme_module.c render_paint.
 * Subdivided into 8x8 squares, Square ID 0..299 numbered left-to-right,
 * top-to-bottom => 20 columns x 15 rows.  Squares arrive intra/inter coded;
 * a square not transmitted this frame keeps its previous content (inter-frame
 * persistence), which is exactly what a receiver gets for free by keeping a
 * persistent framebuffer and only overwriting squares that appear on the wire.
 */
#ifndef CUSEEME_VIDEO_H
#define CUSEEME_VIDEO_H

#include <stdint.h>

#define CUSM_VID_W      160
#define CUSM_VID_H      120
#define CUSM_SQ         8
#define CUSM_SQ_COLS    (CUSM_VID_W / CUSM_SQ)   /* 20 */
#define CUSM_SQ_ROWS    (CUSM_VID_H / CUSM_SQ)   /* 15 */
#define CUSM_SQ_COUNT   (CUSM_SQ_COLS * CUSM_SQ_ROWS) /* 300 */

typedef struct {
    /* Persistent 4-bit (0..15) framebuffer, one byte per pixel, row-major.
     * Persists across frames so inter-coded / untransmitted squares retain
     * their previous value. */
    uint8_t pix[CUSM_VID_W * CUSM_VID_H];
    int     got_any;     /* at least one square ever decoded */
} cusm_video_t;

void cusm_video_init(cusm_video_t *v);

/* Decode all squares contained in one video packet's data block
 * (proto vid_data / vid_len). Updates the persistent framebuffer in place.
 * Returns the number of squares decoded (>=0), or -1 on a malformed block. */
int cusm_video_decode(cusm_video_t *v, const uint8_t *data, int len);

/* Expand the 4-bit framebuffer to an 8-bit grayscale buffer (0=black..255=white).
 * The encoder emits spec-correct VidCodec-TD polarity (0=white..15=black on the
 * wire); the display convention is the inverse, so cusm_video_to_gray8 inverts
 * the nibble. out must hold 160*120 bytes. */
void cusm_video_to_gray8(const cusm_video_t *v, uint8_t *out);

#endif /* CUSEEME_VIDEO_H */
