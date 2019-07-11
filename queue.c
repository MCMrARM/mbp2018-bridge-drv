#include "queue.h"
#include "pci.h"

#define REG_DOORBELL_BASE 0x44000

struct bce_queue_cq *bce_create_cq(struct bce_device *dev, int qid, u32 el_count)
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
        return NULL;
    }
    q->reg_mem_dma = dev->reg_mem_dma;
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

void bce_destroy_cq(struct bce_device *dev, struct bce_queue_cq *cq)
{
    dma_free_coherent(&dev->pci->dev, cq->el_count * sizeof(struct bce_qe_completion), cq->data, cq->dma_handle);
    kfree(cq);
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
    if (target_sq->expected_completion_idx != e->completion_index) {
        pr_err("Completion index mismatch; this is likely going to make this driver unusable\n");
        return;
    }
    if (target_sq->completion)
        target_sq->completion(target_sq, e->completion_index, e->status, e->data_size, e->result);
    target_sq->expected_completion_idx = (target_sq->expected_completion_idx + 1) % target_sq->el_count;
}

void bce_handle_cq_completions(struct bce_device *dev, struct bce_queue_cq *cq)
{
    struct bce_qe_completion *e;e = bce_cq_element(cq, cq->index);
    if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
        return;
    while (true) {
        e = bce_cq_element(cq, cq->index);
        if (!(e->flags & BCE_COMPLETION_FLAG_PENDING))
            break;
        mb();
        bce_handle_cq_completion(dev, e);
        mb();
        e->flags = 0;
        cq->index = (cq->index + 1) % cq->el_count;
    }
    iowrite32(cq->index, (u32 *) ((u8 *) dev->reg_mem_dma +  REG_DOORBELL_BASE) + cq->qid);
}


struct bce_queue_sq *bce_create_sq(struct bce_device *dev, int qid, u32 el_size, u32 el_count,
        bce_sq_completion compl, void *userdata)
{
    struct bce_queue_sq *q;
    q = kzalloc(sizeof(struct bce_queue_sq), GFP_KERNEL);
    q->qid = qid;
    q->type = BCE_QUEUE_SQ;
    q->el_size = el_size;
    q->el_count = el_count;
    q->data = dma_alloc_coherent(&dev->pci->dev, el_count * el_size,
                                 &q->dma_handle, GFP_KERNEL);
    q->completion = compl;
    q->userdata = userdata;
    if (!q->data) {
        pr_err("DMA queue memory alloc failed\n");
        kfree(q);
        return NULL;
    }
    return q;
}

void bce_get_sq_memcfg(struct bce_queue_sq *sq, struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg)
{
    cfg->qid = (u16) sq->qid;
    cfg->el_count = (u16) sq->el_count;
    cfg->vector_or_cq = (u16) cq->qid;
    cfg->_pad = 0;
    cfg->addr = sq->dma_handle;
    cfg->length = sq->el_count * sq->el_size;
}

void bce_destroy_sq(struct bce_device *dev, struct bce_queue_sq *sq)
{
    dma_free_coherent(&dev->pci->dev, sq->el_count * sq->el_size, sq->data, sq->dma_handle);
    kfree(sq);
}


static void bce_cmdq_completion(struct bce_queue_sq *q, u32 idx, u32 status, u64 data_size, u64 result);

struct bce_queue_cmdq *bce_create_cmdq(struct bce_device *dev, int qid, u32 el_count)
{
    struct bce_queue_cmdq *q;
    q = kzalloc(sizeof(struct bce_queue_cmdq), GFP_KERNEL);
    q->reg_mem_dma = dev->reg_mem_dma;
    q->sq = bce_create_sq(dev, qid, BCE_CMD_SIZE, el_count, bce_cmdq_completion, q);
    if (!q->sq) {
        kfree(q);
        return NULL;
    }
    spin_lock_init(&q->lck);
    init_completion(&q->nospace_cmpl);
    q->tres = kzalloc(sizeof(struct bce_queue_cmdq_result_el*) * el_count, GFP_KERNEL);
    if (!q->tres) {
        kfree(q);
        return NULL;
    }
    return q;
}

