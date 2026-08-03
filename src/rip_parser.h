/*
 * rip_parser.h - RIPscrip 1.54 wire parser driving a RipEga surface.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 *
 * The parser is fed arbitrary byte chunks as they arrive off the wire
 * (rip_parser_feed) and assembles them into RIPscrip command lines:
 *
 *   - A command line starts at column 0 with "!|", or anywhere with a
 *     Ctrl-A (SOH) or Ctrl-B (STX) in place of the "!" (rule 12).
 *   - Commands are separated by "|" and several share one line (rule 4).
 *   - A trailing backslash continues the line (rule 5), with the
 *     double-backslash caveat of rule 13.
 *   - Any other line is raw TTY text, counted and dropped for now.
 *
 * Each command is a level prefix (absent for level 0), a command type
 * character, and fixed-width base-36 "MegaNum" arguments, with an
 * optional trailing text argument.
 *
 * Adding a command means adding one case to rip_parser_dispatch and, if
 * it is not level 0, one entry to the level-1 table. Nothing else in
 * this file needs to change.
 */

#ifndef RIP_PARSER_H
#define RIP_PARSER_H

#include "rip_ega.h"
#include "rip_button.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- level-1 buttons and mouse regions ---------------------------- */
#define RIP_MAX_BUTTONS 64
#define RIP_BTN_LABEL   48
#define RIP_BTN_CMD     48

typedef struct RipButton {
    int  x0, y0, x1, y1;    /* region rectangle, inclusive, surface coords */
    /* The part of the region that inverts while the mouse is held down.
     * 1.54 says the special effects invert with the button because they
     * are part of its image, "all except for the Recessed effect", which
     * "is NEVER considered part of the actual button image". So this is
     * the base image plus the bevel and nothing else. For a RIP_MOUSE
     * region, which has no chrome, it is simply the region itself. */
    int  ix0, iy0, ix1, iy1;
    int  hotkey;            /* ASCII value of the hotkey, 0 = none         */
    int  flags;             /* RIP_BUTTON flags, kept verbatim             */
    int  invert;            /* invert while held: 1U Invertable, 1M clk    */
    char label[RIP_BTN_LABEL];  /* display label, may be empty             */
    char command[RIP_BTN_CMD];  /* host command, caret escapes expanded    */
} RipButton;

/* Longest line we will assemble, including continuations. The 1.54
 * recommendation is that a physical line stays modem friendly; the
 * continuation rule means a logical line can be longer, so this is set
 * well above anything the emitter produces. */
#define RIP_LINE_MAX  8192

/* What the parser met but cannot draw yet. Every unimplemented command
 * increments a counter instead of being silently discarded, so a phase
 * can state exactly what it does not do. */
typedef struct RipStats {
    unsigned long commands;         /* commands dispatched, total       */
    unsigned long drawn;            /* commands that changed pixels     */
    unsigned long text_cmds;        /* RIP_TEXT / RIP_TEXT_XY drawn     */
    unsigned long font_cmds;        /* RIP_FONT_STYLE seen              */
    unsigned long arc_cmds;         /* circles, ovals, arcs, pies drawn */
    unsigned long bezier_cmds;      /* RIP_BEZIER, deliberately skipped */
    unsigned long flood_cmds;       /* RIP_FILL flood, still deferred   */
    unsigned long level1_cmds;      /* level-1 commands still deferred  */
    unsigned long region_cmds;      /* RIP_MOUSE / kill, fully acted on */
    unsigned long button_cmds;      /* RIP_BUTTON drawn and registered  */
    unsigned long button_undrawn;   /* RIP_BUTTON registered, NOT drawn */
    unsigned long download_blocked; /* downloads that did NOT complete  */
    unsigned long tty_cmds;         /* TTY text window group, deferred  */
    unsigned long unknown_cmds;     /* command char we do not know      */
    unsigned long raw_text_lines;   /* lines routed to the TTY window   */
    unsigned long unsup_fill_pat;   /* fill patterns 02-0B (drawn solid)*/
    unsigned long unsup_line_style; /* non-solid line styles            */
    unsigned long malformed;        /* truncated or bad arguments       */
} RipStats;

