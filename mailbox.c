#include "mailbox.h"
#include <linux/atomic.h>
#include "pci.h"

static irqreturn_t bce_handle_irq(int irq, void *dev);

void bce_mailbox_init(struct bce_mailbox* mb, struct pci_dev* dev, void __iomem *reg_mb)
{
    mb->dev = dev;
    mb->reg_mb = reg_mb;
    init_completion(&mb->mb_completion);
    request_irq(0, bce_handle_irq, 0, "mailbox interrupt", dev);
}

static irqreturn_t bce_handle_irq(int irq, void *dev)
{
    struct bce_device *bce = pci_get_drvdata(dev);
    complete(&bce->mbox.mb_completion);
    return IRQ_HANDLED;
}

static void bce_mailbox_retrive_response(struct bce_mailbox* mb, u64* recv);

int bce_mailbox_send(struct bce_mailbox* mb, u64 msg, u64* recv)
{
    void __iomem *regb;

    if (atomic_cmpxchg(&mb->mb_taken, 0, 1) != 0) {
        return -EEXIST; // We don't support two messages at once
    }
    reinit_completion(&mb->mb_completion);

    pr_debug("bce_mailbox_send: %llx", msg);
    regb = mb->reg_mb + 0x208;
    iowrite32((u32) msg, regb);
    iowrite32((u32) (msg >> 32), regb + 1);
    iowrite32(0, regb + 2);
    iowrite32(0, regb + 3);

    wait_for_completion(&mb->mb_completion);
    bce_mailbox_retrive_response(mb, recv);
    pr_debug("bce_mailbox_send: reply %llx", msg);

    atomic_set(&mb->mb_taken, 0);
    return 0;
}

static void bce_mailbox_retrive_response(struct bce_mailbox* mb, u64* recv) {
    void __iomem *regb;
    u32 lo, hi;
    int count = ioread32(mb->reg_mb + 0x42);
    while (count--) {
        regb = mb->reg_mb + 0x204;
        lo = ioread32(regb);
        hi = ioread32(regb + 1);
        ioread32(regb + 2);
        ioread32(regb + 3);
        *recv = ((u64) hi << 32) | lo;
    }
}