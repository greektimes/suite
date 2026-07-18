/* cuseeme_module.c - F7 "CU-SeeMe Live TV" tab: receive-only CU-SeeMe client.
 *
 * Lifecycle (locked brief):
 *   - tab created with NO connection (status "Disconnected")
 *   - on activate: start a worker thread that performs the OpenContinue
 *     handshake to cu-seeme.greektv.ca:7648 and receives media
 *   - on deactivate: signal the worker, send a CloseSocket (Message 6),
 *     tear the UDP socket + waveOut down, return to "Disconnected"
 *   - on shutdown: same clean teardown
 *   - on connection loss/timeout: retry with 5s..30s backoff
 *
 * Threading: the worker owns the socket, decoders and waveOut. It never
 * touches GDI/HWND directly - it converts each completed frame to an 8-bit
 * gray staging buffer under g_lock and PostMessages the render window to
 * repaint. The UI thread paints (DIB StretchDIBits + overlay) and, on a 1s
 * timer, formats the status strip from shared counters.
 *
 * Video is decoded at native 160x120; the render doubles it to 320x240 for
 * display (2x nearest-pixel via SetStretchBltMode COLORONCOLOR, crisp/no
 * blur), centered. The wire format and decode stay native 160x120 - only the
 * on-screen render is scaled.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "cuseeme_module.h"
#include "cuseeme_proto.h"
#include "cuseeme_video.h"
#include "cuseeme_deltamod.h"
#include "cuseeme_mulaw.h"
#include "cuseeme_idvi.h"
#include "suite_shell.h"
#include "suite_logo.h"   /* grayscale logo draw for the Start screen */
#include "suite_res.h"    /* IDR_LOGO_TV */
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUSM_RENDER_CLASS   "MGTCuSeeMeRender"
#define WM_APP_CUSM_FRAME   (WM_APP + 31)
#define CUSM_TIMER_STATUS   1
#define CUSM_TIMER_CONNECT  2      /* "Connecting..." dot animation */
#define CUSM_STATUS_H       22
#define CUSM_STRIP_H        52     /* top connection strip (mirrors IRC/WAIS) */
#define CUSM_CONNECT_MS     300    /* dot-cycle interval (mirrors LiveTV) */
#define CUSM_CONNECT_FRAMES 4

/* Connection-strip control IDs (7300+). */
#define IDC_CUSM_REFL_EDIT       7301
#define IDC_CUSM_PORT_EDIT       7302
#define IDC_CUSM_AUDIO_COMBO     7303
#define IDC_CUSM_CONNECT_BTN     7304
#define IDC_CUSM_DISCONNECT_BTN  7305
#define IDC_CUSM_STATUS          7306

/* F7 UI lifecycle, mirroring the Modern Live TV tab (SPLASH/LOADING/PLAYING).
 * This gates WHEN the worker runs; it does not change the decode or 2x render. */
enum {
    CUSM_UI_SPLASH = 0,   /* Start screen: gray logo + caption + Start button */
    CUSM_UI_CONNECTING,   /* worker running, "Connecting" dots, awaiting 1st frame */
    CUSM_UI_LIVE          /* live 320x240 render + Stop button */
};

enum {
    ST_DISCONNECTED = 0,
    ST_CONNECTING,
    ST_HANDSHAKING,
    ST_RECEIVING,
    ST_LOST
};

/* waveOut buffer pool: audio arrives in ~200 ms chunks (1600 samples). */
#define CUSM_NWBUF      24
#define CUSM_WBUF_SAMP  8192   /* generous upper bound per packet */

/* ---- shared state (guarded by g_lock except where noted) ---- */
static CRITICAL_SECTION g_lock;
static int       g_lock_init = 0;
static int       g_state = ST_DISCONNECTED;
static uint8_t   g_gray[CUSM_VID_W * CUSM_VID_H];  /* latest frame, display gray */
static int       g_have_frame = 0;
static unsigned  g_frames_total = 0;     /* frame-end packets received */
static unsigned  g_drops_total = 0;      /* video frames completed while a real
                                            unified-0x10 stream gap occurred =
                                            incomplete/suspect frames (network loss) */
static unsigned  g_video_pkts = 0;
static unsigned  g_audio_bytes = 0;      /* compressed audio bytes (for kbps) */
static unsigned  g_audio_unknown = 0;    /* audio packets with unhandled format id */
static int       g_retry_secs = 5;

/* ---- UI-thread state ---- */
static HWND      g_hContent = NULL;
static HWND      g_hRender  = NULL;
static HWND      g_hStatus  = NULL;
static int       g_controls_created = 0;
static int       g_class_reg = 0;
static HBITMAP   g_hDib = NULL;          /* 8-bit gray DIB section, 160x120 */
static void     *g_dibBits = NULL;
/* Gray BITMAPINFO (header + 256-entry identity palette) shared by the DIB
 * section and by StretchDIBits, so the frame stays in the 8-bit grayscale
 * domain end-to-end (COLORONCOLOR nearest-pixel scaling, no interpolation). */
static struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } g_dibInfo;

/* ---- Splash/Connecting/Live chrome state (UI thread only) ---- */
static int       g_ui_state = CUSM_UI_SPLASH;
static int       g_connect_frame = 0;         /* 0..3 dot count */
static RECT      g_caption_rect = {0, 0, 0, 0};
static HFONT     g_hCaptionFont = NULL;

