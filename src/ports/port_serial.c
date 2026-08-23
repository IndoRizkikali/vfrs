/*
 * port_serial.c - Serial Port Interface
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>

typedef struct {
    char        device[VFR_MAX_PATH];
    u32         baudrate;
    HANDLE      hCom;
    COMMTIMEOUTS timeouts;
    thread_t    thread;
    volatile int running;
} serial_priv_t;

/* Serial background reader thread with HDLC flag accumulator */
static thread_ret_t THREAD_CALL serial_reader_thread(void *arg)
{
    vfr_port_t *port = (vfr_port_t *)arg;
    serial_priv_t *priv = (serial_priv_t *)port->priv;
    u8 accum[4096];
    size_t accum_len = 0;
    int in_frame = 0;
    u8 chunk[512];
    DWORD bytes_read;

    while (priv->running) {
        /* Chunked read buffer — processes incoming stream in memory */
        if (!ReadFile(priv->hCom, chunk, sizeof(chunk), &bytes_read, NULL)) {
            if (priv->running) {
                msleep(10);
            }
            continue;
        }

        if (bytes_read == 0) {
            /* Timeout, check loop condition */
            continue;
        }

        for (DWORD i = 0; i < bytes_read; i++) {
            u8 byte = chunk[i];
            if (byte == HDLC_FLAG) {
                if (in_frame) {
                    /* Closing flag! We have a complete frame. */
                    if (accum_len > 2) {
                        /* Append closing flag and pass to switch input */
                        accum[accum_len++] = HDLC_FLAG;
                        port->stats.rx_frames++;
                        port->stats.rx_bytes += accum_len;
                        fr_switch_input(g_vfrs, port, accum, accum_len);
                    }
                    /* Reset accumulator and preserve this flag as the start of the next frame */
                    accum_len = 0;
                    accum[accum_len++] = HDLC_FLAG;
                } else {
                    /* Opening flag! */
                    in_frame = 1;
                    accum_len = 0;
                    accum[accum_len++] = HDLC_FLAG;
                }
            } else {
                if (in_frame) {
                    if (accum_len < sizeof(accum) - 2) {
                        accum[accum_len++] = byte;
                    } else {
                        /* Overflow: discard malformed frame and wait for the next flag */
                        in_frame = 0;
                        accum_len = 0;
                    }
                }
            }
        }
    }

    return THREAD_RET;
}

/* Serial port send function — wraps raw frame in HDLC flags + byte-oriented stuffing (Async HDLC). */
static int port_serial_send(vfr_port_t *port, const u8 *frame, size_t len)
{
    u8 out_stuffed[4096];
    size_t stuffed_len, total;
    serial_priv_t *priv;

    if (!port || !port->priv) return -1;
    priv = (serial_priv_t *)port->priv;

    if (len + 2 > sizeof(out_stuffed) - 2) return -1;

    mutex_lock(&port->mutex);
    if (priv->hCom == INVALID_HANDLE_VALUE) {
        mutex_unlock(&port->mutex);
        return -1;
    }

    /* Log to PCAP if port-specific capture is enabled (DLT_FRELAY format) */
    if (port->capture) {
        /* Normalize capture: strip FCS for standard DLT_FRELAY if trailing FCS included */
        size_t pcap_len = (len >= 2) ? len - 2 : len;
        pcap_writer_write(port->capture, frame, pcap_len);
    }

    /* HDLC byte-oriented stuffing (Async HDLC) + flag wrapping */
    stuffed_len = hdlc_stuff(frame, len, out_stuffed + 1, sizeof(out_stuffed) - 2);
    total = stuffed_len + 2;
    if (total + 2 > sizeof(out_stuffed)) {
        mutex_unlock(&port->mutex);
        return -1;
    }
    out_stuffed[0] = HDLC_FLAG;
    out_stuffed[stuffed_len + 1] = HDLC_FLAG;

    size_t written_total = 0;
    while (written_total < total) {
        DWORD written = 0;
        if (!WriteFile(priv->hCom, out_stuffed + written_total, (DWORD)(total - written_total), &written, NULL) || written == 0) {
            mutex_unlock(&port->mutex);
            return -1;
        }
        written_total += written;
    }

    port->stats.tx_frames++;
    port->stats.tx_bytes += len;  /* bill the original frame size, not the stuffed size */
    mutex_unlock(&port->mutex);

    return (int)len;
}

