/* cuseeme_proto.c - CU-SeeMe wire format / OpenContinue handshake.
 * Clean-room from cusm_hdr-SE.96-02-29, ocp-SE.96-02-29, video-SE.96-02-29,
 * Audio-RBK.96-02-29 (ftp.icm.edu.pl/packages/cu-seeme/html/).
 * Big-endian wire order, confirmed by live capture from the reflector. */
#include "cuseeme_proto.h"
#include <string.h>

uint16_t cusm_rd16(const uint8_t *p, int off) {
    return (uint16_t)((p[off] << 8) | p[off + 1]);
}
uint32_t cusm_rd32(const uint8_t *p, int off) {
    return ((uint32_t)p[off] << 24) | ((uint32_t)p[off+1] << 16) |
           ((uint32_t)p[off+2] << 8) | (uint32_t)p[off+3];
}
void cusm_wr16(uint8_t *p, int off, uint16_t v) {
    p[off] = (uint8_t)(v >> 8); p[off+1] = (uint8_t)(v & 0xff);
}
void cusm_wr32(uint8_t *p, int off, uint32_t v) {
    p[off]   = (uint8_t)(v >> 24); p[off+1] = (uint8_t)(v >> 16);
    p[off+2] = (uint8_t)(v >> 8);  p[off+3] = (uint8_t)(v & 0xff);
}

int cusm_parse(const uint8_t *buf, int n, cusm_packet_t *out) {
    memset(out, 0, sizeof(*out));
    if (n < CUSM_HDR_LEN) return 0;                 /* cusm_hdr-SE: 26-byte hdr */
    out->dest_family = cusm_rd16(buf, CUSM_OFF_DESTFAM);
    out->src_addr    = cusm_rd32(buf, CUSM_OFF_SRCADDR);
    out->seq         = cusm_rd32(buf, CUSM_OFF_SEQ);
    out->message     = cusm_rd16(buf, CUSM_OFF_MSG);
    out->data_type   = cusm_rd16(buf, CUSM_OFF_DTYPE);
    out->plen        = cusm_rd16(buf, CUSM_OFF_PLEN);

    if (out->data_type == CUSM_DT_VIDEO_SMALL ||
        out->data_type == CUSM_DT_VIDEO_BIG) {
        /* video-SE / live wire: 2-byte length at 0x1A, data at 0x1C */
        if (n < CUSM_VID_OFF_DATA) return 0;
        int vlen = (int)cusm_rd16(buf, CUSM_VID_OFF_LEN);
        if (CUSM_VID_OFF_DATA + vlen > n) vlen = n - CUSM_VID_OFF_DATA;
        if (vlen < 0) vlen = 0;
        out->vid_data = buf + CUSM_VID_OFF_DATA;
        out->vid_len  = vlen;
    } else if (out->data_type == CUSM_DT_AUDIO) {
        /* Audio-RBK: VAT header (8 bytes) then nsid speaker IPs (4 each) */
        if (n < CUSM_VAT_OFF_NSID + CUSM_VAT_HDR_LEN) return 0;
        out->aud_nsid   = buf[CUSM_VAT_OFF_NSID] & CUSM_VAT_NSIDMASK;
        int flags       = buf[CUSM_VAT_OFF_FLAGS];
        out->aud_format = flags & CUSM_VAT_FMTMASK;
        out->aud_newts  = (flags & CUSM_VAT_NEWTS) ? 1 : 0;
        int data_off = CUSM_VAT_OFF_NSID + CUSM_VAT_HDR_LEN + 4 * out->aud_nsid;
        if (data_off > n) data_off = n;
        out->aud_data = buf + data_off;
        out->aud_len  = n - data_off;
    } else if (out->data_type == CUSM_DT_OPENCONTINUE) {
        /* ocp-SE: the fixed OC body ends at 0x38. The reflector relays each
         * sender's OpenContinue so a lurker can discover participants; expose
         * Send Mode so the caller can identify a video sender to subscribe to.
         * The sender's advertised address is src_addr (parsed above). */
        if (n >= CUSM_OC_OFF_CLIENTINFO) {
            out->oc_sendmode = buf[CUSM_OC_OFF_SENDMODE];
            out->oc_recvmode = buf[CUSM_OC_OFF_RECVMODE];
            out->oc_flags    = buf[CUSM_OC_OFF_FLAGS];
        }
    }
    out->valid = 1;
    return 1;
}

