/*
 * rip_parser.c - RIPscrip 1.54 wire parser.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Clean-room from docs/ripscrip-ref/RIPSCRIP-1.54.DOC. No Win32, no GDI.
 */

#include "rip_parser.h"
#include "rip_text.h"

#include <string.h>
#include <stdlib.h>

#define RIP_MAX_NUMERIC_ARGS  32
#define RIP_MAX_POLY_POINTS   512   /* 1.54 caps npoints at 512 */

/* ------------------------------------------------------------------ */
/* MegaNum: fixed width, base 36, digits 0-9 then A-Z.                 */
/* ------------------------------------------------------------------ */

int rip_meganum(const char *s, int width)
{
    int i, v = 0;
    if (!s || width <= 0) return -1;
    for (i = 0; i < width; i++) {
        int c = (unsigned char)s[i], d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else return -1;
        v = v * 36 + d;
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Command descriptor table.                                           */
/*                                                                     */
/* Each entry is a level prefix ("" for level 0), the command type      */
/* character, and an argument layout string:                           */
/*                                                                     */
/*   '1'-'9'  one MegaNum argument of that many characters             */
/*   'V'      npoints:2 followed by npoints (x:2, y:2) pairs           */
/*   'T'      a text argument running to the end of the command        */
/*                                                                     */
/* Every command in 1.54 is listed, including the ones this phase does */
/* not draw, because knowing an argument layout is what lets the       */
/* parser step over a command it cannot render and still find the next */
/* command on the same line.                                           */
/* ------------------------------------------------------------------ */

typedef struct RipCmdDesc {
    const char *level;
    char        cmd;
    const char *args;
} RipCmdDesc;

#define RIP_ESC  '\x1b'

static const RipCmdDesc RIP_CMDS[] = {
    /* --- Level 0 --------------------------------------------------- */
    { "", 'w',  "222211"           },  /* TEXT_WINDOW                  */
    { "", 'v',  "2222"             },  /* VIEWPORT                     */
    { "", '*',  ""                 },  /* RESET_WINDOWS                */
    { "", 'e',  ""                 },  /* ERASE_WINDOW                 */
    { "", 'E',  ""                 },  /* ERASE_VIEW                   */
    { "", 'g',  "22"               },  /* GOTOXY                       */
    { "", 'H',  ""                 },  /* HOME                         */
    { "", '>',  ""                 },  /* ERASE_EOL                    */
    { "", 'c',  "2"                },  /* COLOR                        */
    { "", 'Q',  "2222222222222222" },  /* SET_PALETTE (16 entries)     */
    { "", 'a',  "22"               },  /* ONE_PALETTE                  */
    { "", 'W',  "2"                },  /* WRITE_MODE                   */
    { "", 'm',  "22"               },  /* MOVE                         */
    { "", 'T',  "T"                },  /* TEXT                         */
    { "", '@',  "22T"              },  /* TEXT_XY                      */
    { "", 'Y',  "2222"             },  /* FONT_STYLE                   */
    { "", 'X',  "22"               },  /* PIXEL                        */
    { "", 'L',  "2222"             },  /* LINE                         */
    { "", 'R',  "2222"             },  /* RECTANGLE                    */
    { "", 'B',  "2222"             },  /* BAR                          */
    { "", 'C',  "222"              },  /* CIRCLE                       */
    { "", 'O',  "222222"           },  /* OVAL                         */
    { "", 'o',  "2222"             },  /* FILLED_OVAL                  */
    { "", 'A',  "22222"            },  /* ARC                          */
    { "", 'V',  "222222"           },  /* OVAL_ARC                     */
    { "", 'I',  "22222"            },  /* PIE_SLICE                    */
    { "", 'i',  "222222"           },  /* OVAL_PIE_SLICE               */
    { "", 'Z',  "222222222"        },  /* BEZIER                       */
    { "", 'P',  "V"                },  /* POLYGON                      */
    { "", 'p',  "V"                },  /* FILL_POLYGON                 */
    { "", 'l',  "V"                },  /* POLYLINE            (v1.54)  */
    { "", 'F',  "222"              },  /* FILL                         */
    { "", '=',  "242"              },  /* LINE_STYLE                   */
    { "", 'S',  "22"               },  /* FILL_STYLE                   */
    { "", 's',  "222222222"        },  /* FILL_PATTERN                 */
    { "", '#',  ""                 },  /* NO_MORE                      */

    /* --- Level 1 --------------------------------------------------- */
    { "1", 'M', "22222115T"        },  /* MOUSE                        */
    { "1", 'K', ""                 },  /* KILL_MOUSE_FIELDS            */
    { "1", 'T', "22222"            },  /* BEGIN_TEXT                   */
    { "1", 't', "1T"               },  /* REGION_TEXT                  */
    { "1", 'E', ""                 },  /* END_TEXT                     */
    { "1", 'C', "22221"            },  /* GET_IMAGE                    */
    { "1", 'P', "2221"             },  /* PUT_IMAGE                    */
    { "1", 'W', "1T"               },  /* WRITE_ICON                   */
    { "1", 'I', "22212T"           },  /* LOAD_ICON                    */
    /* BUTTON_STYLE: wid:2 hgt:2 orient:2 flags:4 size:2 dfore:2 dback:2
     * bright:2 dark:2 surface:2 grp_no:2 flags2:2 uline_col:2
     * corner_col:2 res:6, which is 36 characters. An earlier phase had
     * this at 12 and silently skipped the other 24 as trailing text. */
    { "1", 'B', "222422222222226"  },  /* BUTTON_STYLE                 */
    { "1", 'U', "2222211T"         },  /* BUTTON                       */
    { "1", 'D', "32T"              },  /* DEFINE                       */
    { "1", 'G', "222222"           },  /* COPY_REGION                  */
    { "1", 'R', "8T"               },  /* READ_SCENE                   */
    { "1", 'F', "24T"              },  /* FILE_QUERY                   */
    { "1", RIP_ESC, "13T"          },  /* QUERY                        */

    /* --- Level 9 (system) ------------------------------------------ */
    { "9", RIP_ESC, "1124"         }   /* ENTER_BLOCK_MODE (Zmodem)    */
};

static const RipCmdDesc *rip_find_cmd(const char *level, char cmd)
{
    size_t i;
    for (i = 0; i < sizeof RIP_CMDS / sizeof RIP_CMDS[0]; i++)
        if (RIP_CMDS[i].cmd == cmd && strcmp(RIP_CMDS[i].level, level) == 0)
            return &RIP_CMDS[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

void rip_parser_reset(RipParser *p)
{
    if (!p) return;
    p->draw_color   = 15;
    p->fill_pattern = 1;    /* solid */
    p->fill_color   = 15;
    p->line_style   = 0;    /* solid */
    p->line_thick   = 1;
    p->font         = 0;    /* default 8x8 bitmap font */
    p->font_dir     = RIP_DIR_HORIZ;
    p->font_size    = 1;
    p->cur_x        = 0;
    p->cur_y        = 0;
    /* 1.54 RIP_RESET_WINDOWS: "all Mouse Regions and Mouse Buttons are
     * deleted". This function is that command's handler, so the scene's
     * regions go with it. */
    p->button_count = 0;
    rip_button_style_default(&p->btn_style);
    if (p->surf) rip_ega_reset(p->surf);
}

/* Commands parsed but not rendered. A RIP_BUTTON counts here only when
 * its style is one we cannot construct (Icon or Clipboard artwork); a
 * plain button that we draw does not, and a RIP_MOUSE region never does
 * because it is invisible by definition. Downloads that did not
 * complete are counted too, which since Phase 4 means a transfer that
 * failed, was refused or was cancelled rather than one held back. */
unsigned long rip_stats_deferred(const RipStats *st)
{
    if (!st) return 0;
    return st->level1_cmds + st->bezier_cmds + st->flood_cmds +
           st->tty_cmds + st->button_undrawn + st->download_blocked;
}

/* The service prefixes every issue-download host command with this
 * verb, e.g. "RGETgt114". See rip_parser.h for why this check exists. */
#define RIP_DOWNLOAD_VERB  "RGET"

int rip_command_is_download(const char *cmd)
{
    if (!cmd) return 0;
    return strncmp(cmd, RIP_DOWNLOAD_VERB,
                   sizeof RIP_DOWNLOAD_VERB - 1) == 0;
}

/* ------------------------------------------------------------------ */
/* Level-1 clickable regions                                           */
/*                                                                     */
/* Two commands feed one table. RIP_MOUSE (1M) is the invisible hotspot */
/* the service puts over each icon; RIP_BUTTON (1U) carries a hotkey    */
/* and, on this service, is also used with a zero-size rectangle parked */
/* off in the bottom-right corner purely to register a hotkey with no   */
/* visible region. Both send their text to the host when triggered.     */
/* ------------------------------------------------------------------ */

/* Expand the caret escapes 1.54 allows in a host command string: a
 * caret plus a character means that control character, so "RWX^M" ends
 * in a real carriage return. A doubled caret is a literal caret. */
static void rip_expand_caret(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (cap == 0) return;
    while (*src && o + 1 < cap) {
        if (src[0] == '^' && src[1]) {
            if (src[1] == '^') {
                dst[o++] = '^';
            } else {
                int c = (unsigned char)src[1];
                if (c >= 'a' && c <= 'z') c -= 32;
                dst[o++] = (char)(c & 0x1F);
            }
            src += 2;
            continue;
        }
        dst[o++] = *src++;
    }
    dst[o] = '\0';
}

static void rip_copy_field(char *dst, size_t cap, const char *src, size_t n)
{
    if (cap == 0) return;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Split "icon<>label<>command" into its three fields. Fields the string
 * does not reach come back empty; on this service the icon field is
 * always empty and the label is empty for the hotkey-only buttons. */
static void rip_split3(const char *s,
                       char *a, size_t an,
                       char *b, size_t bn,
                       char *c, size_t cn)
{
    const char *start[3];
    size_t len[3];
    const char *p = s;
    int n = 0;

    start[0] = start[1] = start[2] = s;
    len[0] = len[1] = len[2] = 0;

    while (*p && n < 2) {
        if (p[0] == '<' && p[1] == '>') {
            len[n] = (size_t)(p - start[n]);
            n++;
            p += 2;
            start[n] = p;
        } else {
            p++;
        }
    }
    while (*p) p++;
    len[n] = (size_t)(p - start[n]);

    rip_copy_field(a, an, start[0], len[0]);
    rip_copy_field(b, bn, start[1], len[1]);
    rip_copy_field(c, cn, start[2], len[2]);
}

static void rip_add_region(RipParser *p, const RipButton *src)
{
    RipButton b = *src;
    int t;

    if (p->button_count >= RIP_MAX_BUTTONS) return;
    if (b.command[0] == '\0') return;      /* nothing to send: not useful */

    if (b.x1 < b.x0) { t = b.x0; b.x0 = b.x1; b.x1 = t; }
    if (b.y1 < b.y0) { t = b.y0; b.y0 = b.y1; b.y1 = t; }
    if (b.ix1 < b.ix0) { t = b.ix0; b.ix0 = b.ix1; b.ix1 = t; }
    if (b.iy1 < b.iy0) { t = b.iy0; b.iy0 = b.iy1; b.iy1 = t; }

    p->buttons[p->button_count++] = b;
}

int rip_button_at(const RipParser *p, int x, int y)
{
    int i;
    if (!p) return -1;
    for (i = p->button_count - 1; i >= 0; i--) {   /* last in, first out */
        const RipButton *b = &p->buttons[i];
        if (x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1)
            return i;
    }
    return -1;
}

static int rip_ascii_upper(int c)
{
    return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

int rip_button_by_hotkey(const RipParser *p, int ascii)
{
    int i;
    if (!p || ascii == 0) return -1;
    for (i = 0; i < p->button_count; i++) {
        int hk = p->buttons[i].hotkey;
        if (hk != 0 && rip_ascii_upper(hk) == rip_ascii_upper(ascii))
            return i;
    }
    return -1;
}

void rip_parser_init(RipParser *p, RipEga *surf)
{
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->surf = surf;
    p->at_line_start = 1;
    /* Installed here rather than in the tab so the offline harness
     * renders through exactly the same metrics the tab does. */
    rip_text_metrics_init();
    rip_parser_reset(p);
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

/* Returns non-zero if the command changed pixels. */
static int rip_exec(RipParser *p, const char *level, char cmd,
                    const int *a, int na,
                    const int *pts, int npts,
                    const char *text)
{
    RipEga *s = p->surf;

    if (level[0] == '1') {
        /* The descriptor table has already decoded this command's fixed
         * fields into a[] and its trailing text into `text`, so the
         * region commands read straight out of them.
         *
         *   1U RIP_BUTTON  x0:2 y0:2 x1:2 y1:2 hotkey:2 flags:1 res:1
         *                  then "icon<>label<>command"
         *   1M RIP_MOUSE   num:2 x0:2 y0:2 x1:2 y1:2 clk:1 clr:1 res:5
         *                  then command
         */
        RipButton b;

        switch (cmd) {
        case 'B':                       /* RIP_BUTTON_STYLE */
            if (na < 14) { p->stats.malformed++; return 0; }
            p->btn_style.wid        = a[0];
            p->btn_style.hgt        = a[1];
            p->btn_style.orient     = a[2];
            p->btn_style.flags      = a[3];
            p->btn_style.bevsize    = a[4];
            p->btn_style.dfore      = a[5]  & 0x0F;
            p->btn_style.dback      = a[6]  & 0x0F;
            p->btn_style.bright     = a[7]  & 0x0F;
            p->btn_style.dark       = a[8]  & 0x0F;
            p->btn_style.surface    = a[9]  & 0x0F;
            p->btn_style.grp_no     = a[10];
            p->btn_style.flags2     = a[11];
            p->btn_style.uline_col  = a[12] & 0x0F;
            p->btn_style.corner_col = a[13] & 0x0F;
            /* Defines only; nothing appears on screen for this command. */
            return 0;

        case 'U': {                     /* RIP_BUTTON */
            int drew;
            if (na < 7) { p->stats.malformed++; return 0; }
            memset(&b, 0, sizeof b);
            b.x0 = a[0]; b.y0 = a[1];
            b.x1 = a[2]; b.y1 = a[3];
            b.hotkey = a[4];
            b.flags  = a[5];
            {
                char icon[RIP_BTN_LABEL], raw[RIP_BTN_CMD];
                rip_split3(text, icon, sizeof icon,
                           b.label, sizeof b.label,
                           raw, sizeof raw);
                rip_expand_caret(b.command, sizeof b.command, raw);
            }

            drew = rip_button_draw(s, &p->btn_style,
                                   b.x0, b.y0, b.x1, b.y1,
                                   b.label, b.hotkey,
                                   p->font, p->font_size);

            /* Two rectangles, and they are not the same one. The image
             * rectangle is what was just painted and what inverts on a
             * press; the outer rectangle additionally covers the recess,
             * which is clickable but never inverts. Both are taken
             * before the command's own corners are overwritten. */
            rip_button_image_rect(&p->btn_style, b.x0, b.y0, b.x1, b.y1,
                                  &b.ix0, &b.iy0, &b.ix1, &b.iy1);
            rip_button_outer_rect(&p->btn_style, b.x0, b.y0, b.x1, b.y1,
                                  &b.x0, &b.y0, &b.x1, &b.y1);
            /* 1.54: Invertable "is only useful when combined with the
             * Button is a Mouse Button flag", so both are required. */
            b.invert = ((p->btn_style.flags & RIP_BF_INVERTABLE) &&
                        (p->btn_style.flags & RIP_BF_MOUSE)) ? 1 : 0;
            rip_add_region(p, &b);

            if (drew) {
                p->stats.button_cmds++;
            } else {
                /* Icon or Clipboard artwork we cannot build. Registered
                 * so it still responds, but counted as not rendered. */
                p->stats.button_undrawn++;
            }
            return drew;
        }

        case 'M':                       /* RIP_MOUSE */
            if (na < 7) { p->stats.malformed++; return 0; }
            memset(&b, 0, sizeof b);
            b.x0 = a[1]; b.y0 = a[2];
            b.x1 = a[3]; b.y1 = a[4];
            b.invert = a[5] ? 1 : 0;    /* clk: invert while held */
            /* A mouse region has no chrome, so its image is itself. */
            b.ix0 = b.x0; b.iy0 = b.y0; b.ix1 = b.x1; b.iy1 = b.y1;
            rip_expand_caret(b.command, sizeof b.command, text);
            rip_add_region(p, &b);
            p->stats.region_cmds++;
            return 0;

        case 'K':                       /* RIP_KILL_MOUSE_FIELDS */
            /* How the service separates one screen's regions from the
             * next: it kills the old set before drawing a new scene. */
            p->button_count = 0;
            p->stats.region_cmds++;
            return 0;

        default:
            /* Icons, button styles, images, clipboard and region text
             * are still deferred. Counted, not drawn. */
            p->stats.level1_cmds++;
            return 0;
        }
    }
    if (level[0] == '9') {
        /* Block mode / Zmodem transfer is a later phase. */
        p->stats.level1_cmds++;
        return 0;
    }

    switch (cmd) {

    /* ---- screen and window state ---- */
    case '*':                       /* RIP_RESET_WINDOWS */
        rip_parser_reset(p);
        return 1;

    case 'E':                       /* RIP_ERASE_VIEW */
        rip_ega_erase_view(s);
        return 1;

    case 'v':                       /* RIP_VIEWPORT */
        if (na < 4) { p->stats.malformed++; return 0; }
        rip_ega_set_viewport(s, a[0], a[1], a[2], a[3]);
        return 0;

    case 'W':                       /* RIP_WRITE_MODE */
        if (na < 1) { p->stats.malformed++; return 0; }
        s->write_mode = (a[0] == 1) ? RIP_WMODE_XOR : RIP_WMODE_COPY;
        return 0;

    /* ---- colour and style ---- */
    case 'c':                       /* RIP_COLOR */
        if (na < 1) { p->stats.malformed++; return 0; }
        p->draw_color = a[0] & 0x0F;
        return 0;

    case 'S':                       /* RIP_FILL_STYLE */
        if (na < 2) { p->stats.malformed++; return 0; }
        p->fill_pattern = a[0];
        p->fill_color   = a[1] & 0x0F;
        /* Patterns 02-0B are hatches; they are drawn solid for now. */
        if (p->fill_pattern >= 2 && p->fill_pattern <= 11)
            p->stats.unsup_fill_pat++;
        return 0;

    case '=':                       /* RIP_LINE_STYLE */
        if (na < 3) { p->stats.malformed++; return 0; }
        p->line_style = a[0];
        p->line_thick = a[2];
        if (p->line_style != 0) p->stats.unsup_line_style++;
        return 0;

    case 'Q': {                     /* RIP_SET_PALETTE */
        unsigned char pal[16];
        int i;
        if (na < 16) { p->stats.malformed++; return 0; }
        for (i = 0; i < 16; i++)
            pal[i] = (unsigned char)(a[i] & 0x3F);
        rip_ega_set_palette16(s, pal);
        return 1;                   /* on-screen colours change at once */
    }

    case 'a':                       /* RIP_ONE_PALETTE */
        if (na < 2) { p->stats.malformed++; return 0; }
        rip_ega_set_one_palette(s, a[0], a[1]);
        return 1;

    /* ---- primitives ---- */
    case 'X':                       /* RIP_PIXEL */
        if (na < 2) { p->stats.malformed++; return 0; }
        rip_ega_pixel(s, a[0], a[1], p->draw_color);
        return 1;

    case 'L':                       /* RIP_LINE */
        if (na < 4) { p->stats.malformed++; return 0; }
        rip_ega_line(s, a[0], a[1], a[2], a[3],
                     p->draw_color, p->line_thick);
        return 1;

    case 'R':                       /* RIP_RECTANGLE */
        if (na < 4) { p->stats.malformed++; return 0; }
        rip_ega_rect(s, a[0], a[1], a[2], a[3],
                     p->draw_color, p->line_thick);
        return 1;

    case 'B':                       /* RIP_BAR */
        if (na < 4) { p->stats.malformed++; return 0; }
        rip_ega_bar(s, a[0], a[1], a[2], a[3],
                    p->fill_pattern, p->fill_color);
        return 1;

    case 'P':                       /* RIP_POLYGON */
        if (npts < 2) { p->stats.malformed++; return 0; }
        rip_ega_polygon(s, pts, npts, p->draw_color, p->line_thick);
        return 1;

    case 'p':                       /* RIP_FILL_POLYGON */
        if (npts < 2) { p->stats.malformed++; return 0; }
        rip_ega_fill_polygon(s, pts, npts, p->fill_pattern, p->fill_color);
        rip_ega_polygon(s, pts, npts, p->draw_color, p->line_thick);
        return 1;

    case 'l':                       /* RIP_POLYLINE (v1.54) */
        if (npts < 2) { p->stats.malformed++; return 0; }
        rip_ega_polyline(s, pts, npts, p->draw_color, p->line_thick);
        return 1;

    /* ---- text (Phase 2) ---- */
    case 'Y':                       /* RIP_FONT_STYLE */
        if (na < 3) { p->stats.malformed++; return 0; }
        p->font      = a[0];
        p->font_dir  = (a[1] == 1) ? RIP_DIR_VERT : RIP_DIR_HORIZ;
        p->font_size = a[2];
        if (p->font < 0 || p->font > 10) p->font = 0;
        if (p->font_size < 1) p->font_size = 1;
        if (p->font_size > 10) p->font_size = 10;
        p->stats.font_cmds++;
        return 0;

    case 'T': {                     /* RIP_TEXT, at the current position */
        int adv = rip_text_draw(s, p->font, p->font_dir, p->font_size,
                                p->cur_x, p->cur_y, text, p->draw_color);
        /* 1.54: the drawing position ends immediately right of the text
         * so a following RIP_TEXT continues from there. */
        if (p->font_dir == RIP_DIR_VERT) p->cur_y -= adv;
        else                             p->cur_x += adv;
        p->stats.text_cmds++;
        return 1;
    }

    case '@': {                     /* RIP_TEXT_XY */
        int adv;
        if (na < 2) { p->stats.malformed++; return 0; }
        p->cur_x = a[0];
        p->cur_y = a[1];
        adv = rip_text_draw(s, p->font, p->font_dir, p->font_size,
                            p->cur_x, p->cur_y, text, p->draw_color);
        if (p->font_dir == RIP_DIR_VERT) p->cur_y -= adv;
        else                             p->cur_x += adv;
        p->stats.text_cmds++;
        return 1;
    }

    case 'm':                       /* RIP_MOVE */
        if (na < 2) { p->stats.malformed++; return 0; }
        p->cur_x = a[0];
        p->cur_y = a[1];
        return 0;

    /* ---- curves (Phase 2) ---- */
    case 'C':                       /* RIP_CIRCLE, aspect corrected */
        if (na < 3) { p->stats.malformed++; return 0; }
        rip_ega_circle(s, a[0], a[1], a[2], p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'O':                       /* RIP_OVAL: an elliptical arc */
        if (na < 6) { p->stats.malformed++; return 0; }
        rip_ega_arc(s, a[0], a[1], a[2], a[3], a[4], a[5],
                    p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'o':                       /* RIP_FILLED_OVAL */
        if (na < 4) { p->stats.malformed++; return 0; }
        rip_ega_fill_ellipse(s, a[0], a[1], a[2], a[3],
                             p->fill_pattern, p->fill_color);
        rip_ega_ellipse(s, a[0], a[1], a[2], a[3],
                        p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'A':                       /* RIP_ARC, aspect corrected */
        if (na < 5) { p->stats.malformed++; return 0; }
        rip_ega_arc(s, a[0], a[1], a[2], a[3],
                    a[4], rip_ega_aspect_y(a[4]),
                    p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'V':                       /* RIP_OVAL_ARC */
        if (na < 6) { p->stats.malformed++; return 0; }
        rip_ega_arc(s, a[0], a[1], a[2], a[3], a[4], a[5],
                    p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'I':                       /* RIP_PIE_SLICE, aspect corrected */
        if (na < 5) { p->stats.malformed++; return 0; }
        rip_ega_pie(s, a[0], a[1], a[2], a[3],
                    a[4], rip_ega_aspect_y(a[4]),
                    p->fill_pattern, p->fill_color,
                    p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    case 'i':                       /* RIP_OVAL_PIE_SLICE */
        if (na < 6) { p->stats.malformed++; return 0; }
        rip_ega_pie(s, a[0], a[1], a[2], a[3], a[4], a[5],
                    p->fill_pattern, p->fill_color,
                    p->draw_color, p->line_thick);
        p->stats.arc_cmds++;
        return 1;

    /* ---- parsed, not drawn ---- */
    case 'Z':
        /* RIP_BEZIER stays deferred on purpose: the server never emits
         * it, and 1.54 does not publish the curve's algorithm, so there
         * is nothing to implement clean-room from. */
        p->stats.bezier_cmds++;
        return 0;

    case 'F':                       /* RIP_FILL (flood) */
        p->stats.flood_cmds++;
        return 0;

    case 'w':                       /* RIP_TEXT_WINDOW       */
    case 'e':                       /* RIP_ERASE_WINDOW      */
    case 'g':                       /* RIP_GOTOXY            */
    case 'H':                       /* RIP_HOME              */
    case '>':                       /* RIP_ERASE_EOL         */
    case '#':                       /* RIP_NO_MORE           */
        /* The TTY text window is a later phase. Arguments are consumed
         * correctly, which is what keeps the rest of the line parsing. */
        p->stats.tty_cmds++;
        return 0;

    case 's':                       /* RIP_FILL_PATTERN, custom 8x8 */
        p->stats.unsup_fill_pat++;
        return 0;
    }

    p->stats.unknown_cmds++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* One assembled command line                                          */
/* ------------------------------------------------------------------ */

/* Copy a text argument out of the line, undoing the backslash escapes
 * of rules 11 and 13, stopping at an unescaped '|'. Returns the number
 * of source characters consumed. */
static int rip_take_text(const char *src, int len, char *out, int outsz)
{
    int i = 0, o = 0;
    while (i < len) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[i + 1];
            if (n == '|' || n == '!' || n == '\\') {
                if (o < outsz - 1) out[o++] = n;
                i += 2;
                continue;
            }
        }
        if (c == '|') break;
        if (o < outsz - 1) out[o++] = c;
        i++;
    }
    if (outsz > 0) out[o] = '\0';
    return i;
}

/* Skip a command whose layout we do not know: run to the next unescaped
 * delimiter. */
static int rip_skip_unknown(const char *src, int len)
{
    int i = 0;
    while (i < len) {
        if (src[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (src[i] == '|') break;
        i++;
    }
    return i;
}

/* Parse and execute every command on one line body. `body` starts at
 * the first '|' of the line. Returns non-zero if anything was drawn. */
static int rip_run_line(RipParser *p, const char *body, int len)
{
    int pos = 0, drew = 0;

    while (pos < len) {
        char level[10];
        int  nlevel = 0;
        char cmd;
        int  a[RIP_MAX_NUMERIC_ARGS];
        int  na = 0;
        int  npts = 0;
        int *pts = NULL;
        char text[RIP_LINE_MAX];
        const RipCmdDesc *desc;
        const char *ap;
        int bad = 0;

        if (body[pos] != '|') { pos++; continue; }
        pos++;
        if (pos >= len) break;

        /* Level prefix: leading digits, the first of which is 1-9. */
        if (body[pos] >= '1' && body[pos] <= '9') {
            while (pos < len && nlevel < (int)sizeof level - 1 &&
                   body[pos] >= '0' && body[pos] <= '9')
                level[nlevel++] = body[pos++];
        }
        level[nlevel] = '\0';
        if (pos >= len) { p->stats.malformed++; break; }

        cmd = body[pos++];
        p->stats.commands++;
        text[0] = '\0';

        desc = rip_find_cmd(level, cmd);
        if (!desc) {
            p->stats.unknown_cmds++;
            pos += rip_skip_unknown(body + pos, len - pos);
            continue;
        }

        for (ap = desc->args; *ap && !bad; ap++) {
            if (*ap == 'T') {
                pos += rip_take_text(body + pos, len - pos,
                                     text, (int)sizeof text);
                break;
            }
            if (*ap == 'V') {
                int n, i;
                n = (len - pos >= 2) ? rip_meganum(body + pos, 2) : -1;
                if (n < 0) { bad = 1; break; }
                pos += 2;
                if (n > RIP_MAX_POLY_POINTS) n = RIP_MAX_POLY_POINTS;
                if (len - pos < n * 4) { bad = 1; break; }
                pts = (int *)malloc(sizeof(int) * (size_t)n * 2);
                if (!pts) { bad = 1; break; }
                for (i = 0; i < n * 2; i++) {
                    int v = rip_meganum(body + pos, 2);
                    if (v < 0) { bad = 1; break; }
                    pts[i] = v;
                    pos += 2;
                }
                npts = bad ? 0 : n;
                break;
            }
            {
                int width = *ap - '0';
                int v;
                if (width < 1 || width > 9) { bad = 1; break; }
                if (len - pos < width) { bad = 1; break; }
                v = rip_meganum(body + pos, width);
                if (v < 0) { bad = 1; break; }
                pos += width;
                if (na < RIP_MAX_NUMERIC_ARGS) a[na++] = v;
            }
        }

        if (bad) {
            p->stats.malformed++;
            pos += rip_skip_unknown(body + pos, len - pos);
        } else {
            if (rip_exec(p, level, cmd, a, na, pts, npts, text)) {
                drew = 1;
                p->stats.drawn++;
            }
        }
        free(pts);

        /* Anything trailing before the next delimiter is ignored text
         * (rule 6). */
        pos += rip_skip_unknown(body + pos, len - pos);
    }
    return drew;
}

/* Decide what a completed physical line is and run it. */
static int rip_handle_line(RipParser *p, char *line, int len)
{
    int i;

    /* A command line starts at column 0 with "!|" (rules 1 and 2). */
    if (len >= 2 && line[0] == '!' && line[1] == '|')
        return rip_run_line(p, line + 1, len - 1);

    /* Rule 12: Ctrl-A (SOH) or Ctrl-B (STX) may replace the '!' and may
     * appear in any column. */
    for (i = 0; i + 1 < len; i++) {
        if ((line[i] == '\x01' || line[i] == '\x02') && line[i + 1] == '|')
            return rip_run_line(p, line + i + 1, len - i - 1);
    }

    /* Rule 6: anything else is raw text for the TTY window, which is a
     * later phase. */
    if (len > 0) p->stats.raw_text_lines++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Byte feed and line assembly                                         */
/* ------------------------------------------------------------------ */

/* Count the backslashes ending the buffer. An odd number means the last
 * one is a line continuation (rules 5 and 13). */
static int rip_trailing_backslashes(const char *s, int len)
{
    int n = 0;
    while (n < len && s[len - 1 - n] == '\\') n++;
    return n;
}

int rip_parser_feed(RipParser *p, const unsigned char *data, int len)
{
    int i, drew = 0;
    if (!p || !data) return 0;

    for (i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\r' || c == '\n') {
            /* CR, LF and CRLF all end one physical line. The LF of a
             * CRLF pair must be swallowed explicitly rather than left to
             * the empty-line guard below: when the CR ended a line that
             * was a backslash continuation (rule 5) the buffer is not
             * empty, so the guard would let the LF terminate the very
             * line the backslash asked to continue. That splits long
             * wrapped commands, which is exactly what the weather map's
             * multi-line fill polygons are. */
            if (c == '\n' && p->last_was_cr) { p->last_was_cr = 0; continue; }
            p->last_was_cr = (c == '\r');

            if (p->line_len == 0 && !p->continued) continue;

            if (rip_trailing_backslashes(p->line, p->line_len) % 2 == 1) {
                /* Continuation: drop the backslash, keep the buffer. */
                p->line_len--;
                p->continued = 1;
                continue;
            }
            p->line[p->line_len] = '\0';
            if (rip_handle_line(p, p->line, p->line_len)) drew = 1;
            p->line_len = 0;
            p->continued = 0;
            continue;
        }

        p->last_was_cr = 0;

        if (p->line_len < RIP_LINE_MAX - 1) {
            p->line[p->line_len++] = c;
        } else {
            /* Overlong logical line: cut it here rather than lose the
             * stream. Counted so it is visible. */
            p->line[p->line_len] = '\0';
            if (rip_handle_line(p, p->line, p->line_len)) drew = 1;
            p->stats.malformed++;
            p->line_len = 0;
            p->continued = 0;
        }
    }
    return drew;
}
