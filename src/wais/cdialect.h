/*                               -*- Mode: C -*- 
 * $Basename: cdialect.h $
 * $Revision: 1.3.1.12.1.3 $
 * Author          : Harry Morris, morris@think.com
 * Created On      : 5.29.90
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 11:39:49 1997
 * Language        : C
 * Update Count    : 36
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

#ifndef C_DIALECT
#define C_DIALECT

/* AIX requires this to be the first thing in the file.  */
#ifdef __GNUC__
#define alloca __builtin_alloca
#else /* not __GNUC__ */
#ifdef I_ALLOCA
#include <alloca.h>
#else /* not I_ALLOCA */
#ifdef _AIX
 #pragma alloca
#else /* not _AIX */
char *alloca ();
#endif /* not _AIX */
#endif /* not I_ALLOCA */
#endif /* not __GNUC__ */

#include "config.h"
#include "confmagic.h"

#ifdef I_STDLIB
#include <stdlib.h>
#endif /* I_STDLIB */

#ifdef I_STRING
#include <string.h>
#else /* not I_STRING */
#include <strings.h>
#endif /* not I_STRING */

#ifdef I_TIME
#include <time.h>
#endif /* I_TIME */
#ifdef I_SYS_TIME
#include <sys/time.h>
#endif /* I_SYS_TIME */

#ifdef I_SYS_TYPES
#include <sys/types.h> 
#endif /* I_SYS_TYPES */

#ifdef I_UNISTD
#include <unistd.h>
#endif /* I_UNISTD */

#ifdef I_DIRENT
#include <dirent.h>
#ifdef DIRNAMLEN
#define NLENGTH(dirent) ((dirent)->d_namlen)
#else /* not DIRNAMLEN */
#define NLENGTH(dirent) (strlen((dirent)->d_name))
#endif /* not DIRNAMLEN */
#ifdef I_SYS_NDIR
#include <sys/ndir.h>
#endif /* I_SYS_NDIR */


#ifdef I_NDIR
#include <ndir.h>
#endif /* I_NDIR */
#else
#ifdef I_SYS_DIR
#include <sys/dir.h>
#endif /* I_SYS_DIR */
#endif /* I_DIRENT */



#ifdef I_STRING
#include <string.h>
#else /* not I_STRING */
#include <strings.h>
#endif /* not I_STRING */

#ifdef I_MEMORY
#include <memory.h>
#endif /* I_MEMORY */

/* set up the function prototyping convention */
#ifdef CAN_PROTOTYPE
#ifdef _AP
#undef _AP
#endif
#define _AP(args) args
#define ANSI_LIKE
#else /* not CAN_PROTOTYPE */
#define K_AND_R
#ifdef _AP
#undef _AP
#endif
#define _AP(args) ()
#endif /* not CAN_PROTOTYPE */

#include <ctype.h>              /* local version redefines string functions */

#if 0 /* HAS_GETWD */
extern char* getcwd _AP((char *buf, size_t size));
#endif

extern char* getuser_name _((void));
extern char* getuser_env_name _((void));

/*
 * How about the memory copy routines ?
 */
#ifndef HAS_SAFE_BCOPY
#if HAS_SAFE_MEMCPY
/* ok: have memcpy() and it does overlapping copies */
#define bcopy(s, d, n) memcpy ((d), (s), (n))
#else /* not HAS_SAFE_MEMCPY */
/* Neither bcopy() nor memcpy() do overlapping copies */
#ifdef bcopy
#undef bcopy
#endif
#define bcopy(d, s, n) mybcopy ((d), (s), (n))
#define NEED_MYBCOPY
#endif /* not HAS_SAFE_MEMCPY */
#endif /* not HAS_SAFE_BCOPY */


#ifndef HAS_STRDUP
extern char * strdup _((char *s));
#endif /* not HAS_STRDUP */
#endif /* not C_DIALECT */

#if ((BYTEORDER==0x1234) || (BYTEORDER==0x12345678))
#define LITTLEENDIAN
#else
#define BIGENDIAN
#endif

#if (INTSIZE==4)
#define FOUR_BYTE int
#else
#if (LONGSIZE==4)
#define FOUR_BYTE long
#else
WhatTypeHasFourBytes
#endif
#endif

/* Comment this back in to figure out what the compiler thinks we are */
/*
#ifdef ANSI_LIKE
WeareAnsiLike
#endif
#ifdef PROTO_ANSI
WeareProtoAnsi
#endif
#ifdef K_AND_R
WeareKandR
#endif
Crash-and-Burn
*/
/* End of chunk to comment back in */

/*----------------------------------------------------------------------*/


