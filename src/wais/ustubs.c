/*                               -*- Mode: C -*- 
 * $Basename: ustubs.c $
 * $Revision: 1.5.1.7.1.2 $
 * Author          : Harry Morris, morris@think.com
 * Created On      : 4.14.90
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 10:50:10 1997
 * Language        : C
 * Update Count    : 17
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

#ifndef lint
static char *PRCSid = "$Id: ustubs.c 1.5.1.7.1.2 Mon, 05 May 1997 11:54:27 +0200 pfeifer $";
#endif


/*----------------------------------------------------------------------*/
/* stubs for silly non-ansi c compilers                                 */
/*----------------------------------------------------------------------*/

#include "ustubs.h"
#include "cutil.h" /* for substrcmp and NULL */

#ifndef MAXPATHLEN              /* Should be defined in sys/param.h */
#define MAXPATHLEN 1024
#endif

#ifndef HAS_GETWD
char           *
getwd (pathname)
     char           *pathname;
{
  return ((char *) getcwd (pathname, MAXPATHLEN));
}
#endif

#if 0 /* ndef HAS_GETCWD */
static  char        getcwdbuf[MAXPATHLEN];

char           *
getcwd (buf, size)
     char           *buf;
     size_t          size;
{
  char           *p;

  if (!getwd (getcwdbuf))
    return NULL;
  getcwdbuf[MAXPATHLEN - 1] = '\0';
  strncpy (buf, getcwdbuf, MAXPATHLEN);
  return getcwdbuf;
}
#endif

#ifdef I_PWD
#include <pwd.h> 
#endif

static char usernamebuffer[9];

char           *
getuser_name ()
{
#ifdef HAS_GETPWUID
  struct passwd  *p;
  Uid_t           uid;

  uid = getuid ();
  if (p = getpwuid (uid)) {
    strncpy (usernamebuffer, p->pw_name, 9);
    usernamebuffer[8] = '\0';
    return usernamebuffer;
  }
#endif
  return getuser_env_name ();
}

char           *
getuser_env_name ()
{
  if (getenv ("USER"))
    return getenv ("USER");
  if (getenv ("LOGNAME"))
    return getenv ("LOGNAME");
  return "nobody";
}


/*----------------------------------------------------------------------*/

#ifndef HAS_MEMMOVE

void           *
memmove (str1, str2, n)
     void           *str1;
     const void     *str2;
     size_t          n;
{
  bcopy ((char *) str2, (char *) str1, (long) n);
  return (str1);
}
#endif

/*----------------------------------------------------------------------*/

#ifndef ANSI_LIKE  

/* atoi is not defined k&r. copied from the book */
long
atol (s)
     char           *s;
{
  long            i, n, sign;

  /* skip white space */
  for (i = 0; s[i] == ' ' || s[i] == 'n' || s[i] == 't'; i++);
  sign = 1;
  if (s[i] == '+' || s[i] == '-')
    sign = (s[i++] == '+') ? 1 : -1;
  for (n = 0; s[i] >= '0' && s[i] <= '9'; i++)
    n = 10 * n + s[i] - '0';
  return (sign * n);
}

#if 0
char *strstr(src,sub)
char *src;
char *sub;
{
  /* this is a poor implementation until the ANSI version catches on */
  char *ptr;
  for(ptr = src; (long)ptr <= (long)src + strlen(src) - strlen(sub); ptr++){
    if(substrcmp(ptr, sub))
      return(ptr);
  }
  return(NULL);
}
#endif

#ifndef HAS_REMOVE
indent:Standard input:5: Error: Unmatched #endif
int remove 
_AP ((char *filename))
{
  return (unlink (filename));
}
#endif

#endif /* ndef ANSI_LIKE */


