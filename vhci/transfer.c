#include "transfer.h"
#include "../queue.h"
#include "vhci.h"
#include <linux/usb/hcd.h>

#define BCE_VHCI_MSG_TRANSFER_REQUEST 0x1000
#define BCE_VHCI_MSG_CONTROL_TRANSFER_STATUS 0x1005

static void bce_vhci_control_transfer_queue_completion(struct bce_queue_sq *sq);

static int bce_vhci_urb_update(struct bce_vhci_urb *urb, struct bce_vhci_message *msg);
static int bce_vhci_urb_transfer_completion(struct bce_vhci_urb *urb, struct bce_sq_completion_data *c);

void bce_vhci_create_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_transfer_queue *q,
        struct usb_host_endpoint *endp, bce_vhci_device_t dev_addr, enum dma_data_direction dir)
{
    char name[0x21];
    INIT_LIST_HEAD(&q->evq);
    INIT_LIST_HEAD(&q->giveback_urb_list);
    spin_lock_init(&q->urb_lock);
    q->vhci = vhci;
    q->endp = endp;
    q->dev_addr = dev_addr;
    q->cq = bce_create_cq(vhci->dev, 0x100);
    if (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) {
        snprintf(name, sizeof(name), "VHC1-%i-%02x", dev_addr, 0x80 | usb_endpoint_num(&endp->desc));
        q->sq_in = bce_create_sq(vhci->dev, q->cq, name, 0x100, DMA_FROM_DEVICE,
                bce_vhci_control_transfer_queue_completion, q);
    }
    if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL) {
        snprintf(name, sizeof(name), "VHC1-%i-%02x", dev_addr, usb_endpoint_num(&endp->desc));
        q->sq_out = bce_create_sq(vhci->dev, q->cq, name, 0x100, DMA_TO_DEVICE,
                                  bce_vhci_control_transfer_queue_completion, q);
    }
}

void bce_vhci_destroy_transfer_queue(struct bce_vhci *vhci, struct bce_vhci_transfer_queue *q)
{
    if (q->sq_in)
        bce_destroy_sq(vhci->dev, q->sq_in);
    if (q->sq_out)
        bce_destroy_sq(vhci->dev, q->sq_out);
    bce_destroy_cq(vhci->dev, q->cq);
}

static void bce_vhci_transfer_queue_defer_event(struct bce_vhci_transfer_queue *q, struct bce_vhci_message *msg)
{
    struct bce_vhci_list_message *lm;
    lm = kmalloc(sizeof(struct bce_vhci_list_message), GFP_KERNEL);
    INIT_LIST_HEAD(&lm->list);
    lm->msg = *msg;
    list_add_tail(&lm->list, &q->evq);
}

static void bce_vhci_transfer_queue_giveback(struct bce_vhci_transfer_queue *q)
{
    struct urb *urb;
    spin_lock(&q->urb_lock);
    while (!list_empty(&q->giveback_urb_list)) {
        urb = list_first_entry(&q->giveback_urb_list, struct urb, urb_list);
        list_del(&urb->urb_list);

        spin_unlock(&q->urb_lock);
        usb_hcd_giveback_urb(q->vhci->hcd, urb, urb->status);
        spin_lock(&q->urb_lock);
    }
    spin_unlock(&q->urb_lock);
}

void bce_vhci_transfer_queue_deliver_pending(struct bce_vhci_transfer_queue *q)
{
    struct urb *urb;
    struct bce_vhci_list_message *lm;

    while (!list_empty(&q->endp->urb_list) && !list_empty(&q->evq)) {
        urb = list_first_entry(&q->endp->urb_list, struct urb, urb_list);

        lm = list_first_entry(&q->evq, struct bce_vhci_list_message, list);
        if (bce_vhci_urb_update(urb->hcpriv, &lm->msg) == -EAGAIN)
            break;
        list_del(&lm->list);
        kfree(lm);
    }
}

