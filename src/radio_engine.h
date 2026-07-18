/*
 * radio_engine.h - Native streaming-radio playback engine for the Suite.
 *
 * Part of the Montreal Greek Times Unicorn Suite v0.2.0.
 * Copyright (c) 2026 Dimitri Papadopoulos and The Montreal Greek Times.
 * AGPLv3 -- see /COPYING.
 *
 * Replaces the IMFMediaEngine path (player_service.c) for Live Radio.
 * player_service.c remains the Live TV engine and is now Live-TV-only.
 *
 * Pipeline:
 *
 *   HTTPS / HTTP Icecast stream
 *       -> WinHTTP streaming GET (continuous read on a worker thread)
 *       -> ICY-MetaInt stripper + ADTS frame parser
 *       -> Microsoft AAC Decoder MFT
 *          (CLSID 32D186A7-218F-4C75-8876-DD77273A8999; ships in
 *           mfplat.dll on every supported Windows; HE-AAC / HE-AAC v2)
 *       -> PCM ring buffer (~500 ms)
 *       -> WASAPI render client (shared mode, event-driven,
 *          AUTOCONVERTPCM so the system matches the device rate)
 *       -> audio device
 *
 * Threading: ONE worker thread does network read + ICY strip + ADTS
 * framing + MFT decode + ring write. A dedicated WASAPI render thread
 * pulls from the ring on the audio event. The caller's (UI) thread is
 * never blocked. State callbacks fire on the worker thread.
 *
 * No third-party libraries; stays AGPLv3 by virtue of the rest of the
 * Suite.
 */
#ifndef RADIO_ENGINE_H_INCLUDED
#define RADIO_ENGINE_H_INCLUDED

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioEngine RadioEngine;

typedef enum {
    RE_STATE_IDLE = 0,
    RE_STATE_CONNECTING,
    RE_STATE_PLAYING,
    RE_STATE_STOPPED,
    RE_STATE_ERROR
} REState;

/* Fired on engine state transitions. `detail` is a short human-readable
 * note (may be NULL). Invoked on the engine worker thread; the consumer
 * marshals to its UI thread as needed (InvalidateRect is thread-safe). */
typedef void (*REStateCb)(REState state, const char *detail, void *user);

/* Fired when an ICY StreamTitle metadata block is decoded. `title` is a
 * UTF-8 / Latin-1 C string (the raw ICY bytes). Invoked on the worker
 * thread. Not displayed by the current dispatch; wired for future use. */
typedef void (*REMetaCb)(const char *icy_stream_title, void *user);

RadioEngine *re_create(void);
void         re_destroy(RadioEngine *e);

void         re_set_state_cb(RadioEngine *e, REStateCb cb, void *user);
void         re_set_meta_cb (RadioEngine *e, REMetaCb  cb, void *user);

/* Start streaming `icecast_url`. Launches the worker thread and returns
 * immediately (non-blocking); state is reported via the state callback.
 * Returns FALSE only if the engine could not even spawn the worker. */
BOOL         re_start(RadioEngine *e, const wchar_t *icecast_url);

/* Stop playback: signals the worker, joins it, tears down WASAPI / MFT /
 * WinHTTP, releases all handles. Idempotent; safe to call when idle. */
void         re_stop (RadioEngine *e);

/* Last error string (engine-owned, valid until the next call). Never
 * NULL; empty string when no error has occurred. */
const char  *re_last_error(RadioEngine *e);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_ENGINE_H_INCLUDED */
