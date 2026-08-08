/*
 * nabu_serial.c - the serial leg. See nabu_serial.h for what and why.
 *
 * Part of the Montreal Greek Times Unicorn Suite.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * Licensed under the GNU Affero General Public License v3.0 or later.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "nabu_serial.h"
#include "nabu_channel.h"

/* One session. There is only ever one NABU on the end of one cable, and
 * a second bridge would need a second tab, so this is file scope rather
 * than a handle the caller carries around. */
static HANDLE   g_com  = INVALID_HANDLE_VALUE;
static SOCKET   g_sock = INVALID_SOCKET;
static int      g_wsa  = 0;
static uint64_t g_to_nabu   = 0;
static uint64_t g_to_server = 0;

static nabu_serial_log_fn g_log = NULL;
static void              *g_log_user = NULL;

static void logf_(const char *fmt, ...)
{
    char    buf[320];
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log(g_log_user, buf);
}

/* ------------------------------------------------------------------ */
/* The cable                                                           */
/* ------------------------------------------------------------------ */

/*
 * COM10 and above cannot be opened by their bare name; the DOS device
 * namespace only reaches COM1 to COM9 that way and everything above
 * needs the \\.\ prefix. Using the prefix for all of them is legal and
 * saves a special case, so that is what this does.
 */
static void com_device_path(const char *name, char *out, size_t cap)
{
    if (!name) { snprintf(out, cap, "%s", ""); return; }
    if (strncmp(name, "\\\\.\\", 4) == 0) snprintf(out, cap, "%s", name);
    else                                  snprintf(out, cap, "\\\\.\\%s", name);
}

static int com_open(const char *name, char *err, size_t errcap)
{
    char        path[64];
    DCB         dcb;
    COMMTIMEOUTS to;

    com_device_path(name, path, sizeof path);

    /* No FILE_FLAG_OVERLAPPED: the relay polls with timeouts that make
     * every read return at once, so synchronous handles are simpler and
     * cannot leave an I/O in flight at teardown. */
    g_com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (g_com == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, errcap,
                 (e == ERROR_FILE_NOT_FOUND)
                     ? "%s is not there"
                     : (e == ERROR_ACCESS_DENIED)
                         ? "%s is already in use by another program"
                         : "could not open %s (error %lu)",
                 name, (unsigned long)e);
        return -1;
    }

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(g_com, &dcb)) {
        snprintf(err, errcap, "could not read the state of %s", name);
        goto fail;
    }

    dcb.BaudRate        = NABU_SERIAL_BAUD;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = (NABU_SERIAL_STOPBITS == 2) ? TWOSTOPBITS
                                                      : ONESTOPBIT;
    /*
     * No flow control of any kind. The HCCA is a bare asynchronous line
     * with no handshake wires in play, and letting the driver hold bytes
     * back for a CTS that will never come would be exactly the kind of
     * interference this bridge exists not to do.
     */
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;
    dcb.fNull           = FALSE;      /* a NUL byte is data, not nothing */
    dcb.fAbortOnError   = FALSE;

    /*
     * 111860 IS NOT A STANDARD RATE, AND THAT IS THE INTERESTING PART.
     *
     * Win32 takes the literal number rather than an enumerated constant,
     * so whether it takes at all is up to the driver underneath.
     * FTDI-class adapters divide a 24 MHz clock and accept it happily;
     * some others round to the nearest rate they know, and a few refuse.
     *
     * A refusal is reported and the connection is abandoned. It is NOT
     * quietly retried at 115200. That would work, roughly, because 115200
     * is within about 3 percent of the real rate and asynchronous
     * receivers tolerate a few percent, and it is what nabud falls back
     * to. But a bridge that silently runs at the wrong speed produces
     * corruption that looks like a bad cable or a bad server, and the
     * operator deserves to be told which of those it is.
     */
    if (!SetCommState(g_com, &dcb)) {
        snprintf(err, errcap,
                 "%s will not run at %d baud, 8N%d. This is the NABU's "
                 "native rate and not a standard one, so the port's driver "
                 "has to support it; an FTDI-based adapter does",
                 name, NABU_SERIAL_BAUD, NABU_SERIAL_STOPBITS);
        goto fail;
    }

    /*
     * Reads return immediately with whatever has arrived, including
     * nothing. That is what makes the relay non-blocking without
     * overlapped I/O: ReadIntervalTimeout of MAXDWORD with both total
     * timeouts at zero is the documented way to ask for exactly that.
     */
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    /* Writes are small and the line is fast; a bounded wait keeps a
     * wedged driver from hanging the tab. */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 2000;
    if (!SetCommTimeouts(g_com, &to)) {
        snprintf(err, errcap, "could not set the timeouts on %s", name);
        goto fail;
    }

    /* Anything the driver buffered before we were configured predates
     * this session and is not ours to forward. */
    PurgeComm(g_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 0;

fail:
    CloseHandle(g_com);
    g_com = INVALID_HANDLE_VALUE;
    return -1;
}

