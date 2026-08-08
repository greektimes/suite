/*
 * nabu_serial.h - the serial leg: a real NABU on a cable, a channel
 *                 server on a socket, and nothing in between.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 *
 * WHAT THIS IS FOR.
 *
 * A real NABU Personal Computer has no network. It has an HCCA port, a
 * serial line that expected an adapter box on the end of a cable running
 * to the head end. Owners of real machines today run a separate Internet
 * Adapter program that opens that serial port and relays the adapter
 * protocol to a channel server over TCP. This is the Suite doing that
 * job, so the Suite becomes the missing serial leg rather than another
 * content server.
 *
 * THE SUITE ADDS NOTHING TO THE STREAM. Once the relay is running, every
 * byte off the cable goes to the socket and every byte off the socket
 * goes to the cable, in order, unexamined and unmodified. The Suite does
 * not know or care what the bytes mean, which is the whole point: the
 * real machine boots its own genuine ROM and talks to nabud exactly as
 * it would through any other adapter.
 *
 * The one thing sent that the NABU did not say is the channel selection,
 * and it goes out BEFORE the relay starts and only ever towards the
 * server. See nabu_serial_open. The cable never sees it.
 */

#ifndef NABU_SERIAL_H
#define NABU_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE NABU'S NATIVE LINE RATE.
 *
 * Not a round number and not a guess. The machine clocks its UART from
 * the NTSC colourburst crystal:
 *
 *     3.579545 MHz  /  2               /  16
 *     colourburst      on-board divider   on-chip divider on the TR1863
 *
 * which is 111860.625 bits per second. Verified against nabud's own
 * source, nabud/conn.c, which derives it the same way and defines
 * NABU_NATIVE_BPS as ((3579540 / 2) / 16). The integer value both ends
 * use is therefore 111860.
 *
 * NOTE that a widely repeated figure of 111865 is wrong; the derivation
 * above is what the hardware actually does.
 */
#define NABU_SERIAL_BAUD 111860

/*
 * EIGHT DATA BITS, NO PARITY, TWO STOP BITS.
 *
 * The native protocol is 8N1. Two stop bits is deliberate and is what
 * nabud defaults to, in its own words because "it's much more reliable
 * if we use 2 stop bits. Otherwise, the NABU can get out of sync when
 * receiving a stream of bytes in a packet." The extra stop bit costs a
 * little throughput and buys the machine time to keep up, so the Suite
 * defaults the same way and for the same reason.
 */
#define NABU_SERIAL_STOPBITS 2

/* One line of the connection log, as the tab will show it. */
typedef void (*nabu_serial_log_fn)(void *user, const char *line);

typedef struct {
    const char *com_port;   /* "COM3", or a \\.\COM12 form for high numbers */
    const char *host;
    int         port;
    int         channel;    /* 0 to leave the server on its own default */
} nabu_serial_config_t;

/*
 * Open the cable and the socket, select the channel on the socket, and
 * leave both ready to relay. Returns 0 on success. On failure nothing is
 * left open and err says what went wrong.
 *
 * The order matters and is deliberate: the serial port is opened FIRST,
 * because a wrong COM port or a rate the driver will not accept is the
 * likely failure and there is no reason to bother a channel server with
 * a session that cannot work.
 */
int nabu_serial_open(const nabu_serial_config_t *cfg,
                     nabu_serial_log_fn log_fn, void *log_user,
                     char *err, size_t errcap);

/*
 * Move whatever is waiting, in both directions, once. Returns the number
 * of bytes moved, or -1 if either end has gone away.
 *
 * Non-blocking by construction, so the caller owns the pacing and can
 * check its own stop event as often as it likes.
 */
int nabu_serial_pump(void);

/* Bytes relayed since the last open, for the log. */
void nabu_serial_counts(uint64_t *to_nabu, uint64_t *to_server);

/* Close both ends. Safe to call from any half-open state, and safe to
 * call twice. */
void nabu_serial_close(void);

/*
 * The machine's serial ports, newest enumeration each call. Fills names
 * with up to max entries of the form "COM3" and returns how many.
 */
int nabu_serial_enumerate(char names[][16], int max);

#ifdef __cplusplus
}
#endif

#endif /* NABU_SERIAL_H */
