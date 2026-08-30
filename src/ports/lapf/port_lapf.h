/*
 * port_lapf.h - LAPF (Q.922) Link Access Procedure for Frame Relay
 * VFRS - Virtual Frame Relay Switch
 */

#ifndef PORT_LAPF_H
#define PORT_LAPF_H

#include "vfr.h"

/* LAPF States */
#define LAPF_STATE_TEI_ASSIGNED           4  /* Link disconnected */
#define LAPF_STATE_AWAITING_ESTABLISHMENT 5  /* Sent SABME, waiting for UA */
#define LAPF_STATE_AWAITING_RELEASE       6  /* Sent DISC, waiting for UA */
#define LAPF_STATE_ESTABLISHED            7  /* Link up, active transfer */
#define LAPF_STATE_TIMER_RECOVERY         8  /* T200 expired, waiting for poll response */

/* MDL-ERROR indication codes per Q.922 Table V-1 / Appendix V */
#define LAPF_MDL_ERROR_A  'A'  /* Unsolicited supervisory response F=1 (state 7) */
#define LAPF_MDL_ERROR_B  'B'  /* Unsolicited DM response F=1 (states 7,8) */
#define LAPF_MDL_ERROR_C  'C'  /* Unsolicited UA response F=1 (states 4,7,8) */
#define LAPF_MDL_ERROR_D  'D'  /* Unsolicited UA response F=0 (states 4,5,6,7,8) */
#define LAPF_MDL_ERROR_E  'E'  /* Unsolicited DM response F=0 (states 7,8) */
#define LAPF_MDL_ERROR_F  'F'  /* Peer-initiated re-establishment SABME (states 7,8) */
#define LAPF_MDL_ERROR_G  'G'  /* N200 exhaustion SABME (state 5) */
#define LAPF_MDL_ERROR_H  'H'  /* N200 exhaustion DISC (state 6) */
#define LAPF_MDL_ERROR_I  'I'  /* N200 exhaustion status enquiry (state 8) */
#define LAPF_MDL_ERROR_J  'J'  /* N(R) error (states 7,8) */
#define LAPF_MDL_ERROR_K  'K'  /* Receipt of FRMR response (states 7,8) */
#define LAPF_MDL_ERROR_L  'L'  /* Receipt of non-implemented frame (states 4-8) */
#define LAPF_MDL_ERROR_N  'N'  /* Receipt of frame with wrong size (states 4-8) */
#define LAPF_MDL_ERROR_O  'O'  /* N201 error (states 4-8) */

/* LAPF Control Field Types (Modulo 128) */
#define LAPF_CTRL_I        0x00  /* Bit 1 is 0 */
#define LAPF_CTRL_S_RR     0x01  /* RR S-frame type (00) */
#define LAPF_CTRL_S_RNR    0x05  /* RNR S-frame type (01) */
#define LAPF_CTRL_S_REJ    0x09  /* REJ S-frame type (10) */
#define LAPF_CTRL_U_SABME  0x6F  /* SABME U-frame (011P1111) */
#define LAPF_CTRL_U_DM     0x0F  /* DM U-frame (000F1111) */
#define LAPF_CTRL_U_UI     0x03  /* UI U-frame (000P0011) */
#define LAPF_CTRL_U_DISC   0x43  /* DISC U-frame (010P0011) */
#define LAPF_CTRL_U_UA     0x63  /* UA U-frame (011F0011) */
#define LAPF_CTRL_U_FRMR   0x87  /* FRMR U-frame (100F0111) */
#define LAPF_CTRL_U_XID    0xAF  /* XID U-frame (101P1111) */

/* FRMR error condition bits per Q.921 §3.10.1 / Figure 6/Q.921 (Octet 9 of info field) */
#define LAPF_FRMR_W        0x01  /* Bit 1 (W): Undefined or non-implemented control field */
#define LAPF_FRMR_X        0x02  /* Bit 2 (X): I-field not permitted / incorrect S/U length (W also set) */
#define LAPF_FRMR_Y        0x04  /* Bit 3 (Y): I-field exceeded maximum established length (N201) */
#define LAPF_FRMR_Z        0x08  /* Bit 4 (Z): Invalid N(R) sequence number */

/* Deferred callback entry for queue-then-flush pattern (F-10).
 * During lapf_handle_frame, callbacks are queued here while holding the mutex.
 * After the mutex is released, they are invoked outside the lock. */
#define LAPF_MAX_DEFERRED  8

typedef enum {
    LAPF_CB_NONE = 0,
    LAPF_CB_L3_DATA,       /* Deliver L3 payload via lapf_l3_recv_cb */
    LAPF_CB_EVENT,         /* Deliver event via on_event callback */
} lapf_cb_type_t;

typedef struct {
    lapf_cb_type_t  type;
    const u8       *data;      /* Pointer to payload (only valid during frame processing) */
    size_t          len;       /* Payload length */
    int             event;     /* LAPF_EVENT_* for CB_EVENT type */
    u32             dlci;      /* DLCI for the event */
} lapf_deferred_cb_t;

/* Derive default window size (k) from access rate per Q.922 §5.9.4.
 * Access rate 0 means "unknown/unconfigured" → use k=32 (conservative default). */
