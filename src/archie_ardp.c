/*
 * archie_ardp.c - Prospero/ARDP Archie client transport (shared service).
 *
 * Clean-room implementation of the client half of the Prospero ARDP + VFS
 * wire protocol, written from docs/archie-ref/.../spec/WIRE_SPEC.md (the
 * single source of truth for every byte offset) and cross-checked against
 * the reference client shape in scripts/archie-selftest.sh. No vintage
 * Archie or ISI Prospero source is compiled or copied here.
 *
 * Wire summary (WIRE_SPEC.md, UDP 1525):
 *   Request : 9-octet v0 binary header [hdrlen=9, CID(2), seq(2), total(2),
 *             rcvd-through(2)] followed by an ASCII, newline-delimited
 *             payload:
 *                 VERSION 1 <swid>
 *                 AUTHENTICATOR UNAUTHENTICATED <user>
 *                 DIRECTORY ASCII ARCHIE/MATCH(<maxhits>,0,<type>)/<term>
 *                 LIST ATTRIBUTES COMPONENTS
 *             The empty COMPONENTS list makes the server enumerate the
 *             match directory (it supplies the '*' all-results subname).
 *   Reply   : a VERSION 1 client gets CID forced to 0, so a single-packet
 *             reply is a 1-octet \x01 header + payload; multi-packet
 *             replies use a 7-octet \x07,cid,seq,total header per packet
 *             and are reassembled by seq (1-based) up to total. Completion
 *             is the ARDP total-packet count, never a text EOF token.
 *   Body    : "LINK L <target> <name> <hosttype> <host> <hsonametype>
 *             <hsoname> <version> <magic>" lines, each optionally followed
 *             by "LINK-INFO CACHED <aname> ASCII <value>" attribute lines.
 *             Fields use Prospero single-quote quoting ('' -> literal ').
 *             Zero matches is the line "NONE-FOUND" (success, not error).
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "archie_ardp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Request Connection ID, fresh per query.
 *
 * This used to be a fixed 0x4D47 ('M','G'). That was safe for us, because a
 * VERSION 1 server forces the reply CID to 0 and every query opens its own
 * socket, but it was not safe for the SERVER: the reference Prospero doneQ
 * duplicate cache matches an incoming request on (CID, peer) alone
 * (ardp_accept.c:363) and then discards it (:415), so a server that
 * implemented the reference cache faithfully would have answered every
 * search after the first with the first search's results. We were the only
 * client on the network reusing a CID, and therefore the only reason that
 * cache could not be adopted. Every other client allocates per request;
 * xarchie does it with next_conn_id++ at dirsend.c:495.
 *
 * Retransmits WITHIN one query still reuse the same value: that is the
 * duplicate-resend path and the server is meant to recognise it. */
static unsigned short archie_next_cid(void)
{
    static volatile LONG counter = 0;
    LONG v;

    if (counter == 0) {
        /* Seed once from the tick count so two runs of the Suite do not
         * start from the same place. xarchie seeds from rand() for the
         * same reason. */
        InterlockedCompareExchange(&counter,
                                   (LONG)(GetTickCount() & 0x7FFFu) + 1, 0);
    }
    v = InterlockedIncrement(&counter);
    v &= 0xFFFF;
    if (v == 0) v = 1;                 /* 0 is reserved on the wire */
    return (unsigned short)v;
}

/* Reassembly ceiling. 1024 packets * 1250 payload bytes = ~1.25 MB, far
 * above any real Archie result set (maxhits caps at 2500 hits). A reply
 * claiming more packets than this is treated as a protocol error rather
 * than trusted into a huge allocation. */
#define ARCHIE_MAX_PKTS 1024

/* Software id sent on the VERSION line. The server reads only the integer
 * version (atoi past "VERSION "), so the trailing id is informational and
 * matches how a real WSArchie announces itself. */
#define ARCHIE_SWID "MGT-Unicorn-Archie/1.0"

/* ------------------------------------------------------------------ */
/* Byte helpers (network order, matching the server's bput16/bget16).  */
/* ------------------------------------------------------------------ */

static void put16(unsigned char *b, unsigned v)
{
    b[0] = (unsigned char)((v >> 8) & 0xff);
    b[1] = (unsigned char)(v & 0xff);
}

static unsigned get16(const unsigned char *b)
{
    return ((unsigned)b[0] << 8) | (unsigned)b[1];
}