void bce_vhci_transfer_queue_event(struct bce_vhci_transfer_queue *q, struct bce_vhci_message *msg)
{
    struct bce_vhci_urb *turb;
    struct urb *urb;
    spin_lock(&q->urb_lock);
    bce_vhci_transfer_queue_deliver_pending(q);

    if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST &&
        (!list_empty(&q->evq) || list_empty(&q->endp->urb_list))) {
        bce_vhci_transfer_queue_defer_event(q, msg);
        goto complete;
    }
    if (list_empty(&q->endp->urb_list)) {
        pr_err("bce-vhci: Unexpected control queue event\n");
        goto complete;
    }
    urb = list_first_entry(&q->endp->urb_list, struct urb, urb_list);
    turb = urb->hcpriv;
    if (bce_vhci_urb_update(turb, msg) == -EAGAIN)
        bce_vhci_transfer_queue_defer_event(q, msg);

complete:
    spin_unlock(&q->urb_lock);
    bce_vhci_transfer_queue_giveback(q);
}

static void bce_vhci_control_transfer_queue_completion(struct bce_queue_sq *sq)
{
    struct bce_sq_completion_data *c;
    struct urb *urb;
    struct bce_vhci_transfer_queue *q = sq->userdata;
    spin_lock(&q->urb_lock);
    while ((c = bce_next_completion(sq))) {
        if (list_empty(&q->endp->urb_list)) {
            pr_err("bce-vhci: Got a completion while no requests are pending\n");
            continue;
        }
        pr_info("bce-vhci:  => got a transfer queue completion\n");
        urb = list_first_entry(&q->endp->urb_list, struct urb, urb_list);
        bce_vhci_urb_transfer_completion(urb->hcpriv, c);
        bce_notify_submission_complete(sq);
    }
    bce_vhci_transfer_queue_deliver_pending(q);
    spin_unlock(&q->urb_lock);
    bce_vhci_transfer_queue_giveback(q);
}


static int bce_vhci_urb_data_start(struct bce_vhci_urb *urb, unsigned long *timeout);

int bce_vhci_urb_create(struct bce_vhci_transfer_queue *q, struct urb *urb)
{
    int status = 0;
    struct bce_vhci_urb *vurb;
    vurb = kzalloc(sizeof(struct bce_vhci_urb), GFP_KERNEL);
    urb->hcpriv = vurb;

    vurb->q = q;
    vurb->urb = urb;
    vurb->dir = usb_urb_dir_in(urb) ? DMA_FROM_DEVICE : DMA_TO_DEVICE;
    vurb->is_control = (usb_endpoint_num(&urb->ep->desc) == 0);

    spin_lock(&q->urb_lock);
    usb_hcd_link_urb_to_ep(q->vhci->hcd, urb);

    if (vurb->is_control)
        vurb->state = BCE_VHCI_URB_CONTROL_SETUP;
    else
        status = bce_vhci_urb_data_start(vurb, NULL);
    if (status) {
        usb_hcd_unlink_urb_from_ep(q->vhci->hcd, urb);
        urb->hcpriv = NULL;
        kfree(vurb);
    } else {
        bce_vhci_transfer_queue_deliver_pending(q);
    }
    spin_unlock(&q->urb_lock);
    pr_info("bce-vhci: URB created\n");
    return 0;
}

static void bce_vhci_urb_complete(struct bce_vhci_urb *urb, int status)
{
    struct bce_vhci_transfer_queue *q = urb->q;
    struct bce_vhci *vhci = q->vhci;
    struct urb *real_urb = urb->urb;
    pr_info("bce-vhci: URB complete %i\n", status);
    usb_hcd_unlink_urb_from_ep(vhci->hcd, real_urb);
    real_urb->hcpriv = NULL;
    real_urb->status = status;
    kfree(urb);
    list_add(&real_urb->urb_list, &q->giveback_urb_list);
}

int bce_vhci_urb_cancel(struct bce_vhci_transfer_queue *q, struct urb *urb, int status)
{
    int ret = 0;
    struct bce_vhci_urb *vurb;
    pr_info("bce-vhci: URB cancel\n");
    spin_lock(&q->urb_lock);
    if ((ret = usb_hcd_check_unlink_urb(q->vhci->hcd, urb, status))) {
        spin_unlock(&q->urb_lock);
        return ret;
    }
    vurb = urb->hcpriv;
    usb_hcd_unlink_urb_from_ep(q->vhci->hcd, urb);
    bce_vhci_transfer_queue_deliver_pending(q); /* TODO: Probably not the right place? */
    spin_unlock(&q->urb_lock);
    kfree(vurb);
    usb_hcd_giveback_urb(q->vhci->hcd, urb, status);
    return ret;
}


