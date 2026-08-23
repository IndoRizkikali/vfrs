/*
 * port_pipe.c - Named Pipe Interface
 * VFRS - Virtual Frame Relay Switch
 * Windows Named Pipes (compatible with dynamips)
 */

#include "vfr.h"
#include <string.h>

#ifdef _WIN32

#include <windows.h>

/* Named pipe private data */
typedef struct {
    char        pipename[VFR_MAX_PATH];
    int         server;
    HANDLE      hPipe;
    thread_t    thread;
    volatile int running;
    int         connected;
} pipe_priv_t;

/* Background named pipe reader thread.
 * Named pipe uses full standard Frame Relay format (Flag + HDLC stuffing + FCS)
 * per frame_format.md §2.1, identical to the serial transport.
 * Reads byte-by-byte, accumulating octets between 0x7E flag delimiters.
 */
static thread_ret_t THREAD_CALL pipe_reader_thread(void *arg)
{
    vfr_port_t *port = (vfr_port_t *)arg;
    pipe_priv_t *priv = (pipe_priv_t *)port->priv;
    u8 accum[4096];
    size_t accum_len = 0;
    int in_frame = 0;
    u8 byte;
    DWORD bytes_read;
    char full_name[VFR_MAX_PATH + 32];

    snprintf(full_name, sizeof(full_name), "\\\\.\\pipe\\%s", priv->pipename);

    while (priv->running) {
        if (priv->server) {
            /* Server mode: wait for client to connect */
            BOOL connected = ConnectNamedPipe(priv->hPipe, NULL) ?
                             TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!connected) {
                if (priv->running) {
                    msleep(100);
                }
                continue;
            }
            mutex_lock(&port->mutex);
            priv->connected = 1;
            port_set_status(port, PORT_STATUS_UP);
            mutex_unlock(&port->mutex);
            LOG_INFO("Named pipe client connected to %s", full_name);
        } else {
            /* Client mode: if not connected, attempt connection */
            if (!priv->connected) {
                priv->hPipe = CreateFileA(
                    full_name,
                    GENERIC_READ | GENERIC_WRITE,
                    0, NULL, OPEN_EXISTING, 0, NULL
                );
                if (priv->hPipe == INVALID_HANDLE_VALUE) {
                    msleep(1000); /* retry after 1s */
                    continue;
                }
                mutex_lock(&port->mutex);
                priv->connected = 1;
                port_set_status(port, PORT_STATUS_UP);
                mutex_unlock(&port->mutex);
                LOG_INFO("Connected to named pipe server: %s", full_name);
            }
        }

        /* HDLC frame accumulation loop — byte-by-byte between 0x7E flags.
         * Identical logic to serial_reader_thread in port_serial.c.
         * Flag sharing is preserved: the closing flag of one frame is kept
         * as the opening flag of the next. */
        in_frame = 0;
        accum_len = 0;
        while (priv->running && priv->connected) {
            DWORD bytes_avail = 0;
            if (!PeekNamedPipe(priv->hPipe, NULL, 0, NULL, &bytes_avail, NULL)) {
                goto lost;
            }
            if (bytes_avail == 0) {
                msleep(10);
                continue;
            }

            if (!ReadFile(priv->hPipe, &byte, 1, &bytes_read, NULL)) {
                goto lost;
            }
            if (bytes_read == 0) {
                continue;  /* no data yet, loop */
            }

            if (byte == HDLC_FLAG) {
                if (in_frame) {
                    /* Closing flag: submit frame if it has content beyond the flag */
                    if (accum_len > 2) {
                        accum[accum_len++] = HDLC_FLAG;
                        port->stats.rx_frames++;
                        port->stats.rx_bytes += accum_len;
                        fr_switch_input(g_vfrs, port, accum, accum_len);
                    }
                    /* Preserve closing flag as opening flag of next frame */
                    accum_len = 0;
                    accum[accum_len++] = HDLC_FLAG;
                } else {
                    /* Opening flag */
                    in_frame = 1;
                    accum_len = 0;
                    accum[accum_len++] = HDLC_FLAG;
                }
            } else {
                if (in_frame) {
                    if (accum_len < sizeof(accum) - 2) {
                        accum[accum_len++] = byte;
                    } else {
                        /* Buffer overflow: discard, wait for next flag */
                        LOG_WARN("Pipe frame overflow on %s: discarding", port->name);
                        in_frame = 0;
                        accum_len = 0;
                    }
                }
            }
        }
        continue;

    lost:
        mutex_lock(&port->mutex);
        priv->connected = 0;
        port_set_status(port, PORT_STATUS_DOWN);
        if (priv->server) {
            DisconnectNamedPipe(priv->hPipe);
        } else {
            if (priv->hPipe != INVALID_HANDLE_VALUE) {
                CloseHandle(priv->hPipe);
                priv->hPipe = INVALID_HANDLE_VALUE;
            }
        }
        mutex_unlock(&port->mutex);
        LOG_INFO("Named pipe connection lost on %s", port->name);
        in_frame = 0;
        accum_len = 0;
    }

    return THREAD_RET;
}

