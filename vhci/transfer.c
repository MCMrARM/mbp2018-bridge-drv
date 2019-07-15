#include "transfer.h"
#include "../queue.h"
#include "vhci.h"
#include <linux/usb/hcd.h>

#define BCE_VHCI_MSG_TRANSFER_REQUEST 0x1000
#define BCE_VHCI_MSG_TRANSFER_STATUS 0x1005

static void bce_vhci_control_transfer_queue_completion(struct bce_queue_sq *sq);

void bce_vhci_create_control_transfer_queue(struct bce_vhci *vhci, struct usb_device *udev,
        struct bce_vhci_control_transfer_queue *q, bce_vhci_device_t dev_addr)
{
    char name[0x21];
    INIT_LIST_HEAD(&q->evq);
    q->vhci = vhci;
    q->udev = udev;
    q->dev_addr = dev_addr;
    q->cq = bce_create_cq(vhci->dev, 0x100);
    snprintf(name, sizeof(name), "VHC1-%i-%02x", dev_addr, 0x80);
    q->sq_in = bce_create_sq(vhci->dev, q->cq, name, 0x100, DMA_FROM_DEVICE, bce_vhci_control_transfer_queue_completion, q);
    snprintf(name, sizeof(name), "VHC1-%i-%02x", dev_addr, 0);
    q->sq_out = bce_create_sq(vhci->dev, q->cq, name, 0x100, DMA_TO_DEVICE, bce_vhci_control_transfer_queue_completion, q);
}

void bce_vhci_destroy_control_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_control_transfer_queue *q)
{
    bce_destroy_sq(vhci->dev, q->sq_in);
    bce_destroy_sq(vhci->dev, q->sq_out);
    bce_destroy_cq(vhci->dev, q->cq);
}

void bce_vhci_control_queue_event(struct bce_vhci_control_transfer_queue *q, struct bce_vhci_message *msg)
{
    struct bce_vhci_list_message *lm;
    struct bce_vhci_control_urb *curb;
    struct urb *urb;
    if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST &&
        (!list_empty(&q->evq) || list_empty(&q->udev->ep0.urb_list))) {
add_to_queue:
        lm = kmalloc(sizeof(struct bce_vhci_list_message), GFP_KERNEL);
        INIT_LIST_HEAD(&lm->list);
        lm->msg = *msg;
        list_add_tail(&lm->list, &q->evq);
        return;
    }
    if (list_empty(&q->udev->ep0.urb_list)) {
        pr_err("bce-vhci: Unexpected control queue event\n");
        return;
    }
    urb = list_first_entry(&q->udev->ep0.urb_list, struct urb, urb_list);
    curb = urb->hcpriv;
    if (bce_vhci_control_urb_update(curb, msg)) {
        goto add_to_queue;
    }
    if (curb->is_complete) {
        usb_hcd_unlink_urb_from_ep(q->vhci->hcd, urb);
        usb_hcd_giveback_urb(q->vhci->hcd, urb, 0);
    }
}

void bce_vhci_control_queue_try_deliver_events(struct bce_vhci_control_transfer_queue *q, struct urb *urb)
{
    struct bce_vhci_list_message *lm;
    while (!list_empty(&q->evq)) {
        lm = list_first_entry(&q->evq, struct bce_vhci_list_message, list);
        bce_vhci_control_urb_update(urb->hcpriv, &lm->msg);
        list_del(&lm->list);
        kfree(lm);
    }
}

static void bce_vhci_control_transfer_queue_completion(struct bce_queue_sq *sq)
{
    struct bce_vhci_control_urb *curb;
    struct bce_sq_completion_data *c;
    struct urb *urb;
    struct bce_vhci_control_transfer_queue *q = sq->userdata;
    while ((c = bce_next_completion(sq))) {
        if (list_empty(&q->udev->ep0.urb_list)) {
            pr_err("bce-vhci: Got a completion while no requests are pending\n");
            continue;
        }
        pr_info("bce-vhci: Got a control queue completion\n");
        urb = list_first_entry(&q->udev->ep0.urb_list, struct urb, urb_list);
        bce_vhci_control_urb_transfer_completion(urb->hcpriv, c);
        bce_notify_submission_complete(sq);

        curb = urb->hcpriv;
        if (curb->is_complete) {
            usb_hcd_unlink_urb_from_ep(q->vhci->hcd, urb);
            usb_hcd_giveback_urb(q->vhci->hcd, urb, 0);
        }
    }
}

void bce_vhci_control_urb_create(struct bce_vhci_control_transfer_queue *q, struct urb *urb)
{
    struct bce_vhci_control_urb *vurb;
    vurb = kzalloc(sizeof(struct bce_vhci_control_urb), GFP_KERNEL);
    urb->hcpriv = vurb;

    vurb->q = q;
    vurb->urb = urb;
    vurb->dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    vurb->is_setup = true;

    bce_vhci_control_queue_try_deliver_events(q, urb);
}

