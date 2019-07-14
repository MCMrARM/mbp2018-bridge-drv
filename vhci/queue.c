#include "queue.h"
#include "vhci.h"
#include "../pci.h"


static void bce_vhci_message_queue_completion(struct bce_queue_sq *sq);

int bce_vhci_message_queue_create(struct bce_vhci *vhci, struct bce_vhci_message_queue *ret, const char *name)
{
    int status;
    ret->cq = bce_create_cq(vhci->dev, VHCI_EVENT_QUEUE_EL_COUNT);
    if (!ret->cq)
        return -EINVAL;
    ret->sq = bce_create_sq(vhci->dev, ret->cq, name, VHCI_EVENT_QUEUE_EL_COUNT, DMA_TO_DEVICE,
                            bce_vhci_message_queue_completion, ret);
    if (!ret->sq) {
        status = -EINVAL;
        goto fail_cq;
    }
    ret->data = dma_alloc_coherent(&vhci->dev->pci->dev, sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                                   &ret->dma_addr, GFP_KERNEL);
    if (!ret->data) {
        status = -EINVAL;
        goto fail_sq;
    }
    return 0;

fail_sq:
    bce_destroy_sq(vhci->dev, ret->sq);
    ret->sq = NULL;
fail_cq:
    bce_destroy_cq(vhci->dev, ret->cq);
    ret->cq = NULL;
    return status;
}

void bce_vhci_message_queue_destroy(struct bce_vhci *vhci, struct bce_vhci_message_queue *q)
{
    if (!q->cq)
        return;
    dma_free_coherent(&vhci->dev->pci->dev, sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                      q->data, q->dma_addr);
    bce_destroy_sq(vhci->dev, q->sq);
    bce_destroy_cq(vhci->dev, q->cq);
}

static void bce_vhci_message_queue_completion(struct bce_queue_sq *sq)
{
    while (bce_next_completion(sq))
        bce_notify_submission_complete(sq);
}



static void bce_vhci_event_queue_completion(struct bce_queue_sq *sq);
static void bce_vhci_submit_pending(struct bce_vhci_event_queue *q, size_t count);

int bce_vhci_event_queue_create(struct bce_vhci *vhci, struct bce_vhci_event_queue *ret, const char *name,
        bce_vhci_event_queue_callback cb)
{
    ret->vhci = vhci;
    ret->cb = cb;

    ret->sq = bce_create_sq(vhci->dev, vhci->ev_cq, name, VHCI_EVENT_QUEUE_EL_COUNT, DMA_FROM_DEVICE,
                            bce_vhci_event_queue_completion, ret);
    if (!ret->sq)
        return -EINVAL;
    ret->data = dma_alloc_coherent(&vhci->dev->pci->dev, sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                                   &ret->dma_addr, GFP_KERNEL);
    if (!ret->data) {
        bce_destroy_sq(vhci->dev, ret->sq);
        ret->sq = NULL;
        return -EINVAL;
    }

    bce_vhci_submit_pending(ret, VHCI_EVENT_PENDING_COUNT);
    return 0;
}

void bce_vhci_event_queue_destroy(struct bce_vhci *vhci, struct bce_vhci_event_queue *q)
{
    if (!q->sq)
        return;
    dma_free_coherent(&vhci->dev->pci->dev, sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                      q->data, q->dma_addr);
    bce_destroy_sq(vhci->dev, q->sq);
}

static void bce_vhci_event_queue_completion(struct bce_queue_sq *sq)
{
    struct bce_vhci_event_queue *ev = sq->userdata;
    struct bce_vhci_message *msg;
    size_t cnt = 0;

    while (bce_next_completion(sq)) {
        msg = &ev->data[sq->head];
        pr_info("bce-vhci: Got event: %x %x %x %llx\n", msg->status, msg->cmd, msg->param1, msg->param2);
        ev->cb(ev, msg);

        bce_notify_submission_complete(sq);
        ++cnt;
    }
    bce_vhci_submit_pending(ev, cnt);
}

static void bce_vhci_submit_pending(struct bce_vhci_event_queue *q, size_t count)
{
    int idx;
    struct bce_qe_submission *s;
    while (count--) {
        if (bce_reserve_submission(q->sq, NULL)) {
            pr_err("bce-vhci: Failed to reserve an event queue submission\n");
            break;
        }
        idx = q->sq->tail;
        s = bce_next_submission(q->sq);
        bce_set_submission_single(s,
                                  q->dma_addr + idx * sizeof(struct bce_vhci_message), sizeof(struct bce_vhci_message));
    }
    bce_submit_to_device(q->sq);
}


void bce_vhci_command_queue_create(struct bce_vhci_command_queue *ret, struct bce_vhci_message_queue *mq)
{
    ret->mq = mq;
    INIT_LIST_HEAD(&ret->completion_list);
    spin_lock_init(&ret->completion_list_lock);
}

void bce_vhci_command_queue_destroy(struct bce_vhci_command_queue *cq)
{
    struct bce_vhci_message abort_msg;
    memset(&abort_msg, 0, sizeof(abort_msg));
    abort_msg.status = BCE_VHCI_ABORT;
    while (!list_empty(&cq->completion_list))
        bce_vhci_command_queue_deliver_completion(cq, &abort_msg);
}

void bce_vhci_command_queue_deliver_completion(struct bce_vhci_command_queue *cq, struct bce_vhci_message *msg)
{
    struct bce_vhci_command_queue_completion *c;

    spin_lock(&cq->completion_list_lock);
    if (list_empty(&cq->completion_list)) {
        pr_err("bce-vhci: Unexpected command queue completion delivery\n");
        spin_unlock(&cq->completion_list_lock);
        return;
    }
    c = list_first_entry(&cq->completion_list, struct bce_vhci_command_queue_completion, list_head);
    c->deleted = true;
    list_del(&c->list_head);
    spin_unlock(&cq->completion_list_lock);

    *c->result = *msg;
    mb();
    complete(&c->completion);
}

int bce_vhci_command_queue_execute(struct bce_vhci_command_queue *cq, struct bce_vhci_message *req,
        struct bce_vhci_message *res, unsigned long timeout)
{
    int sidx;
    struct bce_qe_submission *s;
    int status;
    struct bce_vhci_command_queue_completion c;
    INIT_LIST_HEAD(&c.list_head);
    c.result = res;
    c.deleted = false;
    init_completion(&c.completion);

    if ((status = bce_reserve_submission(cq->mq->sq, &timeout)))
        return status;

    spin_lock(&cq->completion_list_lock);
    list_add_tail(&c.list_head, &cq->completion_list);

    sidx = cq->mq->sq->tail;
    s = bce_next_submission(cq->mq->sq);
    cq->mq->data[sidx] = *req;
    bce_set_submission_single(s, cq->mq->dma_addr + sizeof(struct bce_vhci_message) * sidx,
            sizeof(struct bce_vhci_message));
    bce_submit_to_device(cq->mq->sq);
    spin_unlock(&cq->completion_list_lock);

    if (!wait_for_completion_timeout(&c.completion, timeout)) {
        /* we ran out of time, delete the request if needed */
        spin_lock(&cq->completion_list_lock);
        if (!c.deleted)
            list_del(&c.list_head);
        spin_unlock(&cq->completion_list_lock);

        return -ETIMEDOUT;
    }

    if ((res->cmd & ~0x8000) != req->cmd) {
        pr_err("bce-vhci: Possible desync, cmd reply mismatch req=%x, res=%x\n", req->cmd, res->cmd);
        return -EIO;
    }
    if (res->status == BCE_VHCI_SUCCESS)
        return 0;
    return res->status;
}
