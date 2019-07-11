#ifndef BCE_QUEUE_H
#define BCE_QUEUE_H

#include <linux/completion.h>
#include <linux/pci.h>

#define BCE_CMD_SIZE 0x40

struct bce_device;

enum bce_queue_type {
    BCE_QUEUE_CQ, BCE_QUEUE_SQ
};
struct bce_queue {
    int qid;
    int type;
};
struct bce_queue_cq {
    int qid;
    int type;
    u32 el_count;
    dma_addr_t dma_handle;
    void *data;

    int index;
};
struct bce_queue_sq;
typedef void (*bce_sq_completion)(struct bce_queue_sq *q, u32 idx, u32 status, u64 data_size, u64 result);
struct bce_queue_sq {
    int qid;
    int type;
    u32 el_size;
    u32 el_count;
    dma_addr_t dma_handle;
    void *data;

    u32 head, tail;
    bce_sq_completion completion;
};

struct bce_queue_memcfg {
    u16 qid;
    u16 el_count;
    u16 vector_or_cq;
    u16 _pad;
    u64 addr;
    size_t length;
};

enum bce_qe_completion_status {
    BCE_COMPLETION_SUCCESS = 0,
    BCE_COMPLETION_ERROR = 1,
    BCE_COMPLETION_ABORTED = 2,
    BCE_COMPLETION_NO_SPACE = 3,
    BCE_COMPLETION_OVERRUN = 4
};
enum bce_qe_completion_flags {
    BCE_COMPLETION_FLAG_PENDING = 0x8000
};
struct bce_qe_completion {
    u64 data_size;
    u64 result;
    ushort qid;
    ushort completion_index;
    ushort status; // bce_qe_completion_status
    ushort flags;  // bce_qe_completion_flags
};

static __always_inline void *bce_sq_element(struct bce_queue_sq *q, int i) {
    return (void *) ((u8 *) q->data + q->el_size * i);
}
static __always_inline void *bce_cq_element(struct bce_queue_cq *q, int i) {
    return (void *) ((struct bce_qe_completion *) q->data + i);
}

struct bce_queue_cq *bce_create_cq(struct bce_device *dev, int qid, u32 el_count);
void bce_get_cq_memcfg(struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg);
void bce_destroy_cq(struct bce_device *dev, struct bce_queue_cq *cq);
void bce_handle_cq_completions(struct bce_device *dev, struct bce_queue_cq *cq);

struct bce_queue_sq *bce_create_sq(struct bce_device *dev, int qid, u32 el_size, u32 el_count, bce_sq_completion compl);
void bce_get_sq_memcfg(struct bce_queue_sq *sq, struct bce_queue_cq *cq, struct bce_queue_memcfg *cfg);
void bce_destroy_sq(struct bce_device *dev, struct bce_queue_sq *sq);


#endif //BCEDRIVER_MAILBOX_H
