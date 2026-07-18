/*
 * qotd_proto.h - Generic raw TCP read (RFC 865 QOTD as the canonical user).
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Connect, send nothing, read bytes until server EOF, post chunks.
 * Same event/chunk vocabulary as telnet_proto for symmetry.
 */

#ifndef QOTD_PROTO_H
#define QOTD_PROTO_H

#include "telnet_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QotdProto QotdProto;

int  qotd_proto_open(const char *host, int port,
                     HWND notify_hwnd, UINT notify_msg,
                     QotdProto **out);

void qotd_proto_close(QotdProto *q);

#ifdef __cplusplus
}
#endif

#endif /* QOTD_PROTO_H */