static int bce_vhci_urb_data_start(struct bce_vhci_urb *urb, unsigned long *timeout)
{
    struct bce_vhci_message msg;
    struct bce_qe_submission *s;
    int reservation1, reservation2 = -EFAULT;

    if (urb->dir == DMA_TO_DEVICE) {
        if (urb->urb->transfer_buffer_length > 0)
            urb->state = BCE_VHCI_URB_WAITING_FOR_TRANSFER_REQUEST;
        else
            urb->state = BCE_VHCI_URB_DATA_TRANSFER_COMPLETE;
        return 0;
    } else {
        pr_info("bce-vhci: DMA from device %llx %x\n", urb->urb->transfer_dma, urb->urb->transfer_buffer_length);

        /* Reserve both a message and a submission, so we don't run into issues later. */
        reservation1 = bce_reserve_submission(urb->q->vhci->msg_asynchronous.sq, timeout);
        if (!reservation1)
            reservation2 = bce_reserve_submission(urb->q->sq_in, timeout);
        if (reservation1 || reservation2) {
            pr_err("bce-vhci: Failed to reserve a submission for URB data transfer\n");
            if (!reservation1)
                bce_cancel_submission_reservation(urb->q->sq_in);
            return -ENOMEM;
        }

        msg.cmd = BCE_VHCI_MSG_TRANSFER_REQUEST;
        msg.status = 0;
        msg.param1 = ((urb->urb->ep->desc.bEndpointAddress & 0x8Fu) << 8) | urb->q->dev_addr;
        msg.param2 = urb->urb->transfer_buffer_length;
        bce_vhci_message_queue_write(&urb->q->vhci->msg_asynchronous, &msg);

        s = bce_next_submission(urb->q->sq_in);
        bce_set_submission_single(s, urb->urb->transfer_dma, urb->urb->transfer_buffer_length);
        bce_submit_to_device(urb->q->sq_in);

        urb->state = BCE_VHCI_URB_WAITING_FOR_COMPLETION;
        return 0;
    }
}

static int bce_vhci_urb_send_out_data(struct bce_vhci_urb *urb, dma_addr_t addr, size_t size)
{
    struct bce_qe_submission *s;
    unsigned long timeout = 0;
    if (bce_reserve_submission(urb->q->sq_out, &timeout)) {
        pr_err("bce-vhci: Failed to reserve a submission for URB data transfer\n");
        return -EPIPE;
    }

    s = bce_next_submission(urb->q->sq_out);
    bce_set_submission_single(s, addr, size);
    bce_submit_to_device(urb->q->sq_out);
    return 0;
}

static int bce_vhci_urb_data_update(struct bce_vhci_urb *urb, struct bce_vhci_message *msg)
{
    int status;
    if (urb->state == BCE_VHCI_URB_WAITING_FOR_TRANSFER_REQUEST) {
        if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST) {
            if (msg->param2 != urb->urb->transfer_buffer_length)
                pr_err("bce-vhci: Device requested wrong transfer buffer length\n");
            if ((status = bce_vhci_urb_send_out_data(urb, urb->urb->transfer_dma,
                    min(urb->urb->transfer_buffer_length, (u32) msg->param2))))
                return status;
            urb->state = BCE_VHCI_URB_WAITING_FOR_COMPLETION;
            return 0;
        }
    }
    pr_err("bce-vhci: %s URB unexpected message (state = %x, msg: %x %x %x %llx)\n",
            (urb->is_control ? "Control (data update)" : "Data"), urb->state,
            msg->cmd, msg->status, msg->param1, msg->param2);
    return -EAGAIN;
}

