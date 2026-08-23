/*
 * pcap.h - VFRS PCAP Packet Capture Interface
 * Virtual Frame Relay Switch
 */

#ifndef VFR_PCAP_H
#define VFR_PCAP_H

#include "platform.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * PCAP Support (DLT_FRELAY = 107)
 * ============================================================ */

#define PCAP_MAGIC           0xa1b2c3d4
#define PCAP_SWAPPED_MAGIC   0xd4c3b2a1
#define PCAP_VERSION_MAJOR    2
#define PCAP_VERSION_MINOR   4
#define PCAP_LINKTYPE_FRELAY 107

#pragma pack(push, 1)

typedef struct pcap_hdr_s {
    u32 magic;
    u16 version_major;
    u16 version_minor;
    u32 thiszone;
    u32 sigfigs;
    u32 snaplen;
    u32 network;
} pcap_hdr_t;

typedef struct pcaprec_hdr_s {
    u32 ts_sec;
    u32 ts_usec;
    u32 incl_len;
    u32 orig_len;
} pcaprec_hdr_t;

#pragma pack(pop)

/* PCAP writer context */
typedef struct pcap_writer_s {
    file_fd_t       fd;
    mutex_t         mutex;
    char            filename[VFR_MAX_PATH];
    u32             pkt_count;
    u32             snaplen;
} pcap_writer_t;

VFR_API int pcap_writer_init(pcap_writer_t *pw, const char *filename, u32 snaplen);
VFR_API int pcap_writer_write(pcap_writer_t *pw, const u8 *data, size_t len);
VFR_API int pcap_writer_close(pcap_writer_t *pw);

#ifdef __cplusplus
}
#endif

#endif /* VFR_PCAP_H */
