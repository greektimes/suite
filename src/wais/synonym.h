/*                               -*- Mode: C -*- 
 * $Basename: synonym.h $
 * $Revision: 1.2.1.2.1.2 $
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 10:57:32 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */


#ifndef __SYNONYM_H
#  define _SYNONYM_H
#include "cdialect.h"
struct s_Synonym {
  char *root;
  char *key;
};

typedef struct s_Synonym t_Synonym;

#define synptr( elem ) syn_Table[*syn_Table_Size-1].elem
#define SYN_FILE_LINE_LENGTH 2048

#ifdef CACHE_SYN
/* use shared memory to keep around synonym tables */
#include <sys/ipc.h>
#include <sys/shm.h>

#define MAX_SYN_CACHE 16

/* key to master shmem area */
extern int cacheSynId;

struct s_cacheSyn {
  int id;
  char synfile [256];
  int table_size;
};
typedef struct s_cacheSyn t_cacheSyn;

#endif /* CACHE_SYN */

/* prototypes */

int syn_compare _AP((const void*, const void* ));
char* lookup_Synonym _AP(( char*,t_Synonym*,int ));
void syn_ReadFile _AP(( char*,t_Synonym**,int* ));
void syn_Free _AP(( t_Synonym*,int* ));
#endif
