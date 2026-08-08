/*
 * Copyright 2022, 2023 S. V. Nickolas.
 * Copyright 2023 Marcin Wołoszczuk.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following condition:  The
 * above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * This code abstracts away the few differences between BSD Sockets (used by
 * Linux, OSX, etc.), Winsock (used by Windows) and Watt-32 (used by MS-DOS).
 * 
 * From our perspective all these APIs are more or less the same except for
 * whether/how they are set up and shut down, and whether they use the regular
 * close() function to close a socket, or they need their own closesocket()
 * instead (we define closesocket to close on *x to hide this difference).
 * 
 * The MS-DOS code has not been tested successfully, and the Windows code has
 * not been tested at all.
 */

#include <stdint.h>
#include <stdio.h>
#include "marduk_diag.h"   /* MGT: no console in the Suite */
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <sys/socket.h>
# include <sys/times.h>
# include <sys/time.h>
# include <sys/types.h>
# include <netinet/in.h>
# include <netinet/tcp.h>   /* MGT: TCP_NODELAY, see modem_init */
# include <netdb.h>
# ifdef __MSDOS__
#  include <tcp.h>
# else
#  define closesocket close /* BSD Socket API does not distinguish */
# endif
#endif

/*
 * Version of modem.c to interface with DJ Sures' emulator.
 * 
 * This is intended as a temporary solution until the emulator is 
 * reimplemented within Marduk itself.
 */

static int status;

/*
 * MGT: the socket handle was "static int mosock".
 *
 * On Win64 SOCKET is UINT_PTR, i.e. 64 bits, and INVALID_SOCKET is
 * (SOCKET)(~0). Storing that in an int truncates the handle and makes
 * the "mosock==INVALID_SOCKET" test below compare a truncated value
 * against -1. Upstream says the Windows code has never been tested;
 * this is one of the reasons it could not have worked as built.
 */
#ifdef _WIN32
typedef SOCKET marduk_socket_t;
#else
typedef int marduk_socket_t;
#endif
static marduk_socket_t mosock;

uint8_t modem_bytes_available (void)
{
 struct timeval timeval;
 fd_set fds;
 int e;
 
 if (!status) return 0;
 
 timeval.tv_sec=0;
 timeval.tv_usec=0;
 
 FD_ZERO(&fds);
 FD_SET(mosock, &fds);
 
 e=select(mosock+1, &fds, 0, 0, &timeval);
 if (e==-1)
 {
  marduk_diag("NABU modem: select() failed, error %d\n", marduk_socket_error());
  return 0;
 }
 
 if (!e) /* Data on 0 sockets */
 {
  return 0;
 }
 return 1;    
}

uint8_t modem_read (uint8_t *b)
{
 if (!status) return 0;
 if (modem_bytes_available()) {
   recv(mosock, b, 1, 0);
   return 1;
 }
 return 0;
}

void modem_write (uint8_t data)
{
 if (status) send(mosock, &data, 1, 0);
 return;
}

int modem_init (char *server, char *port)
{
 int e;
 struct addrinfo hints, *result;
 
#ifdef _WIN32
 WSADATA wsadata;
#endif
 
 status=0;

#ifdef __MSDOS__
 /*
  * Watt-32 requires initialization (this is also where it does DHCP).
  * 
  * It simplifies things for us a ton that Watt-32 mostly uses the BSD socket
  * APIs, because very little modification is necessary to interface with it.
  * 
  * XXX: This is currently fatal if the packet driver is missing.  How to set
  *      up to be non-fatal, and just return us an error code?
  */
 if (sock_init())
 {
  marduk_diag("NABU modem: TCP library (Watt-32) failed to initialize\n");
  return -1;
 }
#endif

#ifdef _WIN32
 /*
  * Winsock also requires initialization, and like Watt-32, its interface is
  * more or less the same as that of BSD.
  */
 if (WSAStartup(MAKEWORD(2,2), &wsadata))
 {
  marduk_diag("NABU modem: Winsock failed to initialize\n");
  return -1;
 }
#endif
 
 memset(&hints,0,sizeof(struct addrinfo));
 hints.ai_family=AF_INET;
 hints.ai_socktype=SOCK_STREAM;
 /* hints.ai_flags=(AI_NUMERICHOST | AI_NUMERICSERV); */
 hints.ai_protocol=IPPROTO_TCP;
 e=getaddrinfo(server, port, &hints, &result);
 if (e)
 {
  marduk_diag("NABU modem: cannot resolve %s port %s (getaddrinfo error %d)\n", server, port, e);
  return -1;
 }
 
 mosock=socket(result->ai_family, result->ai_socktype, result->ai_protocol);
#ifdef _WIN32
 if (mosock==INVALID_SOCKET)
#else
 if (mosock<0)
#endif
 {
  marduk_diag("NABU modem: could not create a socket, error %d\n",
              marduk_socket_error());
  freeaddrinfo(result);
  return -1;
 }
 
 e=connect(mosock, result->ai_addr, result->ai_addrlen);
 freeaddrinfo(result);
 if (e!=0)
 {
  marduk_diag("NABU modem: connection to %s port %s failed, error %d\n",
              server, port, marduk_socket_error());
  return -1;
 }
 /*
  * MGT: DISABLE NAGLE ON THIS SOCKET.
  *
  * This is a MODERN NETWORK ACCOMMODATION and it does not change one byte
  * of what goes on the wire. The NABU's HCCA is a serial line to an
  * adapter sitting on the end of a cable; it has no notion of packets,
  * and nothing in the NABU or in nabud can tell whether the bytes of a
  * request crossed the internet in one TCP segment or four. Only the
  * TIMING changes, and it changes back towards the hardware, not away
  * from it.
  *
  * Why it is needed. modem_write sends ONE BYTE PER send(), because that
  * is the shape of the emulation: the Z80 executes an OUT to port $80 and
  * one byte is what the machine has to give. Nagle's algorithm holds any
  * small write while an earlier small write is still unacknowledged, so
  * when the firmware writes a run of bytes with no read in between (the
  * four byte pack number, the two byte acknowledgement) only the first
  * one leaves immediately and the rest wait for the server's ACK. The
  * server has nothing to say until it has the whole request, so it does
  * not answer, and its delayed acknowledgement timer runs to the end.
  *
  * Measured on the live channel on 2026-08-07, before this change: every
  * packet of the boot image cost three stalls of about 116, 266 and 283
  * milliseconds, while the thousand bytes of payload behind them arrived
  * in about ten. The stalls, not the link, were the whole boot time.
  *
  * A failure here is not fatal. The boot still works, it is just slow, so
  * this says so and carries on.
  */
 {
  int nodelay=1;
  if (setsockopt(mosock, IPPROTO_TCP, TCP_NODELAY,
                 (const char *)&nodelay, sizeof nodelay)!=0)
  {
   marduk_diag("NABU modem: could not disable Nagle, error %d "
               "(the channel will still load, but slowly)\n",
               marduk_socket_error());
  }
 }

 marduk_diag("NABU modem: connected to %s port %s\n", server, port);
 status=1;

 return 0;
}

void modem_deinit (void)
{
 if (!status) return;
 marduk_diag("NABU modem: shutting down\n");
 closesocket(mosock);
#ifdef _WIN32
 WSACleanup();
#endif
 /*
  * MGT: upstream left status set here. Upstream only ever tears the modem
  * down on the way out of main(), so it never mattered; the Suite
  * connects and disconnects repeatedly from one process, and a stale
  * status of 1 means the next session reads and writes a closed socket.
  */
 status=0;
}