/* ------------------------------------------------------------------ */
/* Reply field tokenizer with Prospero single-quote quoting.           */
/* Skips leading blanks; a field beginning with ' is quoted and ends   */
/* at the next lone ' (a doubled '' inside is one literal '). Returns   */
/* 1 if a field was read, 0 at end of line. Advances *pp past it.       */
/* ------------------------------------------------------------------ */

static int next_field(const char **pp, char *out, int outsz)
{
    const char *p = *pp;
    int oi = 0;

    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') { *pp = p; if (outsz > 0) out[0] = '\0'; return 0; }

    if (*p == '\'') {
        p++;                                   /* opening quote */
        for (;;) {
            char c = *p;
            if (c == '\0') break;              /* unterminated: stop here */
            if (c == '\'') {
                if (p[1] == '\'') {            /* '' -> literal ' */
                    if (oi < outsz - 1) out[oi++] = '\'';
                    p += 2;
                    continue;
                }
                p++;                            /* closing quote */
                break;
            }
            if (oi < outsz - 1) out[oi++] = c;
            p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            if (oi < outsz - 1) out[oi++] = *p;
            p++;
        }
    }
    if (outsz > 0) out[oi] = '\0';
    *pp = p;
    return 1;
}

/* An unquoted literal "NULL" field means "no value" per the spec. */
static void denull(char *s)
{
    if (strcmp(s, "NULL") == 0) s[0] = '\0';
}

