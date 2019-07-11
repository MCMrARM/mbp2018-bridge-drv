#ifndef BCE_MAILBOX_H
#define BCE_MAILBOX_H

#include <linux/completion.h>
#include <linux/pci.h>

struct bce_mailbox {
    void __iomem *reg_mb;

    atomic_t mb_status; // possible statuses: 0 (no msg), 1 (has active msg), 2 (got reply)
    struct completion mb_completion;
    uint64_t mb_result;
};

enum bce_message_type {
    BCE_MB_REGISTER_COMMAND_SQ = 0x7,          // to-device
    BCE_MB_REGISTER_COMMAND_CQ = 0x8,          // to-device
    BCE_MB_REGISTER_COMMAND_QUEUE_REPLY = 0xA, // to-host
    BCE_MB_SET_FW_PROTOCOL_VERSION = 0xC       // both
};

#define BCE_MB_MSG(type, value) (((u64) (type) << 58) | (value & 0x3FFFFFFFFFFFFFFLL))
#define BCE_MB_TYPE(v) ((u32) (v >> 58))
#define BCE_MB_VALUE(v) (v & 0x3FFFFFFFFFFFFFFLL)

void bce_mailbox_init(struct bce_mailbox *mb, void __iomem *reg_mb);

int bce_mailbox_send(struct bce_mailbox *mb, u64 msg, u64* recv);

int bce_mailbox_handle_interrupt(struct bce_mailbox *mb);

#endif //BCEDRIVER_MAILBOX_H
