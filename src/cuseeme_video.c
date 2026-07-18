/* cuseeme_video.c - CU-SeeMe 16-gray 8x8-square video decoder.
 * Clean-room from VidCodec-TD.96-04-03 ("Video Encoding", Tim Dorcey).
 *
 * Per-square wire format (VidCodec-TD "Video Packet Format"):
 *   00h Square ID        (2 bytes, big-endian, 0..299 for 160x120)
 *   02h C1..C8           (4 bytes = eight 4-bit row codes, high nibble first)
 *   06h IntraRow Data    (0..32 bytes: 0/1/2/4 bytes per row per C[i])
 * This pattern repeats for as many squares as are present in the packet.
 *
 * Row reconstruction (VidCodec-TD):
 *   Row[i] = Row[i-1] - InterRow[i] + IntraRow[i]      for i = 1..8
 * with Row[0]'s predecessor taken as 0x88888888, 32-bit integer arithmetic.
 * Each 32-bit Row holds 8 pixels, 4 bits each, leftmost pixel = most
 * significant nibble.
 *
 * InterRow value and IntraRow bit-width are selected by the 4-bit code C
 * (VidCodec-TD table).  IntraRow data is carried as whole bytes per row
 * (0/1/2/4), not a packed sub-byte bitstream (confirmed against the live wire:
 * square byte counts predict the next Square ID offset exactly).
 */
#include "cuseeme_video.h"
#include <string.h>

/* VidCodec-TD code table, indexed by C (0..15):
 *   inter[C]   = InterRow constant
 *   intrabits[C] = number of IntraRow bits (0, 8, 16 or 32) */
static const uint32_t k_inter[16] = {
    0x00000000u, /* 0 */ 0xDDDDDDDEu, /* 1 */ 0xEEEEEEEFu, /* 2 */ 0x00000000u, /* 3 */
    0xEEEEEEEFu, /* 4 */ 0x00000000u, /* 5 */ 0x11111111u, /* 6 */ 0x00000000u, /* 7 */
    0x11111111u, /* 8 */ 0x22222222u, /* 9 */ 0x11111111u, /* A */ 0x22222222u, /* B */
    0x33333333u, /* C */ 0x22222222u, /* D */ 0x33333333u, /* E */ 0x44444444u  /* F */
};
static const uint8_t k_intrabits[16] = {
    32, /* 0 */  0, /* 1 */  8, /* 2 */ 16, /* 3 */
     0, /* 4 */  8, /* 5 */ 16, /* 6 */  0, /* 7 */
     8, /* 8 */ 16, /* 9 */  0, /* A */  8, /* B */
    16, /* C */  0, /* D */  8, /* E */ 16  /* F */
};

void cusm_video_init(cusm_video_t *v) {
    memset(v, 0, sizeof(*v));
    /* 0x8 nibble = mid gray, matching the codec's neutral predecessor. */
    memset(v->pix, 0x08, sizeof(v->pix));
    v->got_any = 0;
}

/* Spread 8 IntraRow data bits abcdefgh -> 000a000b000c000d000e000f000g000h
 * (VidCodec-TD: the bit becomes the LS bit of each nibble, MSB = leftmost). */
static uint32_t intra8(uint8_t b) {
    uint32_t r = 0; int k;
    for (k = 0; k < 8; k++) {
        uint32_t bit = (b >> (7 - k)) & 1u;       /* a = MSB ... h = LSB */
        r |= bit << (4 * (7 - k));                /* nibble k, LS bit */
    }
    return r;
}
/* Spread 16 IntraRow data bits ab cd ef gh ij kl mn op -> 00ab00cd...00op
 * (two bits per nibble). */
static uint32_t intra16(uint8_t b0, uint8_t b1) {
    uint32_t r = 0; int k;
    uint16_t w = (uint16_t)((b0 << 8) | b1);
    for (k = 0; k < 8; k++) {
        uint32_t pair = (w >> (2 * (7 - k))) & 0x3u; /* (a,b) first ... (o,p) */
        r |= pair << (4 * (7 - k));
    }
    return r;
}

/* Write one decoded 32-bit row of 8 pixels into the framebuffer at (px,py). */
static void put_row(cusm_video_t *v, int px, int py, uint32_t row) {
    int x;
    if (py < 0 || py >= CUSM_VID_H) return;
    for (x = 0; x < CUSM_SQ; x++) {
        int gx = px + x;
        if (gx < 0 || gx >= CUSM_VID_W) continue;
        uint8_t nib = (uint8_t)((row >> (4 * (7 - x))) & 0xF); /* left = MS nibble */
        v->pix[py * CUSM_VID_W + gx] = nib;
    }
}

int cusm_video_decode(cusm_video_t *v, const uint8_t *data, int len) {
    int p = 0, squares = 0;
    while (p + 6 <= len) {                         /* need Square ID + 8 codes */
        int sid = (data[p] << 8) | data[p + 1];
        p += 2;
        if (sid < 0 || sid >= CUSM_SQ_COUNT) return squares; /* out of range: stop */
        uint8_t codes[8];
        codes[0] = (data[p]   >> 4) & 0xF; codes[1] = data[p]   & 0xF;
        codes[2] = (data[p+1] >> 4) & 0xF; codes[3] = data[p+1] & 0xF;
        codes[4] = (data[p+2] >> 4) & 0xF; codes[5] = data[p+2] & 0xF;
        codes[6] = (data[p+3] >> 4) & 0xF; codes[7] = data[p+3] & 0xF;
        p += 4;

        /* validate we have all IntraRow bytes for this square before decoding */
        int need = 0, i;
        for (i = 0; i < 8; i++) need += k_intrabits[codes[i]] / 8;
        if (p + need > len) return squares;        /* truncated tail: stop clean */

        int col = sid % CUSM_SQ_COLS;
        int rrow = sid / CUSM_SQ_COLS;
        int px = col * CUSM_SQ;
        int py = rrow * CUSM_SQ;

        uint32_t prev = 0x88888888u;               /* predecessor of row 0 */
        for (i = 0; i < 8; i++) {
            uint8_t c = codes[i];
            int bits = k_intrabits[c];
            uint32_t intra = 0, row;
            if (bits == 8) {
                intra = intra8(data[p]); p += 1;
                row = prev - k_inter[c] + intra;
            } else if (bits == 16) {
                intra = intra16(data[p], data[p+1]); p += 2;
                row = prev - k_inter[c] + intra;
            } else if (bits == 32) {
                /* "the additional data is the actual value of the current row" */
                row = ((uint32_t)data[p] << 24) | ((uint32_t)data[p+1] << 16) |
                      ((uint32_t)data[p+2] << 8) | (uint32_t)data[p+3];
                p += 4;
            } else { /* bits == 0 */
                row = prev - k_inter[c];           /* intra = 0 */
            }
            put_row(v, px, py + i, row);
            prev = row;
        }
        squares++;
        v->got_any = 1;
    }
    return squares;
}

void cusm_video_to_gray8(const cusm_video_t *v, uint8_t *out) {
    int i, n = CUSM_VID_W * CUSM_VID_H;
    for (i = 0; i < n; i++) {
        int nib = v->pix[i] & 0xF;
        /* The encoder emits spec-correct VidCodec-TD polarity:
         * 0 = white .. 15 = black on the wire. Display convention is the
         * inverse (0 = black .. 255 = white on screen), so invert. */
        out[i] = (uint8_t)((15 - nib) * 17);       /* invert: 0=white..15=black -> 0=black..255=white */
    }
}
