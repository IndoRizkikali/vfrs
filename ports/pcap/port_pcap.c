/*
 * port_pcap.c - PCAP Writer
 * VFRS - Virtual Frame Relay Switch
 * LINKTYPE_FRELAY (107)
 */

#include "vfr.h"

#include <string.h>

/* PCAP magic and version */
#define PCAP_MAGIC           0xa1b2c3d4
#define PCAP_MAGIC_SWAPPED   0xd4c3b2a1
#define PCAP_VERSION_MAJOR   2
#define PCAP_VERSION_MINOR   4

/* ============================================================
 * PCAP Writer
 * ============================================================ */

int pcap_writer_init(pcap_writer_t *pw, const char *filename, u32 snaplen)
{
    pcap_hdr_t hdr;
#ifdef _WIN32
    wchar_t wpath[VFR_MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, wpath, VFR_MAX_PATH);
    pw->fd = CreateFileW(wpath, GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pw->fd == INVALID_HANDLE_VALUE) {
        LOG_ERROR("Failed to create PCAP file '%s': %lu", filename, GetLastError());
        return -1;
    }
#else
    pw->fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pw->fd < 0) {
        LOG_ERROR("Failed to create PCAP file '%s': %s", filename, strerror(errno));
        return -1;
    }
#endif

    /* Initialize mutex */
    mutex_init(&pw->mutex);

    /* Store filename and snaplen */
    strncpy(pw->filename, filename, sizeof(pw->filename) - 1);
    pw->snaplen = snaplen ? snaplen : 65535;
    pw->pkt_count = 0;

    /* Write global header */
    hdr.magic = PCAP_MAGIC;
    hdr.version_major = PCAP_VERSION_MAJOR;
    hdr.version_minor = PCAP_VERSION_MINOR;
    hdr.thiszone = 0;
    hdr.sigfigs = 0;
    hdr.snaplen = pw->snaplen;
    hdr.network = PCAP_LINKTYPE_FRELAY;  /* Frame Relay */

    if (file_write(pw->fd, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        LOG_ERROR("Failed to write PCAP header");
        file_close(pw->fd);
        mutex_destroy(&pw->mutex);
        return -1;
    }

    LOG_INFO("PCAP writer initialized: %s (snaplen=%u)", filename, pw->snaplen);
    return 0;
}

int pcap_writer_write(pcap_writer_t *pw, const u8 *data, size_t len)
{
    pcaprec_hdr_t rec_hdr;
    struct timespec ts;
    u8 write_buf[8192 + sizeof(pcaprec_hdr_t)];

    if (!pw || pw->fd == INVALID_HANDLE_VALUE || len > 8192) return -1;

    /* Get current time */
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Build packet record header */
    rec_hdr.ts_sec = (u32)ts.tv_sec;
    rec_hdr.ts_usec = (u32)(ts.tv_nsec / 1000);
    rec_hdr.incl_len = (u32)len;
    rec_hdr.orig_len = (u32)len;

    size_t total_write = sizeof(rec_hdr) + len;
    memcpy(write_buf, &rec_hdr, sizeof(rec_hdr));
    memcpy(write_buf + sizeof(rec_hdr), data, len);

    mutex_lock(&pw->mutex);

    /* Write record header and packet payload in a single write call */
    if ((size_t)file_write(pw->fd, write_buf, total_write) != total_write) {
        mutex_unlock(&pw->mutex);
        return -1;
    }

    pw->pkt_count++;

    /* Periodically flush file buffers to disk every 32 packets */
    if ((pw->pkt_count & 31) == 0) {
#ifdef _WIN32
        FlushFileBuffers(pw->fd);
#endif
    }

    mutex_unlock(&pw->mutex);

    return 0;
}

int pcap_writer_close(pcap_writer_t *pw)
{
    if (!pw) return 0;

    mutex_lock(&pw->mutex);
    if (pw->fd != INVALID_HANDLE_VALUE) {
        LOG_INFO("PCAP writer closed: %s (%u packets)",
                 pw->filename, pw->pkt_count);
        file_close(pw->fd);
        pw->fd = INVALID_HANDLE_VALUE;
    }
    mutex_unlock(&pw->mutex);
    mutex_destroy(&pw->mutex);

    return 0;
}

/* ============================================================
 * PCAP File Reader (for debugging)
 * ============================================================ */

/* Byte swap for big-endian/little-endian conversion */
static inline u32 pcap_bswap32(u32 x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0xFF000000U) >> 24);
}

typedef struct {
    FILE        *fp;
    pcap_hdr_t  hdr;
    int         swapped;
} pcap_reader_t;

pcap_reader_t *pcap_reader_open(const char *filename)
{
    pcap_reader_t *reader;

    reader = calloc(1, sizeof(pcap_reader_t));
    if (!reader) return NULL;

    reader->fp = fopen(filename, "rb");
    if (!reader->fp) {
        free(reader);
        return NULL;
    }

    /* Read global header */
    if (fread(&reader->hdr, sizeof(reader->hdr), 1, reader->fp) != 1) {
        fclose(reader->fp);
        free(reader);
        return NULL;
    }

    /* Check magic number */
    if (reader->hdr.magic == PCAP_MAGIC_SWAPPED) {
        reader->swapped = 1;
    } else if (reader->hdr.magic != PCAP_MAGIC) {
        fclose(reader->fp);
        free(reader);
        return NULL;
    }

    return reader;
}

void pcap_reader_close(pcap_reader_t *reader)
{
    if (!reader) return;
    if (reader->fp) fclose(reader->fp);
    free(reader);
}

int pcap_reader_read(pcap_reader_t *reader, u8 *buf, size_t max_len,
                     u32 *ts_sec, u32 *ts_usec)
{
    pcaprec_hdr_t rec_hdr;
    size_t len;

    if (!reader || !reader->fp) return -1;

    if (fread(&rec_hdr, sizeof(rec_hdr), 1, reader->fp) != 1) {
        return -1;
    }

    /* Byte swap if needed */
    if (reader->swapped) {
        rec_hdr.ts_sec = pcap_bswap32(rec_hdr.ts_sec);
        rec_hdr.ts_usec = pcap_bswap32(rec_hdr.ts_usec);
        rec_hdr.incl_len = pcap_bswap32(rec_hdr.incl_len);
        rec_hdr.orig_len = pcap_bswap32(rec_hdr.orig_len);
    }

    if (ts_sec) *ts_sec = rec_hdr.ts_sec;
    if (ts_usec) *ts_usec = rec_hdr.ts_usec;

    len = rec_hdr.incl_len;
    if (len > max_len) len = max_len;

    if (fread(buf, 1, len, reader->fp) != len) {
        return -1;
    }

    /* Skip rest if truncated */
    if (rec_hdr.incl_len > len) {
        fseek(reader->fp, rec_hdr.incl_len - len, SEEK_CUR);
    }

    return (int)len;
}