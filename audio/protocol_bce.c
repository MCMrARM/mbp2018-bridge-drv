#include "protocol_bce.h"

#include "audio.h"

static void aaudio_bce_out_queue_completion(struct bce_queue_sq *sq);
static void aaudio_bce_in_queue_completion(struct bce_queue_sq *sq);
static int aaudio_bce_queue_init(struct aaudio_device *dev, struct aaudio_bce_queue *q, const char *name, int direction,
                                 bce_sq_completion cfn);
void aaudio_bce_in_queue_submit_pending(struct aaudio_bce_queue *q, size_t count);

int aaudio_bce_init(struct aaudio_device *dev)
{
    int status;
    struct aaudio_bce *bce = &dev->bcem;
    bce->cq = bce_create_cq(dev->bce, 0x80);
    if (!bce->cq)
        return -EINVAL;
    if ((status = aaudio_bce_queue_init(dev, &bce->qout, "com.apple.BridgeAudio.IntelToARM", DMA_TO_DEVICE,
            aaudio_bce_out_queue_completion))) {
        return status;
    }
    if ((status = aaudio_bce_queue_init(dev, &bce->qin, "com.apple.BridgeAudio.ARMToIntel", DMA_FROM_DEVICE,
            aaudio_bce_in_queue_completion))) {
        return status;
    }
    aaudio_bce_in_queue_submit_pending(&bce->qin, bce->qin.el_count);
    return 0;
}

int aaudio_bce_queue_init(struct aaudio_device *dev, struct aaudio_bce_queue *q, const char *name, int direction,
        bce_sq_completion cfn)
{
    int status;
    q->cq = dev->bcem.cq;
    q->el_size = AAUDIO_BCE_QUEUE_ELEMENT_SIZE;
    q->el_count = AAUDIO_BCE_QUEUE_ELEMENT_COUNT;
    /* NOTE: The Apple impl uses 0x80 as the queue size, however we use 21 (in fact 20) to simplify the impl */
    q->sq = bce_create_sq(dev->bce, q->cq, name, (u32) (q->el_count + 1), direction, cfn, dev);
    if (!q->sq)
        return -EINVAL;
    spin_lock_init(&q->spinlock);

    q->data = dma_alloc_coherent(&dev->bce->pci->dev, q->el_size * q->el_count, &q->dma_addr, GFP_KERNEL);
    if (!q->data) {
        bce_destroy_sq(dev->bce, q->sq);
        return -EINVAL;
    }
    return 0;
}

static void aaudio_send_create_tag(struct aaudio_bce_queue *q, char tag[4])
{
    char tag_zero[5];
    snprintf(tag_zero, 5, "S%03d", q->next_el_index);
    ++q->next_el_index;
    memcpy(tag, tag_zero, 4);
}

int aaudio_send_prepare(struct aaudio_bce_queue *q, struct aaudio_msg *msg, unsigned long *timeout)
{
    int status;
    size_t index;
    void *dptr;
    struct aaudio_msg_header *header;
    if ((status = bce_reserve_submission(q->sq, timeout)))
        return status;
    spin_lock(&q->spinlock);
    index = q->data_tail;
    dptr = (u8 *) q->data + index * q->el_size;
    msg->data = dptr;
    header = dptr;
    aaudio_send_create_tag(q, header->tag);
    header->type = AAUDIO_MSG_TYPE_NOTIFICATION;
    header->device_id = 0;
    return 0;
}

void aaudio_send(struct aaudio_bce_queue *q, struct aaudio_msg *msg)
{
    struct bce_qe_submission *s = bce_next_submission(q->sq);
    pr_info("aaudio: Sending command data\n");
    print_hex_dump(KERN_INFO, "aaudio:OUT ", DUMP_PREFIX_NONE, 32, 1, msg->data, msg->size, true);
    bce_set_submission_single(s, q->dma_addr + (dma_addr_t) (msg->data - q->data), msg->size);
    bce_submit_to_device(q->sq);
    q->data_tail = (q->data_tail + 1) % q->el_size;
    spin_unlock(&q->spinlock);
}

static void aaudio_bce_out_queue_completion(struct bce_queue_sq *sq)
{
    while (bce_next_completion(sq)) {
        pr_info("aaudio: Send confirmed\n");
        bce_notify_submission_complete(sq);
    }
}

static void aaudio_bce_in_queue_completion(struct bce_queue_sq *sq)
{
    struct aaudio_device *dev = sq->userdata;
    struct aaudio_bce_queue *q = &dev->bcem.qin;
    struct bce_sq_completion_data *c;
    void *ptr;
    size_t cnt = 0;

    mb();
    while ((c = bce_next_completion(sq))) {
        ptr = (u8 *) q->data + q->data_head * q->el_size;
        pr_info("aaudio: Received command data %llx\n", c->data_size);
        if (c->data_size > 256)
            c->data_size = 256;
        print_hex_dump(KERN_INFO, "aaudio:IN ", DUMP_PREFIX_NONE, 32, 1, ptr, c->data_size, true);

        q->data_head = (q->data_head + 1) % q->el_size;

        bce_notify_submission_complete(sq);
        ++cnt;
    }
    aaudio_bce_in_queue_submit_pending(q, cnt);
}

void aaudio_bce_in_queue_submit_pending(struct aaudio_bce_queue *q, size_t count)
{
    struct bce_qe_submission *s;
    while (count--) {
        if (bce_reserve_submission(q->sq, NULL)) {
            pr_err("aaudio: Failed to reserve an event queue submission\n");
            break;
        }
        s = bce_next_submission(q->sq);
        bce_set_submission_single(s, q->dma_addr + (dma_addr_t) (q->data_tail * q->el_size), q->el_size);
        q->data_tail = (q->data_tail + 1) % q->el_size;
    }
    bce_submit_to_device(q->sq);
}
