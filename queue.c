#include "queue.h"
#include "pci.h"


static void bce_queue_handle_completion(struct bce_device *dev, struct bce_qe_completion *e)
{
    struct bce_queue *target;
    struct bce_queue_sq *target_sq;
    if (e->qid >= BCE_MAX_QUEUE_COUNT) {
        pr_err("Device sent a response for qid (%u) >= BCE_MAX_QUEUE_COUNT", e->qid);
        return;
    }
    target = dev->queues[e->qid];
    if (!target || target->type != BCE_QUEUE_SQ) {
        pr_err("Device sent a response for qid (%u), which does not exist", e->qid);
        return;
    }
    target_sq = (struct bce_queue_sq *) target;
    if (target_sq->expected_completion_index != e->completion_index) {
        pr_err("Completion index mismatch; this is likely going to make this driver unusable");
        return;
    }
    if (target_sq->completion)
        target_sq->completion(target_sq, e->completion_index, e->status, e->data_size, e->result);
    ++target_sq->expected_completion_index;
}

void bce_queue_handle_completions(struct bce_device *dev, struct bce_queue_cq *cq)
{
    while (true) {
        struct bce_qe_completion *e = bce_queue_cq_element(cq, cq->index);
        if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
            break;
        mb();
        bce_queue_handle_completion(dev, e);
        mb();
        e->flags = 0;
        ++cq->index;
    }
}