/* ---- connection strip (WAIS/IRC-style) + committed session config ---- */
static HWND      g_hReflLbl, g_hReflEdit;
static HWND      g_hPortLbl, g_hPortEdit;
static HWND      g_hAudioLbl, g_hAudioCombo;
static HWND      g_hConnectBtn, g_hDisconnectBtn;
static int       g_connected = 0;             /* UI session gate */
static char      g_reflector[256] = CUSEEME_HOST;   /* committed by Connect */
static int       g_port = CUSEEME_PORT;             /* reflector port (dest) */
static int       g_audio_codec = CUSM_AUDF_IDVI;    /* chosen decoder */

static const char *cusm_codec_name(int fmt) {
    return (fmt == CUSM_AUDF_DELTAMOD) ? "Delta-Mod" : "Intel DVI";
}

/* ---- worker thread control ---- */
static HANDLE    g_thread = NULL;
static HANDLE    g_stopEvent = NULL;     /* signalled to ask worker to exit */
static volatile LONG g_running = 0;

/* rate sampling (UI thread only) */
static unsigned  g_last_frames = 0;
static unsigned  g_last_abytes = 0;

/* ============================ waveOut ============================ */

static HWAVEOUT  g_wave = NULL;
static WAVEHDR   g_whdr[CUSM_NWBUF];
static short    *g_wbuf[CUSM_NWBUF];
static int       g_wbuf_init = 0;

static void waveout_open(void) {
    WAVEFORMATEX wf;
    int i;
    if (g_wave) return;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = CUSM_AUDIO_RATE;   /* 8000 */
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (wf.wBitsPerSample / 8) * wf.nChannels;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wave = NULL;
        return;
    }
    if (!g_wbuf_init) {
        for (i = 0; i < CUSM_NWBUF; i++)
            g_wbuf[i] = (short *)malloc(CUSM_WBUF_SAMP * sizeof(short));
        g_wbuf_init = 1;
    }
    memset(g_whdr, 0, sizeof(g_whdr));
}

