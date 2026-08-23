/*
 * port_tcp.c - TCP Port Interface
 * VFRS - Virtual Frame Relay Switch
 * Uses 4-byte big-endian length prefix (dynamips compatible)
 */

#include "vfr.h"
#include <string.h>

/* Reconnection parameters */
#define TCP_RECONNECT_MIN   1000    /* 1 second */
#define TCP_RECONNECT_MAX   60000   /* 60 seconds */
#define TCP_RECONNECT_MULT  2       /* Exponential backoff multiplier */

/* Forward declarations */
static thread_ret_t THREAD_CALL tcp_server_reader_thread(void *arg);
static thread_ret_t THREAD_CALL tcp_client_thread(void *arg);

/* TCP free function — stops threads, closes descriptors, and cleans up memory */
static void port_tcp_free(vfr_port_t *port)
{
    tcp_priv_t *priv;

    if (!port || !port->priv) return;
    priv = (tcp_priv_t *)port->priv;

    priv->running = 0;
    priv->connected = 0;

    /* Close sockets to wake up threads blocked on accept() or recv() */
    if (priv->listen_fd != INVALID_SOCKET) {
        shutdown(priv->listen_fd, SD_BOTH);
        closesocket(priv->listen_fd);
        priv->listen_fd = INVALID_SOCKET;
    }

    mutex_lock(&port->mutex);
    if (port->fd != INVALID_SOCKET) {
        shutdown(port->fd, SD_BOTH);
        closesocket(port->fd);
        port->fd = INVALID_SOCKET;
    }
    mutex_unlock(&port->mutex);

    /* Wait for threads to terminate */
    if (priv->accept_thread != 0) {
        thread_join(priv->accept_thread, NULL);
        priv->accept_thread = 0;
    }

    if (priv->reader_thread != 0) {
        thread_join(priv->reader_thread, NULL);
        priv->reader_thread = 0;
    }

    free(priv);
    port->priv = NULL;
}

/* TCP complete-send loop — ensures all bytes of header and payload are sent */
static int port_tcp_send_all(socket_fd_t fd, const u8 *buf, size_t total_len)
{
    size_t sent = 0;
    while (sent < total_len) {
        int ret = send(fd, (const char *)buf + sent, (int)(total_len - sent), 0);
        if (ret <= 0) {
            return -1;
        }
        sent += (size_t)ret;
    }
    return 0;
}

/* TCP send with length prefix — sends 4-byte big-endian length header
 * followed by the frame data in wire format (compatible with dynamips). */
static int port_tcp_send(vfr_port_t *port, const u8 *frame, size_t len)
{
    u8 send_buf[8196];
    tcp_priv_t *priv;

    if (!port || !port->priv || len + 4 > sizeof(send_buf)) return -1;
    priv = (tcp_priv_t *)port->priv;

    mutex_lock(&port->mutex);
    if (port->fd == INVALID_SOCKET || !priv->connected) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Build 4-byte length header in big-endian wire format followed by frame body */
    send_buf[0] = (u8)(len >> 24);
    send_buf[1] = (u8)(len >> 16);
    send_buf[2] = (u8)(len >> 8);
    send_buf[3] = (u8)len;
    memcpy(send_buf + 4, frame, len);

    size_t total_len = len + 4;
    if (port_tcp_send_all(port->fd, send_buf, total_len) < 0) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Log to PCAP if port-specific capture is enabled */
    if (port->capture) {
        pcap_writer_write(port->capture, frame, len);
    }

    port->stats.tx_frames++;
    port->stats.tx_bytes += total_len;
    mutex_unlock(&port->mutex);
    return 0;
}

/* TCP receive — dummy function since receiving is handled by background reader threads */
static int port_tcp_recv(vfr_port_t *port, u8 *buf, size_t max_len)
{
    (void)port; (void)buf; (void)max_len;
    msleep(10); /* Prevent spin loops if called */
    return -1;
}



