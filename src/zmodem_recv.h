/*
 * zmodem_recv.h - Zmodem RECEIVE session for the Suite.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Receive only. The Suite never sends a file and never runs a command
 * for a sender; see the security notes in zmodem_recv.c.
 *
 * SHAPE. A session owns a worker thread. The caller pushes payload
 * bytes in from wherever its transport delivers them and the worker
 * pulls them out one at a time, which is the shape the vendored Zmodem
 * core wants. Nothing blocks the UI thread for longer than a memcpy.
 *
 *   caller (UI thread)                 worker thread
 *   ------------------                 -------------
 *   zmodem_recv_push(bytes)  --ring--> zm_recv() one byte at a time
 *                            <-post--  ZMR_EVT_PROGRESS / ZMR_EVT_DONE
 *   zmodem_recv_finish()               (joins, hands back the tail)
 *
 * LAYERING. This module sits on CLEAN payload bytes. The telnet IAC
 * layer (0xFF doubling, RFC 854) is the OUTER one and is already peeled
 * off by telnet_proto.c on the way in and re-applied on the way out.
 * Zmodem's own ZDLE (0x18) escaping is the INNER layer and lives in the
 * vendored core. Neither layer can see the other's escapes.
 */

#ifndef ZMODEM_RECV_H
#define ZMODEM_RECV_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WPARAM values for the notify message. */
#define ZMR_EVT_PROGRESS  1   /* LPARAM = ZmrProgress *, free with zmodem_recv_free_progress */
#define ZMR_EVT_DONE      2   /* LPARAM = 0; call zmodem_recv_finish() to collect the result */

/* Outcome of a finished session. */
typedef enum {
    ZMR_OK = 0,            /* file received and closed                  */
    ZMR_CANCELLED,         /* the user cancelled; ZCAN was sent         */
    ZMR_REFUSED,           /* the sender asked for something we refuse  */
    ZMR_TOO_BIG,           /* advertised or actual size over the cap    */
    ZMR_BAD_NAME,          /* no usable filename could be derived       */
    ZMR_IO_ERROR,          /* could not create or write the local file  */
    ZMR_PROTOCOL_ERROR,    /* unrecoverable Zmodem error                */
    ZMR_TIMEOUT,           /* the sender went quiet                     */
    ZMR_ABORTED            /* the transport closed under us             */
} ZmrStatus;

typedef struct ZmrProgress {
    unsigned long long received;   /* bytes written so far              */
    unsigned long long total;      /* advertised size, 0 if unknown     */
    char               name[260];  /* sanitized local file name         */
} ZmrProgress;

typedef struct ZmrResult {
    ZmrStatus          status;
    unsigned long long received;
    unsigned long long total;
    char               name[260];   /* sanitized local file name        */
    char               path[MAX_PATH]; /* full local path, empty if none */
    char               detail[160]; /* human-readable one-liner         */

    /* Bytes that arrived after the Zmodem conversation ended. They
     * belong to whatever the session goes back to being (for the RIP
     * tab, the scene stream) and must be handed on, not dropped. Freed
     * by zmodem_recv_free_result(). */
    unsigned char     *tail;
    int                tail_len;
} ZmrResult;

typedef struct ZmodemRecv ZmodemRecv;

/* How the session writes its replies back to the far end. Called on the
 * worker thread, so it must be safe there; telnet_proto_send() is.
 * Returns 0 on success. */
typedef int (*ZmrSendFn)(void *ctx, const void *buf, size_t len);

/* Start a session. `download_dir` may be NULL, in which case the
 * default download directory is used (see zmodem_recv_default_dir).
 * Returns 0 and writes *out on success. */
int  zmodem_recv_start(const char *download_dir,
                       HWND notify_hwnd, UINT notify_msg,
                       ZmrSendFn send_fn, void *send_ctx,
                       ZmodemRecv **out);

/* Hand freshly-arrived payload bytes to the session. Safe to call from
 * the UI thread; it copies into a ring buffer and returns. */
void zmodem_recv_push(ZmodemRecv *z, const unsigned char *data, int len);

/* Ask for a clean abort. The worker sends the Zmodem cancel sequence
 * and unwinds. Safe from the UI thread; returns immediately. */
void zmodem_recv_cancel(ZmodemRecv *z);

/* Stop the worker, collect the result, and free the session. Always
 * returns a result (never NULL) which the caller frees with
 * zmodem_recv_free_result(). After this the handle is invalid. */
ZmrResult *zmodem_recv_finish(ZmodemRecv *z);

void zmodem_recv_free_result(ZmrResult *r);
void zmodem_recv_free_progress(ZmrProgress *p);

/* Where downloads land: %MGT_ZMODEM_DIR% if set, else the user's
 * Downloads folder, else the temporary directory. Writes at most
 * `cap` bytes including the terminator and returns `out`. */
char *zmodem_recv_default_dir(char *out, size_t cap);

/* Turn whatever a sender put in a ZFILE frame into a name that is safe
 * to create inside the download directory. Exposed for testing; see
 * zmodem_recv.c for the rules. Returns 0 if nothing usable could be
 * derived, in which case `out` holds the fallback name. */
int zmodem_recv_sanitize_name(const char *raw, char *out, size_t cap);

/* The ceiling a single transfer may not cross, in bytes. */
unsigned long long zmodem_recv_max_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMODEM_RECV_H */
