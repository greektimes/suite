/*                               -*- Mode: C -*- 
 * any_stemmer.h -- 
 * ITIID           : $ITI$ $Header $__Header$
 * Author          : Ulrich Pfeifer
 * Created On      : Fri May  3 08:57:20 1996
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Fri May  3 14:29:00 1996
 * Language        : C
 * Update Count    : 7
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1996, Universität Dortmund, all rights reserved.
 * 
 * $$
 * $Log: any_stemmer.h,v $
 * Revision 2.2  1997/02/04 17:12:09  pfeifer
 * Switched to CVS
 *
 * Revision 2.0.1.1  1996/12/27 16:06:52  pfeifer
 * patch69: Interface for different stemmers.
 *
 */

#ifndef ANY_STEMMER_H
#define ANY_STEMMER_H
#include "cdialect.h"

#define ENGLISH_STEMMING_TAG 's'
#define GERMAN_GRUNDFORM_TAG 'g'
#define GERMAN_STAMMFORM_TAG 't'
#define IS_STEMMED(A) \
	(((A) == ENGLISH_STEMMING_TAG) || ((A) == GERMAN_GRUNDFORM_TAG) || ((A) == GERMAN_STAMMFORM_TAG))

char * any_stemmer _AP((char tag, char *word));

extern char  index_stemming;

#endif /* ANY_STEMMER_H */