static int bce_vhci_urb_transfer(struct bce_vhci_control_transfer_queue *q, dma_addr_t addr, size_t len,
        enum dma_data_direction dir)
{
    int status;
    unsigned long timeout = 0;
    struct bce_qe_submission *s;
    struct bce_queue_sq *sq = dir == DMA_TO_DEVICE ? q->sq_out : q->sq_in;
    if ((status = bce_reserve_submission(sq, &timeout))) /* Not sure what's the right timeout */
        return status;
    s = bce_next_submission(sq);
    bce_set_submission_single(s, addr, len);
    bce_submit_to_device(sq);
    return 0;
}

static void bce_vhci_control_urb_check_status(struct bce_vhci_control_urb *urb)
{
    if (urb->received_status == 0)
        return;
    if (urb->is_setup || urb->is_waiting_for_completion)
        return;
    urb->is_waiting_for_transfer_start = false;
    urb->is_complete = true;
}

int bce_vhci_control_urb_update(struct bce_vhci_control_urb *urb, struct bce_vhci_message *msg)
{
    if (msg->cmd == BCE_VHCI_MSG_TRANSFER_STATUS) {
        urb->received_status = msg->status;
        bce_vhci_control_urb_check_status(urb);
        return;
    }

    if (urb->is_setup) {
        if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST) {
            if (bce_vhci_urb_transfer(urb->q, urb->urb->setup_dma, sizeof(struct usb_ctrlrequest), DMA_TO_DEVICE)) {
                pr_err("bce-vhci: Failed to start URB setup transfer\n");
                return 0; /* TODO: fail the URB? */
            }
            pr_info("bce-vhci: Sent setup %x\n", urb->urb->setup_dma);
            return 0;
        }
    } else if (urb->is_waiting_for_transfer_start) {
        if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST) {
            if (msg->param2 != urb->urb->transfer_buffer_length)
                pr_err("bce-vhci: Device requested wrong transfer buffer length\n");
            if (bce_vhci_urb_transfer(urb->q, urb->urb->transfer_dma,
                    min(urb->urb->transfer_buffer_length, (u32) msg->param2), DMA_TO_DEVICE)) {
                pr_err("bce-vhci: Failed to start URB data transfer\n");
                return; /* TODO: fail the URB? */
            }
            urb->is_waiting_for_transfer_start = false;
            urb->is_waiting_for_completion = true;
            bce_vhci_control_urb_check_status(urb);
            return 0;
        }
    }
    pr_err("bce-vhci: Control URB unexpected message\n");
    return -EAGAIN;
}

void bce_vhci_control_urb_transfer_completion(struct bce_vhci_control_urb *urb, struct bce_sq_completion_data *c)
{
    if (urb->is_setup) {
        if (c->data_size != sizeof(struct usb_ctrlrequest))
            pr_err("bce-vhci: transfer complete data size mistmatch for usb_ctrlrequest (%llx instead of %lx)\n", c->data_size, sizeof(struct usb_ctrlrequest));
        urb->is_setup = false;
        if (urb->dir == DMA_TO_DEVICE) {
            if (urb->urb->transfer_buffer_length > 0) {
                urb->is_waiting_for_transfer_start = true;
            } else {
                urb->is_waiting_for_completion = true;
            }
        } else {
            pr_info("bce-vhci: DMA from device %x %x\n", urb->urb->transfer_dma, urb->urb->transfer_buffer_length);
            struct bce_vhci_message msg;
            msg.cmd = 0x1000;
            msg.status = 0;
            msg.param1 = ((urb->urb->ep->desc.bEndpointAddress & 0x8Fu) << 8) | urb->q->dev_addr;
            msg.param2 = urb->urb->transfer_buffer_length;
            if (bce_vhci_message_queue_send(&urb->q->vhci->msg_asynchronous, &msg, NULL)) {
                pr_err("bce-vhci: Failed to send URB data transfer start message\n");
                return; /* TODO: fail the URB? */
            }
            if (bce_vhci_urb_transfer(urb->q, urb->urb->transfer_dma,
                    urb->urb->transfer_buffer_length, DMA_FROM_DEVICE)) {
                pr_err("bce-vhci: Failed to start URB data transfer\n");
                return; /* TODO: fail the URB? */
            }
            urb->is_waiting_for_completion = true;
            pr_info("bce-vhci: Transfer started\n");
        }
    } else if (urb->is_waiting_for_completion) {
        urb->urb->actual_length = (u32) c->data_size;
        urb->is_waiting_for_completion = false;
        bce_vhci_control_urb_check_status(urb);
    } else {
        pr_err("bce-vhci: Control URB unexpected completion\n");
    }
}