#include "vfr.h"

void spsc_queue_init(vfr_spsc_queue_t *q)
{
    if (!q) return;
    q->head = 0;
    q->tail = 0;
    mutex_init(&q->lock);
}

void spsc_queue_destroy(vfr_spsc_queue_t *q)
{
    if (!q) return;
    mutex_destroy(&q->lock);
}

int spsc_queue_push(vfr_spsc_queue_t *q, const u8 *data, size_t len, u16 port_idx)
{
    if (!q || !data || len > 2048) return -1;

    mutex_lock(&q->lock);
    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    /* Check if queue is full */
    if (((current_head + 1) % SPSC_QUEUE_SIZE) == current_tail) {
        mutex_unlock(&q->lock);
        return -1; /* Full */
    }

    /* Copy frame metadata and payload */
    vfr_ctrl_frame_t *slot = &q->ring[current_head];
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->src_port_idx = port_idx;

    q->head = (current_head + 1) % SPSC_QUEUE_SIZE;
    mutex_unlock(&q->lock);
    return 0;
}

int spsc_queue_pop(vfr_spsc_queue_t *q, vfr_ctrl_frame_t *out_frame)
{
    if (!q || !out_frame) return -1;

    mutex_lock(&q->lock);
    uint32_t current_head = q->head;
    uint32_t current_tail = q->tail;

    /* Check if queue is empty */
    if (current_tail == current_head) {
        mutex_unlock(&q->lock);
        return -1; /* Empty */
    }

    *out_frame = q->ring[current_tail];
    q->tail = (current_tail + 1) % SPSC_QUEUE_SIZE;
    mutex_unlock(&q->lock);
    return 0;
}