/* ------------------------------------------------------------------ */
/* The socket                                                          */
/* ------------------------------------------------------------------ */

static int sock_open(const char *host, int port, char *err, size_t errcap)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    int  nodelay = 1;
    u_long nb = 1;

    snprintf(portstr, sizeof portstr, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        snprintf(err, errcap, "cannot resolve %s", host);
        return -1;
    }

    g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        snprintf(err, errcap, "could not create a socket");
        return -1;
    }

    if (connect(g_sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        snprintf(err, errcap, "could not reach %s port %d", host, port);
        return -1;
    }
    freeaddrinfo(res);

    /* The same reasoning as the emulator path, and for a stronger
     * reason here: a real NABU's request is a handful of single bytes
     * and Nagle would hold every one of them behind an acknowledgement.
     * See docs/2026-08-07_NABU_NODELAY.md. */
    if (setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&nodelay, sizeof nodelay) != 0)
        logf_("Warning: could not disable Nagle on the socket. "
              "The link will work but will be slower than it should be.");

    /* Non-blocking, so the relay never parks in recv while a byte is
     * waiting on the cable. */
    ioctlsocket(g_sock, FIONBIO, &nb);
    return 0;
}

/* The transport nabu_channel wants, on the socket, while it is still
 * blocking-enough to be simple: the channel exchange happens before the
 * relay and is the only place the Suite speaks for itself. */
static int ch_write(void *ctx, uint8_t b)
{
    (void)ctx;
    return send(g_sock, (const char *)&b, 1, 0) == 1;
}

