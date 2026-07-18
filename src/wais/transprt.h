/*                               -*- Mode: C -*- 
 * $Basename: transprt.h $
 * $Revision: 1.4.1.1 $
 * Author          : Brewster Kahle, Tracy Shen
 * Created On      : 11/14/90
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 10:54:27 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

#ifndef TRANSPRT_H
#define TRANSPRT_H

#include "cdialect.h"

#include "cutil.h"

#ifdef __cplusplus
/* declare these as C style functions */
extern "C"
	{
#endif /* def __cplusplus */

boolean transportCode _AP((long encoding,char* data,long* len));
boolean transportDecode _AP((long encoding,char* data,long* len));

void transportCodeH _AP((char* data,long* len));
void transportDecodeH _AP((char* data,long* len));

void transportCodeI _AP((char* data,long* len));
void transportDecodeI _AP((char* data,long* len));

#ifdef __cplusplus
	}
#endif /* def __cplusplus */
#endif /* ndef TRANSPRT_H */
