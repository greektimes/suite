/*
 * naplps_decode.h - NAPLPS (videotex) presentation-layer decoder.
 *                   A shared Suite service: any module may drive it.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ---------------------------------------------------------------------
 * PROVENANCE
 *
 * Every byte format decoded by this module was derived solely from the
 * published standard
 *
 *     ANSI X3.110-1983 / CSA T500-1983
 *     "Videotex/Teletext Presentation Level Protocol Syntax
 *      (North American PLPS)", published December 1983, 175 pages.
 *     sha256 of the reference scan worked from:
 *     d5a3a649a0bbef5ca34fa9ff54145078ff1c1b65b98c3f41999482e13f5fa537
 *
 * The reference copy is held outside this repository and is NOT
 * redistributed with the Suite: it is an ANSI and CSA copyrighted
 * document, held for deriving an independent implementation exactly as
 * RIPSCRIP-1.54.DOC is held for the RIPscrip renderer.
 *
 * No NAPLPS decoder or renderer source was read, copied or adapted.
 *
 * DISCLOSED CROSS-CHECKS, because a provenance claim that overstates its
 * own isolation is worse than one that describes it accurately:
 *
 *   - naplps.js by Nick Fox-Gieg (github.com/n1ckfg/Telidon, MIT) is used
 *     as a BLACK-BOX cross-check oracle: it is run on the same frames and
 *     its output compared. Where the two disagree, the disagreement is
 *     resolved by re-reading the standard, never by matching the other
 *     implementation.
 *   - Personality+III 2.18a by Microstar Software Ltd. (1992), a period
 *     DOS terminal, is used as a BLACK-BOX rendering oracle under DOSBox:
 *     screenshots in, judgement out.
 *   - boom.nap and its instruction-by-instruction annotation from the
 *     same repository are used as a READ-ONLY segmentation fixture.
 *   - README.PP3, Microstar Software Ltd., 1992-08-26, supplies two facts
 *     the 1983 standard deliberately leaves blank because the ISO
 *     registration had not happened yet: the final character of the
 *     "complete code" designation (ESC % A enters NAPLPS, ESC % @
 *     returns to ASCII), and the ALEXtel Subset profile's 7E1 character
 *     frame and default function key sequences. Both are cited as coming
 *     from that document and not from the standard.
 *
 * Two byte values used here are NOT in the standard, which left them for
 * the ISO Registration Authority, and are recorded as empirical:
 *   ESC 2/5 4/1  enter NAPLPS      (from README.PP3 section 9.0)
 *   ESC 2/2 4/6  designate C1 set  (observed in boom.nap, consistent
 *                                   with the standard's ESC 2/2 F4 form)
 * ---------------------------------------------------------------------
 *
 * SCOPE. This decoder implements the videotex Service Reference Model
 * subset of Appendix D, which is the profile the ALEXtel and any other
 * period videotex terminal was built to. What is implemented, what is
 * recognised-and-skipped, and why, is listed in naplps_decode.c beside
 * the dispatch table. Section 8.3.2 of the standard requires that
 * unimplemented features be skipped SAFELY rather than executed as
 * garbage, so every excluded opcode is still parsed for length and its
 * operands consumed.
 *
 * This translation unit is deliberately free of Win32 and GDI calls.
 */

#ifndef NAPLPS_DECODE_H_INCLUDED
#define NAPLPS_DECODE_H_INCLUDED

#include <stddef.h>
#include "naplps_surface.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Counters. These exist because of the counter-honesty rule this project
 * adopted during the RIPscrip work: a thing REGISTERED is not a thing
 * RENDERED, and a decoder that silently swallows what it cannot do is
 * indistinguishable from one that has a bug. Anything the decoder
 * declines to execute increments something here, and the tab can show it.
 */
typedef struct naplps_stats {
    unsigned long bytes_in;
    unsigned long chars_drawn;
    unsigned long pdi_executed;      /* opcodes actually executed         */
    unsigned long pdi_skipped;       /* recognised, deliberately not done */
    unsigned long esc_unknown;       /* ESC sequences consumed, not acted  */
    unsigned long esc_malformed;     /* aborted per 4.3.2, byte re-executed*/
    unsigned long operand_short;     /* padded per 5.3.2.2.5               */
    unsigned long waits;             /* WAIT commands honoured             */
    unsigned long waits_malformed;   /* WAIT with a bad first operand byte */
} naplps_stats_t;

/* Presentation mode, per README.PP3 9.0 / the standard's 4.3.1.2. */
typedef enum {
    NAPLPS_MODE_NAPLPS = 0,   /* the complete code is designated         */
    NAPLPS_MODE_ASCII         /* ESC % @ handed the line back to plain   */
} naplps_mode_t;

typedef struct naplps_decoder naplps_decoder_t;

naplps_decoder_t *naplps_create(void);
void              naplps_destroy(naplps_decoder_t *d);

/*
 * Full reset to the power-on default state of 4.3 and Table 28: G0
 * primary, G1 PDI, G2 supplementary, G3 mosaic, G0 invoked, DOMAIN
 * defaults (single-value 1 byte, multi-value 3 bytes, 2 dimensions,
 * logical pel 0x0), colour mode 0, drawing colour white, NORMAL TEXT,
 * cursor home, screen cleared, default colour map.
 */
void naplps_reset(naplps_decoder_t *d);

/* Push received bytes through the decoder. Re-entrant per decoder. */
void naplps_feed(naplps_decoder_t *d, const unsigned char *buf, size_t len);

/*
 * Execute any PDI sequence still being accumulated. A PDI sequence is
 * terminated by the next opcode or by a non-numeric byte (5.3.1.1), so
 * the last command in a file or before a pause has nothing to terminate
 * it. Call this at end of file, and when a live stream goes idle.
 */
void naplps_flush(naplps_decoder_t *d);

const naplps_surface_t *naplps_get_surface(const naplps_decoder_t *d);
const naplps_stats_t   *naplps_get_stats(const naplps_decoder_t *d);
naplps_mode_t           naplps_get_mode(const naplps_decoder_t *d);

/*
 * Pending WAIT, in tenths of a second, consumed by the caller.
 * 5.3.2.8 makes WAIT the terminal's business, and Phase 1 5.2 records
 * why it matters: the host cannot know when a frame has finished
 * drawing, so WAIT is the only correct instrument for pacing. Returns
 * the accumulated tenths and clears the counter.
 */
int naplps_take_wait(naplps_decoder_t *d);

/*
 * True once anything at all has been drawn. The tab uses this to decide
 * between the idle hint and the picture, the way the RIPscrip tab does.
 */
int naplps_ever_drew(const naplps_decoder_t *d);

/*
 * Optional segmentation trace, called once per PDI execution with the
 * opcode and the operand bytes that execution consumed.
 *
 * This exists for the gate harness rather than for the tab. Gate G3
 * checks the decoder's segmentation of a known file against an
 * independently written annotation of it, and "the picture looked right"
 * is not that check: a wrong DOMAIN length draws a wrong picture with no
 * error anywhere (5.3.2.2.5), so the segmentation has to be inspected
 * directly.
 */
typedef void (*naplps_trace_fn)(void *ctx, int opcode,
                                const unsigned char *operands, int nbytes);
void naplps_set_trace(naplps_decoder_t *d, naplps_trace_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NAPLPS_DECODE_H_INCLUDED */