/* Pipe free function — stops the reader thread, closes handle, and cleans up memory */
static void port_pipe_free(vfr_port_t *port)
{
    pipe_priv_t *priv;

    if (!port || !port->priv) return;
    priv = (pipe_priv_t *)port->priv;

    priv->running = 0;
    priv->connected = 0;

    /* Close pipe to wake up any blocked ReadFile or ConnectNamedPipe */
    if (priv->hPipe != INVALID_HANDLE_VALUE) {
        if (priv->server) {
            DisconnectNamedPipe(priv->hPipe);
        }
        CloseHandle(priv->hPipe);
        priv->hPipe = INVALID_HANDLE_VALUE;
    }

    /* Wait for thread to exit */
    if (priv->thread != 0) {
        thread_join(priv->thread, NULL);
        priv->thread = 0;
    }

    free(priv);
    port->priv = NULL;
}

/* Pipe send function — wraps frame in HDLC flags + async byte stuffing.
 * Named pipe uses full standard format (Flag + HDLC stuffing + FCS) per
 * frame_format.md §2.1. The frame parameter already includes the FCS bytes
 * (appended by vfrs_switch_frame or fr_build_ui_frame). */
static int port_pipe_send(vfr_port_t *port, const u8 *frame, size_t len)
{
    DWORD written;
    pipe_priv_t *priv;
    u8 out_stuffed[4096];
    size_t stuffed_len, total;

    if (!port || !port->priv) return -1;
    priv = (pipe_priv_t *)port->priv;

    mutex_lock(&port->mutex);
    if (priv->hPipe == INVALID_HANDLE_VALUE || !priv->connected) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* HDLC byte stuffing + flag wrapping (same as port_serial_send) */
    stuffed_len = hdlc_stuff(frame, len, out_stuffed + 1, sizeof(out_stuffed) - 2);
    total = stuffed_len + 2;
    if (total > sizeof(out_stuffed)) {
        mutex_unlock(&port->mutex);
        return -1;  /* frame too large after stuffing */
    }
    out_stuffed[0] = HDLC_FLAG;
    out_stuffed[stuffed_len + 1] = HDLC_FLAG;

    /* Write stuffed frame to pipe */
    size_t written_total = 0;
    while (written_total < total) {
        if (!WriteFile(priv->hPipe, out_stuffed + written_total,
                       (DWORD)(total - written_total), &written, NULL)) {
            mutex_unlock(&port->mutex);
            return -1;
        }
        if (written == 0) {
            mutex_unlock(&port->mutex);
            return -1;  /* pipe closed */
        }
        written_total += written;
    }

    /* Log to PCAP if capture is enabled */
    if (port->capture) {
        pcap_writer_write(port->capture, frame, len);
    }
    if (g_vfrs && g_vfrs->global_capture) {
        pcap_writer_write(g_vfrs->global_capture, frame, len);
    }

    port->stats.tx_frames++;
    port->stats.tx_bytes += len;
    mutex_unlock(&port->mutex);
    return 0;
}

/* Dummy pipe receive function */
static int port_pipe_recv(vfr_port_t *port, u8 *buf, size_t max_len)
{
    (void)port; (void)buf; (void)max_len;
    msleep(10);
    return -1;
}



