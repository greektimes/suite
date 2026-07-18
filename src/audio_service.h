/*
 * audio_service.h - Shared Sun .au (mu-law) audio playback service.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.1.0-mvp.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Montreal Greek Times
 * This file is part of the MGT Unicorn Suite.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The service is a Suite-wide capability: any module that holds a URL
 * or a buffer of .au bytes can dispatch playback through this API and
 * a small "Now Playing" status window appears with a Stop control. The
 * service is single-track (only one playback at a time) and decodes
 * 8-bit mu-law to 16-bit PCM in-memory before handing the buffer to
 * the Windows waveOut device.
 *
 * Thread model: the public entry points are non-blocking and may be
 * called from any UI thread. Network fetch and playback run on an
 * internal worker thread; the status window posts itself messages to
 * stay responsive.
 *
 * libwww is initialized lazily by the web module; the audio service
 * assumes a working HTProfile by the time audio_service_play_url is
 * called from a click path.
 */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup. Idempotent. Registers the status-window class and
 * initializes the mu-law decode table. Safe to call multiple times. */
void audio_service_init(void);

/* Tear down at app exit: stop any active playback, free resources,
 * unregister the window class. Safe to call when nothing is active. */
void audio_service_shutdown(void);

/* Start asynchronous fetch + decode + playback of a Sun .au URL.
 * Non-blocking. The owner HWND parents the status window (may be NULL).
 * Returns 0 on accepted request, non-zero if another playback is in
 * progress and could not be replaced. */
int  audio_service_play_url(HWND owner, const char *url);

/* Decode and play an in-memory .au buffer. The caller retains ownership
 * of buf; the service copies what it needs. Returns 0 on success. */
int  audio_service_play_au_bytes(HWND owner, const void *buf, size_t len);

/* Decode and play an in-memory audio buffer; the format (Sun .au mu-law
 * or MP3) is auto-detected from the first bytes. Used by the F3 Gopher
 * client when dispatching a type 's' or type '9 .mp3' download to the
 * audio service. Caller retains ownership; the service copies. Returns
 * 0 on accepted, non-zero on rejection (currently playing). */
int  audio_service_play_bytes(HWND owner, const void *buf, size_t len);

/* Same as audio_service_play_bytes, but `name` (e.g. a gopher selector or
 * filename) is used to populate the dialog's "Filename:" row. Its last
 * path segment is shown. Pass NULL to hide the row. The service copies
 * what it needs; the caller retains ownership of buf and name. */
int  audio_service_play_bytes_named(HWND owner, const void *buf, size_t len,
                                    const char *name);

/* Stop the current playback (if any) and tear the status window down. */
void audio_service_stop(void);

/* Returns 1 if playback is currently active, 0 if idle. */
int  audio_service_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_SERVICE_H */
