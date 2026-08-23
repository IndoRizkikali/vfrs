/*
 * port_udp.c - UDP Port Interface
 * VFRS - Virtual Frame Relay Switch
 * Compatible with dynamips UDP format
 */

#include "vfr.h"

#include <string.h>

/* UDP port private data */
typedef struct {
    char        lhost[MAX_ADDR_STR];
    u16         lport;
    char        rhost[MAX_ADDR_STR];
    u16         rport;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    int         reuse_addr;
} udp_priv_t;

/* UDP send function */
static int port_udp_send(vfr_port_t *port, const u8 *frame, size_t len)
{
    int ret;
    udp_priv_t *priv = (udp_priv_t *)port->priv;

    if (!port || port->fd == INVALID_SOCKET) return -1;

    ret = sendto(port->fd, (const char *)frame, len, 0,
                 (struct sockaddr *)&priv->remote_addr,
                 sizeof(priv->remote_addr));

    if (ret < 0) {
        LOG_DEBUG("UDP send failed on %s: %d", port->name, WSAGetLastError());
        return -1;
    }

    /* Log to PCAP if capture is enabled */
    if (port->capture) {
        pcap_writer_write(port->capture, frame, len);
    }
    if (g_vfrs && g_vfrs->global_capture) {
        pcap_writer_write(g_vfrs->global_capture, frame, len);
    }

    port->stats.tx_frames++;
    port->stats.tx_bytes += ret;
    return 0;
}

/* UDP receive function */
static int port_udp_recv(vfr_port_t *port, u8 *buf, size_t max_len)
{
    int ret;
    struct sockaddr_in from;
    int from_len = sizeof(from);
    udp_priv_t *priv = (udp_priv_t *)port->priv;

    if (!port || port->fd == INVALID_SOCKET) return -1;

    ret = recvfrom(port->fd, (char *)buf, max_len, 0,
                   (struct sockaddr *)&from, &from_len);

    if (ret < 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            LOG_DEBUG("UDP recv failed on %s: %d", port->name, err);
        }
        return -1;
    }

    port->stats.rx_frames++;
    port->stats.rx_bytes += ret;

    /* For server mode without preset remote address, dynamically learn first client */
    if (port->transport == PORT_TRANS_UDP_SER && priv->remote_addr.sin_port == 0) {
        priv->remote_addr = from;
        strncpy(priv->rhost, inet_ntoa(from.sin_addr), sizeof(priv->rhost) - 1);
        priv->rport = ntohs(from.sin_port);
        LOG_INFO("UDP server port %s dynamically associated with client %s:%u",
                 port->name, priv->rhost, priv->rport);
    } else if (from.sin_addr.s_addr != priv->remote_addr.sin_addr.s_addr ||
               from.sin_port != priv->remote_addr.sin_port) {
        /* Verify source address matches configured remote — drop if spoofed */
        LOG_DEBUG("UDP packet from unexpected source on %s (%s:%u, expected %s:%u)",
                  port->name, inet_ntoa(from.sin_addr), ntohs(from.sin_port),
                  priv->rhost, priv->rport);
        port->stats.dropped++;
        return -1;
    }

    return ret;
}