static void waveout_close(void) {
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (i = 0; i < CUSM_NWBUF; i++) {
        if (g_whdr[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_wave, &g_whdr[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_wave);
    g_wave = NULL;
    memset(g_whdr, 0, sizeof(g_whdr));
}

/* Push decoded PCM to the soundcard. Worker thread only. */
static void waveout_push(const short *pcm, int nsamp) {
    int i;
    if (!g_wave || nsamp <= 0) return;
    if (nsamp > CUSM_WBUF_SAMP) nsamp = CUSM_WBUF_SAMP;
    for (i = 0; i < CUSM_NWBUF; i++) {
        WAVEHDR *h = &g_whdr[i];
        if (h->dwFlags == 0 || (h->dwFlags & WHDR_DONE)) {
            if (h->dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wave, h, sizeof(WAVEHDR));
            memcpy(g_wbuf[i], pcm, nsamp * sizeof(short));
            memset(h, 0, sizeof(*h));
            h->lpData         = (LPSTR)g_wbuf[i];
            h->dwBufferLength = nsamp * sizeof(short);
            if (waveOutPrepareHeader(g_wave, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                waveOutWrite(g_wave, h, sizeof(WAVEHDR));
            return;
        }
    }
    /* all buffers busy: drop this chunk (keeps real-time, avoids latency creep) */
}

/* ============================ worker ============================ */

static void set_state(int s) {
    EnterCriticalSection(&g_lock);
    g_state = s;
    LeaveCriticalSection(&g_lock);
}

/* Discover the local IPv4 toward the reflector for the OpenContinue SrcAddr
 * identifier (ocp-SE: source address is a per-client unique id). */
static uint32_t local_ip_toward(struct sockaddr_in *dst) {
    uint32_t ip = 0;
    SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t != INVALID_SOCKET) {
        struct sockaddr_in loc;
        int ll = sizeof(loc);
        if (connect(t, (struct sockaddr *)dst, sizeof(*dst)) == 0 &&
            getsockname(t, (struct sockaddr *)&loc, &ll) == 0)
            ip = ntohl(loc.sin_addr.s_addr);
        closesocket(t);
    }
    return ip;
}

/* One connect/receive session. Returns when the session ends (timeout, error,
 * or stop requested). Returns 1 if stop was requested (caller should exit). */
static int session_run(void) {
    struct addrinfo hints, *res = NULL;
    struct sockaddr_in dst;
    SOCKET s = INVALID_SOCKET;
    uint32_t my_ip;
    uint16_t my_port = 0;        /* real local UDP port actually bound (see below);
                                    also written into the OpenContinue SrcPort. */
    uint8_t oc[CUSM_OC_MAX_LEN];
    uint32_t sender_ip = 0;      /* advertised addr of the broadcast video sender,
                                    learned from the reflector's relayed OpenContinues;
                                    0 until discovered. May be loopback/private -
                                    echoed verbatim into our subscribing ClientInfo. */
    int oclen, stop = 0;
    /* Monotonic OpenContinue sequence (seed from the ms clock). Not the reconnect
     * fix - the reflector keys its per-IP media slot on the real (IP, UDP port)
     * endpoint, not on this counter - but a non-decreasing seq is correct client
     * behaviour, so keep it. */
    uint32_t seq = (uint32_t)GetTickCount();
    DWORD last_keepalive, last_rx;
    cusm_video_t vid;
    cusm_deltamod_t dm;
    uint32_t last_seq = 0;       /* unified 0x10 seq of the last received packet */
    int      have_last_seq = 0;
    int      frame_lossy = 0;    /* real stream gap seen during the current frame */
    int      stabilized = 0;     /* set once the first full frame has displayed;
                                    drops before this are startup keyframe settle
                                    (we join mid-stream, so the first assembled
                                    frame is inherently partial) and not counted */

    set_state(ST_CONNECTING);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    {
        /* Reflector host:port come from the connection strip (committed into
         * g_reflector/g_port at Connect). The LOCAL bind below stays the
         * fixed client port 7648 (the reconnect-slot trick), independent of
         * the destination port. */
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", g_port);
        if (getaddrinfo(g_reflector, portstr, &hints, &res) != 0 || !res)
            return 0;  /* DNS failure -> caller backs off */
    }
    dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    {
        /* Bind a FIXED local UDP port (7648, the classic CU-SeeMe client port).
         * The reflector keys its per-IP media slot on the real (IP, UDP port)
         * endpoint and reaps it slowly on idle (~60-120 s). Reconnecting from the
         * SAME fixed local port reuses the same NAT-mapped endpoint, so a quick
         * Stop->Start lands on the still-live slot and resumes A/V immediately -
         * exactly what the real Cornell client does. Binding an OS-assigned
         * ephemeral port (the old behaviour) presents a NEW endpoint from an
         * already-occupied IP, which the reflector ignores until the old slot
         * reaps: the reconnect lockout. Proven against the live reflector: fixed
         * port resumes instantly, ephemeral stays silent back-to-back.
         * SO_REUSEADDR lets us re-bind 7648 immediately after the previous
         * socket closed; if 7648 is somehow busy (e.g. a 2nd instance) fall back
         * to ephemeral so media still works, only instant-reconnect is lost. */
        struct sockaddr_in local, bound;
        int bl = sizeof(bound);
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(CUSEEME_PORT);   /* 7648 */
        if (bind(s, (struct sockaddr *)&local, sizeof(local)) != 0) {
            local.sin_port = 0;                 /* fallback: OS-assigned ephemeral */
            bind(s, (struct sockaddr *)&local, sizeof(local));
        }
        if (getsockname(s, (struct sockaddr *)&bound, &bl) == 0)
            my_port = ntohs(bound.sin_port);
    }
    my_ip = local_ip_toward(&dst);

    cusm_video_init(&vid);
    cusm_deltamod_init(&dm);
    waveout_open();

    set_state(ST_HANDSHAKING);
    /* First OC: bare open (no sender known yet -> subscribe_ip 0). The reflector
     * registers us and starts relaying participant OpenContinues, from which we
     * learn the broadcast sender's advertised address (sender_ip). */
    oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port, "MGT Unicorn Suite",
                                    1, 0, 0);
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));

    last_keepalive = GetTickCount();
    last_rx = GetTickCount();

    for (;;) {
        fd_set rf;
        struct timeval tv;
        int r;

        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) { stop = 1; break; }

        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 150000;
        r = select(0, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(s, &rf)) {
            uint8_t rb[4096];
            struct sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s, (char *)rb, sizeof(rb), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                cusm_packet_t pk;
                last_rx = GetTickCount();
                if (cusm_parse(rb, n, &pk)) {
                    /* Real packet-loss detection on the reflector's unified 0x10
                     * counter. Audio+video+control all share one strictly-
                     * increasing sequence, so it is contiguous unless the network
                     * actually drops a datagram; a forward jump > 1 therefore means
                     * >= 1 packet was genuinely lost. Remember that so the frame in
                     * flight is flagged incomplete at its VID_END. Reordered or
                     * duplicate packets (seq <= last) are ignored, not counted. */
                    if (have_last_seq) {
                        if (pk.seq > last_seq + 1) frame_lossy = 1;
                        if (pk.seq > last_seq)     last_seq = pk.seq;
                    } else {
                        last_seq = pk.seq;
                        have_last_seq = 1;
                    }
                    if (pk.data_type == CUSM_DT_VIDEO_SMALL) {
                        set_state(ST_RECEIVING);
                        cusm_video_decode(&vid, pk.vid_data, pk.vid_len);
                        EnterCriticalSection(&g_lock);
                        g_video_pkts++;
                        if (pk.message == CUSM_MSG_VID_END) {
                            cusm_video_to_gray8(&vid, g_gray);
                            g_have_frame = 1;
                            g_frames_total++;
                            if (!stabilized) {
                                /* First full frame decoded + displayed = settle
                                 * point. Any gap up to here is the startup
                                 * keyframe painting in from gray, not real loss,
                                 * so zero the counter and start counting honestly
                                 * from the next frame on. */
                                stabilized = 1;
                                g_drops_total = 0;
                            } else if (frame_lossy) {
                                /* frame completed while a real gap occurred -> suspect */
                                g_drops_total++;
                            }
                            frame_lossy = 0;
                        }
                        LeaveCriticalSection(&g_lock);
                        if (pk.message == CUSM_MSG_VID_END && g_hRender)
                            PostMessageA(g_hRender, WM_APP_CUSM_FRAME, 0, 0);
                    } else if (pk.data_type == CUSM_DT_AUDIO) {
                        /* Dispatch on the VAT-header format id (Audio-RBK):
                         *   30 -> IDVI (Intel DVI / IMA ADPCM) - the live wire
                         *   26 -> DELTAMOD (legacy, dormant)
                         *    0 -> G.711 mu-law (PCMU, dormant)
                         * All decode to 8 kHz mono 16-bit PCM and share the same
                         * waveOut playout path.
                         *
                         * As of the 2026-06-30 encoder fix the reflector emits
                         * VAT format 30 = IDVI: 204-byte blocks (4-byte seed
                         * header + 200 nibble bytes = 400 samples, timestamp
                         * +400). The Cornell-exact decode conventions live in
                         * cuseeme_idvi.[ch]. Unknown formats fall through to the
                         * unknown-format counter and are dropped (silent). */
                        /* The connection strip's "Audio Codec" selects the
                         * decoder (committed into g_audio_codec at Connect),
                         * overriding the wire format id: whatever the user
                         * picked is what runs on the audio payload. Default
                         * Intel DVI matches the reflector's current format 30
                         * emission; Delta-Mod engages the format-26 decoder. */
                        static short pcm[CUSM_WBUF_SAMP];
                        int ns;
                        if (g_audio_codec == CUSM_AUDF_DELTAMOD)
                            ns = cusm_deltamod_decode(&dm, pk.aud_data, pk.aud_len, pcm);
                        else
                            ns = cusm_idvi_decode(pk.aud_data, pk.aud_len, pcm);
                        if (ns > 0) {
                            set_state(ST_RECEIVING);
                            waveout_push(pcm, ns);
                            EnterCriticalSection(&g_lock);
                            g_audio_bytes += (unsigned)pk.aud_len;
                            LeaveCriticalSection(&g_lock);
                        }
                    } else if (pk.data_type == CUSM_DT_OPENCONTINUE) {
                        /* Participant discovery: the reflector relays each
                         * sender's OpenContinue. A relayed OC whose Send Mode
                         * advertises video (SMALL/BIG) identifies the broadcast
                         * source ("GreekTV"); its advertised address is the
                         * packet SrcAddr. Record it so the next keepalive
                         * subscribes to it. We do NOT reject a loopback/private
                         * advertised address - on a reflector the media is
                         * relayed to us on this socket, not dialed at that addr.
                         * Our own/lurker OCs advertise Send Mode NONE and are
                         * ignored by this test. */
                        if ((pk.oc_sendmode == CUSM_SEND_SMALL ||
                             pk.oc_sendmode == CUSM_SEND_BIG) &&
                            pk.src_addr != 0)
                            sender_ip = pk.src_addr;
                    }
                    /* DataType 105 welcome / 100 ack confirm liveness only. */
                }
            }
        }

        /* keepalive every 2 s (ocp-SE: Windows sends OpenContinue every 2 s) */
        if (GetTickCount() - last_keepalive >= 2000) {
            last_keepalive = GetTickCount();
            seq++;
            /* Once the broadcast sender is known, every keepalive carries the
             * subscribing ClientInfo (clientCount=1) so the reflector relays its
             * video+audio to us. Before discovery sender_ip is 0 -> bare OC. */
            oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port,
                                            "MGT Unicorn Suite", 1, 0, sender_ip);
            sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
        }
        /* connection lost: no media for 8 s */
        if (GetTickCount() - last_rx >= 8000) break;
    }

    /* Clean teardown: send Close (Message 6) on EVERY exit path (stop or timeout),
     * retransmitted twice since UDP is unreliable. Note the fixed-port bind above
     * is what actually guarantees instant reconnect (the reflector's per-IP slot,
     * keyed on the real (IP,port) endpoint, is reused); the Close is just polite
     * cleanup and does not by itself free the slot within the reap window. */
    seq++;
    oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port, "MGT Unicorn Suite",
                                    1, 1, 0);
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
    closesocket(s);
    waveout_close();
    return stop;
}