/* TCP connect function (for client mode) */
int tcp_connect(vfr_port_t *port)
{
    tcp_priv_t *priv = (tcp_priv_t *)port->priv;
    int ret;

    if (!port) return -1;

    mutex_lock(&port->mutex);
    if (port->fd != INVALID_SOCKET) {
        closesocket(port->fd);
        port->fd = INVALID_SOCKET;
    }

    port->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (port->fd == INVALID_SOCKET) {
        LOG_ERROR("TCP client socket creation failed: %d", WSAGetLastError());
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Set TCP_NODELAY for low latency */
    int flag = 1;
    setsockopt(port->fd, IPPROTO_TCP, TCP_NODELAY,
               (const char *)&flag, sizeof(flag));

    /* Set SO_KEEPALIVE */
    setsockopt(port->fd, SOL_SOCKET, SO_KEEPALIVE,
               (const char *)&flag, sizeof(flag));

    /* Connect (blocking) */
    mutex_unlock(&port->mutex);  /* release during connection attempt */
    ret = connect(port->fd, (struct sockaddr *)&priv->remote_addr,
                  sizeof(priv->remote_addr));
    mutex_lock(&port->mutex);

    if (ret < 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAECONNREFUSED) {
            LOG_DEBUG("TCP connect failed to %s:%u: %d",
                      priv->rhost, priv->rport, err);
        }
        closesocket(port->fd);
        port->fd = INVALID_SOCKET;
        mutex_unlock(&port->mutex);
        return -1;
    }

    priv->connected = 1;
    priv->reconnect_delay = TCP_RECONNECT_MIN;
    port_set_status(port, PORT_STATUS_UP);

    LOG_INFO("TCP connected to %s:%u", priv->rhost, priv->rport);
    mutex_unlock(&port->mutex);
    return 0;
}

/* TCP port operations */
static const port_ops_t tcp_ops = {
    .send  = port_tcp_send,
    .recv  = port_tcp_recv,
    .poll  = NULL,
    .free  = port_tcp_free
};

/* TCP client connection & reader thread */
/* TCP client connection & reader thread */
static thread_ret_t THREAD_CALL tcp_client_thread(void *arg)
{
    vfr_port_t *port = (vfr_port_t *)arg;
    tcp_priv_t *priv = (tcp_priv_t *)port->priv;
    u8 buf[8192];

    while (priv->running) {
        if (!priv->connected) {
            if (tcp_connect(port) < 0) {
                /* Exponential backoff (interruptible) */
                u32 delay = priv->reconnect_delay;
                while (priv->running && delay > 0) {
                    u32 sleep_time = (delay > 100) ? 100 : delay;
                    msleep(sleep_time);
                    delay -= sleep_time;
                }
                u32 next_delay = priv->reconnect_delay * TCP_RECONNECT_MULT;
                priv->reconnect_delay = (next_delay < priv->max_reconnect_delay) ? next_delay : priv->max_reconnect_delay;
                continue;
            }
        }

        /* We are connected! Blocking read loop */
        while (priv->running && priv->connected) {
            u8 header[4];
            size_t header_read = 0;
            while (header_read < 4 && priv->running && priv->connected) {
                int ret = recv(port->fd, (char *)header + header_read, (int)(4 - header_read), 0);
                if (ret <= 0) {
                    goto lost;
                }
                header_read += (size_t)ret;
            }

            if (!priv->running || !priv->connected) break;

            u32 frame_len = ((u32)header[0] << 24) |
                            ((u32)header[1] << 16) |
                            ((u32)header[2] << 8)  |
                            (u32)header[3];

            if (frame_len > sizeof(buf) || frame_len > 65535) {
                LOG_WARN("TCP client frame too large: %u bytes", frame_len);
                goto lost;
            }

            size_t total_read = 0;
            while (total_read < frame_len && priv->running && priv->connected) {
                int ret = recv(port->fd, (char *)buf + total_read, (int)(frame_len - total_read), 0);
                if (ret <= 0) {
                    goto lost;
                }
                total_read += (size_t)ret;
            }

            if (priv->running && priv->connected) {
                port->stats.rx_frames++;
                port->stats.rx_bytes += total_read + 4;
                fr_switch_input(g_vfrs, port, buf, total_read);
            }
        }
        continue;

    lost:
        mutex_lock(&port->mutex);
        priv->connected = 0;
        if (port->fd != INVALID_SOCKET) {
            closesocket(port->fd);
            port->fd = INVALID_SOCKET;
        }
        port_set_status(port, PORT_STATUS_DOWN);
        mutex_unlock(&port->mutex);
        LOG_INFO("TCP client connection lost on %s, will reconnect...", port->name);
    }
    return THREAD_RET;
}