static inline u8 lapf_default_k_for_rate(u32 access_rate_bps) {
    if (access_rate_bps == 0)       return 32;   /* Unknown → conservative */
    if (access_rate_bps <= 16000)   return 3;    /* 16 kbit/s */
    if (access_rate_bps <= 64000)   return 7;    /* 64 kbit/s */
    if (access_rate_bps <= 384000)  return 32;   /* 384 kbit/s */
    return 40;                                   /* 1.536/1.920 Mbit/s and above */
}

typedef struct {
    u8 data[2048];
    size_t len;
} lapf_tx_frame_t;

typedef struct {
    u32         dlci;
    int         state;
    u8          k;                  /* Window size (default: 7 per X.36 §10.3) */
    u8          n200;               /* Max retransmissions (default: 3) */
    u16         n201;               /* Max I-frame size (default: 1598) */
    u32         t200;               /* Retransmission timer (ms, default: 1000) */
    u32         t203;               /* Idle timer (ms, default: 30000) */

    /* Modulo 128 variables */
    u8          v_s;                /* Send state variable (0-127) */
    u8          v_r;                /* Receive state variable (0-127) */
    u8          v_a;                /* Acknowledge state variable (0-127) */
    u8          peer_receiver_busy;
    u8          own_receiver_busy;
    u8          retransmission_count;
    u8          acknowledgement_pending;
    u8          rej_exception;      /* 1 if in REJ exception condition */

    /* Dynamic Windowing (ITU-T Q.922 Appendix I) */
    u8          v_k;                /* Current working window size (1 <= v_k <= k) */
    u8          n_w;                /* Dynamic window step size (default: 5) */
    u16         ia_ct;              /* Information acknowledge counter */

    /* Connection Management Parameter Negotiation (ITU-T Q.922 Appendix III) */
    vfr_timer_t tm20_timer;         /* TM20 parameter negotiation timer (2.5s) */
    u8          nm20_retries;       /* NM20 retry count (max 3) */

    /* L3 event callback (F-01/F-02): invoked for DL-ESTABLISH / DL-RELEASE indications.
     * Set during initialization or by the SVC module. Called OUTSIDE the port mutex
     * via the deferred callback flush mechanism. */
    void          (*on_event)(vfr_port_t *port, u32 dlci, int event_type);

    /* Timers */
    vfr_timer_t t200_timer;
    vfr_timer_t t203_timer;

    /* Transmitted but unacknowledged outstanding frames queue */
    lapf_tx_frame_t tx_window[128];

    /* Backlog queue for frames waiting to be sent when window is full */
    lapf_tx_frame_t tx_backlog[64];
    int             backlog_head;
    int             backlog_tail;

    /* Statistics */
    u64         sabme_tx;
    u64         sabme_rx;
    u64         ua_tx;
    u64         ua_rx;
    u64         disc_tx;
    u64         disc_rx;
    u64         dm_tx;
    u64         dm_rx;
    u64         frmr_tx;
    u64         frmr_rx;
    u64         rr_tx;
    u64         rr_rx;
    u64         rnr_tx;
    u64         rnr_rx;
    u64         rej_tx;
    u64         rej_rx;
    u64         i_tx;
    u64         i_rx;
    u64         t200_expires;
    u64         t203_expires;
    u64         xid_tx;
    u64         xid_rx;
    u64         ui_tx;
    u64         ui_rx;
} vfr_lapf_state_t;

/* Internal centralized frame builder (DL-CORE) */
int port_dl_build_frame(vfr_port_t *port, u32 dlci, u8 ctrl_byte, const u8 *payload, size_t len,
                        int cr, int fecn, int becn, int de, u8 *out_buf, size_t max_len, size_t *out_len);

/* Initialize LAPF on a port for a specific DLCI */
int lapf_port_init(vfr_port_t *port, u32 dlci, u8 k, u8 n200, u16 n201, u32 t200, u32 t203);

/* Process LAPF timer ticks */
void lapf_poll_timer(vfr_port_t *port);

/* Handle incoming LAPF frame (excl. Q.922 address) */
int lapf_handle_frame(vfr_port_t *port, const u8 *frame, size_t len);

/* Send Layer 3 payload over LAPF (encapsulates into I-frame) */
int lapf_send_l3(vfr_port_t *port, u32 dlci, const u8 *data, size_t len);

/* Establish LAPF link actively for a specific DLCI */
int lapf_establish_link(vfr_port_t *port, u32 dlci);

/* Release LAPF link actively for a specific DLCI */
int lapf_release_link(vfr_port_t *port, u32 dlci);

/* Set L3 event callback for DL-ESTABLISH/DL-RELEASE indications */
void lapf_set_event_cb(vfr_port_t *port, u32 dlci,
                       void (*on_event)(vfr_port_t *port, u32 dlci, int event_type));

/* Callback invoked when Layer 3 payload is received */
void lapf_l3_recv_cb(vfr_port_t *port, const u8 *data, size_t len);

/* Free LAPF context */
void lapf_free(vfr_port_t *port);

#endif /* PORT_LAPF_H */