/* Dummy serial receive function */
static int port_serial_recv(vfr_port_t *port, u8 *buf, size_t max_len)
{
    (void)port; (void)buf; (void)max_len;
    msleep(10);
    return -1;
}



/* Serial free function */
static void port_serial_free(vfr_port_t *port)
{
    serial_priv_t *priv;

    if (!port || !port->priv) return;
    priv = (serial_priv_t *)port->priv;

    priv->running = 0;

    /* Cancel pending ReadFile operations and close COM port */
    if (priv->hCom != INVALID_HANDLE_VALUE) {
        CancelIoEx(priv->hCom, NULL);
        CloseHandle(priv->hCom);
        priv->hCom = INVALID_HANDLE_VALUE;
    }

    /* Wait for thread to exit */
    if (priv->thread != 0) {
        thread_join(priv->thread, NULL);
        priv->thread = 0;
    }

    free(priv);
    port->priv = NULL;
}

/* Serial operations */
static const port_ops_t serial_ops = {
    .send = port_serial_send,
    .recv = port_serial_recv,
    .poll = NULL,
    .free = port_serial_free
};

vfr_port_t *port_serial_create(const char *name, int type,
                               const char *device, u32 baudrate)
{
    vfr_port_t *port;
    serial_priv_t *priv;
    DCB dcb;

    if (!name || !device) return NULL;

    port = port_create(name, type, PORT_TRANS_SERIAL);
    if (!port) return NULL;

    priv = calloc(1, sizeof(serial_priv_t));
    if (!priv) {
        port_free(port);
        return NULL;
    }

    port->priv = priv;
    priv->running = 1;

    strncpy(priv->device, device, sizeof(priv->device) - 1);
    priv->baudrate = baudrate;

    /* Open COM port (blocking mode) */
    priv->hCom = CreateFileA(device, GENERIC_READ | GENERIC_WRITE,
                              0, NULL, OPEN_EXISTING,
                              0, NULL);

    if (priv->hCom == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Failed to open serial port %s: %lu", device, GetLastError());
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Configure DCB */
    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    dcb.BaudRate = baudrate;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;

    if (!SetCommState(priv->hCom, &dcb)) {
        LOG_ERROR("Failed to configure serial port: %lu", GetLastError());
        CloseHandle(priv->hCom);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Set timeouts for read block behaviour */
    priv->timeouts.ReadIntervalTimeout = MAXDWORD;
    priv->timeouts.ReadTotalTimeoutMultiplier = 0;
    priv->timeouts.ReadTotalTimeoutConstant = 100; /* 100ms read timeout */
    priv->timeouts.WriteTotalTimeoutMultiplier = 0;
    priv->timeouts.WriteTotalTimeoutConstant = 100;
    SetCommTimeouts(priv->hCom, &priv->timeouts);

    port->ops = &serial_ops;
    port_set_status(port, PORT_STATUS_UP);

    /* Start background reader thread */
    if (thread_create(&priv->thread, serial_reader_thread, port) != 0) {
        LOG_ERROR("Failed to start serial reader thread for %s", name);
        CloseHandle(priv->hCom);
        free(priv);
        port_free(port);
        return NULL;
    }

    /* Initialize congestion access rate to the configured baudrate */
    cgst_init(port, baudrate, 1000);

    LOG_INFO("Serial port created: %s %s @ %u baud", name, device, baudrate);

    return port;
}

#else
/* POSIX serial implementation placeholder */
vfr_port_t *port_serial_create(const char *name, int type,
                               const char *device, u32 baudrate)
{
    (void)name; (void)type; (void)device; (void)baudrate;
    LOG_ERROR("Serial ports not implemented on this platform");
    return NULL;
}
#endif