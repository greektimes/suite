/*
 * nabu_channel.h - the adapter's CHANGE_CHANNEL exchange, in one place.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 *
 * WHY THIS IS ITS OWN UNIT.
 *
 * Two things in the Suite now need to tell a channel server which channel
 * to serve: the built-in emulator, which talks to it through the vendored
 * Marduk modem, and the serial bridge, which talks to it through a socket
 * of its own while a real NABU is on the other end of a cable. The bytes
 * are identical and the order matters, so there is exactly one copy of
 * them here and both callers pass in their own way of moving a byte.
 *
 * THE EXCHANGE, verified against nabud's own source (nabud/adaptor.c,
 * adaptor_msg_change_channel, with the constants from
 * libnabud/nabu_proto.h):
 *
 *   we send    0x85                        NABU_MSG_CHANGE_CHANNEL
 *   adapter    0x10 0x06                   NABU_MSGSEQ_ACK
 *   we send    channel low, channel high   little endian, nabu_get_uint16
 *   adapter    0xE4                        NABU_STATE_CONFIRMED
 *
 * This is the classic adapter message a real NABU sends, not an
 * extension, so a server cannot tell it from a machine changing channel.
 */

#ifndef NABU_CHANNEL_H
#define NABU_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Move one byte out. Return nonzero on success. */
typedef int (*nabu_ch_write_fn)(void *ctx, uint8_t b);

/* Take one byte in, waiting up to ms milliseconds. Return nonzero if a
 * byte arrived. */
typedef int (*nabu_ch_read_fn)(void *ctx, uint8_t *b, unsigned ms);

/* Valid channel numbers, from nabud's image_channel_select, which
 * rejects anything outside this range and keeps the current channel. */
#define NABU_CHANNEL_MIN 1
#define NABU_CHANNEL_MAX 256

/*
 * Run the exchange. Returns 1 if the adapter confirmed, 0 otherwise, in
 * which case err says why and the caller stays on whatever channel the
 * server was already configured for.
 *
 * Failing is never fatal by design. nabud treats a channel it does not
 * recognise the same way: it logs it, keeps the current channel, and
 * still confirms.
 */
int nabu_channel_select(int channel,
                        nabu_ch_write_fn write_fn,
                        nabu_ch_read_fn  read_fn,
                        void *ctx,
                        char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* NABU_CHANNEL_H */
