/*
 * port_common.c - Common Port Infrastructure
 * VFRS - Virtual Frame Relay Switch
 */

#include "vfr.h"
#include <ctype.h>

/* ============================================================
 * Utility Functions
 * ============================================================ */

u64 get_tick_count(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

u64 tick_diff(u64 start, u64 end)
{
    return (end >= start) ? (end - start) : ((u64)-1 - start + end);
}

/* Timer management */
int timer_is_expired(vfr_timer_t *t)
{
    if (!t || t->interval == 0) return 0;
    return (get_tick_count() >= t->expire);
}

void timer_set(vfr_timer_t *t, u32 interval_ms)
{
    t->interval = interval_ms;
    t->expire = get_tick_count() + interval_ms;
}

void timer_cancel(vfr_timer_t *t)
{
    if (t) {
        t->expire = 0;
        t->interval = 0;
    }
}

/* ============================================================
 * Mutex / Threading
 * ============================================================ */

#ifdef _WIN32

int mutex_init(mutex_t *m)
{
    InitializeCriticalSection(m);
    return 0;
}

void mutex_destroy(mutex_t *m)
{
    DeleteCriticalSection(m);
}

void mutex_lock(mutex_t *m)
{
    EnterCriticalSection(m);
}

void mutex_unlock(mutex_t *m)
{
    LeaveCriticalSection(m);
}

#else

int mutex_init(mutex_t *m)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    return ret;
}

void mutex_destroy(mutex_t *m)
{
    pthread_mutex_destroy(m);
}

void mutex_lock(mutex_t *m)
{
    pthread_mutex_lock(m);
}

void mutex_unlock(mutex_t *m)
{
    pthread_mutex_unlock(m);
}

#endif

/* Thread functions */
#ifdef _WIN32

int thread_create(thread_t *tid, thread_ret_t (THREAD_CALL *func)(void*), void *arg)
{
    *tid = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL);
    return (*tid != NULL) ? 0 : -1;
}

int thread_join(thread_t tid, thread_ret_t *retval)
{
    DWORD rc = WaitForSingleObject(tid, INFINITE);
    if (retval && rc != WAIT_FAILED) {
        GetExitCodeThread(tid, (LPDWORD)retval);
    }
    CloseHandle(tid);
    return (rc == WAIT_OBJECT_0) ? 0 : -1;
}

int thread_detach(thread_t tid)
{
    CloseHandle(tid);
    return 0;
}

#else

int thread_create(thread_t *tid, thread_ret_t (THREAD_CALL *func)(void*), void *arg)
{
    return pthread_create(tid, NULL, (void *(*)(void*))func, arg);
}

int thread_join(thread_t tid, thread_ret_t *retval)
{
    return pthread_join(tid, retval);
}

int thread_detach(thread_t tid)
{
    return pthread_detach(tid);
}

#endif

/* ============================================================
 * Network Initialization (Winsock2)
 * ============================================================ */

#ifdef _WIN32

static int g_winsock_init = 0;

int winsock_init(void)
{
    WSADATA wsa_data;
    int err;

    if (g_winsock_init) return 0;

    err = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (err != 0) {
        LOG_ERROR("Winsock initialization failed: %d", err);
        return -1;
    }

    if (LOBYTE(wsa_data.wVersion) != 2 || HIBYTE(wsa_data.wVersion) != 2) {
        LOG_ERROR("Winsock version 2.2 not available");
        WSACleanup();
        return -1;
    }

    g_winsock_init = 1;
    LOG_INFO("Winsock initialized: version %d.%d",
             LOBYTE(wsa_data.wVersion), HIBYTE(wsa_data.wVersion));
    return 0;
}

void winsock_shutdown(void)
{
    if (g_winsock_init) {
        WSACleanup();
        g_winsock_init = 0;
    }
}

int set_nonblock(socket_fd_t fd, int nb)
{
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode);
}

#else

int set_nonblock(int fd, int nb)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nb) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

#endif

/* ============================================================
 * Port Base Operations
 * ============================================================ */

void port_stats_init(vfr_port_t *port)
{
    if (!port) return;
    memset(&port->stats, 0, sizeof(port->stats));
}