typedef struct RipParser {
    RipEga  *surf;

    /* Graphics state carried between commands. */
    int draw_color;        /* RIP_COLOR                                 */
    int fill_pattern;      /* RIP_FILL_STYLE pattern                    */
    int fill_color;        /* RIP_FILL_STYLE colour                     */
    int line_style;        /* RIP_LINE_STYLE style                      */
    int line_thick;        /* RIP_LINE_STYLE thickness                  */

    /* Text state (Phase 2). */
    int font;              /* RIP_FONT_STYLE font number 0-10           */
    int font_dir;          /* RIP_FONT_STYLE direction                  */
    int font_size;         /* RIP_FONT_STYLE size 1-10                  */
    int cur_x, cur_y;      /* drawing position: RIP_MOVE and after text */

    /* Line assembly. */
    char line[RIP_LINE_MAX];
    int  line_len;
    int  at_line_start;    /* next byte would be column 0               */
    int  continued;        /* previous physical line ended with "\"     */
    int  last_was_cr;      /* so a CRLF pair ends one line, not two     */

    /* Clickable regions for the CURRENT scene. Cleared by
     * RIP_RESET_WINDOWS and by RIP_KILL_MOUSE_FIELDS, which is how the
     * service separates one screen's regions from the next. */
    RipButton buttons[RIP_MAX_BUTTONS];
    int       button_count;

    /* Current RIP_BUTTON_STYLE. Every RIP_BUTTON that follows is drawn
     * and sized by this until the next style arrives. */
    RipButtonStyle btn_style;

    RipStats stats;
} RipParser;

/* Bind a parser to a surface and reset both. */
void rip_parser_init(RipParser *p, RipEga *surf);

/* Reset graphics state and the surface, as RIP_RESET_WINDOWS does.
 * Statistics are left alone. */
void rip_parser_reset(RipParser *p);

/* Feed bytes straight off the wire. Returns non-zero if anything was
 * drawn, so the caller knows whether to repaint. */
int  rip_parser_feed(RipParser *p, const unsigned char *data, int len);

/* Decode a fixed-width MegaNum (base 36). Returns -1 if the field is
 * short or holds a character outside 0-9 A-Z (lower case accepted). */
int  rip_meganum(const char *s, int width);

/* Commands that were parsed correctly but not drawn, which is what the
 * tab reports as its deferred tally. After Phase 2 this is the level-1
 * family (icons, buttons, mouse regions), flood fill, Bezier and the TTY
 * text window group, and nothing else. */
unsigned long rip_stats_deferred(const RipStats *st);

/* Topmost region containing (x, y) in surface coordinates, or -1. 1.54
 * scans mouse regions last in, first out, so a region defined later sits
 * on top of an earlier one. */
int rip_button_at(const RipParser *p, int x, int y);

/* Region whose hotkey matches this ASCII value, case-insensitive for
 * letters, or -1. */
int rip_button_by_hotkey(const RipParser *p, int ascii);

/* True if this host command would make the server start a file
 * transfer.
 *
 * This is the one place a host command's TEXT is inspected rather than
 * simply relayed. It used to exist so the command could be held back,
 * because there was no Zmodem receiver and the frames would have been
 * drawn as punctuation. There is one now, so the answer is used the
 * other way round: the tab switches the payload stream over to the
 * receiver BEFORE sending a command this returns true for. The service
 * marks its download commands with a distinct verb, so the check is a
 * prefix match on that verb and nothing else. Dispatch itself never
 * looks at the string. */
int rip_command_is_download(const char *cmd);

#ifdef __cplusplus
}
#endif

#endif /* RIP_PARSER_H */