void bce_destroy_cmdq(struct bce_device *dev, struct bce_queue_cmdq *cmdq)
{
    bce_destroy_sq(dev, cmdq->sq);
    kfree(cmdq->tres);
    kfree(cmdq);
}

void bce_cmdq_completion(struct bce_queue_sq *q, u32 idx, u32 status, u64 data_size, u64 result)
{
    int nospace_notify;
    struct bce_queue_cmdq_result_el *el;
    struct bce_queue_cmdq *cmdq = q->userdata;
    spin_lock(&cmdq->lck);
    el = cmdq->tres[cmdq->head];
    if (el) {
        el->result = result;
        el->status = status;
        mb();
        complete(&el->cmpl);
    } else {
        pr_err("bce: Unexpected command queue completion\n");
    }
    cmdq->tres[cmdq->head] = NULL;
    cmdq->head = cmdq->head + 1;
    nospace_notify = cmdq->nospace_cntr--;
    spin_unlock(&cmdq->lck);
    if (nospace_notify > 0)
        complete(&cmdq->nospace_cmpl);
}

static __always_inline void *bce_cmd_start(struct bce_queue_cmdq *cmdq, struct bce_queue_cmdq_result_el *res)
{
    void *ret;
    init_completion(&res->cmpl);

    spin_lock(&cmdq->lck);
    while ((cmdq->tail + 1) % cmdq->sq->el_count == cmdq->head) { // No free elements
        ++cmdq->nospace_cntr;
        spin_unlock(&cmdq->lck);
        wait_for_completion(&cmdq->nospace_cmpl);
        spin_lock(&cmdq->lck);
    }

    ret = bce_sq_element(cmdq->sq, cmdq->tail);
    cmdq->tres[cmdq->tail] = res;
    cmdq->tail = (cmdq->tail + 1) % cmdq->sq->el_count;
    return ret;
}

static __always_inline void bce_cmd_finish(struct bce_queue_cmdq *cmdq, struct bce_queue_cmdq_result_el *res)
{
    iowrite32(cmdq->tail, (u32 *) ((u8 *) cmdq->reg_mem_dma +  REG_DOORBELL_BASE) + cmdq->sq->qid);
    spin_unlock(&cmdq->lck);

    wait_for_completion(&res->cmpl);
    mb();
}

u32 bce_cmd_register_queue(struct bce_queue_cmdq *cmdq, struct bce_queue_memcfg *cfg, const char *name, bool isdirin)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_register_memory_queue_cmd *cmd = bce_cmd_start(cmdq, &res);
    cmd->cmd = BCE_CMD_REGISTER_MEMORY_QUEUE;
    cmd->flags = (u16) ((name ? 2 : 0) | (isdirin ? 1 : 0));
    cmd->qid = cfg->qid;
    cmd->el_count = cfg->el_count;
    cmd->vector_or_cq = cfg->vector_or_cq;
    if (name) {
        cmd->name_len = (u16) min(strlen(name), (size_t) 0x20);
        memcpy(cmd->name, name, cmd->name_len);
    } else {
        cmd->name_len = 0;
    }
    cmd->addr = cfg->addr;
    cmd->length = cfg->length;

    bce_cmd_finish(cmdq, &res);
    return res.status;
}

u32 bce_cmd_unregister_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_simple_memory_queue_cmd *cmd = bce_cmd_start(cmdq, &res);
    cmd->cmd = BCE_CMD_UNREGISTER_MEMORY_QUEUE;
    cmd->flags = 0;
    cmd->qid = qid;
    bce_cmd_finish(cmdq, &res);
    return res.status;
}

u32 bce_cmd_flush_memory_queue(struct bce_queue_cmdq *cmdq, u16 qid)
{
    struct bce_queue_cmdq_result_el res;
    struct bce_cmdq_simple_memory_queue_cmd *cmd = bce_cmd_start(cmdq, &res);
    cmd->cmd = BCE_CMD_FLUSH_MEMORY_QUEUE;
    cmd->flags = 0;
    cmd->qid = qid;
    bce_cmd_finish(cmdq, &res);
    return res.status;
}