int port_set_status(vfr_port_t *port, int status)
{
    int old_status;
    if (!port) return -1;

    mutex_lock(&port->mutex);
    old_status = port->status;
    port->status = status;
    mutex_unlock(&port->mutex);

    LOG_INFO("Port %s status: %s", port->name,
             status == PORT_STATUS_UP ? "UP" :
             (status == PORT_STATUS_CONN ? "CONNECTED" : "DOWN"));

    if (old_status != status && g_vfrs) {
        vfrs_propagate_port_status_change(g_vfrs, port);
    }
    return 0;
}

int port_get_status(vfr_port_t *port)
{
    int status;
    if (!port) return PORT_STATUS_DOWN;

    mutex_lock(&port->mutex);
    status = port->status;
    mutex_unlock(&port->mutex);

    return status;
}

/* Default send function - raw socket send */
int port_default_send(vfr_port_t *port, const u8 *frame, size_t len)
{
    int ret;
    if (!port || port->fd == INVALID_SOCKET) return -1;

    ret = send(port->fd, (const char *)frame, len, 0);
    if (ret < 0) {
#ifdef _WIN32
        LOG_DEBUG("send() failed on %s: %d", port->name, WSAGetLastError());
#else
        LOG_DEBUG("send() failed on %s: %d", port->name, errno);
#endif
        return -1;
    }

    return 0;
}

/* Default receive function - raw socket receive */
int port_default_recv(vfr_port_t *port, u8 *buf, size_t max_len)
{
    int ret;
    struct sockaddr_in from;
    int from_len = sizeof(from);

    if (!port || port->fd == INVALID_SOCKET) return -1;

    ret = recvfrom(port->fd, (char *)buf, max_len, 0,
                   (struct sockaddr *)&from, &from_len);
    if (ret < 0) {
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            LOG_DEBUG("recvfrom() failed on %s: %d", port->name, err);
        }
#else
        int err = errno;
        if (err != EWOULDBLOCK && err != EAGAIN) {
            LOG_DEBUG("recvfrom() failed on %s: %d", port->name, err);
        }
#endif
        return -1;
    }

    return ret;
}

