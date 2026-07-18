/* cuseeme_proto.h - CU-SeeMe wire format, OpenContinue handshake, framing.
 *
 * Clean-room from the public Dorcey-team specs at
 *   ftp.icm.edu.pl/packages/cu-seeme/html/
 * Specifically:
 *   cusm_hdr-SE.96-02-29   - 26-byte CU-SeeMe header (every packet)
 *   ocp-SE.96-02-29        - OpenContinue packet (handshake / keepalive / close)
 *   video-SE.96-02-29      - video packet framing
 *   Audio-RBK.96-02-29     - VAT audio header, format ids (DELTAMOD = 26)
 *   OCExtraHeader-RBK / Pruning.html / AuxData.html - inbound OC extensions
 *
 * Wire byte order is BIG-ENDIAN (network order). The spec header diagrams are
 * drawn MSB-first and CU-SeeMe is Mac-origin; confirmed against a live capture
 * from the reflector (DataType / PacketLength decode cleanly big-endian).
 *
 * Receive-only client: never advertises send-video (Send Mode = 0).
 */
#ifndef CUSEEME_PROTO_H
#define CUSEEME_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* ---- CU-SeeMe header field offsets (cusm_hdr-SE) ---- */
#define CUSM_HDR_LEN          0x1A   /* 26 bytes */
#define CUSM_OFF_DESTFAM      0x00
#define CUSM_OFF_DESTPORT     0x02
#define CUSM_OFF_DESTADDR     0x04
#define CUSM_OFF_SRCFAM       0x08
#define CUSM_OFF_SRCPORT      0x0A
#define CUSM_OFF_SRCADDR      0x0C
#define CUSM_OFF_SEQ          0x10
#define CUSM_OFF_MSG          0x14
#define CUSM_OFF_DTYPE        0x16
#define CUSM_OFF_PLEN         0x18

/* ---- Message values (cusm_hdr-SE 14h) ---- */
#define CUSM_MSG_OPEN         1      /* open / keepalive (OpenContinue) */
#define CUSM_MSG_CLOSE        6      /* close connection (OpenContinue) */
#define CUSM_MSG_VID_MORE     0      /* more video packets to come this frame */
#define CUSM_MSG_VID_END      20     /* frame end - update image on screen */

/* ---- Data Type values (cusm_hdr-SE 16h) ---- */
#define CUSM_DT_VIDEO_SMALL   1      /* 160x120 */
#define CUSM_DT_VIDEO_BIG     2      /* 320x240 */
#define CUSM_DT_AUDIO         3
#define CUSM_DT_ACK           100    /* connectivity ack, nothing else to send */
#define CUSM_DT_OPENCONTINUE  101
#define CUSM_DT_KICK_TEXT     104    /* display text then disconnect */
#define CUSM_DT_WELCOME_TEXT  105    /* display reflector welcome message */
#define CUSM_DT_REFINTEROP    106
#define CUSM_DT_AUXSTREAM     107
#define CUSM_DT_RATECTL_A     110
#define CUSM_DT_RATECTL_B     111
#define CUSM_DT_AUXCTL        256
#define CUSM_DT_AUXDATA       257

/* ---- OpenContinue body offsets (ocp-SE), relative to packet start ---- */
#define CUSM_OC_OFF_CLIENTCOUNT  0x1A
#define CUSM_OC_OFF_OCSEQ        0x1C
#define CUSM_OC_OFF_USERNAME     0x20   /* pascal string, 20 bytes */
#define CUSM_OC_OFF_SENDMODE     0x34
#define CUSM_OC_OFF_RECVMODE     0x35
#define CUSM_OC_OFF_FLAGS        0x36
#define CUSM_OC_OFF_VERSION      0x37
#define CUSM_OC_OFF_CLIENTINFO   0x38

/* ---- ClientInfo array entry (ocp-SE, one per known participant) ----
 * 12-byte fixed record. This is how a client tells the reflector which
 * participants it wants to receive from (subscribe) or prune. To subscribe to
 * a sender we emit one entry naming that sender by IP with "I Will Recv" = 1.
 * Verbatim per ocp-SE.96-02-29: IP Address (4), Flags (1), Aux Data Prune (1),
 * I Will Recv (1), I Will Send (1), Packets Expected (2), Packets Received (2). */
#define CUSM_CI_ENTRY_LEN        12
#define CUSM_CI_OFF_IPADDR        0   /* 4: the participant this entry refers to */
#define CUSM_CI_OFF_FLAGS         4   /* 1: per-participant boolean states */
#define CUSM_CI_OFF_AUXPRUNE      5   /* 1: which Aux Data types are desired */
#define CUSM_CI_OFF_IWILLRECV     6   /* 1: request this participant's video */
#define CUSM_CI_OFF_IWILLSEND     7   /* 1: I will send video to this participant */
#define CUSM_CI_OFF_PKTSEXP       8   /* 2: packets expected since last OC */
#define CUSM_CI_OFF_PKTSRECV     10   /* 2: packets received since last OC */

/* Largest OpenContinue we build: fixed body + one subscribing ClientInfo. */
#define CUSM_OC_MAX_LEN          (CUSM_OC_OFF_CLIENTINFO + CUSM_CI_ENTRY_LEN)  /* 0x44 */

/* OpenContinue Send Mode (ocp-SE 34h) */
#define CUSM_SEND_NONE        0x00   /* will NOT send video (receive-only) */
#define CUSM_SEND_SMALL       0x01
#define CUSM_SEND_BIG         0x02
/* OpenContinue Receive Mode (ocp-SE 35h) */
#define CUSM_RECV_NONE        0
#define CUSM_RECV_VIDEO       1
/* OpenContinue Flags (ocp-SE 36h) */
#define CUSM_FLAG_RECV_AUDIO  0x01
#define CUSM_FLAG_SEND_AUDIO  0x02
#define CUSM_FLAG_LURK_AUDIO  0x04
#define CUSM_FLAG_WINDOWS     0x08
#define CUSM_FLAG_PRIV_AUDIO  0x10
#define CUSM_FLAG_SEND_VER    0x20

