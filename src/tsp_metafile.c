/*
 * tsp_metafile.c - See tsp_metafile.h.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "tsp_metafile.h"

#include <stdlib.h>
#include <string.h>

#define TSP_PREFIX     "TSIP>>"
#define TSP_PREFIX_LEN 6
#define TSP_SCHEME     "http://"
#define TSP_SCHEME_LEN 7

static int tsp_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n'
        || c == '\f' || c == '\v';
}

char *tsp_metafile_resolve(const char *data, size_t len)
{
    const char *start;
    const char *end;
    const char *url;
    size_t      url_len;
    char       *result;

    if (!data || len == 0) return NULL;

    /* Trim leading whitespace (covers a stray BOM-less indent and any
     * CR/LF). */
    start = data;
    end   = data + len;
    while (start < end && tsp_is_space(*start)) start++;

    /* Trim trailing whitespace, including the LF terminator and a
     * defensive CR from a CRLF historical file. */
    while (end > start && tsp_is_space(*(end - 1))) end--;

    if (end - start < TSP_PREFIX_LEN) return NULL;

    /* Case-sensitive "TSIP>>" prefix check. */
    if (memcmp(start, TSP_PREFIX, TSP_PREFIX_LEN) != 0) return NULL;

    url = start + TSP_PREFIX_LEN;
    /* The host/path may itself have leading spaces in a sloppy file. */
    while (url < end && tsp_is_space(*url)) url++;
    url_len = (size_t)(end - url);
    if (url_len == 0) return NULL;

    /* Prepend "http://" since libwww here is HTTP-only and the 1997
     * pointer carries no scheme. */
    result = (char *)malloc(TSP_SCHEME_LEN + url_len + 1);
    if (!result) return NULL;
    memcpy(result, TSP_SCHEME, TSP_SCHEME_LEN);
    memcpy(result + TSP_SCHEME_LEN, url, url_len);
    result[TSP_SCHEME_LEN + url_len] = '\0';
    return result;
}
