/*
 * marduk_diag.h - diagnostic hook for the vendored Marduk sources.
 *
 * NOT UPSTREAM. Added by The Montreal Greek Times, 2026-08-05.
 * See MGT-CHANGES.md.
 *
 * Upstream Marduk is a console program: it reports what the virtual
 * modem is doing with printf, fprintf(stderr, ...) and perror. The Suite
 * is built with -mwindows and has no console, so those calls would
 * vanish silently and take the only diagnosis of a failed connection
 * with them.
 *
 * Every such call in the vendored files now goes through marduk_diag
 * instead. The Suite provides the implementation (src/nabu_core.c), and
 * routes it wherever it is useful: the headless test prints it, and the
 * tab will surface the last line as its status in a later phase.
 */

#ifndef MARDUK_DIAG_H
#define MARDUK_DIAG_H

/* printf-style. Implemented in src/nabu_core.c. */
void marduk_diag(const char *fmt, ...);

/* Last platform socket error as a number, for the perror replacements.
 * WSAGetLastError on Windows, errno elsewhere. */
int  marduk_socket_error(void);

#endif /* MARDUK_DIAG_H */