/* UDP poll function */
static int port_udp_poll(vfr_port_t *port, u32 timeout_ms)
{
    fd_set read_fds;
    struct timeval tv;
    int ret;

    if (!port || port->fd == INVALID_SOCKET) return 0;

    FD_ZERO(&read_fds);
    FD_SET((unsigned)port->fd, &read_fds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(0, &read_fds, NULL, NULL, &tv);
    return ret;
}

static void port_udp_free(vfr_port_t *port)
{
    if (!port || !port->priv) return;
    free(port->priv);
    port->priv = NULL;
}

/* UDP port operations */
static const port_ops_t udp_ops = {
    .send  = port_udp_send,
    .recv  = port_udp_recv,
    .poll  = port_udp_poll,
    .free  = port_udp_free
};

vfr_port_t *port_udp_create(const char *name, int type,
                            const char *lhost, u16 lport,
                            const char *rhost, u16 rport)
{
    vfr_port_t *port;
    udp_priv_t *priv;
    int ret;

    if (!name || !rhost) return NULL;

    /* Create base port */
    port = port_create(name, type, PORT_TRANS_UDP);
    if (!port) return NULL;

    /* Allocate private data */
    priv = calloc(1, sizeof(udp_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;

    /* Store addresses */
    strncpy(priv->lhost, lhost ? lhost : "0.0.0.0", sizeof(priv->lhost) - 1);
    priv->lport = lport;
    strncpy(priv->rhost, rhost, sizeof(priv->rhost) - 1);
    priv->rport = rport;

    /* Create UDP socket */
    port->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (port->fd == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP socket for %s: %d",
                  name, WSAGetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set socket options */
    priv->reuse_addr = 1;
    setsockopt(port->fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&priv->reuse_addr, sizeof(priv->reuse_addr));

#ifdef _WIN32
    /* Suppress Windows ICMP port unreachable reset (WSAECONNRESET) */
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(port->fd, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

    /* Bind local address */
    memset(&priv->local_addr, 0, sizeof(priv->local_addr));
    priv->local_addr.sin_family = AF_INET;
    priv->local_addr.sin_port = htons(lport);

    if (lhost && strlen(lhost) > 0) {
        priv->local_addr.sin_addr.s_addr = inet_addr(lhost);
    } else {
        priv->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    ret = bind(port->fd, (struct sockaddr *)&priv->local_addr,
               sizeof(priv->local_addr));
    if (ret < 0) {
        LOG_ERROR("Failed to bind UDP socket %s:%u: %d",
                  lhost ? lhost : "0.0.0.0", lport, WSAGetLastError());
        closesocket(port->fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set remote address */
    memset(&priv->remote_addr, 0, sizeof(priv->remote_addr));
    priv->remote_addr.sin_family = AF_INET;
    priv->remote_addr.sin_port = htons(rport);
    priv->remote_addr.sin_addr.s_addr = inet_addr(rhost);

    /* Set non-blocking */
    set_nonblock(port->fd, 1);

    /* Set operations */
    port->ops = &udp_ops;

    /* Port is ready */
    port_set_status(port, PORT_STATUS_UP);

    LOG_INFO("UDP port created: %s %s:%u -> %s:%u",
             name, priv->lhost, priv->lport, priv->rhost, priv->rport);

    return port;
}

/* Get port configuration */
void port_udp_get_config(vfr_port_t *port, char *lhost, u16 *lport,
                         char *rhost, u16 *rport)
{
    udp_priv_t *priv = (udp_priv_t *)port->priv;
    if (lhost) strcpy(lhost, priv->lhost);
    if (lport) *lport = priv->lport;
    if (rhost) strcpy(rhost, priv->rhost);
    if (rport) *rport = priv->rport;
}

/* Create UDP port in client mode */
vfr_port_t *port_udp_cli_create(const char *name, int type,
                                const char *rhost, u16 rport)
{
    vfr_port_t *port;
    udp_priv_t *priv;
    int ret;

    if (!name || !rhost) return NULL;

    port = port_create(name, type, PORT_TRANS_UDP_CLI);
    if (!port) return NULL;

    priv = calloc(1, sizeof(udp_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    strncpy(priv->lhost, "0.0.0.0", sizeof(priv->lhost) - 1);
    priv->lport = 0;
    strncpy(priv->rhost, rhost, sizeof(priv->rhost) - 1);
    priv->rport = rport;

    port->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (port->fd == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP client socket for %s: %d", name, WSAGetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    priv->reuse_addr = 1;
    setsockopt(port->fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&priv->reuse_addr, sizeof(priv->reuse_addr));

#ifdef _WIN32
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(port->fd, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

    memset(&priv->local_addr, 0, sizeof(priv->local_addr));
    priv->local_addr.sin_family = AF_INET;
    priv->local_addr.sin_port = htons(0);
    priv->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(port->fd, (struct sockaddr *)&priv->local_addr, sizeof(priv->local_addr));
    if (ret < 0) {
        LOG_ERROR("Failed to bind UDP client socket %s: %d", name, WSAGetLastError());
        closesocket(port->fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    struct sockaddr_in bound_addr;
    int bound_len = sizeof(bound_addr);
    if (getsockname(port->fd, (struct sockaddr *)&bound_addr, &bound_len) == 0) {
        priv->lport = ntohs(bound_addr.sin_port);
    }

    memset(&priv->remote_addr, 0, sizeof(priv->remote_addr));
    priv->remote_addr.sin_family = AF_INET;
    priv->remote_addr.sin_port = htons(rport);
    priv->remote_addr.sin_addr.s_addr = inet_addr(rhost);

    set_nonblock(port->fd, 1);
    port->ops = &udp_ops;
    port_set_status(port, PORT_STATUS_UP);

    LOG_INFO("UDP client port created: %s local port %u -> %s:%u",
             name, priv->lport, priv->rhost, priv->rport);

    return port;
}

/* Create UDP port in server mode */
vfr_port_t *port_udp_ser_create(const char *name, int type,
                                const char *lhost, u16 lport)
{
    vfr_port_t *port;
    udp_priv_t *priv;
    int ret;

    if (!name) return NULL;

    port = port_create(name, type, PORT_TRANS_UDP_SER);
    if (!port) return NULL;

    priv = calloc(1, sizeof(udp_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    strncpy(priv->lhost, (lhost && strlen(lhost) > 0) ? lhost : "0.0.0.0", sizeof(priv->lhost) - 1);
    priv->lport = lport;
    priv->rhost[0] = '\0';
    priv->rport = 0;

    port->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (port->fd == INVALID_SOCKET) {
        LOG_ERROR("Failed to create UDP server socket for %s: %d", name, WSAGetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    priv->reuse_addr = 1;
    setsockopt(port->fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&priv->reuse_addr, sizeof(priv->reuse_addr));

#ifdef _WIN32
    #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
    #endif
    BOOL bNewBehavior = FALSE;
    DWORD dwBytesReturned = 0;
    WSAIoctl(port->fd, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
#endif

    memset(&priv->local_addr, 0, sizeof(priv->local_addr));
    priv->local_addr.sin_family = AF_INET;
    priv->local_addr.sin_port = htons(lport);
    if (lhost && strlen(lhost) > 0) {
        priv->local_addr.sin_addr.s_addr = inet_addr(lhost);
    } else {
        priv->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    ret = bind(port->fd, (struct sockaddr *)&priv->local_addr, sizeof(priv->local_addr));
    if (ret < 0) {
        LOG_ERROR("Failed to bind UDP server socket %s %s:%u: %d",
                  name, priv->lhost, lport, WSAGetLastError());
        closesocket(port->fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    set_nonblock(port->fd, 1);
    port->ops = &udp_ops;
    port_set_status(port, PORT_STATUS_UP);

    LOG_INFO("UDP server port created: %s listening on %s:%u",
             name, priv->lhost, priv->lport);

    return port;
}