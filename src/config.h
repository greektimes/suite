/*
 * config.h - Windows/MinGW compatibility configuration for freeWAIS-sf 2.2.14
 *
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Part of the Greek Times WAIS Search project (v0.9.1-beta2).
 * https://github.com/greektimes/wais
 */

#ifndef _config_h_
#define _config_h_

/* MinGW-w64 is little-endian x86/x86_64 */
#define BYTEORDER 0x1234

/* Integer sizes for MinGW-w64 */
#define INTSIZE 4
#define LONGSIZE 4

/* Standard headers available in MinGW */
#define I_STDLIB 1
#define I_STRING 1
#define I_TIME 1
#define I_SYS_TYPES 1
#define I_MEMORY 1

/* We do NOT have these Unix-specific headers on Windows */
/* #undef I_UNISTD */
/* #undef I_SYS_TIME */
/* #undef I_DIRENT */
/* #undef I_SYS_DIR */
/* #undef I_SYS_NDIR */
/* #undef I_NDIR */
/* #undef I_ALLOCA */

/* ANSI C prototypes supported */
#define CAN_PROTOTYPE 1

/* Prototype macro (used by some freeWAIS headers as shorthand for _AP) */
#ifndef _
#define _(args) args
#endif

/* Standard functions available */
#define HAS_STRDUP 1

/* Berkeley bcopy/bzero/index compat - we map these via confmagic.h */
/* #undef HAS_BCOPY */
/* #undef HAS_BZERO */
/* #undef HAS_INDEX */
#define HAS_SAFE_MEMCPY 1

/* We have TCP/IP via Winsock */
#define TCPIP 1

/* MinGW provides gethostbyname etc. via Winsock */
#define HAS_GETHOSTENT 1

/* No syslog on Windows */
/* #undef HAS_SYSLOG */

/* Suppress sys_errlist (MinGW uses strerror) */
/* #undef HAS_SYS_ERRLIST */

/* MinGW provides standard vprintf/vsprintf/vfprintf */
#define HAS_VPRINTF 1

/* MAXNAMLEN for docid.c */
#ifndef MAXNAMLEN
#define MAXNAMLEN 256
#endif

/* Buffer size */
#ifndef BUFSZ
#define BUFSZ 100000
#endif

/* Version string announced to the world */
#define VERSION_STRING "freeWAIS-sf 2.2.14 (MGT Windows Native Client)"

/* Strerror mapping */
#define Strerror(e) strerror(e)

/* We have varargs via stdarg.h */
#define I_STDARG 1

/* Suppress Unix-specific features in ustubs.c */
#define HAS_GETWD 1
#define HAS_MEMMOVE 1
/* Do NOT define I_PWD or HAS_GETPWUID (no pwd.h on Windows) */

/* Not needed for client-only build */
/* #undef LOCAL_SEARCH */

#endif /* _config_h_ */
