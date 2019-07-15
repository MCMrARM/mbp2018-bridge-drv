#ifndef BCEDRIVER_TRANSFER_H
#define BCEDRIVER_TRANSFER_H

#include <linux/usb.h>
#include "queue.h"
#include "command.h"
#include "../queue.h"

struct bce_vhci_list_message {
    struct list_head list;
    struct bce_vhci_message msg;
};
struct bce_vhci_control_transfer_queue {
    struct bce_vhci *vhci;
    struct usb_device *udev;
    bce_vhci_device_t dev_addr;
    struct bce_queue_cq *cq;
    struct bce_queue_sq *sq_in;
    struct bce_queue_sq *sq_out;
    struct list_head evq;
};
struct bce_vhci_control_urb {
    struct urb *urb;
    struct bce_vhci_control_transfer_queue *q;
    enum dma_data_direction dir;
    bool is_setup;
    bool is_waiting_for_transfer_start;
    bool is_waiting_for_completion;
    bool is_complete;
    int received_status;
};

void bce_vhci_create_control_transfer_queue(struct bce_vhci *vhci, struct usb_device *udev,
        struct bce_vhci_control_transfer_queue *q, bce_vhci_device_t dev_addr);
void bce_vhci_destroy_control_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_control_transfer_queue *q);
void bce_vhci_control_queue_event(struct bce_vhci_control_transfer_queue *q, struct bce_vhci_message *msg);

void bce_vhci_control_urb_create(struct bce_vhci_control_transfer_queue *q, struct urb *urb);
int bce_vhci_control_urb_update(struct bce_vhci_control_urb *urb, struct bce_vhci_message *msg);
void bce_vhci_control_urb_transfer_completion(struct bce_vhci_control_urb *urb, struct bce_sq_completion_data *c);

#endif //BCEDRIVER_TRANSFER_H
