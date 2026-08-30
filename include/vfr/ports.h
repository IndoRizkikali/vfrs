/*
 * ports.h - VFRS Port Architecture & Driver Interfaces
 * Virtual Frame Relay Switch
 */

#ifndef VFR_PORTS_H
#define VFR_PORTS_H

#include "platform.h"
#include "types.h"
#include "pcap.h"
#include "pvc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct vfrs_ctx_s vfrs_ctx_t;
typedef struct vfr_port_s vfr_port_t;

/* ============================================================
 * Port Types & Transport Constants
 * ============================================================ */

#define PORT_TYPE_UNI  0
#define PORT_TYPE_NNI  1

#define PORT_TRANS_UDP          0
#define PORT_TRANS_TCP          1
#define PORT_TRANS_TCP_CLI      2
#define PORT_TRANS_TCP_SER      3
#define PORT_TRANS_SERIAL       4
#define PORT_TRANS_PIPE         5
#define PORT_TRANS_UDP_CLI      6
#define PORT_TRANS_UDP_SER      7
#define PORT_TRANS_L2TPV3_FR    8
#define PORT_TRANS_L2TPV3_HDLC  9

/* Port status */
#define PORT_STATUS_DOWN     0
#define PORT_STATUS_UP        1
#define PORT_STATUS_CONN     2   /* Connected (for connection-oriented) */

typedef struct vfr_port_stats_s {
    u64 tx_frames;
    u64 rx_frames;
    u64 tx_bytes;
    u64 rx_bytes;
    u64 fcs_errors;
    u64 dropped;
    u64 switched;
} vfr_port_stats_t;

/* TCP port private data */
typedef struct tcp_priv_s tcp_priv_t;
struct tcp_priv_s {
    char        lhost[64];
    u16         lport;
    char        rhost[64];
    u16         rport;
    int         mode;
    int         connected;
    int         reconnect;
    u32         reconnect_delay;
    u32         max_reconnect_delay;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    thread_t    accept_thread;
    thread_t    reader_thread;
    socket_fd_t listen_fd;
    volatile int running;
};

/* Port interface function table */
typedef struct port_ops_s port_ops_t;
typedef int (*port_send_fn)(vfr_port_t *port, const u8 *frame, size_t len);
typedef int (*port_recv_fn)(vfr_port_t *port, u8 *buf, size_t max_len);
typedef int (*port_poll_fn)(vfr_port_t *port, u32 timeout_ms);
typedef void (*port_free_fn)(vfr_port_t *port);

struct port_ops_s {
    port_send_fn    send;
    port_recv_fn    recv;
    port_poll_fn    poll;
    port_free_fn    free;
};

/* Dynamic LAPF context node */
#define PORT_LAPF_HASH_SIZE 64

struct vfr_lapf_node_s {
    u32                     dlci;
    void                   *state;
    struct vfr_lapf_node_s *next;
};
typedef struct vfr_lapf_node_s vfr_lapf_node_t;

/* SAP callback types */
typedef void (*port_dl_ui_cb_fn)(struct vfr_port_s *port, u32 dlci, const u8 *data, size_t len);
typedef void (*port_dl_xid_cb_fn)(struct vfr_port_s *port, u32 dlci, const u8 *data, size_t len);
typedef void (*port_dl_l3_cb_fn)(struct vfr_port_s *port, u32 dlci, const u8 *data, size_t len);

struct port_dl_sap_s {
    u32                     dlci;
    port_dl_ui_cb_fn        ui_cb;
    port_dl_xid_cb_fn       xid_cb;
    port_dl_l3_cb_fn        l3_cb;
    struct port_dl_sap_s   *next;
};
typedef struct port_dl_sap_s port_dl_sap_t;

/* Base port structure */
struct vfr_port_s {
    char            name[VFR_MAX_NAME_LEN];
    int             type;           /* UNI or NNI */
    int             transport;      /* UDP, TCP, etc. */
    u8              dlcibit;        /* DLCI length (10 or 23 bits) */
    socket_fd_t     fd;             /* Socket or file descriptor */
    int             status;
    mutex_t         mutex;
    vfr_timer_t     poll_timer;     /* For LMI polling */

    /* Statistics */
    vfr_port_stats_t stats;

    /* Capture */
    pcap_writer_t   *capture;

    /* LMI state pointer */
    void            *lmi_ctx;

    /* Congestion management state pointer */
    void            *cgst_ctx;

