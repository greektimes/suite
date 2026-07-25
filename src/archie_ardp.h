/*
 * archie_ardp.h - Prospero/ARDP Archie client transport (shared service).
 *
 * A clean-room implementation of the client half of the Prospero ARDP +
 * VFS wire protocol that period Archie clients (WSArchie) speak over
 * UDP 1525, written against docs/archie-ref/.../spec/WIRE_SPEC.md. The
 * Suite's live Archie server already accepts this exact wire shape.
 *
 * This unit is a self-contained transport + reply parser with no UI or
 * module dependencies, so any module could drive an Archie query through
 * it (the same pool-independence discipline the server side follows and
 * the WAIS transport/UI split the Suite already uses). The Archie tab in
 * archie_module.c is only its first consumer.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef ARCHIE_ARDP_H
#define ARCHIE_ARDP_H

/* Winsock2 must precede windows.h; win_platform.h owns that ordering and
 * is included by the .c. This header stays platform-neutral (plain C). */

#define ARCHIE_DEFAULT_SERVER  "archie.greektimes.ca"
#define ARCHIE_DEFAULT_PORT    1525

/* ARDP client defaults, per WIRE_SPEC.md "Client defaults: 4s timeout,
 * 3 retries; ARDP_PTXT_LEN payload cap 1250." */
#define ARCHIE_TIMEOUT_MS      4000
#define ARCHIE_RETRIES         3
#define ARCHIE_PTXT_LEN        1250

/* maxhits clamps to 1..2500 per the server (arch_dsdb.c). */
#define ARCHIE_MAXHITS_MIN     1
#define ARCHIE_MAXHITS_MAX     2500
#define ARCHIE_MAXHITS_DEFAULT 100

/* Base search type. Combined with the exact_first modifier (see
 * archie_query) it maps to the hsoname MATCH type char, canonical from
 * xarchie types.h / WIRE_SPEC.md:
 *     type            exact_first=0   exact_first=1
 *     SUBSTR_CI            'S'             's'
 *     SUBSTR_CS            'C'             'c'
 *     REGEX               'R'             'r'
 *     EXACT               '='          (no exact-first form)
 * Regex is case-insensitive server-side regardless of the case radios. */
typedef enum {
    ARCHIE_SUBSTR_CI = 0,   /* substring, case insensitive (default) */
    ARCHIE_SUBSTR_CS,       /* substring, case sensitive              */
    ARCHIE_EXACT,           /* exact match                            */
    ARCHIE_REGEX            /* regular expression                     */
} archie_search_type_t;

/* Outcome of a query. ARCHIE_OK covers both hits and a clean zero-match
 * NONE-FOUND reply (which is success on the wire, not an error). */
typedef enum {
    ARCHIE_OK = 0,
    ARCHIE_ERR_DNS,         /* server name did not resolve                */
    ARCHIE_ERR_SOCKET,      /* socket/sendto failure                      */
    ARCHIE_ERR_TIMEOUT,     /* no (complete) reply after all retries      */
    ARCHIE_ERR_SERVER,      /* ERROR / FAILURE / VERSION-NOT-SUPPORTED    */
    ARCHIE_ERR_NOMEM,
    ARCHIE_ERR_PROTOCOL     /* malformed reply framing                    */
} archie_status_t;

/* One file hit, fully resolved for handing to FTP. host is the FTP host,
 * hsoname the full path on it; dir/filename are hsoname split at the last
 * '/'. size/mode/date come from the LINK-INFO attribute lines (empty if
 * the server did not send that attribute). */
typedef struct {
    char name[256];     /* LINK <name> field as sent                    */
    char host[256];     /* FTP host (e.g. ftp.greektimes.ca)            */
    char hsoname[512];  /* full path on host (e.g. /pub/HELLO.txt)      */
    char dir[512];      /* dirname(hsoname)                             */
    char filename[256]; /* basename(hsoname)                            */
    char size[64];      /* SIZE value, digits only (e.g. "68")          */
    char mode[48];      /* UNIX-MODES value (e.g. -rw-r--r--)           */
    char date[96];      /* LAST-MODIFIED value                          */
} archie_hit_t;

typedef struct {
    archie_status_t status;
    char            message[256];  /* human-readable status/error line   */
    archie_hit_t   *hits;          /* malloc'd array, hit_count entries   */
    int             hit_count;
    int             none_found;    /* 1 if the server returned NONE-FOUND */
    char            warning[256];  /* first WARNING line, minus the keyword;
                                    * empty when the server sent none. A
                                    * warning is not an error and does not
                                    * suppress hits, but discarding it
                                    * silently is its own small defect. */
} archie_result_t;

/* Blocking Archie MATCH query. Intended to be called off the UI thread.
 * Resolves `server`, sends a VERSION 1 MATCH request over UDP `port`,
 * reassembles the (possibly multi-packet) reply, and parses file hits
 * into *out. On ARCHIE_OK the caller owns out->hits and must release it
 * with archie_result_free. Returns out->status for convenience.
 *
 * Winsock must already be initialized (the Suite calls WSAStartup once in
 * WinMain). Thread-safe: keeps no global state. */
archie_status_t archie_query(const char *server, int port,
                             const char *term, archie_search_type_t type,
                             int exact_first, int maxhits,
                             archie_result_t *out);

/* Release the hit array held by a result filled by archie_query. Safe to
 * call on a zeroed/failed result. Does not free the result struct itself. */
void archie_result_free(archie_result_t *out);

#endif /* ARCHIE_ARDP_H */