/* Default poll function - select() based */
int port_default_poll(vfr_port_t *port, u32 timeout_ms)
{
    fd_set read_fds;
    struct timeval tv;
    int ret;

    if (!port || port->fd == INVALID_SOCKET) return 0;

    FD_ZERO(&read_fds);
    FD_SET(port->fd, &read_fds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select((int)(port->fd + 1), &read_fds, NULL, NULL, &tv);
    return ret;
}

/* ============================================================
 * Port Name Parsing
 * ============================================================ */

/* Parse port name into type and indices */
int port_parse_name(const char *name, int *type, int *group, int *index)
{
    const char *p = name;
    int t = PORT_TYPE_UNI;
    int g = 0, i = 0;

    if (!name) return -1;

    /* Parse type */
    if (strncmp(p, "uni", 3) == 0) {
        t = PORT_TYPE_UNI;
        p += 3;
    } else if (strncmp(p, "nni", 3) == 0) {
        t = PORT_TYPE_NNI;
        p += 3;
    } else {
        LOG_ERROR("Invalid port name: must start with 'uni' or 'nni'");
        return -1;
    }

    /* Parse group number */
    if (*p == '\0' || !isdigit((unsigned char)*p)) {
        LOG_ERROR("Invalid port name format: expected group number after type: %s", name);
        return -1;
    }
    while (*p && isdigit((unsigned char)*p)) {
        g = g * 10 + (*p - '0');
        p++;
    }

    /* Expect '/' separator */
    if (*p != '/') {
        LOG_ERROR("Invalid port name format: expected '/' after group: %s", name);
        return -1;
    }
    p++;

    /* Parse index number */
    if (*p == '\0' || !isdigit((unsigned char)*p)) {
        LOG_ERROR("Invalid port name format: expected index number after '/': %s", name);
        return -1;
    }
    while (*p && isdigit((unsigned char)*p)) {
        i = i * 10 + (*p - '0');
        p++;
    }

    if (*p != '\0') {
        LOG_ERROR("Invalid port name: trailing characters '%s'", p);
        return -1;
    }

    if (type) *type = t;
    if (group) *group = g;
    if (index) *index = i;

    return 0;
}

/* ============================================================
 * Port Initialization
 * ============================================================ */

vfr_port_t *port_create(const char *name, int type, int transport)
{
    vfr_port_t *port;

    if (!name) return NULL;

    port = calloc(1, sizeof(vfr_port_t));
    if (!port) {
        LOG_ERROR("Failed to allocate port: %s", name);
        return NULL;
    }

    strncpy(port->name, name, VFR_MAX_NAME_LEN - 1);
    port->type = type;
    port->transport = transport;
    port->dlcibit = 10;
    port->status = PORT_STATUS_DOWN;
    port->fd = INVALID_SOCKET;

    mutex_init(&port->mutex);
    port_stats_init(port);

    LOG_DEBUG("Port created: %s (type=%d, transport=%d)",
              name, type, transport);

    cgst_init(port, g_vfrs ? g_vfrs->access_rate : 0, 1000);

    return port;
}

void port_free(vfr_port_t *port)
{
    if (!port) return;

    /* Call transport-specific destructor first */
    if (port->ops && port->ops->free) {
        port->ops->free(port);
    }

    mutex_lock(&port->mutex);

    /* Close socket/file descriptor if not already closed by the transport destructor */
    if (port->fd != INVALID_SOCKET) {
        if (port->transport <= PORT_TRANS_TCP_SER ||
            port->transport == PORT_TRANS_PIPE) {
            shutdown(port->fd, SD_BOTH);
        }
        closesocket(port->fd);
        port->fd = INVALID_SOCKET;
    }

    mutex_unlock(&port->mutex);
    mutex_destroy(&port->mutex);

    /* Close capture file */
    if (port->capture) {
        pcap_writer_close(port->capture);
        free(port->capture);
        port->capture = NULL;
    }

    lapf_free(port);
    cgst_free(port);
    svc_free_port(port);
    lmi_free(port);

    if (port->dlci_array) {
        free(port->dlci_array);
        port->dlci_array = NULL;
    }

    LOG_INFO("Port freed: %s", port->name);
    free(port);
}

/* ============================================================
 * File I/O (Windows-compatible)
 * ============================================================ */

#ifdef _WIN32

file_fd_t file_open(const char *path, int for_write)
{
    DWORD access = GENERIC_READ | (for_write ? GENERIC_WRITE : 0);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD create = for_write ? CREATE_ALWAYS : OPEN_EXISTING;
    return CreateFileA(path, access, share, NULL, create,
                       FILE_ATTRIBUTE_NORMAL, NULL);
}

void file_close(file_fd_t fd)
{
    if (fd && fd != INVALID_HANDLE_VALUE) {
        CloseHandle(fd);
    }
}

size_t file_read(file_fd_t fd, void *buf, size_t len)
{
    DWORD bytes_read = 0;
    if (ReadFile(fd, buf, (DWORD)len, &bytes_read, NULL)) {
        return bytes_read;
    }
    return 0;
}

size_t file_write(file_fd_t fd, const void *buf, size_t len)
{
    size_t total_written = 0;
    while (total_written < len) {
        DWORD written = 0;
        if (!WriteFile(fd, (const char *)buf + total_written,
                       (DWORD)(len - total_written), &written, NULL) || written == 0) {
            break;
        }
        total_written += written;
    }
    return total_written;
}

#else

file_fd_t file_open(const char *path, int for_write)
{
    int flags = for_write ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
    return open(path, flags, 0644);
}

void file_close(file_fd_t fd)
{
    if (fd >= 0) close(fd);
}

size_t file_read(file_fd_t fd, void *buf, size_t len)
{
    return read(fd, buf, len);
}

size_t file_write(file_fd_t fd, const void *buf, size_t len)
{
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, (const char *)buf + total_written, len - total_written);
        if (written <= 0) {
            break;
        }
        total_written += (size_t)written;
    }
    return total_written;
}

#endif

/* ============================================================
 * Port-local Unified DLCI Routing Table (Option A / future Option C)
 * ============================================================ */

static int compare_dlci_entries(const void *a, const void *b) {
    u32 dlci_a = (*(vfr_dlci_entry_t **)a)->dlci_in;
    u32 dlci_b = (*(vfr_dlci_entry_t **)b)->dlci_in;
    return (dlci_a > dlci_b) - (dlci_a < dlci_b);
}