int cusm_build_opencontinue(uint8_t *buf, uint32_t seq, uint32_t my_ip,
                            uint16_t my_port, const char *user_name,
                            int want_audio, int do_close, uint32_t subscribe_ip) {
    memset(buf, 0, CUSM_OC_MAX_LEN);
    /* CU-SeeMe header (cusm_hdr-SE) */
    cusm_wr16(buf, CUSM_OFF_DESTFAM,  2);   /* OC packets may use family 2 */
    cusm_wr16(buf, CUSM_OFF_DESTPORT, 0);   /* Conference ID = 0 for OC */
    cusm_wr32(buf, CUSM_OFF_DESTADDR, 0);
    cusm_wr16(buf, CUSM_OFF_SRCFAM,   1);   /* originating at client */
    /* Mirror the real bound local port (caller binds fixed 7648 for instant
     * reconnect - see cuseeme_module.c). Fall back to 7648 if unresolved. */
    cusm_wr16(buf, CUSM_OFF_SRCPORT,  (uint16_t)(my_port ? my_port : 7648));
    cusm_wr32(buf, CUSM_OFF_SRCADDR,  my_ip);  /* unique client identifier */
    cusm_wr32(buf, CUSM_OFF_SEQ,      seq);
    cusm_wr16(buf, CUSM_OFF_MSG,      (uint16_t)(do_close ? CUSM_MSG_CLOSE
                                                          : CUSM_MSG_OPEN));
    cusm_wr16(buf, CUSM_OFF_DTYPE,    CUSM_DT_OPENCONTINUE);

    /* OpenContinue body (ocp-SE) */
    cusm_wr16(buf, CUSM_OC_OFF_CLIENTCOUNT, (uint16_t)(subscribe_ip ? 1 : 0));
    cusm_wr32(buf, CUSM_OC_OFF_OCSEQ, seq);

    /* User Name: pascal string (ocp-SE 20h): byte0 = len, then up to 19 chars */
    {
        int ul = 0;
        if (user_name) { while (user_name[ul] && ul < 19) ul++; }
        buf[CUSM_OC_OFF_USERNAME] = (uint8_t)ul;
        if (ul) memcpy(buf + CUSM_OC_OFF_USERNAME + 1, user_name, ul);
    }

    buf[CUSM_OC_OFF_SENDMODE] = CUSM_SEND_NONE;     /* receive-only */
    buf[CUSM_OC_OFF_RECVMODE] = CUSM_RECV_VIDEO;
    buf[CUSM_OC_OFF_FLAGS]    = (uint8_t)(CUSM_FLAG_WINDOWS |
                                 (want_audio ? CUSM_FLAG_RECV_AUDIO : 0));
    buf[CUSM_OC_OFF_VERSION]  = 0x01;

    /* Client Info array (ocp-SE). Empty for a bare open/keepalive; when
     * subscribe_ip is set, carry exactly one entry that requests that sender's
     * media. This is the receive-subscription the reflector waits for: a lurker
     * with clientCount=0 gets nothing relayed. The IP names the advertised
     * sender verbatim - loopback/private feeds are echoed, not filtered, because
     * on a reflector the media arrives on our existing socket, not by dialing
     * that address. */
    int plen = CUSM_OC_OFF_CLIENTINFO;              /* 0x38 = 56 bytes */
    if (subscribe_ip) {
        uint8_t *ci = buf + CUSM_OC_OFF_CLIENTINFO;
        cusm_wr32(ci, CUSM_CI_OFF_IPADDR, subscribe_ip);
        /* per-participant recv-audio bit mirrors the top-level recv-audio flag */
        ci[CUSM_CI_OFF_FLAGS]     = (uint8_t)(want_audio ? CUSM_FLAG_RECV_AUDIO : 0);
        ci[CUSM_CI_OFF_AUXPRUNE]  = 0;
        ci[CUSM_CI_OFF_IWILLRECV] = 1;   /* subscribe: send me this sender's video */
        ci[CUSM_CI_OFF_IWILLSEND] = 0;   /* receive-only: we never send video */
        /* Packets Expected / Received left 0 (rate accounting, not required). */
        plen += CUSM_CI_ENTRY_LEN;
    }
    cusm_wr16(buf, CUSM_OFF_PLEN, (uint16_t)plen);
    return plen;
}
