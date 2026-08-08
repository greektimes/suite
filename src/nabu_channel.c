/*
 * nabu_channel.c - the adapter's CHANGE_CHANNEL exchange, in one place.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 *
 * See nabu_channel.h for the exchange and where it was verified.
 */

#include <stdio.h>
#include <string.h>

#include "nabu_channel.h"

/* The message and its replies. Named rather than spelled inline so the
 * comparison below reads as the protocol and not as magic numbers. */
#define MSG_CHANGE_CHANNEL 0x85
#define MSG_ESCAPE         0x10
#define STATUS_GOOD        0x06
#define STATE_CONFIRMED    0xE4

/* The adapter answers within a round trip or it is not going to. Two
 * seconds is generous for that, and short enough that a wrong guess
 * about the far end does not hang the caller. */
#define REPLY_MS 2000

int nabu_channel_select(int channel,
                        nabu_ch_write_fn write_fn,
                        nabu_ch_read_fn  read_fn,
                        void *ctx,
                        char *err, size_t errcap)
{
    uint8_t b;

    if (err && errcap) err[0] = '\0';
    if (!write_fn || !read_fn) {
        if (err) snprintf(err, errcap, "no transport for the channel change");
        return 0;
    }
    if (channel < NABU_CHANNEL_MIN || channel > NABU_CHANNEL_MAX) {
        if (err) snprintf(err, errcap,
                          "channel %d is outside 1 to %d",
                          channel, NABU_CHANNEL_MAX);
        return 0;
    }

    if (!write_fn(ctx, MSG_CHANGE_CHANNEL)) {
        if (err) snprintf(err, errcap, "could not ask for a channel change");
        return 0;
    }

    if (!read_fn(ctx, &b, REPLY_MS) || b != MSG_ESCAPE ||
        !read_fn(ctx, &b, REPLY_MS) || b != STATUS_GOOD) {
        if (err) snprintf(err, errcap,
                          "the server did not acknowledge the channel change");
        return 0;
    }

    /* Little endian, which is what nabu_get_uint16 reads. */
    if (!write_fn(ctx, (uint8_t)(channel & 0xFF)) ||
        !write_fn(ctx, (uint8_t)((channel >> 8) & 0xFF))) {
        if (err) snprintf(err, errcap, "could not send the channel number");
        return 0;
    }

    if (!read_fn(ctx, &b, REPLY_MS) || b != STATE_CONFIRMED) {
        if (err) snprintf(err, errcap,
                          "the server did not confirm channel %d", channel);
        return 0;
    }

    return 1;
}