vfr_dlci_entry_t *port_lookup_dlci(vfr_port_t *port, u32 dlci)
{
    if (!port) return NULL;
    
    if (port->dlcibit == 10) {
        if (dlci < 1024) {
            return port->dlci_lut[dlci];
        }
    } else {
        mutex_lock(&port->mutex);
        if (port->dlci_count > 0 && port->dlci_array) {
            vfr_dlci_entry_t key_struct;
            key_struct.dlci_in = dlci;
            vfr_dlci_entry_t *key_ptr = &key_struct;
            vfr_dlci_entry_t **found = bsearch(&key_ptr, port->dlci_array, port->dlci_count,
                                        sizeof(vfr_dlci_entry_t *), compare_dlci_entries);
            vfr_dlci_entry_t *result = found ? *found : NULL;
            mutex_unlock(&port->mutex);
            return result;
        }
        mutex_unlock(&port->mutex);
    }
    return NULL;
}

int port_add_dlci_entry(vfr_port_t *port, vfr_dlci_entry_t *entry)
{
    if (!port || !entry) return -1;

    mutex_lock(&port->mutex);

    if (port->dlcibit == 10) {
        if (entry->dlci_in < 1024) {
            port->dlci_lut[entry->dlci_in] = entry;
            mutex_unlock(&port->mutex);
            return 0;
        }
    } else {
        /* Ensure there's no duplicate first */
        for (int i = 0; i < port->dlci_count; i++) {
            if (port->dlci_array[i]->dlci_in == entry->dlci_in) {
                /* Already exists, overwrite it */
                port->dlci_array[i] = entry;
                mutex_unlock(&port->mutex);
                return 0;
            }
        }

        /* Expand capacity if needed */
        if (port->dlci_count >= port->dlci_capacity) {
            int new_capacity = port->dlci_capacity == 0 ? 16 : port->dlci_capacity * 2;
            vfr_dlci_entry_t **new_array = realloc(port->dlci_array, new_capacity * sizeof(vfr_dlci_entry_t *));
            if (!new_array) {
                LOG_ERROR("Failed to allocate memory for port DLCI array: %s", port->name);
                mutex_unlock(&port->mutex);
                return -1;
            }
            port->dlci_array = new_array;
            port->dlci_capacity = new_capacity;
        }

        /* Insert and keep sorted */
        int insert_pos = port->dlci_count;
        while (insert_pos > 0 && port->dlci_array[insert_pos - 1]->dlci_in > entry->dlci_in) {
            port->dlci_array[insert_pos] = port->dlci_array[insert_pos - 1];
            insert_pos--;
        }
        port->dlci_array[insert_pos] = entry;
        port->dlci_count++;
        mutex_unlock(&port->mutex);
        return 0;
    }

    mutex_unlock(&port->mutex);
    return -1;
}

int port_del_dlci_entry(vfr_port_t *port, u32 dlci)
{
    if (!port) return -1;

    mutex_lock(&port->mutex);

    if (port->dlcibit == 10) {
        if (dlci < 1024) {
            port->dlci_lut[dlci] = NULL;
            mutex_unlock(&port->mutex);
            return 0;
        }
    } else {
        int found_idx = -1;
        for (int i = 0; i < port->dlci_count; i++) {
            if (port->dlci_array[i]->dlci_in == dlci) {
                found_idx = i;
                break;
            }
        }

        if (found_idx != -1) {
            /* Shift remaining elements left */
            for (int i = found_idx; i < port->dlci_count - 1; i++) {
                port->dlci_array[i] = port->dlci_array[i + 1];
            }
            port->dlci_count--;
            mutex_unlock(&port->mutex);
            return 0;
        }
    }

    mutex_unlock(&port->mutex);
    return -1;
}

/* L2TPv3 draft transport placeholder ops */
static const port_ops_t l2tpv3_ops = {
    .send = port_default_send,
    .recv = port_default_recv,
    .poll = port_default_poll,
    .free = NULL
};

vfr_port_t *port_l2tpv3_create(const char *name, int type, int transport,
                               const char *lhost, const char *rhost, u32 vcid)
{
    vfr_port_t *port;

    if (!name || !rhost) return NULL;

    port = port_create(name, type, transport);
    if (!port) return NULL;

    port->ops = &l2tpv3_ops;
    port->status = PORT_STATUS_DOWN;

    LOG_INFO("Port %s configured for %s (draft/roadmap: local=%s, remote=%s, vcid=%u)",
             name, transport == PORT_TRANS_L2TPV3_FR ? "l2tpv3-fr" : "l2tpv3-hdlc",
             lhost ? lhost : "0.0.0.0", rhost, vcid);

    return port;
}