/* TCP server reader thread */
static thread_ret_t THREAD_CALL tcp_server_reader_thread(void *arg)
{
    vfr_port_t *port = (vfr_port_t *)arg;
    tcp_priv_t *priv = (tcp_priv_t *)port->priv;
    u8 buf[8192];

    while (priv->running && priv->connected) {
        u8 header[4];
        size_t header_read = 0;
        while (header_read < 4 && priv->running && priv->connected) {
            int ret = recv(port->fd, (char *)header + header_read, (int)(4 - header_read), 0);
            if (ret <= 0) {
                goto lost;
            }
            header_read += (size_t)ret;
        }

        if (!priv->running || !priv->connected) break;

        u32 frame_len = ((u32)header[0] << 24) |
                        ((u32)header[1] << 16) |
                        ((u32)header[2] << 8)  |
                        (u32)header[3];

        if (frame_len > sizeof(buf) || frame_len > 65535) {
            LOG_WARN("TCP server reader frame too large: %u bytes", frame_len);
            goto lost;
        }

        size_t total_read = 0;
        while (total_read < frame_len && priv->running && priv->connected) {
            int ret = recv(port->fd, (char *)buf + total_read, (int)(frame_len - total_read), 0);
            if (ret <= 0) {
                goto lost;
            }
            total_read += (size_t)ret;
        }

        if (priv->running && priv->connected) {
            port->stats.rx_frames++;
            port->stats.rx_bytes += total_read + 4;
            fr_switch_input(g_vfrs, port, buf, total_read);
        }
    }
    return THREAD_RET;

lost:
    mutex_lock(&port->mutex);
    priv->connected = 0;
    if (port->fd != INVALID_SOCKET) {
        closesocket(port->fd);
        port->fd = INVALID_SOCKET;
    }
    port_set_status(port, PORT_STATUS_DOWN);
    mutex_unlock(&port->mutex);
    LOG_INFO("TCP server connection lost on %s", port->name);
    return THREAD_RET;
}

