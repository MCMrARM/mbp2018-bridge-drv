#pragma once

#include <linux/pci.h>
#include "mailbox.h"
#include "queue.h"

#define BC_PROTOCOL_VERSION 0x20001
#define BCE_MAX_QUEUE_COUNT 0x100

struct bce_device {
    struct pci_dev *pci;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_mb;
    void __iomem *reg_mem_dma;
    struct bce_mailbox mbox;
    struct bce_queue *queues[BCE_MAX_QUEUE_COUNT];
    struct bce_queue_cq *cmd_cq;
    struct bce_queue_sq *cmd_sq;
};

