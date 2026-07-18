/*                               -*- Mode: C -*- 
 * $Basename: server.h $
 * $Revision: 1.5 $
 * Author          : Harry Morris, morris@think.com
 * Created On      : 5.29.90
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 11:07:43 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

/* this file is a server process for a unix machine that takes input from 
   standard in or from a socket and searches the local search engine on the 
   unix box.
   originally written by harry morris.
*/

#define BUFSZ 100000     /* size of our comm buffer */
