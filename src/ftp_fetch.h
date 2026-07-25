/*
 * ftp_fetch.h - Minimal shared FTP retrieve service (passive, anonymous,
 *               binary), in the Suite's raw-Winsock style.
 *
 * A self-contained control+data FTP client that downloads one file to a
 * local path. It is deliberately not tied to any module: the Archie tab is
 * its first caller, but any module can retrieve a file with the same call.
 *
 * Passive is a hard requirement (works behind NAT/firewalls where active
 * PORT does not), the login is always anonymous, and the transfer is
 * always BINARY (TYPE I) so no file is ever corrupted by line-ending
 * translation. This mirrors xarchie's own default.
 *
 * ftp_fetch_file blocks; it is meant to run on a worker thread, never the
 * UI thread. Winsock must already be initialized (the Suite calls
 * WSAStartup once in WinMain).
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef FTP_FETCH_H
#define FTP_FETCH_H

#define FTP_FETCH_DEFAULT_PORT 21

/* Progress callback: invoked from the worker thread as bytes stream in.
 * It MUST be thread-safe and MUST NOT touch the UI directly. The intended
 * use is to store bytes_so_far in a volatile that a UI-thread timer polls. */
typedef void (*ftp_progress_cb)(void *user, long bytes_so_far);

/* Retrieve host:remote_path over FTP in PASSIVE mode with ANONYMOUS login
 * and BINARY (TYPE I) transfer, writing it to local_path. Blocking; run on
 * a worker thread. host is a DNS name or dotted quad; port is usually 21;
 * remote_path is the absolute server path (e.g. /pub/cuwin/CUSEEME.EXE).
 * progress may be NULL.
 *
 * Returns 1 on success with *out_total set to the bytes written, or 0 on
 * any failure with errbuf filled with a human-readable message. Never
 * crashes on an error path (bad host, refused, missing file, unwritable
 * local path, mid-transfer drop): it cleans up and returns 0. On failure a
 * partial local file is removed. */
int ftp_fetch_file(const char *host, int port,
                   const char *remote_path, const char *local_path,
                   ftp_progress_cb progress, void *user,
                   long *out_total, char *errbuf, int errbuf_sz);

#endif /* FTP_FETCH_H */