    /* Dynamic LAPF Context Hash Table */
    vfr_lapf_node_t *lapf_hash[PORT_LAPF_HASH_SIZE];
    int             lapf_count;

    /* SAP Handlers */
    port_dl_sap_t   *sap_list;

    /* SVC context pointer */
    void            *svc_ctx;

    /* Port-specific ops */
    const port_ops_t *ops;

    /* Port-specific data */
    void            *priv;

    /* FRF.12 Fragmentation */
    size_t          fragment_size;    /* 0 = disabled, >0 = max fragment payload */
    u16             frag_seq;         /* 12-bit sequence counter */
    void            *reasm_ctx;

    /* Port-local routing table (Agent lookup caches pointing to Global DLCI Table) */
    struct vfr_dlci_entry_s *dlci_lut[1024];   /* Direct array lookup for 10-bit DLCI */
    struct vfr_dlci_entry_s **dlci_array;      /* Dynamic sorted array for 23-bit DLCI */
    int             dlci_count;
    int             dlci_capacity;
};

static inline void *port_get_lapf_ctx(struct vfr_port_s *port, u32 dlci) {
    if (!port) return NULL;
    u32 bucket = dlci % PORT_LAPF_HASH_SIZE;
    vfr_lapf_node_t *node = port->lapf_hash[bucket];
    while (node) {
        if (node->dlci == dlci) {
            return node->state;
        }
        node = node->next;
    }
    return NULL;
}

/* UDP port constructors */
VFR_API vfr_port_t *port_udp_create(const char *name, int type,
                                    const char *lhost, u16 lport,
                                    const char *rhost, u16 rport);
VFR_API vfr_port_t *port_udp_cli_create(const char *name, int type,
                                        const char *rhost, u16 rport);
VFR_API vfr_port_t *port_udp_ser_create(const char *name, int type,
                                        const char *lhost, u16 lport);
VFR_API vfr_port_t *port_l2tpv3_create(const char *name, int type, int transport,
                                       const char *lhost, const char *rhost, u32 vcid);

/* TCP port constructors */
VFR_API vfr_port_t *port_tcp_create(const char *name, int type,
                                    const char *lhost, u16 lport,
                                    const char *rhost, u16 rport);
VFR_API vfr_port_t *port_tcp_cli_create(const char *name, int type,
                                        const char *rhost, u16 rport);
VFR_API vfr_port_t *port_tcp_ser_create(const char *name, int type,
                                        const char *lhost, u16 lport);
VFR_API int tcp_connect(vfr_port_t *port);

/* Serial port constructor */
VFR_API vfr_port_t *port_serial_create(const char *name, int type,
                                       const char *device, u32 baudrate);

/* Named pipe constructor */
VFR_API vfr_port_t *port_pipe_create(const char *name, int type,
                                     const char *pipename, int server);

/* Base port operations */
VFR_API vfr_port_t *port_create(const char *name, int type, int transport);
VFR_API void port_free(vfr_port_t *port);
VFR_API void port_stats_init(vfr_port_t *port);
VFR_API int port_set_status(vfr_port_t *port, int status);
VFR_API int port_parse_name(const char *name, int *type, int *group, int *index);

/* Default socket operations */
VFR_API int port_default_send(vfr_port_t *port, const u8 *frame, size_t len);
VFR_API int port_default_recv(vfr_port_t *port, u8 *buf, size_t max_len);
VFR_API int port_default_poll(vfr_port_t *port, u32 timeout_ms);

/* Port-local unified routing table operations */
VFR_API vfr_dlci_entry_t *port_lookup_dlci(vfr_port_t *port, u32 dlci);
VFR_API int port_add_dlci_entry(vfr_port_t *port, vfr_dlci_entry_t *entry);
VFR_API int port_del_dlci_entry(vfr_port_t *port, u32 dlci);

/* Context port lookup / management */
VFR_API vfr_port_t *vfrs_find_port(vfrs_ctx_t *ctx, const char *name);
VFR_API vfr_port_t *vfrs_find_port_unlocked(vfrs_ctx_t *ctx, const char *name);
VFR_API int vfrs_add_port(vfrs_ctx_t *ctx, vfr_port_t *port);
VFR_API int vfrs_remove_port(vfrs_ctx_t *ctx, vfr_port_t *port);

#ifdef __cplusplus
}
#endif

#endif /* VFR_PORTS_H */
