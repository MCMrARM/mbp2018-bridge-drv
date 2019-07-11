#include "queue.h"
#include "pci.h"

struct bce_queue_cq *bce_create_cq(struct bce_device *dev, int qid, int el_count)
{
    struct bce_queue_cq *q;
    q = kzalloc(sizeof(struct bce_queue_cq), GFP_KERNEL);
    q->qid = qid;
    q->type = BCE_QUEUE_CQ;
    q->el_count = el_count;
    q->data = dma_alloc_coherent(&dev->pci->dev, el_count * sizeof(struct bce_qe_completion),
            &q->dma_handle, GFP_KERNEL);
    if (!q->data) {
        pr_err("DMA queue memory alloc failed\n");
        kfree(q);
        return q;
    }
    return q;
}

void bce_get_cq_memcfg(struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg)
{
    cfg->qid = (u16) cq->qid;
    cfg->el_count = (u16) cq->el_count;
    cfg->vector_or_cq = 0;
    cfg->_pad = 0;
    cfg->addr = cq->dma_handle;
    cfg->length = cq->el_count * sizeof(struct bce_qe_completion);
}

void bce_destroy_cq(struct bce_device *dev, struct bce_queue_cq *q)
{
    dma_free_coherent(&dev->pci->dev, q->el_count * sizeof(struct bce_qe_completion), q->data, q->dma_handle);
    kfree(q);
}

static void bce_handle_cq_completion(struct bce_device *dev, struct bce_qe_completion *e)
{
    struct bce_queue *target;
    struct bce_queue_sq *target_sq;
    if (e->qid >= BCE_MAX_QUEUE_COUNT) {
        pr_err("Device sent a response for qid (%u) >= BCE_MAX_QUEUE_COUNT\n", e->qid);
        return;
    }
    target = dev->queues[e->qid];
    if (!target || target->type != BCE_QUEUE_SQ) {
        pr_err("Device sent a response for qid (%u), which does not exist\n", e->qid);
        return;
    }
    target_sq = (struct bce_queue_sq *) target;
    if (target_sq->head != e->completion_index) {
        pr_err("Completion index mismatch; this is likely going to make this driver unusable\n");
        return;
    }
    if (target_sq->completion)
        target_sq->completion(target_sq, e->completion_index, e->status, e->data_size, e->result);
    target_sq->head = (target_sq->head + 1) % target_sq->el_count;
}

void bce_handle_cq_completions(struct bce_device *dev, struct bce_queue_cq *cq)
{
    while (true) {
        struct bce_qe_completion *e = bce_cq_element(cq, cq->index);
        if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
            break;
        mb();
        bce_handle_cq_completion(dev, e);
        mb();
        e->flags = 0;
        ++cq->index;
    }
}