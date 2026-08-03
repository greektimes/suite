/*
 * crc32.c - CRC-32 (the IEEE 802.3 / PKZIP one), from the polynomial.
 *
 * REPLACEMENT FILE. Public domain. Same reason and same provenance as
 * crc16.c in this directory: mbzm's original came from Synchronet's
 * smblib under GNU GPL v2 or later and was removed so that no copyleft
 * code reaches the Suite binary.
 *
 * Placed in the public domain by its author, Dimitri Papadopoulos, 2026.
 *
 * Parameters, matching what the Zmodem core expects:
 *
 *   width      32
 *   polynomial 0x04C11DB7, used in reflected form 0xEDB88320
 *   init       0xFFFFFFFF    (CRC_START_32 in crc32.h)
 *   reflected  yes, in and out
 *   final xor  0xFFFFFFFF    applied by crc32i() below for the data path,
 *                            and by the header path itself, which drives
 *                            the ucrc32() macro and then does "crc = ~crc"
 *
 * The reflected form is what the ucrc32() macro implies: it shifts the
 * register right and indexes on the low byte.
 */

#include <stdint.h>
#include "crc32.h"

long crc32tbl[256];

static int crc32tbl_ready = 0;

static void crc32_build(void)
{
    unsigned int i, bit;
    uint32_t r;

    for (i = 0; i < 256; i++) {
        r = (uint32_t)i;
        for (bit = 0; bit < 8; bit++)
            r = (r & 1u) ? ((r >> 1) ^ 0xEDB88320u) : (r >> 1);
        crc32tbl[i] = (long)r;
    }
    crc32tbl_ready = 1;
}

/* See the note in crc16.c: the ucrc32() macro indexes the table
 * directly, so it has to be built before the first header is parsed. */
void mbzm_crc32_init(void)
{
    if (!crc32tbl_ready) crc32_build();
}

unsigned long crc32i(unsigned long crc, char *buf, unsigned long len)
{
    unsigned long i;

    if (!crc32tbl_ready) crc32_build();
    for (i = 0; i < len; i++)
        crc = (unsigned long)((uint32_t)crc32tbl[(crc ^ (unsigned char)buf[i])
                                                 & 0xff] ^ ((uint32_t)crc >> 8));
    /* The final complement belongs here, not to the caller. The file
     * this one replaces ended with "return(~crc)" and the Zmodem core
     * relies on it: zm_calc_data_crc32() reaches the algorithm only
     * through the crc32() macro, so a data subpacket's CRC-32 would
     * never match if the complement were left out. The BINARY32 HEADER
     * path is the exception, and it is the reason the omission is easy
     * to make: that path drives the ucrc32() macro itself and applies
     * its own "crc = ~crc" afterwards, so it never comes through here. */
    return (unsigned long)(uint32_t)~(uint32_t)crc;
}
