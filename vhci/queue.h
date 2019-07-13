#ifndef BCE_VHCI_QUEUE_H
#define BCE_VHCI_QUEUE_H

#include <linux/types.h>

#define VHCI_EVENT_QUEUE_EL_COUNT 256
#define VHCI_EVENT_PENDING_COUNT 32

struct bce_vhci;

struct bce_vhci_message {
    u16 cmd;
    u16 status;
    u32 param1;
    u64 param2;
};

struct bce_vhci_event_queue {
    struct bce_queue_sq *sq;
    struct bce_vhci_message *data;
    dma_addr_t dma_addr;
};

int bce_vhci_event_queue_create(struct bce_vhci *vhci, struct bce_vhci_event_queue *ret, const char *name);
void bce_vhci_event_queue_destroy(struct bce_vhci *vhci, struct bce_vhci_event_queue *q);

#endif //BCE_VHCI_QUEUE_H
