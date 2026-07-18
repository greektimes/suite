/*
 * audio_meter_service.h - per-session WASAPI peak metering.
 *
 * Backend: IMMDeviceEnumerator -> default eRender / eConsole endpoint
 * -> IAudioSessionManager2 -> IAudioSessionEnumerator -> own-PID
 * session -> QI IAudioMeterInformation. Falls back to device-level
 * metering (IMMDevice::Activate(IID_IAudioMeterInformation)) if the
 * per-session QI is rejected on a given driver.
 *
 * Pure C, raw COM, MinGW UCRT64. Consumed by liveradio_module.
 */
#ifndef AUDIO_METER_SERVICE_H
#define AUDIO_METER_SERVICE_H

#include <windows.h>

typedef struct AudioMeter AudioMeter;

/* Process-wide COM + endpoint manager init. Refcounted, idempotent. */
BOOL  audio_meter_init(void);
void  audio_meter_shutdown(void);

/* Bind to the current process's own audio session. Creating the meter
 * before any audio is playing is allowed: the session lookup retries
 * on each poll until a match appears, then caches. */
AudioMeter* audio_meter_create(void);
void  audio_meter_destroy(AudioMeter *m);

/* Poll the current peak per channel, linear 0.0 .. 1.0. Returns FALSE
 * if no session has been bound yet or if metering is unavailable on
 * this driver. peak_l / peak_r may be NULL (then we still bind the
 * session if it appeared since the last poll). Stereo expected; on
 * mono streams peak_r equals peak_l. */
BOOL  audio_meter_poll(AudioMeter *m, float *peak_l, float *peak_r);

#endif /* AUDIO_METER_SERVICE_H */
