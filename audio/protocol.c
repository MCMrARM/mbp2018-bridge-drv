#include "protocol.h"
#include "protocol_bce.h"
#include "audio.h"

int aaudio_msg_read_base(struct aaudio_msg *msg, struct aaudio_msg_base *base)
{
    if (msg->size < sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base) * 2)
        return -EINVAL;
    *base = *((struct aaudio_msg_base *) ((struct aaudio_msg_header *) msg->data + 1));
    return 0;
}

#define READ_START(type) \
    size_t offset = sizeof(struct aaudio_msg_header) + sizeof(struct aaudio_msg_base); (void)offset; \
    if (((struct aaudio_msg_base *) ((struct aaudio_msg_header *) msg->data + 1))->msg != type) \
        return -EINVAL;
#define READ_DEVID_VAR(devid) *devid = ((struct aaudio_msg_header *) msg->data)->device_id
#define READ_VAL(type) ({ offset += sizeof(type); *((type *) ((u8 *) msg->data + offset - sizeof(type))); })
#define READ_VAR(type, var) *var = READ_VAL(type)

int aaudio_msg_read_start_io_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_START_IO_RESPONSE);
    return 0;
}

int aaudio_msg_read_update_timestamp(struct aaudio_msg *msg, aaudio_device_id_t *devid,
        u64 *timestamp, u64 *update_seed)
{
    READ_START(AAUDIO_MSG_UPDATE_TIMESTAMP);
    READ_DEVID_VAR(devid);
    READ_VAR(u64, timestamp);
    READ_VAR(u64, update_seed);
    return 0;
}

int aaudio_msg_read_set_remote_access_response(struct aaudio_msg *msg)
{
    READ_START(AAUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE);
    return 0;
}

#define WRITE_START_OF_TYPE(typev, devid) \
    size_t offset = sizeof(struct aaudio_msg_header); (void) offset; \
    ((struct aaudio_msg_header *) msg->data)->type = (typev); \
    ((struct aaudio_msg_header *) msg->data)->device_id = (devid);
#define WRITE_START_COMMAND(devid) WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_COMMAND, devid)
#define WRITE_START_RESPONSE() WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_RESPONSE, 0)
#define WRITE_START_NOTIFICATION() WRITE_START_OF_TYPE(AAUDIO_MSG_TYPE_NOTIFICATION, 0)
#define WRITE_VAL(type, value) { *((type *) ((u8 *) msg->data + offset)) = value; offset += sizeof(value); }
#define WRITE_BASE(type) WRITE_VAL(u32, type) WRITE_VAL(u32, 0)
#define WRITE_END() { msg->size = offset; }

void aaudio_msg_write_start_io(struct aaudio_msg *msg, aaudio_device_id_t dev)
{
    WRITE_START_COMMAND(dev);
    WRITE_BASE(AAUDIO_MSG_START_IO);
    WRITE_END();
}

void aaudio_msg_write_set_remote_access(struct aaudio_msg *msg, u64 mode)
{
    WRITE_START_COMMAND(0);
    WRITE_BASE(AAUDIO_MSG_SET_REMOTE_ACCESS);
    WRITE_VAL(u64, mode);
    WRITE_END();
}

void aaudio_msg_write_alive_notification(struct aaudio_msg *msg, u32 proto_ver, u32 msg_ver)
{
    WRITE_START_NOTIFICATION();
    WRITE_BASE(AAUDIO_MSG_NOTIFICATION_ALIVE);
    WRITE_VAL(u32, proto_ver);
    WRITE_VAL(u32, msg_ver);
    WRITE_END();
}

void aaudio_msg_write_update_timestamp_response(struct aaudio_msg *msg)
{
    WRITE_START_RESPONSE();
    WRITE_BASE(AAUDIO_MSG_UPDATE_TIMESTAMP_RESPONSE);
    WRITE_END();
}

#define CMD_SHARED_VARS \
    int status = 0; \
    struct aaudio_send_ctx sctx; \
    struct aaudio_msg reply = aaudio_reply_alloc();
#define CMD_SEND_REQUEST(fn, ...) \
    if ((status = aaudio_send_cmd_sync(a, &sctx, &reply, 500, fn, __VA_ARGS__))) \
        return status;
#define CMD_DEF_SHARED_AND_SEND(fn, ...) \
    CMD_SHARED_VARS \
    CMD_SEND_REQUEST(fn, ##__VA_ARGS__);
#define CMD_HNDL_REPLY_AND_FREE(fn, ...) \
    status = fn(&reply, ##__VA_ARGS__); \
    aaudio_reply_free(&reply); \
    return status;

int aaudio_cmd_set_remote_access(struct aaudio_device *a, u64 mode)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_set_remote_access, mode);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_set_remote_access_response);
}
int aaudio_cmd_start_io(struct aaudio_device *a, aaudio_device_id_t devid)
{
    CMD_DEF_SHARED_AND_SEND(aaudio_msg_write_start_io, devid);
    CMD_HNDL_REPLY_AND_FREE(aaudio_msg_read_start_io_response);
}
