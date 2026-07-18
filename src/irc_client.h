/*
 * irc_client.h - Receive-only IRCv3 transport service for the Suite.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Owns a background worker thread that connects to an IRC server over
 * plaintext TCP, negotiates the IRCv3 capabilities required for the
 * UnrealIRCd +H on-join history replay (server-time, batch,
 * message-tags), registers with a throwaway nick, JOINs one channel,
 * answers PING, and reconnects silently with backoff on any drop.
 *
 * The service parses each line (tags + prefix + command + params) and
 * forwards ONLY user-visible channel traffic -- PRIVMSG and NOTICE --
 * to a UI HWND via PostMessage as a parsed IrcMessage. Everything else
 * (numerics, MOTD, CAP, PING/PONG, JOIN/PART/QUIT/NICK/MODE, and the
 * BATCH open/close framing) is consumed inside this TU and never
 * reaches the caller. Both the +H backlog and the live stream arrive
 * as ordinary PRIVMSG to the channel, so rendering forwarded messages
 * in arrival order yields backlog-then-live with no batch bookkeeping.
 *
 * The service knows nothing about GDI. Host, port, channel, and the
 * nick pattern are all caller-supplied (the viewer module holds the
 * greektimes.ca defaults).
 *
 * Threading: all sends (CAP/NICK/USER/JOIN/PONG) are reactive and
 * happen on the worker thread inside its own read loop, so there is no
 * cross-thread send path and no user-facing send API -- this is a
 * broadcast viewer, not a chat client.
 */

#ifndef IRC_CLIENT_H
#define IRC_CLIENT_H

#include <stddef.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WPARAM values for the notify message posted to the viewer HWND. Every
 * event carries LPARAM = IrcMessage*, freed with irc_client_free_message()
 * on the UI thread. */
#define IRC_EVT_MESSAGE  1   /* channel PRIVMSG/NOTICE (render timestamped) */
#define IRC_EVT_SYSTEM   2   /* server banner line (Connecting/Connected, CAP,
                              * numerics, MOTD); only `text` is meaningful.
                              * Emitted for a MANUAL connect's initial session
                              * only -- suppressed on automatic reconnects. */
#define IRC_EVT_FAILED   3   /* terminal pre-registration failure on a manual
                              * connect; `text` holds the human reason. The
                              * worker stops after this -- no silent retry. */
#define IRC_EVT_REGISTERED 4 /* the session reached numeric 001 (registered
                              * and JOINing). LPARAM is unused (NULL) -- the
                              * viewer updates its status from its own host/
                              * channel. Fired on the initial connect and on
                              * every silent reconnect. */

/* Kind of forwarded message. Only PRIVMSG/NOTICE are channel messages;
 * SYSTEM/FAILED events leave kind unset (the WPARAM event distinguishes). */
typedef enum IrcMsgKind {
    IRC_MSG_PRIVMSG = 1,
    IRC_MSG_NOTICE  = 2
} IrcMsgKind;

/* One forwarded channel message. Allocated by the worker thread and
 * handed to the UI thread in LPARAM; the UI thread owns it and must
 * release it with irc_client_free_message(). Single allocation -- all
 * fields are inline, no nested pointers.
 *
 * `msgid` is the IRCv3 msgid tag (empty if absent). The viewer keeps a
 * seen-msgid set so it can drop duplicates when the +H replay re-sends
 * recent lines after a reconnect. */
typedef struct IrcMessage {
    IrcMsgKind kind;
    char       nick[64];        /* source nick from prefix, "" if none */
    char       target[64];      /* message target, e.g. "#retro" */
    char       msgid[64];       /* IRCv3 msgid tag value, "" if absent */
    char       text[1024];      /* trailing text (UTF-8), NUL-terminated */
    SYSTEMTIME when_local;       /* @time= converted to workstation-local
                                  * time, or the local clock if no tag */
    int        have_server_time; /* 1 if @time= was present and parsed */
} IrcMessage;

typedef struct IrcClient IrcClient;

/* Open a receive-only IRC session on a worker thread.
 *
 *   host, port      TCP endpoint (e.g. irc.greektimes.ca, 6667).
 *   channel         channel to JOIN and mirror (e.g. "#retro").
 *   nick_pattern    registration-nick template; every 'X' is replaced
 *                   with a random digit (if the pattern has no 'X', four
 *                   random digits are appended). Re-randomized on every
 *                   reconnect and on ERR_NICKNAMEINUSE (433). NULL/empty
 *                   falls back to "mgtview-XXXX".
 *   show_banner     nonzero to forward the registration banner (Connecting/
 *                   Connected/CAP/numerics/MOTD) as IRC_EVT_SYSTEM lines on
 *                   the initial session (a manual connect). Automatic
 *                   reconnects after a mid-session drop always suppress the
 *                   banner regardless of this flag.
 *   notify_hwnd/msg where IRC_EVT_* are posted.
 *
 * Failure model: if the initial connect never reaches IRC registration
 * (numeric 001) -- getaddrinfo failure, connection refused, a bounded
 * connect timeout, or the peer closing before 001 -- the worker posts
 * IRC_EVT_FAILED with a human reason and STOPS (no silent-retry loop). A
 * drop AFTER a session has registered still reconnects silently with
 * backoff. A user irc_client_close() during a pending connect tears down
 * silently (no IRC_EVT_FAILED).
 *
 * Returns 0 and writes *out on success; non-zero on failure with *out
 * left NULL. The worker runs until irc_client_close(). */
int  irc_client_open(const char *host, int port,
                     const char *channel, const char *nick_pattern,
                     int show_banner,
                     HWND notify_hwnd, UINT notify_msg,
                     IrcClient **out);

/* Signal the worker to stop, close the socket, join the thread, and
 * free the handle. After this returns the handle is invalid. Messages
 * already queued in the UI thread's message queue may still arrive; the
 * caller frees those normally with irc_client_free_message(). */
void irc_client_close(IrcClient *c);

/* Release an IrcMessage delivered via IRC_EVT_MESSAGE. */
void irc_client_free_message(IrcMessage *m);

#ifdef __cplusplus
}
#endif

#endif /* IRC_CLIENT_H */
