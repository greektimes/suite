/*
 * crc16.c - CRC-16/XMODEM, written from the polynomial definition.
 *
 * REPLACEMENT FILE. Public domain.
 *
 * mbzm ships a crc16.c taken from Synchronet's smblib, which is GNU GPL
 * v2 or later (see licenses/LICENSE.mbzm-smblib-REMOVED.txt). The MGT
 * Unicorn Suite must not ship copyleft code in its binary, so that file
 * was removed and this one written in its place from the algorithm
 * definition alone. No Synchronet, lrzsz or Omen code was consulted.
 *
 * A CRC is arithmetic rather than authorship: this is the textbook
 * table-driven form of the CCITT polynomial. Placed in the public domain
 * by its author, Dimitri Papadopoulos, 2026.
 *
 * Parameters, matching what the Zmodem core expects:
 *
 *   name       CRC-16/XMODEM
 *   width      16
 *   polynomial 0x1021
 *   init       0x0000        (CRC_START_XMODEM in crc16.h)
 *   reflected  no, in or out
 *   final xor  none
 *
 * The table is built on first use, so the only constant in this file is
 * the polynomial itself.
 */

#include <stdint.h>
#include "crc16.h"

unsigned short crc16tbl[256];

static int crc16tbl_ready = 0;

static void crc16_build(void)
{
    unsigned int i, bit;
    unsigned short r;

    for (i = 0; i < 256; i++) {
        r = (unsigned short)(i << 8);
        for (bit = 0; bit < 8; bit++)
            r = (unsigned short)((r & 0x8000u) ? (((unsigned)r << 1) ^ 0x1021u)
                                               : ((unsigned)r << 1));
        crc16tbl[i] = r;
    }
    crc16tbl_ready = 1;
}

/* Callers reach the table through the ucrc16() macro in crc16.h, which
 * indexes crc16tbl directly, so the table must exist before any of them
 * run. The Zmodem session calls this once at start. */
void mbzm_crc16_init(void)
{
    if (!crc16tbl_ready) crc16_build();
}

unsigned short crc16(char *data, unsigned long len)
{
    unsigned short crc = 0;
    unsigned long i;

    if (!crc16tbl_ready) crc16_build();
    for (i = 0; i < len; i++)
        crc = (unsigned short)(crc16tbl[((crc >> 8) & 0xff) ^
                                        (unsigned char)data[i]] ^ (crc << 8));
    return crc;
}