/* TCP server accept thread */
static thread_ret_t THREAD_CALL tcp_accept_thread(void *arg)
{
    vfr_port_t *port = (vfr_port_t *)arg;
    tcp_priv_t *priv = (tcp_priv_t *)port->priv;
    struct sockaddr_in client_addr;
    int addr_len = sizeof(client_addr);
    socket_fd_t client_fd;

    LOG_INFO("TCP server listening on %s:%u", priv->lhost, priv->lport);

    while (priv->running) {
        client_fd = accept(priv->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == INVALID_SOCKET) {
            if (priv->running) {
                msleep(100);
            }
            continue;
        }

        /* Enforce paired mode remote peer filtering */
        if (port->transport == PORT_TRANS_TCP && priv->remote_addr.sin_port != 0) {
            if (client_addr.sin_addr.s_addr != priv->remote_addr.sin_addr.s_addr ||
                client_addr.sin_port != priv->remote_addr.sin_port) {
                LOG_WARN("TCP paired port %s: rejected connection from %s:%u (expected %s:%u)",
                         port->name, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                         priv->rhost, priv->rport);
                closesocket(client_fd);
                continue;
            }
        }

        LOG_INFO("TCP client connected from %s:%u on %s",
                 inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), port->name);

        /* Configure low-latency and keepalive socket options on accepted socket */
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
        setsockopt(client_fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&flag, sizeof(flag));

        mutex_lock(&port->mutex);
        /* If we are already running a reader thread, disconnect old client */
        priv->connected = 0;
        if (port->fd != INVALID_SOCKET) {
            shutdown(port->fd, SD_BOTH);
            closesocket(port->fd);
            port->fd = INVALID_SOCKET;
        }
        mutex_unlock(&port->mutex);

        if (priv->reader_thread != 0) {
            thread_join(priv->reader_thread, NULL);
            priv->reader_thread = 0;
        }

        /* Accept new client connection */
        mutex_lock(&port->mutex);
        port->fd = client_fd;
        priv->connected = 1;
        port_set_status(port, PORT_STATUS_UP);
        mutex_unlock(&port->mutex);

        /* Start a new reader thread for this client */
        if (thread_create(&priv->reader_thread, tcp_server_reader_thread, port) != 0) {
            LOG_WARN("Failed to start reader thread for client on %s", port->name);
            closesocket(client_fd);
            mutex_lock(&port->mutex);
            port->fd = INVALID_SOCKET;
            priv->connected = 0;
            port_set_status(port, PORT_STATUS_DOWN);
            mutex_unlock(&port->mutex);
        }
    }

    return THREAD_RET;
}

