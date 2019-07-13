#ifndef BCE_VHCI_H
#define BCE_VHCI_H

#include <linux/types.h>
#include "../queue.h"

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

struct bce_vhci {
    struct bce_device *dev;
    struct bce_queue_cq *ev_cq;
    struct bce_vhci_event_queue ev_commands;
    struct bce_vhci_event_queue ev_system;
    struct bce_vhci_event_queue ev_isochronous;
    struct bce_vhci_event_queue ev_interrupt;
    struct bce_vhci_event_queue ev_asynchronous;
};

int bce_vhci_create(struct bce_device *dev, struct bce_vhci *vhci);
void bce_vhci_destroy(struct bce_vhci *vhci);

#endif //BCE_VHCI_H
