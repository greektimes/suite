/*                               -*- Mode: C -*- 
 * $Basename: panic.h $
 * $Revision: 1.4.1.1 $
 * Author          : Brewster@think.com
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 11:11:02 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

#ifndef PANIC_H
#define PANIC_H

#include "cdialect.h"

#ifdef __cplusplus
/* declare these as C style functions */
extern "C"
	{
#endif /* def __cplusplus */

#ifdef ANSI_LIKE	/* use ansi */
void	panic _AP((char* format,...)); 
#else /* use K & R */
void	panic _AP(()); 
#endif

#ifdef __cplusplus
	}
#endif /* def __cplusplus */
#endif /* ndef PANIC_H */
