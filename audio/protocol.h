#ifndef AAUDIO_PROTOCOL_H
#define AAUDIO_PROTOCOL_H

#include <linux/types.h>

typedef u64 aaudio_device_id_t;

struct aaudio_msg {
    void *data;
    size_t size;
};

struct aaudio_msg_header {
    char tag[4];
    u8 type;
    aaudio_device_id_t device_id; // Idk, use zero for commands?
};
enum {
    AAUDIO_MSG_TYPE_COMMAND = 1,
    AAUDIO_MSG_TYPE_RESPONSE = 2,
    AAUDIO_MSG_TYPE_NOTIFICATION = 3
};

enum {
    AAUDIO_MSG_START_IO = 0,
    AAUDIO_MSG_SET_REMOTE_ACCESS = 2,

    AAUDIO_MSG_NOTIFICATION_ALIVE = 100
};


void aaudio_msg_set_alive_notification(struct aaudio_msg *msg, u32 proto_ver, u32 msg_ver);

enum {
    AAUDIO_REMOTE_ACCESS_OFF = 0,
    AAUDIO_REMOTE_ACCESS_ON = 2
};
void aaudio_msg_set_remote_access(struct aaudio_msg *msg, u64 mode);

void aaudio_msg_start_io(struct aaudio_msg *msg, aaudio_device_id_t dev);

#endif //AAUDIO_PROTOCOL_H