static DWORD WINAPI worker_proc(LPVOID arg) {
    (void)arg;
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        int stop = session_run();
        if (stop) break;
        /* session ended without a stop request: connection lost -> backoff */
        EnterCriticalSection(&g_lock);
        g_state = ST_LOST;
        LeaveCriticalSection(&g_lock);
        /* interruptible backoff, 5s..30s */
        if (WaitForSingleObject(g_stopEvent, (DWORD)g_retry_secs * 1000) == WAIT_OBJECT_0)
            break;
        EnterCriticalSection(&g_lock);
        if (g_retry_secs < 30) g_retry_secs += 5;
        LeaveCriticalSection(&g_lock);
    }
    set_state(ST_DISCONNECTED);
    return 0;
}

static void worker_start(void) {
    if (g_running) return;
    if (!g_stopEvent) g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_stopEvent);
    EnterCriticalSection(&g_lock);
    g_retry_secs = 5;
    g_frames_total = g_drops_total = g_video_pkts = g_audio_bytes = 0;
    g_last_frames = g_last_abytes = 0;
    g_have_frame = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, worker_proc, NULL, 0, NULL);
}

static void worker_stop(void) {
    if (!g_running) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 4000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    InterlockedExchange(&g_running, 0);
    set_state(ST_DISCONNECTED);
}

/* ============================ rendering ============================ */

