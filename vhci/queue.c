#include "queue.h"
#include "vhci.h"
#include "../pci.h"

static void bce_vhci_event_queue_completion(struct bce_queue_sq *sq);
static void bce_vhci_submit_pending(struct bce_vhci_event_queue *q, size_t count);

int bce_vhci_event_queue_create(struct bce_vhci *vhci, struct bce_vhci_event_queue *ret, const char *name)
{
    ret->sq = bce_create_sq(vhci->dev, vhci->ev_cq, name, VHCI_EVENT_QUEUE_EL_COUNT, DMA_FROM_DEVICE,
                            bce_vhci_event_queue_completion, vhci);
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
    if (q->sq) {
        dma_free_coherent(&vhci->dev->pci->dev, sizeof(struct bce_vhci_message) * VHCI_EVENT_QUEUE_EL_COUNT,
                          q->data, q->dma_addr);
        bce_destroy_sq(vhci->dev, q->sq);
    }
}

static void bce_vhci_event_queue_completion(struct bce_queue_sq *sq)
{
    struct bce_vhci_event_queue *ev = sq->userdata;
    struct bce_vhci_message *msg;
    size_t cnt = 0;

    while (bce_next_completion(sq)) {
        // TODO: Process the event
        msg = &ev->data[sq->head];
        pr_info("bce-vhci: Got event: %x %x %x %llx\n", msg->status, msg->cmd, msg->param1, msg->param2);

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
        if (bce_reserve_submission(q->sq, 0)) {
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