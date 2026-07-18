/*
 * liveradio_module.h - Live Radio tab content pane.
 *
 * Audio-only HE-AAC v2 playback of the live Greek Radio Icecast
 * stream at http://live.greekradio.ca:8000/live . Stereo LED VU
 * meter showing per-session peak levels (audio_meter_service) at
 * 30 Hz while PLAYING. State machine mirrors livetv_module:
 * SPLASH / LOADING / PLAYING / ERROR.
 *
 * Persistence: tab and mode switches hide but never destroy this
 * window. Audio continues across both. The owning shell discards
 * the HWND only at process shutdown.
 */
#ifndef LIVERADIO_MODULE_H
#define LIVERADIO_MODULE_H

#include <windows.h>

HWND liveradio_module_create(HWND parent);

#endif /* LIVERADIO_MODULE_H */
