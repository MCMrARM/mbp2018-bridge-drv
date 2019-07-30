#ifndef AAUDIO_PCM_H
#define AAUDIO_PCM_H

#include <linux/types.h>

struct aaudio_apple_description;
struct snd_pcm_hardware;

int aaudio_create_hw_info(struct aaudio_apple_description *desc, struct snd_pcm_hardware *alsa_hw, size_t buf_size);

#endif //AAUDIO_PCM_H
