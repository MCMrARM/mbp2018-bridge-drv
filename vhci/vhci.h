#ifndef BCE_VHCI_H
#define BCE_VHCI_H

#include "queue.h"

struct usb_hcd;
struct bce_queue_cq;

struct bce_vhci {
    struct bce_device *dev;
    dev_t vdevt;
    struct device *vdev;
    struct usb_hcd *hcd;
    struct bce_vhci_message_queue msg_commands;
    struct bce_vhci_message_queue msg_system;
    struct bce_vhci_message_queue msg_isochronous;
    struct bce_vhci_message_queue msg_interrupt;
    struct bce_vhci_message_queue msg_asynchronous;
    struct bce_vhci_command_queue cq;
    struct bce_queue_cq *ev_cq;
    struct bce_vhci_event_queue ev_commands;
    struct bce_vhci_event_queue ev_system;
    struct bce_vhci_event_queue ev_isochronous;
    struct bce_vhci_event_queue ev_interrupt;
    struct bce_vhci_event_queue ev_asynchronous;
    u16 port_mask;
    u8 port_count;
    u16 port_power_mask;
};

int __init bce_vhci_module_init(void);
void __exit bce_vhci_module_exit(void);

int bce_vhci_create(struct bce_device *dev, struct bce_vhci *vhci);
void bce_vhci_destroy(struct bce_vhci *vhci);

#endif //BCE_VHCI_H
