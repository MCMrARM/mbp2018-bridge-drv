#include "protocol.h"

#include <linux/slab.h>

int aaudio_msg_get_base(struct aaudio_msg *msg, struct aaudio_msg_base *base)
{
    if (msg->size < sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base) * 2)
        return -EINVAL;
    *base = *((struct aaudio_msg_base *) ((struct aaudio_msg_header *) msg->data + 1));
    return 0;
}

#define WRITE_START_COMMAND(devid) \
    size_t offset = sizeof(struct aaudio_msg_header); \
    ((struct aaudio_msg_header *) msg->data)->type = AAUDIO_MSG_TYPE_COMMAND; \
    ((struct aaudio_msg_header *) msg->data)->device_id = (devid);
#define WRITE_START_NOTIFICATION() \
    size_t offset = sizeof(struct aaudio_msg_header); \
    ((struct aaudio_msg_header *) msg->data)->type = AAUDIO_MSG_TYPE_NOTIFICATION; \
    ((struct aaudio_msg_header *) msg->data)->device_id = 0;
#define WRITE_VAL(type, value) { *((type *) ((u8 *) msg->data + offset)) = value; offset += sizeof(value); }
#define WRITE_BASE(type) WRITE_VAL(u32, type) WRITE_VAL(u32, 0)
#define WRITE_END() { msg->size = offset; }

void aaudio_msg_set_alive_notification(struct aaudio_msg *msg, u32 proto_ver, u32 msg_ver)
{
    WRITE_START_NOTIFICATION();
    WRITE_BASE(AAUDIO_MSG_NOTIFICATION_ALIVE);
    WRITE_VAL(u32, proto_ver);
    WRITE_VAL(u32, msg_ver);
    WRITE_END();
}

void aaudio_msg_set_remote_access(struct aaudio_msg *msg, u64 mode)
{
    WRITE_START_COMMAND(0);
    WRITE_BASE(AAUDIO_MSG_SET_REMOTE_ACCESS);
    WRITE_VAL(u64, mode);
    WRITE_END();
}

void aaudio_msg_start_io(struct aaudio_msg *msg, aaudio_device_id_t dev)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_START_IO);
    WRITE_END();
}