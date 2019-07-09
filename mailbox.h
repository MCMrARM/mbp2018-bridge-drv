#ifndef BCE_MAILBOX_H
#define BCE_MAILBOX_H

#include <linux/completion.h>
#include <linux/pci.h>

struct bce_mailbox {
    struct pci_dev *dev;
    void __iomem *reg_mb;

    atomic_t mb_taken; // if someone is currently sending a message
    struct completion mb_completion;
    uint64_t result;
};

enum bce_message_type {
    BCE_MB_SET_FW_PROTOCOL_VERSION = 0xC
};

#define BCE_MB_MSG(type, value) (((u64) type << 58) | (value & 0x3FFFFFFFFFFFFFFLL))
#define BCE_MB_TYPE(v) ((u32) (v >> 58))
#define BCE_MB_VALUE(v) (v & 0x3FFFFFFFFFFFFFFLL)

void bce_mailbox_init(struct bce_mailbox* mb, struct pci_dev* dev, void __iomem *reg_mb);

int bce_mailbox_send(struct bce_mailbox* mb, u64 msg, u64* recv);

#endif //BCEDRIVER_MAILBOX_H