static int ch_read(void *ctx, uint8_t *b, unsigned ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;
    (void)ctx;
    for (;;) {
        int n = recv(g_sock, (char *)b, 1, 0);
        if (n == 1) return 1;
        if (n == 0) return 0;                    /* server hung up */
        if (WSAGetLastError() != WSAEWOULDBLOCK) return 0;
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* Open, pump, close                                                   */
/* ------------------------------------------------------------------ */

int nabu_serial_open(const nabu_serial_config_t *cfg,
                     nabu_serial_log_fn log_fn, void *log_user,
                     char *err, size_t errcap)
{
    WSADATA wsad;

    g_log = log_fn;
    g_log_user = log_user;
    g_to_nabu = g_to_server = 0;
    if (err && errcap) err[0] = '\0';

    if (!cfg || !cfg->com_port || !cfg->com_port[0]) {
        snprintf(err, errcap, "no serial port chosen");
        return -1;
    }

    if (!g_wsa) {
        if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
            snprintf(err, errcap, "could not start Winsock");
            return -1;
        }
        g_wsa = 1;
    }

    /* The cable first. See the header for why. */
    if (com_open(cfg->com_port, err, errcap) != 0) return -1;
    logf_("Serial port %s open at %d baud, 8N%d, no flow control.",
          cfg->com_port, NABU_SERIAL_BAUD, NABU_SERIAL_STOPBITS);

    if (sock_open(cfg->host, cfg->port, err, errcap) != 0) {
        CloseHandle(g_com);
        g_com = INVALID_HANDLE_VALUE;
        return -1;
    }
    logf_("Connected to %s port %d.", cfg->host, cfg->port);

    /*
     * THE CHANNEL, AND THE ONE RULE ABOUT IT.
     *
     * This goes out on the socket only, before a single byte has been
     * relayed in either direction, and the cable never sees any of it.
     * It is configuration aimed at the server, not something inserted
     * into a conversation: at this moment there is no conversation, and
     * the NABU on the other end of the cable has not said anything.
     *
     * A refusal is logged and the session continues on whatever channel
     * the server is configured for, which is exactly what nabud does
     * with a channel it does not recognise.
     */
    if (cfg->channel > 0) {
        char cherr[160];
        if (nabu_channel_select(cfg->channel, ch_write, ch_read, NULL,
                                cherr, sizeof cherr))
            logf_("Channel %d selected on the server.", cfg->channel);
        else
            logf_("Channel %d not set: %s. The server will serve whatever "
                  "channel it is configured for.", cfg->channel, cherr);
    }

    logf_("Relaying. Power on the NABU now if it is not already running.");
    return 0;
}

int nabu_serial_pump(void)
{
    char  buf[4096];
    DWORD got = 0, wrote = 0;
    int   moved = 0;
    int   n;

    if (g_com == INVALID_HANDLE_VALUE || g_sock == INVALID_SOCKET) return -1;

    /*
     * CABLE TO SERVER.
     *
     * Read whatever has arrived and send exactly that, no more and no
     * less. Nothing here looks at a byte's value, so there is no way for
     * this to drop, duplicate or reinterpret one.
     */
    if (!ReadFile(g_com, buf, sizeof buf, &got, NULL)) return -1;
    if (got > 0) {
        int off = 0;
        while (off < (int)got) {
            n = send(g_sock, buf + off, (int)got - off, 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
                return -1;
            }
            off += n;
        }
        g_to_server += got;
        moved += (int)got;
    }

    /* SERVER TO CABLE, the same way round. */
    n = recv(g_sock, buf, sizeof buf, 0);
    if (n > 0) {
        int off = 0;
        while (off < n) {
            if (!WriteFile(g_com, buf + off, (DWORD)(n - off), &wrote, NULL))
                return -1;
            if (wrote == 0) return -1;      /* the write timeout expired */
            off += (int)wrote;
        }
        g_to_nabu += (uint64_t)n;
        moved += n;
    } else if (n == 0) {
        return -1;                          /* server closed the connection */
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return -1;
    }

    return moved;
}

void nabu_serial_counts(uint64_t *to_nabu, uint64_t *to_server)
{
    if (to_nabu)   *to_nabu   = g_to_nabu;
    if (to_server) *to_server = g_to_server;
}

void nabu_serial_close(void)
{
    if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
    if (g_com != INVALID_HANDLE_VALUE) {
        CloseHandle(g_com);
        g_com = INVALID_HANDLE_VALUE;
    }
    /* WSACleanup is deliberately not called. Other tabs hold sockets in
     * this process and Winsock is reference counted per process, so
     * tearing it down here would be taking something that is not ours. */
    g_log = NULL;
    g_log_user = NULL;
}

/* ------------------------------------------------------------------ */
/* Which ports exist                                                   */
/* ------------------------------------------------------------------ */

/*
 * QueryDosDevice with a NULL name returns every device in the DOS
 * namespace, and the COM ports are the entries called COM followed by
 * digits. This is used rather than SetupAPI because it needs no extra
 * library, no device class GUID and no privileges, and because it lists
 * exactly what CreateFile can open, which is the question being asked.
 */
int nabu_serial_enumerate(char names[][16], int max)
{
    char  *buf;
    DWORD  cap = 65536, len;
    int    n = 0;
    const char *p;

    if (!names || max <= 0) return 0;
    buf = (char *)malloc(cap);
    if (!buf) return 0;

    len = QueryDosDeviceA(NULL, buf, cap);
    if (len == 0) { free(buf); return 0; }

    for (p = buf; *p && n < max; p += strlen(p) + 1) {
        int  i;
        if (strncmp(p, "COM", 3) != 0) continue;
        if (!p[3]) continue;
        for (i = 3; p[i]; i++)
            if (p[i] < '0' || p[i] > '9') break;
        if (p[i]) continue;                 /* COMsomething, not a port */
        if (strlen(p) >= 16) continue;
        snprintf(names[n], 16, "%s", p);
        n++;
    }

    free(buf);
    return n;
}
