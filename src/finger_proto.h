/*
 * finger_proto.h - RFC 1288 Finger client. Tiny synchronous helper.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * One worker thread per query. Sends "<query>\r\n", reads bytes until
 * server EOF, posts EVT_DATA chunks and EVT_CLOSED to the UI HWND.
 *
 * Reuses TELNET_EVT_* numeric values and the TelnetDataChunk struct
 * for symmetry with the F6 module's dispatch path; no IAC parsing
 * happens here -- bytes pass through verbatim.
 */

#ifndef FINGER_PROTO_H
#define FINGER_PROTO_H

#include "telnet_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FingerProto FingerProto;

/* Returns 0 on success and writes *out. Posts the same events
 * telnet_proto would, with chunk lparams freed via
 * telnet_proto_free_chunk. */
int  finger_proto_open(const char *host, int port,
                       const char *query,
                       HWND notify_hwnd, UINT notify_msg,
                       FingerProto **out);

void finger_proto_close(FingerProto *f);

#ifdef __cplusplus
}
#endif

#endif /* FINGER_PROTO_H */
