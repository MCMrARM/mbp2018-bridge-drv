#ifndef AAUDIO_PROTOCOL_H
#define AAUDIO_PROTOCOL_H

#include <linux/types.h>

struct aaudio_device;

typedef u64 aaudio_device_id_t;

struct aaudio_msg {
    void *data;
    size_t size;
};

struct __attribute__((packed)) aaudio_msg_header {
    char tag[4];
    u8 type;
    aaudio_device_id_t device_id; // Idk, use zero for commands?
};
struct __attribute__((packed)) aaudio_msg_base {
    u32 msg;
    u32 status;
};

enum {
    AAUDIO_MSG_TYPE_COMMAND = 1,
    AAUDIO_MSG_TYPE_RESPONSE = 2,
    AAUDIO_MSG_TYPE_NOTIFICATION = 3
};

enum {
    AAUDIO_MSG_START_IO = 0,
    AAUDIO_MSG_START_IO_RESPONSE = 1,
    AAUDIO_MSG_UPDATE_TIMESTAMP = 4,
    AAUDIO_MSG_SET_REMOTE_ACCESS = 32,
    AAUDIO_MSG_SET_REMOTE_ACCESS_RESPONSE = 33,
    AAUDIO_MSG_UPDATE_TIMESTAMP_RESPONSE = 34,

    AAUDIO_MSG_NOTIFICATION_ALIVE = 100,
    AAUDIO_MSG_NOTIFICATION_BOOT = 104
};

enum {
    AAUDIO_REMOTE_ACCESS_OFF = 0,
    AAUDIO_REMOTE_ACCESS_ON = 2
};

int aaudio_msg_read_base(struct aaudio_msg *msg, struct aaudio_msg_base *base);

int aaudio_msg_read_start_io_response(struct aaudio_msg *msg);
int aaudio_msg_read_update_timestamp(struct aaudio_msg *msg, aaudio_device_id_t *devid,
        u64 *timestamp, u64 *update_seed);
int aaudio_msg_read_set_remote_access_response(struct aaudio_msg *msg);

void aaudio_msg_write_start_io(struct aaudio_msg *msg, aaudio_device_id_t dev);
void aaudio_msg_write_set_remote_access(struct aaudio_msg *msg, u64 mode);
void aaudio_msg_write_alive_notification(struct aaudio_msg *msg, u32 proto_ver, u32 msg_ver);
void aaudio_msg_write_update_timestamp_response(struct aaudio_msg *msg);


int aaudio_cmd_set_remote_access(struct aaudio_device *a, u64 mode);
int aaudio_cmd_start_io(struct aaudio_device *a, aaudio_device_id_t devid);



#endif //AAUDIO_PROTOCOL_H