static int bce_vhci_urb_data_transfer_completion(struct bce_vhci_urb *urb, struct bce_sq_completion_data *c)
{
    if (urb->state == BCE_VHCI_URB_WAITING_FOR_COMPLETION) {
        urb->urb->actual_length = (u32) c->data_size;
        urb->state = BCE_VHCI_URB_DATA_TRANSFER_COMPLETE;
        if (!urb->is_control)
            bce_vhci_urb_complete(urb, 0);
    } else {
        pr_err("bce-vhci: Data URB unexpected completion\n");
    }
    return 0;
}


static int bce_vhci_urb_control_check_status(struct bce_vhci_urb *urb)
{
    if (urb->received_status == 0)
        return 0;
    if (urb->state == BCE_VHCI_URB_DATA_TRANSFER_COMPLETE ||
        (urb->received_status != BCE_VHCI_SUCCESS && urb->state != BCE_VHCI_URB_CONTROL_SETUP)) {
        urb->state = BCE_VHCI_URB_CONTROL_COMPLETE;
        bce_vhci_urb_complete(urb, 0);
        return -ENOENT;
    }
    return 0;
}

static int bce_vhci_urb_control_update(struct bce_vhci_urb *urb, struct bce_vhci_message *msg)
{
    int status;
    if (msg->cmd == BCE_VHCI_MSG_CONTROL_TRANSFER_STATUS) {
        urb->received_status = msg->status;
        return bce_vhci_urb_control_check_status(urb);
    }

    if (urb->state == BCE_VHCI_URB_CONTROL_SETUP) {
        if (msg->cmd == BCE_VHCI_MSG_TRANSFER_REQUEST) {
            if (bce_vhci_urb_send_out_data(urb, urb->urb->setup_dma, sizeof(struct usb_ctrlrequest))) {
                pr_err("bce-vhci: Failed to start URB setup transfer\n");
                return 0; /* TODO: fail the URB? */
            }
            pr_info("bce-vhci: Sent setup %llx\n", urb->urb->setup_dma);
            return 0;
        }
    } else if (urb->state == BCE_VHCI_URB_WAITING_FOR_TRANSFER_REQUEST ||
               urb->state == BCE_VHCI_URB_WAITING_FOR_COMPLETION) {
        if ((status = bce_vhci_urb_data_update(urb, msg)))
            return status;
        return bce_vhci_urb_control_check_status(urb);
    }
    pr_err("bce-vhci: Control URB unexpected message (state = %x, msg: %x %x %x %llx)\n", urb->state,
            msg->cmd, msg->status, msg->param1, msg->param2);
    return -EAGAIN;
}

static int bce_vhci_urb_control_transfer_completion(struct bce_vhci_urb *urb, struct bce_sq_completion_data *c)
{
    int status;
    unsigned long timeout;

    if (urb->state == BCE_VHCI_URB_CONTROL_SETUP) {
        if (c->data_size != sizeof(struct usb_ctrlrequest))
            pr_err("bce-vhci: transfer complete data size mistmatch for usb_ctrlrequest (%llx instead of %lx)\n",
                    c->data_size, sizeof(struct usb_ctrlrequest));

        timeout = 1000;
        status = bce_vhci_urb_data_start(urb, &timeout);
        if (status) {
            bce_vhci_urb_complete(urb, status);
            return -ENOENT;
        }
        return 0;
    } else if (urb->state == BCE_VHCI_URB_WAITING_FOR_TRANSFER_REQUEST ||
               urb->state == BCE_VHCI_URB_WAITING_FOR_COMPLETION) {
        if ((status = bce_vhci_urb_data_transfer_completion(urb, c)))
            return status;
        return bce_vhci_urb_control_check_status(urb);
    } else {
        pr_err("bce-vhci: Control URB unexpected completion (state = %x)\n", urb->state);
    }
    return 0;
}

static int bce_vhci_urb_update(struct bce_vhci_urb *urb, struct bce_vhci_message *msg)
{
    if (urb->is_control)
        return bce_vhci_urb_control_update(urb, msg);
    else
        return bce_vhci_urb_data_update(urb, msg);
}

static int bce_vhci_urb_transfer_completion(struct bce_vhci_urb *urb, struct bce_sq_completion_data *c)
{
    if (urb->is_control)
        return bce_vhci_urb_control_transfer_completion(urb, c);
    else
        return bce_vhci_urb_data_transfer_completion(urb, c);
}