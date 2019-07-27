#ifndef AAUDIO_PROTOCOL_BCE_H
#define AAUDIO_PROTOCOL_BCE_H

#include "protocol.h"
#include "../queue.h"

#define AAUDIO_BCE_QUEUE_ELEMENT_SIZE 0x1000
#define AAUDIO_BCE_QUEUE_ELEMENT_COUNT 20

struct aaudio_device;

struct aaudio_bce_queue {
    struct bce_queue_cq *cq;
    struct bce_queue_sq *sq;
    void *data;
    dma_addr_t dma_addr;
    size_t data_head, data_tail;
    int next_el_index;
    struct spinlock spinlock;
    size_t el_size, el_count;
};
struct aaudio_bce {
    struct bce_queue_cq *cq;
    struct aaudio_bce_queue qin;
    struct aaudio_bce_queue qout;
};

int aaudio_bce_init(struct aaudio_device *dev);
int aaudio_send_prepare(struct aaudio_bce_queue *q, struct aaudio_msg *msg, unsigned long *timeout);
void aaudio_send(struct aaudio_bce_queue *q, struct aaudio_msg *msg);

#endif //AAUDIO_PROTOCOL_BCE_H