static void copy_bounded(char *dst, int dsz, const char *src)
{
    int i = 0;
    if (dsz <= 0) return;
    for (; src[i] && i < dsz - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* Split hsoname at the last '/' into dir + basename. A leading-slash file
 * such as /HELLO.txt yields dir "/" and file "HELLO.txt". */
static void split_path(const char *hsoname,
                       char *dir, int dsz, char *file, int fsz)
{
    const char *slash = strrchr(hsoname, '/');
    dir[0] = '\0';
    file[0] = '\0';
    if (slash) {
        int dl = (int)(slash - hsoname);
        if (dl <= 0) {
            copy_bounded(dir, dsz, "/");
        } else {
            if (dl > dsz - 1) dl = dsz - 1;
            memcpy(dir, hsoname, (size_t)dl);
            dir[dl] = '\0';
        }
        copy_bounded(file, fsz, slash + 1);
    } else {
        copy_bounded(file, fsz, hsoname);
    }
}

/* SIZE arrives as "<n> bytes"; keep the leading integer. If no digits are
 * present, fall back to the whole trimmed value so nothing is lost. */
static void size_value(const char *val, char *out, int outsz)
{
    int oi = 0;
    const char *p = val;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9' && oi < outsz - 1) out[oi++] = *p++;
    out[oi] = '\0';
    if (oi == 0) copy_bounded(out, outsz, val);
}

/* ------------------------------------------------------------------ */
/* Reply body parser: LINK / LINK-INFO / status lines -> hit array.    */
/* ------------------------------------------------------------------ */

/* One attribute, from either dialect's attribute line, into a hit. */
static void archie_store_attr(archie_hit_t *h, const char *aname,
                              const char *value)
{
    if (strcmp(aname, "SIZE") == 0) {
        size_value(value, h->size, (int)sizeof h->size);
    } else if (strcmp(aname, "UNIX-MODES") == 0) {
        const char *r = value;
        char m[48];
        if (next_field(&r, m, sizeof m))
            copy_bounded(h->mode, (int)sizeof h->mode, m);
    } else if (strcmp(aname, "LAST-MODIFIED") == 0) {
        copy_bounded(h->date, (int)sizeof h->date, value);
    }
}


static archie_status_t parse_body(const char *body, int blen,
                                  archie_result_t *out)
{
    const char *p    = body;
    const char *pend = body + blen;
    archie_hit_t *hits = NULL;
    int   count = 0, cap = 0, cur = -1;
    int   saw_server_error = 0;
    char  err_line[256];

    err_line[0] = '\0';

    while (p < pend) {
        const char *nl = memchr(p, '\n', (size_t)(pend - p));
        int linelen = nl ? (int)(nl - p) : (int)(pend - p);
        char line[1400];
        const char *lp;
        char f[10][512];
        int  nf = 0;

        if (linelen >= (int)sizeof line) linelen = (int)sizeof line - 1;
        memcpy(line, p, (size_t)linelen);
        line[linelen] = '\0';
        while (linelen && (line[linelen - 1] == '\r' ||
                           line[linelen - 1] == ' '))
            line[--linelen] = '\0';
        p = nl ? nl + 1 : pend;
        if (linelen == 0) continue;

        if (strncmp(line, "ATTRIBUTE ", 10) == 0) {
            /* VERSION 5 attribute line. The v1 form is
             *     LINK-INFO <prec> <aname> ASCII <value...>
             * and the v5 form carries an extra "nature" field and a
             * different value type:
             *     ATTRIBUTE <prec> <nature> <aname> SEQUENCE <value...>
             * Same three attributes matter to us, so the value handling is
             * shared with the LINK-INFO branch below. Before this existed
             * the v5 path silently lost every size, mode and date. */
            const char *q = line + 10;
            char prec[64], nature[64], aname[64], atype[16];
            if (cur < 0) continue;
            if (!next_field(&q, prec,   sizeof prec))   continue;
            if (!next_field(&q, nature, sizeof nature)) continue;
            if (!next_field(&q, aname,  sizeof aname))  continue;
            if (!next_field(&q, atype,  sizeof atype))  continue;  /* SEQUENCE */
            while (*q == ' ') q++;                                 /* value   */
            archie_store_attr(&hits[cur], aname, q);
            continue;
        }

        if (strncmp(line, "LINK-INFO ", 10) == 0) {
            /* LINK-INFO <prec> <aname> ASCII <value...>  (value is raw). */
            const char *q = line + 10;
            char prec[64], aname[64], atype[16];
            if (cur < 0) continue;
            if (!next_field(&q, prec,  sizeof prec))  continue;
            if (!next_field(&q, aname, sizeof aname)) continue;
            if (!next_field(&q, atype, sizeof atype)) continue;   /* ASCII */
            while (*q == ' ') q++;                                 /* value */
            archie_store_attr(&hits[cur], aname, q);
            continue;
        }

        if (strncmp(line, "LINK ", 5) == 0) {
            /* LINK L <target> <name> <hosttype> <host> <hsonametype>
             *      <hsoname> <version> <magic> */
            lp = line;
            while (nf < 10 && next_field(&lp, f[nf], (int)sizeof f[nf])) nf++;
            if (nf < 8) continue;                 /* need through hsoname   */
            if (strcmp(f[1], "L") != 0) continue; /* linktype must be L     */
            denull(f[2]); denull(f[3]); denull(f[5]); denull(f[7]);
            if (strcmp(f[2], "EXTERNAL") != 0) continue;  /* file hits only */
            if (f[7][0] == '\0') continue;                /* need a path    */

            if (count == cap) {
                int ncap = cap ? cap * 2 : 32;
                archie_hit_t *nh =
                    (archie_hit_t *)realloc(hits,
                                            (size_t)ncap * sizeof(archie_hit_t));
                if (!nh) { free(hits); return ARCHIE_ERR_NOMEM; }
                hits = nh;
                cap = ncap;
            }
            memset(&hits[count], 0, sizeof(archie_hit_t));
            copy_bounded(hits[count].name,    (int)sizeof hits[count].name,    f[3]);
            copy_bounded(hits[count].host,    (int)sizeof hits[count].host,    f[5]);
            copy_bounded(hits[count].hsoname, (int)sizeof hits[count].hsoname, f[7]);
            split_path(f[7],
                       hits[count].dir,      (int)sizeof hits[count].dir,
                       hits[count].filename, (int)sizeof hits[count].filename);
            cur = count;
            count++;
            continue;
        }

        if (strncmp(line, "NONE-FOUND", 10) == 0) {
            out->none_found = 1;
            continue;
        }
        if (strncmp(line, "ERROR", 5) == 0 ||
            strncmp(line, "FAILURE", 7) == 0 ||
            strncmp(line, "VERSION-NOT-SUPPORTED", 21) == 0) {
            if (!saw_server_error) {
                copy_bounded(err_line, (int)sizeof err_line, line);
                saw_server_error = 1;
            }
            continue;
        }
        if (strncmp(line, "WARNING ", 8) == 0) {
            /* A server warning is not an error and must not suppress the
             * results, but discarding it silently is its own small defect.
             * The v1 path ends a broad search with
             *   WARNING OUT-OF-DATE Some information you wanted can't be
             *   sent. Upgrade to Prospero v5.
             * which is the server telling us it withheld a record. Keep the
             * first one so the UI can show it alongside the hits. Now that
             * we announce VERSION 5 this line should no longer arrive; if it
             * does, something has fallen back to v1 and we want to see it. */
            if (out->warning[0] == '\0')
                copy_bounded(out->warning, (int)sizeof out->warning, line + 8);
            continue;
        }
        /* UNRESOLVED / FORWARDED / VERSION and unknown lines: ignore. */
    }

    out->hits      = hits;
    out->hit_count = count;

    if (count > 0) {
        _snprintf(out->message, sizeof out->message,
                  "%d file%s found.", count, count == 1 ? "" : "s");
        return ARCHIE_OK;
    }
    if (saw_server_error) {
        copy_bounded(out->message, (int)sizeof out->message, err_line);
        return ARCHIE_ERR_SERVER;
    }
    /* No hits and no error: NONE-FOUND or an empty enumeration. Both are a
     * clean "no matches" success on the wire. */
    copy_bounded(out->message, (int)sizeof out->message, "No matches.");
    return ARCHIE_OK;
}

/* ------------------------------------------------------------------ */
/* Build the ASCII request payload (returns length, always NUL-term).  */
/* ------------------------------------------------------------------ */

static int build_payload(char *buf, int bufsz, const char *term,
                         archie_search_type_t type, int exact_first,
                         int maxhits)
{
    char typec;
    if (maxhits < ARCHIE_MAXHITS_MIN) maxhits = ARCHIE_MAXHITS_MIN;
    if (maxhits > ARCHIE_MAXHITS_MAX) maxhits = ARCHIE_MAXHITS_MAX;

    /* Wire-char mapping (canonical): the lowercase forms are the
     * exact-first variants. EXACT has no exact-first form. */
    switch (type) {
    case ARCHIE_EXACT:     typec = '='; break;
    case ARCHIE_SUBSTR_CS: typec = exact_first ? 'c' : 'C'; break;
    case ARCHIE_REGEX:     typec = exact_first ? 'r' : 'R'; break;
    case ARCHIE_SUBSTR_CI:
    default:               typec = exact_first ? 's' : 'S'; break;
    }

    /* Trailing space after COMPONENTS is intentional (the empty component
     * list); the server strips trailing blanks when reading the line. */
    return _snprintf(buf, (size_t)bufsz,
                     "VERSION 5 %s\n"
                     "AUTHENTICATE '' UNAUTHENTICATED ''\n"
                     "DIRECTORY ASCII ARCHIE/MATCH(%d,0,%c)/%s\n"
                     "LIST ATTRIBUTES COMPONENTS \n",
                     ARCHIE_SWID, maxhits, typec, term);
}

/* ------------------------------------------------------------------ */
/* archie_query: resolve, send, reassemble, parse.                     */
/* ------------------------------------------------------------------ */

archie_status_t archie_query(const char *server, int port,
                             const char *term, archie_search_type_t type,
                             int exact_first, int maxhits,
                             archie_result_t *out)
{
    struct sockaddr_in sa;
    unsigned long      inaddr;
    SOCKET             s;
    unsigned char      req[1600];
    unsigned char      hdr[9];
    int                paylen, reqlen;
    DWORD              tmo = ARCHIE_TIMEOUT_MS;

    char  *pkt_data[ARCHIE_MAX_PKTS];
    int    pkt_len[ARCHIE_MAX_PKTS];
    int    total = 0, got = 0, i, attempt;
    int    complete = 0;

    char  *body = NULL;
    int    blen = 0;
    archie_status_t rc = ARCHIE_OK;

    if (!out) return ARCHIE_ERR_PROTOCOL;
    memset(out, 0, sizeof(*out));
    if (!server || !*server || !term || !*term) {
        copy_bounded(out->message, (int)sizeof out->message,
                     "Enter a search term.");
        return ARCHIE_ERR_PROTOCOL;
    }
    if (port <= 0 || port > 65535) port = ARCHIE_DEFAULT_PORT;

    /* Resolve the server (DNS name or dotted quad). */
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    inaddr = inet_addr(server);
    if (inaddr != INADDR_NONE) {
        sa.sin_addr.s_addr = inaddr;
    } else {
        struct hostent *he = gethostbyname(server);
        if (!he || !he->h_addr_list[0]) {
            _snprintf(out->message, sizeof out->message,
                      "Cannot resolve %s.", server);
            return ARCHIE_ERR_DNS;
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }

    /* Build the request datagram: 9-octet v0 header + ASCII payload. */
    paylen = build_payload((char *)req + sizeof hdr,
                            (int)(sizeof req - sizeof hdr),
                            term, type, exact_first, maxhits);
    if (paylen < 0 || paylen >= (int)(sizeof req - sizeof hdr)) {
        copy_bounded(out->message, (int)sizeof out->message,
                     "Search term is too long.");
        return ARCHIE_ERR_PROTOCOL;
    }
    hdr[0] = 9;                       /* header length (v0: octet0==hdrlen) */
    put16(hdr + 1, archie_next_cid()); /* Connection ID, fresh per request  */
    put16(hdr + 3, 1);                /* packet seq                        */
    put16(hdr + 5, 1);                /* total packet count                */
    put16(hdr + 7, 0);                /* received-through                  */
    memcpy(req, hdr, sizeof hdr);
    reqlen = (int)sizeof hdr + paylen;

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        copy_bounded(out->message, (int)sizeof out->message,
                     "Cannot create UDP socket.");
        return ARCHIE_ERR_SOCKET;
    }
    /* connect() the datagram socket so only the server's replies arrive
     * and recv() timeouts are per-server. */
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        closesocket(s);
        copy_bounded(out->message, (int)sizeof out->message,
                     "Cannot reach the Archie server.");
        return ARCHIE_ERR_SOCKET;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);

    for (i = 0; i < ARCHIE_MAX_PKTS; i++) { pkt_data[i] = NULL; pkt_len[i] = 0; }

    /* Send, then collect packets until the reply is complete or the recv
     * times out; retransmit up to ARCHIE_RETRIES times (the server is
     * stateless and recomputes the whole reply on each request). */
    for (attempt = 0; attempt <= ARCHIE_RETRIES && !complete; attempt++) {
        if (send(s, (const char *)req, reqlen, 0) != reqlen) {
            /* transient send failure: let the retry loop try again */
            continue;
        }
        for (;;) {
            unsigned char buf[2048];
            int n = recv(s, (char *)buf, (int)sizeof buf, 0);
            unsigned char o;
            int seq, tot, off;

            if (n <= 0) break;          /* timeout or error -> retransmit */
            o = buf[0];

            if (o == 1 || o == 3) {     /* single packet (v1 uses \x01)   */
                seq = 1; tot = 1; off = (int)o;
            } else if (o == 7) {        /* multi packet                   */
                if (n < 7) continue;
                seq = (int)get16(buf + 3);
                tot = (int)get16(buf + 5);
                off = 7;
            } else {
                continue;               /* ACK/unknown framing: ignore    */
            }
            if (tot < 1 || tot > ARCHIE_MAX_PKTS) continue;
            if (seq < 1 || seq > tot)              continue;
            if (total == 0) total = tot;

            if (!pkt_data[seq - 1]) {
                int plen = n - off;
                if (plen < 0) plen = 0;
                pkt_data[seq - 1] = (char *)malloc((size_t)plen + 1);
                if (!pkt_data[seq - 1]) { rc = ARCHIE_ERR_NOMEM; goto cleanup; }
                if (plen) memcpy(pkt_data[seq - 1], buf + off, (size_t)plen);
                pkt_data[seq - 1][plen] = '\0';
                pkt_len[seq - 1] = plen;
                got++;
            }
            if (total > 0 && got >= total) { complete = 1; break; }
        }
    }

    if (!complete) {
        _snprintf(out->message, sizeof out->message,
                  "No response from %s (Archie/UDP %d).", server, port);
        rc = ARCHIE_ERR_TIMEOUT;
        goto cleanup;
    }

    /* Concatenate the payload in seq order. */
    for (i = 0; i < total; i++) blen += pkt_len[i];
    body = (char *)malloc((size_t)blen + 1);
    if (!body) { rc = ARCHIE_ERR_NOMEM; goto cleanup; }
    {
        int off = 0;
        for (i = 0; i < total; i++) {
            if (pkt_data[i] && pkt_len[i]) {
                memcpy(body + off, pkt_data[i], (size_t)pkt_len[i]);
                off += pkt_len[i];
            }
        }
        body[off] = '\0';
        blen = off;
    }

    rc = parse_body(body, blen, out);
    out->status = rc;

cleanup:
    for (i = 0; i < ARCHIE_MAX_PKTS; i++)
        if (pkt_data[i]) free(pkt_data[i]);
    if (body) free(body);
    closesocket(s);
    out->status = rc;
    return rc;
}

void archie_result_free(archie_result_t *out)
{
    if (!out) return;
    if (out->hits) { free(out->hits); out->hits = NULL; }
    out->hit_count = 0;
}
