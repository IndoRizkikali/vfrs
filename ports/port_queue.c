#include "vfr.h"

void spsc_queue_init(vfr_spsc_queue_t *q)
{
    if (!q) return;
    q->head = 0;
    q->tail = 0;
}

int spsc_queue_push(vfr_spsc_queue_t *q, const u8 *data, size_t len, u16 port_idx)
{
    if (!q || !data || len > 2048) return -1;

    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    /* Check if queue is full */
    if (((current_head + 1) % SPSC_QUEUE_SIZE) == current_tail) {
        return -1; /* Full */
    }

    /* Copy frame metadata and payload */
    vfr_ctrl_frame_t *slot = &q->ring[current_head];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->src_port_idx = port_idx;

    /* Write barrier to ensure data is written before head is advanced */
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    q->head = (current_head + 1) % SPSC_QUEUE_SIZE;
    return 0;
}

int spsc_queue_pop(vfr_spsc_queue_t *q, vfr_ctrl_frame_t *out_frame)
{
    if (!q || !out_frame) return -1;

    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    /* Check if queue is empty */
    if (current_tail == current_head) {
        return -1; /* Empty */
    }

    /* Read barrier to ensure read happens after tail is validated */
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    *out_frame = q->ring[current_tail];

    /* Write barrier to ensure data is read before tail is advanced */
#ifdef _WIN32
    MemoryBarrier();
#else
    __sync_synchronize();
#endif

    q->tail = (current_tail + 1) % SPSC_QUEUE_SIZE;
    return 0;
}