static void ensure_dib(void) {
    if (g_hDib) return;
    {
        HDC dc = GetDC(NULL);
        int i;
        memset(&g_dibInfo, 0, sizeof(g_dibInfo));
        g_dibInfo.h.biSize = sizeof(BITMAPINFOHEADER);
        g_dibInfo.h.biWidth = CUSM_VID_W;
        g_dibInfo.h.biHeight = -CUSM_VID_H;        /* top-down */
        g_dibInfo.h.biPlanes = 1;
        g_dibInfo.h.biBitCount = 8;
        g_dibInfo.h.biCompression = BI_RGB;
        g_dibInfo.h.biClrUsed = 256;
        for (i = 0; i < 256; i++) {
            g_dibInfo.pal[i].rgbRed = g_dibInfo.pal[i].rgbGreen =
                g_dibInfo.pal[i].rgbBlue = (BYTE)i;
        }
        g_hDib = CreateDIBSection(dc, (BITMAPINFO *)&g_dibInfo, DIB_RGB_COLORS,
                                  &g_dibBits, NULL, 0);
        if (dc) ReleaseDC(NULL, dc);
    }
}

/* ---- Start / Connecting chrome (adapted from livetv_module.c) ---- */

static int cusm_dpi(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d ? d : 96;
}

static HFONT cusm_make_font(int pt, int weight, int dpi) {
    LOGFONTA lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -MulDiv(pt, dpi, 72);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyA(lf.lfFaceName, "Segoe UI");
    return CreateFontIndirectA(&lf);
}

static void ensure_ui_fonts(HWND hwnd) {
    int dpi = cusm_dpi(hwnd);
    if (!g_hCaptionFont) g_hCaptionFont = cusm_make_font(18, FW_SEMIBOLD, dpi);
}

