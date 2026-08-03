/*
 * crc32.h - API for the public-domain CRC-32 replacement.
 *
 * REPLACEMENT FILE. Public domain, Dimitri Papadopoulos 2026. See
 * crc32.c for why the original was removed.
 *
 * The names, the macros and the table are kept exactly as the vendored
 * Zmodem core expects them, so that core compiles unmodified.
 */

#ifndef _CRC32_H_
#define _CRC32_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern long crc32tbl[];

unsigned long crc32i(unsigned long crc, char *buf, unsigned long len);

/* Build the table. Call once before any ucrc32() use. */
void mbzm_crc32_init(void);

#ifdef __cplusplus
}
#endif

#define CRC_START_32    ((uint32_t)~0)

#define ucrc32(ch, crc) (crc32tbl[(crc ^ (ch)) & 0xff] ^ (crc >> 8))

#define crc32(x, y)     crc32i(CRC_START_32, x, y)

#endif /* _CRC32_H_ */
