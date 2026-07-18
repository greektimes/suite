/*                               -*- Mode: C -*- 
 * $Basename: list.h $
 * $Revision: 1.5.1.1 $
 * Author          : Brewster@think.com
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 11:16:25 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

/* list utilities */
#ifndef LIST_H
#define LIST_H

#include "cutil.h" /* for boolean */

#ifdef __cplusplus
/* declare these as C style functions */
extern "C"
	{
#endif /* def __cplusplus */

void* car _AP((void **list));

void* first _AP((void **list));

void* second _AP((void **list));

void* last _AP((void **list));

void** cdr _AP((void **list));

void** nth_cdr _AP((void **list,long n));

void** rest _AP((void **list));

void* cadr _AP((void **list));

void* nth _AP((long number, void **list));

void setf_nth _AP((long number,void* elem,void**list));

/* length of a list.  returns -1 if error.*/
long length _AP((void **list));

void mapcar _AP((void **list, void function (void* argument)));

/* pushes the item on the end of the list. returns the list. */
void **collecting _AP((void **list, void *item));

void setf_car _AP((void** list, void* item));

boolean null _AP((void **list));

boolean free_list _AP((void **list));

void sort_list _AP((void** list,int (*cmp)(void* arg1,void* arg2)));

void** remove_item_from_list _AP((void** list,long pos));

#ifdef __cplusplus
	}
#endif /* def __cplusplus */


#endif /* ndef LIST_H */
