#pragma once

#include <linux/pci.h>
#include "mailbox.h"

#define BC_PROTOCOL_VERSION 0x20001

struct bce_device {
    struct pci_dev *dev;
    void __iomem *reg_mem_mb;
    void __iomem *reg_mem_dma;
    struct bce_mailbox mbox;
};

