/*
 * player_service.h - Shared HLS / HTTP video service for the Suite.
 *
 * Backend: IMFMediaEngine (Media Foundation Media Engine) in pure C,
 * MinGW UCRT64. HWND-hosted via MF_MEDIA_ENGINE_PLAYBACK_HWND, hardware
 * decode via MF_MEDIA_ENGINE_DXGI_MANAGER bound to a D3D11 device.
 *
 * The service is consumed by livetv_module (its first consumer) and may
 * be reused by any future module that needs HLS / progressive HTTP video.
 *
 * Architecture rule from the v0.2.0 dispatch: service-first, module-second.
 * No livetv-specific logic lives in player_service.
 *
 * Threading: IMFMediaEngineNotify::EventNotify fires on a Media Foundation
 * worker thread. The service marshals to the UI thread by PostMessage'ing
 * WM_PLAYER_ENGINE_EVENT to the host HWND. The host's WndProc must call
 * player_handle_window_event for that message.
 */

#ifndef PLAYER_SERVICE_H
#define PLAYER_SERVICE_H

#include <windows.h>

typedef struct Player Player;

typedef enum {
    PLAYER_STATE_IDLE = 0,    /* created, no source loaded */
    PLAYER_STATE_LOADING,     /* SetSource called, awaiting LOADEDMETADATA */
    PLAYER_STATE_LOADED,      /* metadata in, ready to Play */
    PLAYER_STATE_PLAYING,     /* engine reports PLAYING */
    PLAYER_STATE_STOPPED,     /* Pause called or user Stop */
    PLAYER_STATE_ERROR        /* MF reported an error event */
} PlayerState;

typedef void (*PlayerStateCb)(Player *p, PlayerState state, void *user);

/* Window message posted by the MF notify thread to the host HWND. The
 * host's WndProc must dispatch this to player_handle_window_event so the
 * engine's state changes get translated to PlayerStateCb invocations on
 * the UI thread. wParam = MF event code, lParam = Player*. */
#define WM_PLAYER_ENGINE_EVENT  (WM_USER + 0x42)

/* Process-wide MF + COM init. Refcounted, idempotent. Returns TRUE on
 * success. Safe to call from any thread. */
BOOL    player_init(void);
void    player_shutdown(void);

/* Construct a player bound to the caller-owned video_hwnd. That HWND
 * IS the MF_MEDIA_ENGINE_PLAYBACK_HWND surface and MF renders the
 * full client area of that HWND. The caller is responsible for sizing
 * it to the letterbox / pillarbox rect (so player_service has no
 * resize logic of its own; that lives in livetv_module per dispatch
 * amendment 2 / Phase L). Returns NULL on failure. */
Player* player_create(HWND video_hwnd);

/* Audio-only construction (dispatch 3 / Live Radio). No PLAYBACK_HWND
 * is bound, so no D3D11 device, no DXGI manager, no video swap chain
 * is created. msg_target receives WM_PLAYER_ENGINE_EVENT exactly as
 * the video player uses GetParent(video_hwnd). MF handles audio
 * rendering through the default WASAPI endpoint. */
Player* player_create_audio(HWND msg_target);

void    player_destroy(Player *p);

/* Loads an HLS (.m3u8) or progressive HTTP video source. Blocks the
 * caller until MF reports LOADEDMETADATA or ERROR, or until 15 s
 * timeout. The state callback (if registered) fires synchronously on
 * completion as well. */
BOOL    player_load_hls(Player *p, const wchar_t *url);

BOOL    player_play(Player *p);
BOOL    player_stop(Player *p);

/* On success, *w and *h receive the intrinsic pixel dimensions reported
 * by IMFMediaEngine::GetNativeVideoSize. Returns FALSE (and leaves the
 * outputs untouched) before LOADEDMETADATA has fired. The caller uses
 * the values to compute aspect ratio for its own letterbox math. */
BOOL    player_get_native_size(Player *p, int *w, int *h);

void    player_set_state_cb(Player *p, PlayerStateCb cb, void *user);

/* Force MF to refresh its DXGI swap chain to match the current
 * client size of the video HWND. Required whenever the caller
 * resizes the video HWND past the size MF originally bound at
 * SetSource time (MF's auto-rebind is unreliable across large
 * size jumps such as Win11 maximize). Safe no-op if not playing. */
void    player_update_video_stream(Player *p);

/* Marshaling entry: the host's WndProc calls this from its
 * WM_PLAYER_ENGINE_EVENT handler. wParam = MF event code, lParam =
 * Player*. Returns 0. */
LRESULT player_handle_window_event(WPARAM wParam, LPARAM lParam);

#endif /* PLAYER_SERVICE_H */
