/*
 * crc16.h - API for the public-domain CRC-16/XMODEM replacement.
 *
 * REPLACEMENT FILE. Public domain, Dimitri Papadopoulos 2026. See
 * crc16.c for why the original was removed.
 *
 * The names, the macro and the table are kept exactly as the vendored
 * Zmodem core expects them, so that core compiles unmodified.
 */

#ifndef _CRC16_H_
#define _CRC16_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned short crc16tbl[];

unsigned short crc16(char *data, unsigned long len);

/* Build the table. Call once before any ucrc16() use. */
void mbzm_crc16_init(void);

#ifdef __cplusplus
}
#endif

#define CRC_START_XMODEM    ((uint16_t)0)

#define ucrc16(ch, crc) (crc16tbl[((crc >> 8) & 0xff) ^ (unsigned char)ch] ^ (crc << 8))

#endif /* _CRC16_H_ */
