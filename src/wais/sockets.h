/*                               -*- Mode: C -*- 
 * sockets.h - Windows/Unix portable socket declarations
 *
 * Original: freeWAIS-sf 2.2.10 (C) Universitaet Dortmund / CNIDR
 * Modified: Montreal Greek Times WAIS Native Client Project
 */

#ifndef sockets_h
#define sockets_h

#include "cdialect.h"
#include "cutil.h"

#ifdef _WIN32
#include "win_platform.h"
#else
#ifndef THINK_C
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif /* THINK_C */
#endif /* _WIN32 */

/*---------------------------------------------------------------------------*/

#ifdef __cplusplus
extern "C" {
#endif

extern char host_name[255], host_address[255];

void open_server _AP((long port,long* socket,long size));
void accept_client_connection _AP((long socket,FILE** file));
void close_client_connection _AP((FILE* file));
void close_server _AP((long socket));
FILE *connect_to_server _AP((char* hname,long port));
void close_connection_to_server _AP((FILE* file));
char *mygethostname _AP((char *hostname, long len));

#ifdef __cplusplus
}
#endif

#endif