/* Pipe operations */
static const port_ops_t pipe_ops = {
    .send = port_pipe_send,
    .recv = port_pipe_recv,
    .poll = NULL,
    .free = port_pipe_free
};

/* Create server pipe */
static vfr_port_t *port_pipe_server(const char *name, int type, const char *pipename)
{
    vfr_port_t *port;
    pipe_priv_t *priv;
    char full_name[VFR_MAX_PATH];

    port = port_create(name, type, PORT_TRANS_PIPE);
    if (!port) return NULL;

    priv = calloc(1, sizeof(pipe_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    priv->server = 1;
    priv->running = 1;
    strncpy(priv->pipename, pipename, sizeof(priv->pipename) - 1);

    /* Build full pipe name */
    snprintf(full_name, sizeof(full_name), "\\\\.\\pipe\\%s", pipename);

    /* Create named pipe (blocking mode) */
    priv->hPipe = CreateNamedPipeA(
        full_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,          /* Max instances */
        4096,       /* Out buffer */
        4096,       /* In buffer */
        0,          /* Timeout */
        NULL        /* Security */
    );

    if (priv->hPipe == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Failed to create named pipe %s: %lu", full_name, GetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    port->ops = &pipe_ops;
    port->status = PORT_STATUS_DOWN;

    /* Start background reader thread */
    if (thread_create(&priv->thread, pipe_reader_thread, port) != 0) {
        LOG_ERROR("Failed to start named pipe reader thread for %s", name);
        CloseHandle(priv->hPipe);
        free(priv);
        port_free(port);
        return NULL;
    }

    LOG_INFO("Named pipe server created: %s", full_name);
    return port;
}

/* Create client pipe */
static vfr_port_t *port_pipe_client(const char *name, int type, const char *pipename)
{
    vfr_port_t *port;
    pipe_priv_t *priv;

    port = port_create(name, type, PORT_TRANS_PIPE);
    if (!port) return NULL;

    priv = calloc(1, sizeof(pipe_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    priv->server = 0;
    priv->running = 1;
    priv->hPipe = INVALID_HANDLE_VALUE;
    strncpy(priv->pipename, pipename, sizeof(priv->pipename) - 1);

    port->ops = &pipe_ops;
    port->status = PORT_STATUS_DOWN;

    /* Start background reader thread (handles connection and reading) */
    if (thread_create(&priv->thread, pipe_reader_thread, port) != 0) {
        LOG_ERROR("Failed to start named pipe client reader thread for %s", name);
        free(priv);
        port_free(port);
        return NULL;
    }

    return port;
}

vfr_port_t *port_pipe_create(const char *name, int type,
                             const char *pipename, int server)
{
    if (server) {
        return port_pipe_server(name, type, pipename);
    } else {
        return port_pipe_client(name, type, pipename);
    }
}

#else
/* Non-Windows pipe implementation - use socketpair as fallback */
#include <sys/socket.h>

typedef struct {
    int         fd;
} pipe_priv_t;

static void port_pipe_free(vfr_port_t *port)
{
    if (!port || !port->priv) return;
    pipe_priv_t *priv = (pipe_priv_t *)port->priv;
    if (priv->fd >= 0) {
        close(priv->fd);
        priv->fd = -1;
    }
    free(priv);
    port->priv = NULL;
}

static const port_ops_t pipe_ops = {
    .send = port_default_send,
    .recv = port_default_recv,
    .poll = port_default_poll,
    .free = port_pipe_free
};

vfr_port_t *port_pipe_create(const char *name, int type,
                             const char *pipename, int server)
{
    vfr_port_t *port;
    pipe_priv_t *priv;
    int sv[2];

    (void)server;

    port = port_create(name, type, PORT_TRANS_PIPE);
    if (!port) return NULL;

    priv = calloc(1, sizeof(pipe_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;

    /* Create socket pair for Unix domain */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        LOG_ERROR("Failed to create socket pair: %s", strerror(errno));
        free(priv);
        port_free(port);
        return NULL;
    }

    port->fd = sv[0];  /* Local end */

    port->ops = &pipe_ops;
    port_set_status(port, PORT_STATUS_UP);

    LOG_INFO("Socket pair created for pipe simulation");
    return port;
}
#endif