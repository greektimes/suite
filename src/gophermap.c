/*
 * gophermap.c - Parser for the line-oriented "gophermap" reply.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "gophermap.h"

#include <stdlib.h>
#include <string.h>

#define GOPHERMAP_INITIAL_CAP 64

static char *malloc_copy(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void gophermap_free(GopherItem *items, int count)
{
    int i;
    if (!items) return;
    for (i = 0; i < count; i++) {
        free(items[i].display);
        free(items[i].selector);
        free(items[i].host);
    }
    free(items);
}

/* Append one item to a dynamic array. Returns 0 on success. */
static int items_push(GopherItem **arr, int *count, int *cap, GopherItem *src)
{
    if (*count >= *cap) {
        int new_cap = (*cap) * 2;
        GopherItem *new_arr;
        if (new_cap <= 0) new_cap = GOPHERMAP_INITIAL_CAP;
        new_arr = (GopherItem *)realloc(*arr, (size_t)new_cap * sizeof(GopherItem));
        if (!new_arr) return 1;
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[*count] = *src;
    (*count)++;
    return 0;
}

int gophermap_parse(const char *buf, size_t len, GopherItem **out_items)
{
    GopherItem *items = NULL;
    int         count = 0;
    int         cap   = 0;
    size_t      i     = 0;

    if (!out_items) return 0;
    *out_items = NULL;
    if (!buf || len == 0) return 0;

    items = (GopherItem *)malloc(GOPHERMAP_INITIAL_CAP * sizeof(GopherItem));
    if (!items) return 0;
    cap = GOPHERMAP_INITIAL_CAP;

    while (i < len) {
        size_t       line_start = i;
        size_t       line_end;
        size_t       line_len;
        const char  *line;
        GopherItem   it;
        const char  *t1, *t2, *t3;
        const char  *display_start, *display_end;
        size_t       j;

        /* Find end of line (\n). */
        while (i < len && buf[i] != '\n') i++;
        line_end = i;
        if (i < len) i++;   /* skip \n */

        /* Trim trailing \r. */
        if (line_end > line_start && buf[line_end - 1] == '\r')
            line_end--;

        line_len = line_end - line_start;
        line     = buf + line_start;

        if (line_len == 0) continue;
        /* Lone "." terminator: stop. */
        if (line_len == 1 && line[0] == '.') break;

        memset(&it, 0, sizeof(it));
        it.port = 70;

        /* Type char is the first byte. If the line has no tabs at all
         * (no protocol fields), it's a malformed informational line:
         * treat as type 'i' with the entire line as display. */
        {
            int has_tab = 0;
            for (j = 0; j < line_len; j++) {
                if (line[j] == '\t') { has_tab = 1; break; }
            }
            if (!has_tab) {
                it.type     = 'i';
                it.display  = malloc_copy(line, line_len);
                it.selector = malloc_copy("", 0);
                it.host     = malloc_copy("", 0);
                if (!it.display || !it.selector || !it.host) {
                    free(it.display); free(it.selector); free(it.host);
                    gophermap_free(items, count);
                    return 0;
                }
                if (items_push(&items, &count, &cap, &it)) {
                    gophermap_free(items, count);
                    return 0;
                }
                continue;
            }
        }

        /* Standard shape: <type><display>\t<selector>\t<host>\t<port> */
        it.type       = line[0];
        display_start = line + 1;

        t1 = NULL; t2 = NULL; t3 = NULL;
        for (j = 1; j < line_len; j++) {
            if (line[j] == '\t') {
                if      (!t1) t1 = line + j;
                else if (!t2) t2 = line + j;
                else if (!t3) { t3 = line + j; break; }
            }
        }

        display_end = t1 ? t1 : (line + line_len);
        it.display  = malloc_copy(display_start,
                                  (size_t)(display_end - display_start));

        if (t1) {
            const char *sel_start = t1 + 1;
            const char *sel_end   = t2 ? t2 : (line + line_len);
            it.selector = malloc_copy(sel_start, (size_t)(sel_end - sel_start));
        } else {
            it.selector = malloc_copy("", 0);
        }

        if (t2) {
            const char *host_start = t2 + 1;
            const char *host_end   = t3 ? t3 : (line + line_len);
            it.host = malloc_copy(host_start, (size_t)(host_end - host_start));
        } else {
            it.host = malloc_copy("", 0);
        }

        if (t3) {
            const char *port_start = t3 + 1;
            const char *port_end   = line + line_len;
            char        port_buf[16];
            size_t      pn = (size_t)(port_end - port_start);
            int         p;
            if (pn >= sizeof(port_buf)) pn = sizeof(port_buf) - 1;
            memcpy(port_buf, port_start, pn);
            port_buf[pn] = '\0';
            p = atoi(port_buf);
            if (p > 0 && p <= 65535) it.port = p;
        }

        if (!it.display || !it.selector || !it.host) {
            free(it.display); free(it.selector); free(it.host);
            gophermap_free(items, count);
            return 0;
        }
        if (items_push(&items, &count, &cap, &it)) {
            gophermap_free(items, count);
            return 0;
        }
    }

    *out_items = items;
    return count;
}