/* ---- VAT audio sub-header (Audio-RBK), relative to packet start ---- */
#define CUSM_VAT_OFF_NSID     0x1A   /* 1 byte: number of speakers (low 6 bits) */
#define CUSM_VAT_OFF_FLAGS    0x1B   /* 1 byte: 0x80 NEWTS, low 5 bits = format */
#define CUSM_VAT_OFF_CONFID   0x1C   /* 2 bytes */
#define CUSM_VAT_OFF_TS       0x1E   /* 4 bytes (samples) */
#define CUSM_VAT_HDR_LEN      0x08   /* nsid+flags+confid+ts */
#define CUSM_VAT_NEWTS        0x80
#define CUSM_VAT_FMTMASK      0x1F
#define CUSM_VAT_NSIDMASK     0x3F
#define CUSM_AUDF_DELTAMOD    26     /* 16 kb/s 2-bit delta mod (legacy, dormant) */
#define CUSM_AUDF_MULAW       0      /* G.711 mu-law (PCMU), Audio-RBK nominal id (dormant) */
#define CUSM_AUDF_IDVI        30     /* Intel DVI / IMA ADPCM, 8 kHz mono ~32 kb/s */
/* Live-wire note (2026-06-30): the reworked encoder emits VAT format 30 = IDVI
 * (Intel DVI / IMA ADPCM), NOT mu-law/format-0. Blocks are 242-byte packets:
 * 38 header bytes then a 204-byte IDVI block at 0x26 (4-byte seed header +
 * 200 nibble bytes = 400 samples, timestamp +400, seq +1). The Cornell-exact
 * decode conventions (per-packet reseed, big-endian seed predictor,
 * HIGH-nibble-first, delta = (step*mag)>>2 + (step>>3)) are implemented in
 * cuseeme_idvi.[ch]. DELTAMOD and mu-law remain dormant (their format ids do
 * not appear on the current wire) but are kept for completeness. */

/* Video framing, confirmed against the live wire:
 * 26-byte header, then a 2-byte big-endian "Length of Video Data" at 0x1A,
 * then the compressed square data at 0x1C. (The video-SE diagram nominally
 * shows these at 0x26/0x28; the live encoder packs them tight at 0x1A/0x1C.) */
#define CUSM_VID_OFF_LEN      0x1A
#define CUSM_VID_OFF_DATA     0x1C

/* big-endian readers */
uint16_t cusm_rd16(const uint8_t *p, int off);
uint32_t cusm_rd32(const uint8_t *p, int off);
void     cusm_wr16(uint8_t *p, int off, uint16_t v);
void     cusm_wr32(uint8_t *p, int off, uint32_t v);

/* Parsed view of a received packet. */
typedef struct {
    int      valid;          /* header well-formed and length-consistent */
    uint16_t dest_family;
    uint16_t data_type;      /* CUSM_DT_* */
    uint16_t message;        /* CUSM_MSG_* */
    uint32_t seq;
    uint32_t src_addr;
    uint16_t plen;           /* header-declared packet length */
    /* OpenContinue (data_type 101): the reflector relays each sender's OC so a
     * lurker can discover participants. oc_sendmode != NONE marks a video
     * sender to subscribe to; its advertised address is src_addr (above), which
     * may legitimately be loopback/private and must NOT be rejected. */
    uint8_t  oc_sendmode;    /* ocp-SE 0x34: originator's Send Mode */
    uint8_t  oc_recvmode;    /* ocp-SE 0x35 */
    uint8_t  oc_flags;       /* ocp-SE 0x36 */
    /* video: */
    const uint8_t *vid_data; /* points into packet at 0x1C */
    int      vid_len;        /* bytes of compressed video data */
    /* audio: */
    int      aud_nsid;
    int      aud_format;     /* low 5 bits of VAT flags */
    int      aud_newts;
    const uint8_t *aud_data; /* points past VAT header + speaker IPs */
    int      aud_len;        /* bytes of compressed audio data */
} cusm_packet_t;

/* Classify and bounds-check a received UDP payload. Returns 1 if valid. */
int cusm_parse(const uint8_t *buf, int n, cusm_packet_t *out);

/* Build an OpenContinue packet (receive-only) into buf (>= CUSM_OC_MAX_LEN).
 * my_port is the client's real local UDP port, written into the header SrcPort.
 *   The reconnect-critical part is the SOCKET bind (the caller binds a fixed local
 *   port 7648 so the reflector's per-IP media slot, keyed on the real (IP,port)
 *   endpoint, is reused on reconnect); this field just mirrors that bound port on
 *   the wire. Falls back to 7648 if 0.
 * do_close non-zero => Message 6 (close), else Message 1 (open/keepalive).
 * want_audio non-zero => set the recv-audio flag.
 * subscribe_ip non-zero => emit one ClientInfo entry (clientCount=1) that
 *   subscribes to that sender (I Will Recv = 1). This is the receive-request the
 *   reflector needs; without it clientCount is 0 and no video is relayed.
 *   subscribe_ip is the sender's advertised address exactly as received (may be
 *   loopback/private on a reflector feed) - it is echoed verbatim, never filtered.
 * Returns packet length in bytes. */
int cusm_build_opencontinue(uint8_t *buf, uint32_t seq, uint32_t my_ip,
                            uint16_t my_port, const char *user_name,
                            int want_audio, int do_close, uint32_t subscribe_ip);

#endif /* CUSEEME_PROTO_H */