/* Create TCP port in paired mode */
vfr_port_t *port_tcp_create(const char *name, int type,
                             const char *lhost, u16 lport,
                             const char *rhost, u16 rport)
{
    vfr_port_t *port;
    tcp_priv_t *priv;
    int ret;

    if (!name || !rhost) return NULL;

    port = port_create(name, type, PORT_TRANS_TCP);
    if (!port) return NULL;

    priv = calloc(1, sizeof(tcp_priv_t));
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
    priv->mode = 0;  /* Paired mode */
    priv->running = 1;

    /* Create listening socket */
    priv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (priv->listen_fd == INVALID_SOCKET) {
        LOG_ERROR("Failed to create TCP paired listen socket: %d", WSAGetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set options */
    int flag = 1;
    setsockopt(priv->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&flag, sizeof(flag));

    /* Bind */
    memset(&priv->local_addr, 0, sizeof(priv->local_addr));
    priv->local_addr.sin_family = AF_INET;
    priv->local_addr.sin_port = htons(lport);
    priv->local_addr.sin_addr.s_addr = lhost ? inet_addr(lhost) : htonl(INADDR_ANY);

    ret = bind(priv->listen_fd, (struct sockaddr *)&priv->local_addr,
               sizeof(priv->local_addr));
    if (ret < 0) {
        LOG_ERROR("Failed to bind TCP paired socket: %d", WSAGetLastError());
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Listen */
    ret = listen(priv->listen_fd, 1);
    if (ret < 0) {
        LOG_ERROR("Failed to listen on TCP paired socket: %d", WSAGetLastError());
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set remote address for paired mode */
    memset(&priv->remote_addr, 0, sizeof(priv->remote_addr));
    priv->remote_addr.sin_family = AF_INET;
    priv->remote_addr.sin_port = htons(rport);
    priv->remote_addr.sin_addr.s_addr = inet_addr(rhost);

    port->ops = &tcp_ops;
    port->status = PORT_STATUS_DOWN;

    /* Start accept thread */
    if (thread_create(&priv->accept_thread, tcp_accept_thread, port) != 0) {
        LOG_ERROR("Failed to start accept thread for paired TCP port: %s", name);
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    LOG_INFO("TCP paired port created: %s listening on %s:%u",
             name, priv->lhost, priv->lport);

    return port;
}

/* Create TCP port in client mode */
vfr_port_t *port_tcp_cli_create(const char *name, int type,
                                const char *rhost, u16 rport)
{
    vfr_port_t *port;
    tcp_priv_t *priv;

    if (!name || !rhost) return NULL;

    port = port_create(name, type, PORT_TRANS_TCP_CLI);
    if (!port) return NULL;

    priv = calloc(1, sizeof(tcp_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    priv->mode = 1;  /* Client mode */
    priv->reconnect = 1;
    priv->running = 1;
    priv->reconnect_delay = TCP_RECONNECT_MIN;
    priv->max_reconnect_delay = TCP_RECONNECT_MAX;
    priv->listen_fd = INVALID_SOCKET;

    strncpy(priv->rhost, rhost, sizeof(priv->rhost) - 1);
    priv->rport = rport;
    strcpy(priv->lhost, "0.0.0.0");
    priv->lport = 0;

    /* Set remote address */
    memset(&priv->remote_addr, 0, sizeof(priv->remote_addr));
    priv->remote_addr.sin_family = AF_INET;
    priv->remote_addr.sin_port = htons(rport);
    priv->remote_addr.sin_addr.s_addr = inet_addr(rhost);

    port->ops = &tcp_ops;
    port->status = PORT_STATUS_DOWN;

    LOG_INFO("TCP client port created: %s -> %s:%u",
             name, priv->rhost, priv->rport);

    /* Start client thread which connects and reads packets in the background */
    if (thread_create(&priv->reader_thread, tcp_client_thread, port) != 0) {
        LOG_ERROR("Failed to start client thread for TCP client port: %s", name);
        free(priv);
        port_free(port);
        return NULL;
    }

    return port;
}

/* Create TCP port in server mode */
vfr_port_t *port_tcp_ser_create(const char *name, int type,
                                const char *lhost, u16 lport)
{
    vfr_port_t *port;
    tcp_priv_t *priv;
    int ret;

    if (!name) return NULL;

    port = port_create(name, type, PORT_TRANS_TCP_SER);
    if (!port) return NULL;

    priv = calloc(1, sizeof(tcp_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    priv->mode = 2;  /* Server mode */
    priv->running = 1;

    strncpy(priv->lhost, lhost ? lhost : "0.0.0.0", sizeof(priv->lhost) - 1);
    priv->lport = lport;

    /* Create listening socket */
    priv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (priv->listen_fd == INVALID_SOCKET) {
        LOG_ERROR("Failed to create TCP server socket: %d", WSAGetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set options */
    int flag = 1;
    setsockopt(priv->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&flag, sizeof(flag));

    /* Bind */
    memset(&priv->local_addr, 0, sizeof(priv->local_addr));
    priv->local_addr.sin_family = AF_INET;
    priv->local_addr.sin_port = htons(lport);
    priv->local_addr.sin_addr.s_addr = lhost ? inet_addr(lhost) : htonl(INADDR_ANY);

    ret = bind(priv->listen_fd, (struct sockaddr *)&priv->local_addr,
               sizeof(priv->local_addr));
    if (ret < 0) {
        LOG_ERROR("Failed to bind TCP server socket: %d", WSAGetLastError());
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Listen */
    ret = listen(priv->listen_fd, 5);
    if (ret < 0) {
        LOG_ERROR("Failed to listen on TCP server: %d", WSAGetLastError());
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    port->ops = &tcp_ops;
    port->status = PORT_STATUS_DOWN;

    /* Start accept thread for server-mode TCP ports. */
    if (thread_create(&priv->accept_thread, tcp_accept_thread, port) != 0) {
        LOG_ERROR("Failed to start accept thread for TCP server port: %s", name);
        closesocket(priv->listen_fd);
        free(priv);
        port_free(port);
        return NULL;
    }

    LOG_INFO("TCP server port created: %s listening on %s:%u",
             name, priv->lhost, priv->lport);

    return port;
}