/* Responsive centered logo square (mirrors lt_compute_logo_rect). */
static void cusm_compute_logo_rect(HWND hwnd, RECT *out) {
    RECT rc;
    int w, h, dpi, shorter, dim, max_dim, min_dim, block_h, top, min_top;
    GetClientRect(hwnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    dpi = cusm_dpi(hwnd);
    shorter = (w < h) ? w : h;
    dim = shorter * 32 / 100;
    max_dim = MulDiv(300, dpi, 96);
    min_dim = MulDiv(120, dpi, 96);
    if (dim > max_dim) dim = max_dim;
    if (dim < min_dim) dim = min_dim;
    block_h = dim + MulDiv(60, dpi, 96);   /* logo + caption (no button now) */
    top = rc.top + (h - block_h) / 2;
    min_top = rc.top + MulDiv(20, dpi, 96);
    if (top < min_top) top = min_top;
    out->left   = rc.left + (w - dim) / 2;
    out->top    = top;
    out->right  = out->left + dim;
    out->bottom = out->top + dim;
}

/* Paint the SPLASH / CONNECTING chrome into the (already bg-filled) mem DC:
 * grayscale MGT-TV logo and caption. Starting is the strip's Connect button;
 * there is no in-render Start button. */
static void cusm_paint_splash(HWND hwnd, HDC mem, int w, int h) {
    int dpi = cusm_dpi(hwnd);
    RECT lrc, tr;
    HFONT oldf;
    HICON icon;

    ensure_ui_fonts(hwnd);
    cusm_compute_logo_rect(hwnd, &lrc);

    /* Grayscale (luminance-desaturated) MGT-TV logo; app-icon fallback. */
    icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    if (!suite_logo_draw_gray(mem, IDR_LOGO_TV, lrc.left, lrc.top,
                              lrc.right - lrc.left) && icon)
        DrawIconEx(mem, lrc.left, lrc.top, icon, lrc.right - lrc.left,
                   lrc.bottom - lrc.top, 0, NULL, DI_NORMAL);

    /* Caption 24 px below the logo. */
    SetBkMode(mem, TRANSPARENT);
    tr.left = 0; tr.right = w;
    tr.top = lrc.bottom + MulDiv(24, dpi, 96);
    tr.bottom = tr.top + MulDiv(40, dpi, 96);
    g_caption_rect = tr;   /* cached so the dot timer invalidates just this band */
    SetTextColor(mem, RGB(255, 255, 255));
    oldf = (HFONT)SelectObject(mem, g_hCaptionFont);
    if (g_ui_state == CUSM_UI_CONNECTING) {
        /* Same 3-dot cycle as LiveTV: "Connecting" pinned, only dots move. */
        static const char *dots[CUSM_CONNECT_FRAMES] = { "   ", ".  ", ".. ", "..." };
        char connect_buf[24];
        RECT mr;
        int f = g_connect_frame % CUSM_CONNECT_FRAMES, fw;
        snprintf(connect_buf, sizeof(connect_buf), "Connecting%s", dots[f]);
        mr.left = 0; mr.top = tr.top; mr.right = w; mr.bottom = tr.bottom;
        DrawTextA(mem, "Connecting...", -1, &mr,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        fw = mr.right - mr.left;
        tr.left = (w - fw) / 2;
        DrawTextA(mem, connect_buf, -1, &tr,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    } else {
        DrawTextA(mem, "Live CU-SeeMe TV", -1, &tr,
                  DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(mem, oldf);
}

static void render_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP membm, oldbm;
    int W, H, vx, vy, have;
    HBRUSH bg;

    GetClientRect(hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;

    /* double-buffer the whole render area to kill flicker */
    mem = CreateCompatibleDC(dc);
    membm = CreateCompatibleBitmap(dc, W, H);
    oldbm = (HBITMAP)SelectObject(mem, membm);

    bg = CreateSolidBrush(RGB(16, 16, 16));   /* dark period-authentic surround */
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    if (g_ui_state != CUSM_UI_LIVE) {
        /* Start screen (SPLASH) or Connecting screen: gray logo + caption
         * (+ Start button in SPLASH). No video until the session is live. */
        cusm_paint_splash(hwnd, mem, W, H);
    } else {
        vx = (W - CUSM_VID_W * 2) / 2;   /* centre the 2x (320x240) render */
        vy = (H - CUSM_VID_H * 2) / 2;
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;

        ensure_dib();
        EnterCriticalSection(&g_lock);
        have = g_have_frame;
        if (have && g_dibBits)
            memcpy(g_dibBits, g_gray, CUSM_VID_W * CUSM_VID_H);
        LeaveCriticalSection(&g_lock);

        if (have && g_dibBits) {
            /* 2x pixel-doubled display: draw the native 160x120 gray DIB straight
             * to a 320x240 destination via StretchDIBits, keeping the raw DIB bytes
             * with their gray BITMAPINFO end-to-end. COLORONCOLOR
             * (STRETCH_DELETESCANS) is nearest-pixel: it replicates existing gray
             * palette entries without interpolation - crisp, and no colour is
             * introduced (stays in the grayscale domain). */
            SetStretchBltMode(mem, COLORONCOLOR);
            StretchDIBits(mem, vx, vy, CUSM_VID_W * 2, CUSM_VID_H * 2,
                          0, 0, CUSM_VID_W, CUSM_VID_H,
                          g_dibBits, (BITMAPINFO *)&g_dibInfo, DIB_RGB_COLORS, SRCCOPY);
        } else {
            /* no frame yet: black 320x240 video rect */
            RECT vr; vr.left = vx; vr.top = vy;
            vr.right = vx + CUSM_VID_W * 2; vr.bottom = vy + CUSM_VID_H * 2;
            HBRUSH blk = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(mem, &vr, blk);
            DeleteObject(blk);
        }

        /* Video renders clean - no on-image overlay and no in-render Stop
         * button. The frame/drop/kbps readout lives in the bottom status
         * bar (see update_status); stopping is the strip's Disconnect. */
    }

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbm);
    DeleteObject(membm);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static const char *state_text(int st, int retry) {
    static char buf[160];
    switch (st) {
        case ST_CONNECTING:
            snprintf(buf, sizeof(buf), "Connecting to %s:%d...",
                     CUSEEME_HOST, CUSEEME_PORT);
            return buf;
        case ST_HANDSHAKING:
            return "Handshaking...";
        case ST_LOST:
            snprintf(buf, sizeof(buf),
                     "Connection lost - retrying in %ds...", retry);
            return buf;
        case ST_DISCONNECTED:
            return "Disconnected";
        default:
            return "Disconnected";
    }
}

static void update_status(void) {
    int st, retry;
    unsigned frames, drops, abytes;
    char buf[192];
    EnterCriticalSection(&g_lock);
    st = g_state; retry = g_retry_secs;
    frames = g_frames_total; drops = g_drops_total; abytes = g_audio_bytes;
    LeaveCriticalSection(&g_lock);

    if (st == ST_RECEIVING) {
        unsigned fps = frames - g_last_frames;          /* timer fires ~1/s */
        unsigned akbps = ((abytes - g_last_abytes) * 8) / 1000;
        g_last_frames = frames;
        g_last_abytes = abytes;
        /* Include the engaged audio decoder so the chosen codec is visible
         * (and externally verifiable) while receiving. */
        snprintf(buf, sizeof(buf),
                 "Receiving (v: %u fps, a: %u kbps [%s], %u drops)",
                 fps, akbps, cusm_codec_name(g_audio_codec), drops);
        if (g_hStatus) SetWindowTextA(g_hStatus, buf);
    } else {
        if (g_hStatus) SetWindowTextA(g_hStatus, state_text(st, retry));
    }
}

/* Connection-strip session gate (mirrors WAIS/IRC). Disconnected:
 * Reflector/Port/Audio + Connect enabled, Disconnect grayed. Connected:
 * those grayed, Disconnect enabled. */
static void cusm_set_connected_state(int connected) {
    g_connected = connected;
    EnableWindow(g_hReflEdit,       !connected);
    EnableWindow(g_hPortEdit,       !connected);
    EnableWindow(g_hAudioCombo,     !connected);
    EnableWindow(g_hConnectBtn,     !connected);
    EnableWindow(g_hDisconnectBtn,   connected);
}

/* Connect: read reflector / port / audio-codec from the strip, commit them,
 * and start the receive worker. Replaces the old Start button / click path. */
static void cusm_connect(void) {
    char refl[256], ports[16];
    int  port, sel;

    if (g_connected) return;
    if (!g_hReflEdit || !g_hPortEdit || !g_hAudioCombo) return;

    GetWindowTextA(g_hReflEdit, refl, sizeof(refl));
    GetWindowTextA(g_hPortEdit, ports, sizeof(ports));
    if (!refl[0] || !ports[0]) {
        if (g_hStatus) SetWindowTextA(g_hStatus, "Reflector and port are required.");
        return;
    }
    port = atoi(ports);
    if (port <= 0 || port > 65535) {
        if (g_hStatus) SetWindowTextA(g_hStatus, "Port must be 1-65535.");
        return;
    }
    sel = (int)SendMessageA(g_hAudioCombo, CB_GETCURSEL, 0, 0);
    g_audio_codec = (sel == 1) ? CUSM_AUDF_DELTAMOD : CUSM_AUDF_IDVI;

    strncpy(g_reflector, refl, sizeof(g_reflector) - 1);
    g_reflector[sizeof(g_reflector) - 1] = '\0';
    g_port = port;

    {   /* trace the chosen codec at connect (verifiable via a debugger). */
        char t[320];
        snprintf(t, sizeof(t), "[F7] Connect reflector=%s:%d audio-codec=%s\n",
                 g_reflector, g_port, cusm_codec_name(g_audio_codec));
        OutputDebugStringA(t);
    }

    g_ui_state = CUSM_UI_CONNECTING;
    g_connect_frame = CUSM_CONNECT_FRAMES - 1;   /* seed on full dots */
    worker_start();
    if (g_hRender) SetTimer(g_hRender, CUSM_TIMER_CONNECT, CUSM_CONNECT_MS, NULL);
    cusm_set_connected_state(1);
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

/* Disconnect: stop the worker (close, socket, waveOut) and return to the
 * disconnected state with the strip editable. Rendered logo/caption resume. */
static void cusm_disconnect(void) {
    worker_stop();
    if (g_hRender) KillTimer(g_hRender, CUSM_TIMER_CONNECT);
    g_ui_state = CUSM_UI_SPLASH;
    cusm_set_connected_state(0);
    if (g_hStatus) SetWindowTextA(g_hStatus, "Disconnected");
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

static LRESULT CALLBACK render_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_CUSM_FRAME:
            /* First frame after Start = session established: go live. */
            if (g_ui_state == CUSM_UI_CONNECTING) {
                KillTimer(hwnd, CUSM_TIMER_CONNECT);
                g_ui_state = CUSM_UI_LIVE;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == CUSM_TIMER_STATUS) {
                update_status();
                /* repaint overlay counters even between frames while live */
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == CUSM_TIMER_CONNECT) {
                if (g_ui_state == CUSM_UI_CONNECTING) {
                    g_connect_frame = (g_connect_frame + 1) % CUSM_CONNECT_FRAMES;
                    InvalidateRect(hwnd, &g_caption_rect, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            /* Clicking the logo/render area does nothing now (no click-to-
             * start). Starting is the strip's Connect button. */
            SetFocus(hwnd);
            return 0;
        case WM_PAINT:
            render_paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;   /* painted in WM_PAINT */
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ============================ module entry points ============================ */

static void register_render_class(HINSTANCE hInst) {
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = render_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CUSM_RENDER_CLASS;
    RegisterClassExA(&wc);
    g_class_reg = 1;
}

void cuseeme_module_init(void) {
    WSADATA wsa;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
    /* WSAStartup is harmless if the Suite already started Winsock elsewhere. */
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

void cuseeme_module_shutdown(void) {
    worker_stop();
    waveout_close();
    if (g_wbuf_init) {
        int i;
        for (i = 0; i < CUSM_NWBUF; i++) { free(g_wbuf[i]); g_wbuf[i] = NULL; }
        g_wbuf_init = 0;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; g_dibBits = NULL; }
    if (g_hCaptionFont) { DeleteObject(g_hCaptionFont); g_hCaptionFont = NULL; }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = NULL; }
}

void cuseeme_module_activate(HWND content) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    g_hContent = content;
    register_render_class(hInst);

    if (g_controls_created) {
        ShowWindow(g_hReflLbl,       SW_SHOW);
        ShowWindow(g_hReflEdit,      SW_SHOW);
        ShowWindow(g_hPortLbl,       SW_SHOW);
        ShowWindow(g_hPortEdit,      SW_SHOW);
        ShowWindow(g_hAudioLbl,      SW_SHOW);
        ShowWindow(g_hAudioCombo,    SW_SHOW);
        ShowWindow(g_hConnectBtn,    SW_SHOW);
        ShowWindow(g_hDisconnectBtn, SW_SHOW);
        ShowWindow(g_hRender,        SW_SHOW);
        ShowWindow(g_hStatus,        SW_SHOW);
    } else {
        char portbuf[16];
        /* Connection strip: WAIS/IRC-style labels + WS_EX_CLIENTEDGE boxes,
         * an "Audio Codec" dropdown, and right-anchored Connect/Disconnect. */
        g_hReflLbl = CreateWindowA("STATIC", "Reflector:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 80, 22,
            content, NULL, hInst, NULL);
        g_hReflEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", CUSEEME_HOST,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 200, 26, content, (HMENU)(INT_PTR)IDC_CUSM_REFL_EDIT, hInst, NULL);
        g_hPortLbl = CreateWindowA("STATIC", "Port:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 44, 22,
            content, NULL, hInst, NULL);
        snprintf(portbuf, sizeof(portbuf), "%d", CUSEEME_PORT);
        g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", portbuf,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
            0, 0, 55, 26, content, (HMENU)(INT_PTR)IDC_CUSM_PORT_EDIT, hInst, NULL);
        g_hAudioLbl = CreateWindowA("STATIC", "Audio Codec:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 100, 22,
            content, NULL, hInst, NULL);
        g_hAudioCombo = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 120, 140, content, (HMENU)(INT_PTR)IDC_CUSM_AUDIO_COMBO, hInst, NULL);
        SendMessageA(g_hAudioCombo, CB_ADDSTRING, 0, (LPARAM)"Intel DVI");
        SendMessageA(g_hAudioCombo, CB_ADDSTRING, 0, (LPARAM)"Delta-Mod");
        SendMessageA(g_hAudioCombo, CB_SETCURSEL, 0, 0);   /* default Intel DVI */
        g_hConnectBtn = CreateWindowA("BUTTON", "Connect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 85, 30, content, (HMENU)(INT_PTR)IDC_CUSM_CONNECT_BTN, hInst, NULL);
        g_hDisconnectBtn = CreateWindowA("BUTTON", "Disconnect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 95, 30, content, (HMENU)(INT_PTR)IDC_CUSM_DISCONNECT_BTN, hInst, NULL);

        g_hRender = CreateWindowExA(0, CUSM_RENDER_CLASS, "",
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, content, NULL, hInst, NULL);
        g_hStatus = CreateWindowA("STATIC", "Disconnected",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 100, CUSM_STATUS_H, content,
            (HMENU)(INT_PTR)IDC_CUSM_STATUS, hInst, NULL);
        if (g_hFontUI) {
            SendMessageA(g_hReflLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hReflEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hPortLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hPortEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hAudioLbl,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hAudioCombo,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hConnectBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hDisconnectBtn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hStatus,        WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }
        g_controls_created = 1;
    }

    GetClientRect(content, &rc);
    cuseeme_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    /* Manual connection model (matches WAIS/IRC): nothing connects on
     * activation. Start pre-connect and wait for the Connect button. Only
     * force the disconnected UI state when not already in a live session, so
     * switching away and back preserves an active stream. */
    g_ui_state = (g_connected && g_running) ? g_ui_state : CUSM_UI_SPLASH;
    if (!g_connected) cusm_set_connected_state(0);
    SetTimer(g_hRender, CUSM_TIMER_STATUS, 1000, NULL);
    update_status();
    InvalidateRect(g_hRender, NULL, FALSE);
}

void cuseeme_module_deactivate(HWND content) {
    (void)content;
    if (g_hRender) {
        KillTimer(g_hRender, CUSM_TIMER_STATUS);
        KillTimer(g_hRender, CUSM_TIMER_CONNECT);
    }
    /* F7 carries live audio+video: stop the stream on a tab switch so it does
     * not keep playing audio behind another tab (a deliberate difference from
     * IRC's preserve-across-switch). Return to the disconnected state; the
     * strip's Connect restarts it. */
    worker_stop();
    g_ui_state = CUSM_UI_SPLASH;
    if (g_controls_created) cusm_set_connected_state(0);
    if (g_hStatus) SetWindowTextA(g_hStatus, "Disconnected");
    if (!g_controls_created) return;
    ShowWindow(g_hReflLbl,       SW_HIDE);
    ShowWindow(g_hReflEdit,      SW_HIDE);
    ShowWindow(g_hPortLbl,       SW_HIDE);
    ShowWindow(g_hPortEdit,      SW_HIDE);
    ShowWindow(g_hAudioLbl,      SW_HIDE);
    ShowWindow(g_hAudioCombo,    SW_HIDE);
    ShowWindow(g_hConnectBtn,    SW_HIDE);
    ShowWindow(g_hDisconnectBtn, SW_HIDE);
    ShowWindow(g_hRender,        SW_HIDE);
    ShowWindow(g_hStatus,        SW_HIDE);
}

void cuseeme_module_resize(HWND content, int w, int h) {
    const int margin = 16;
    const int row_h  = 26;
    const int btn_h  = 30;
    int strip  = CUSM_STRIP_H;
    int status = CUSM_STATUS_H;
    int render_h;
    (void)content;
    if (!g_controls_created) return;

    /* Connection strip (row 1): Reflector / Port / Audio Codec, with
     * ~34px gaps between groups (mirrors IRC); Connect + Disconnect anchored
     * right so they hold their place when the window widens. */
    MoveWindow(g_hReflLbl,   margin,       18, 80,  22,    TRUE);
    MoveWindow(g_hReflEdit,  margin + 84,  14, 195, row_h, TRUE);
    MoveWindow(g_hPortLbl,   margin + 310, 18, 44,  22,    TRUE);
    MoveWindow(g_hPortEdit,  margin + 358, 14, 55,  row_h, TRUE);
    MoveWindow(g_hAudioLbl,  margin + 443, 18, 100, 22,    TRUE);
    /* Combo height param is the dropped-open list height; the closed box
     * height is derived from the font. */
    MoveWindow(g_hAudioCombo, margin + 547, 14, 120, 160,  TRUE);
    {
        const int conn_w = 85, disc_w = 95, conn_gap = 8;
        int disc_x = w - margin - disc_w;
        int conn_x = disc_x - conn_gap - conn_w;
        MoveWindow(g_hConnectBtn,    conn_x, 12, conn_w, btn_h, TRUE);
        MoveWindow(g_hDisconnectBtn, disc_x, 12, disc_w, btn_h, TRUE);
    }

    /* Render fills between the strip and the status bar. */
    render_h = h - strip - status;
    if (render_h < 0) render_h = 0;
    if (g_hRender) MoveWindow(g_hRender, 0, strip, w, render_h, TRUE);
    if (g_hStatus) MoveWindow(g_hStatus, margin, h - status, w - 2 * margin, status, TRUE);
}

BOOL cuseeme_module_on_command(HWND content, WPARAM wParam, LPARAM lParam) {
    int id   = LOWORD(wParam);
    int code = HIWORD(wParam);
    (void)content; (void)lParam;
    if (code == BN_CLICKED) {
        if (id == IDC_CUSM_CONNECT_BTN)    { cusm_connect();    return TRUE; }
        if (id == IDC_CUSM_DISCONNECT_BTN) { cusm_disconnect(); return TRUE; }
    }
    return FALSE;
}

BOOL cuseeme_module_has_unsaved(void) {
    return FALSE;   /* receive-only stream: nothing to lose on switch */
}
