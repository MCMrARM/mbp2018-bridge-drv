#ifndef AAUDIO_DESCRIPTION_H
#define AAUDIO_DESCRIPTION_H

#include <linux/types.h>

struct aaudio_apple_description {
    double sample_rate;
    u32 format_id;
    u32 format_flags;
    u32 bytes_per_packet;
    u32 frames_per_packet;
    u32 bytes_per_frame;
    u32 channels_per_frame;
    u32 bits_per_channel;
    u32 reserved;
};

enum {
    AAUDIO_FORMAT_LPCM = 0x6c70636d  // 'lpcm'
};

#endif //AAUDIO_DESCRIPTION_H
