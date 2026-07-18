/*                               -*- Mode: C -*- 
 * $Basename: wmessage.c $
 * $Revision: 1.4.1.4.1.3 $
 * Author          : Morris@think.com
 * Created On      : 3.26.90
 * Last Modified By: Ulrich Pfeifer
 * Last Modified On: Mon May  5 10:43:07 1997
 * Language        : C
 * Update Count    : 1
 * Status          : Unknown, Use with caution!
 * 
 * (C) Copyright 1997, Universität Dortmund, all rights reserved.
 * (C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)
 */

/* 
 * This file is for reading and writing the wais packet header.
 * Morris@think.com
 */

#ifndef lint
static char *PRCSid = "$Id: wmessage.c 1.4.1.4.1.3 Mon, 05 May 1997 11:54:27 +0200 pfeifer $";
#endif

/* to do:
 *  add check sum
 *  what do you do when checksum is wrong?
 */

#include "wmessage.h"
#include "ustubs.h"
#include "cutil.h"

/*---------------------------------------------------------------------*/

void 
readWAISPacketHeader(msgBuffer,header_struct)
char* msgBuffer;
WAISMessage *header_struct;
{
  /* msgBuffer is a string containing at least HEADER_LENGTH bytes. */
		    
  memmove(header_struct->msg_len,msgBuffer,(size_t)10); 
  header_struct->msg_type = char_downcase((unsigned long)msgBuffer[10]);
  header_struct->hdr_vers = char_downcase((unsigned long)msgBuffer[11]);
  memmove(header_struct->server,(void*)(msgBuffer + 12),(size_t)10);
  header_struct->compression = char_downcase((unsigned long)msgBuffer[22]);
  header_struct->encoding = char_downcase((unsigned long)msgBuffer[23]);
  header_struct->msg_checksum = char_downcase((unsigned long)msgBuffer[24]);
}
 
/*---------------------------------------------------------------------*/

long
getWAISPacketLength(header)
WAISMessage* header;
/* interpret the length field, this is necessary since the lenght in the
   message is not null terminated, so atol() may get confused.
 */
{ 
  char lenBuf[11];
  memmove(lenBuf,header->msg_len,(size_t)10);
  lenBuf[10] = '\0';
  return(atol(lenBuf));
}

/*---------------------------------------------------------------------*/

#ifdef NOTUSEDYET

static char checkSum _AP((char* string,long len));

static char
checkSum(string,len)
char* string;
long len;
/* XXX the problem with this routine is that it can generate 
   non-ascii values.  Since these values are not being hexized,
   they can (and will) hang up some communication channels.
   */
{
  register long i;
  register char chSum = '\0';
	  
  for (i = 0; i < len; i++)
    chSum = chSum ^ string[i];
	    
  return(chSum);
}	
#endif /* def NOTUSEDYET */

/* this modifies the header argument.  See wais-message.h for the different
 * options for the arguments.
 */
 
void
writeWAISPacketHeader(header,
		      dataLen,
		      type,
		      server,
		      compression,
		      encoding,
		      version)
char* header;
long dataLen;
long type;
char* server;
long compression;
long encoding;
long version;
/* Puts together the new wais before-the-z39-packet header. */
{
  char lengthBuf[11];
  char serverBuf[11];

  long serverLen;

  /* 
     replace the strlen; which reads until it finds '\0' and make
     purify happy. Think time is nor problem. (up)

     serverLen = strlen(server);
     if (serverLen > 10)
     serverLen = 10;
  */
  for (serverLen=0;serverLen<=10 && server[serverLen]; serverLen++);

  sprintf(lengthBuf, "%010ld", dataLen);  
  strncpy(header,lengthBuf,10);

  header[10] = type & 0xFF; 
  header[11] = version & 0xFF;

  strncpy(serverBuf,server,serverLen);       
  strncpy((char*)(header + 12),serverBuf,serverLen);

  header[22] = compression & 0xFF;    
  header[23] = encoding & 0xFF;    
  header[24] = '0'; /* checkSum(header + HEADER_LENGTH,dataLen);   XXX the result must be ascii */	
}              
              
/*---------------------------------------------------------------------*/




