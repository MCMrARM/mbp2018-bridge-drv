#ifndef AAUDIO_H
#define AAUDIO_H

#include <linux/types.h>
#include "../pci.h"
#include "protocol_bce.h"

#define AAUDIO_SIG 0x19870423

#define AAUDIO_DEVICE_MAX_UID_LEN 128

struct snd_card;

struct __attribute__((packed)) __attribute__((aligned(4))) aaudio_buffer_struct_buffer {
    size_t address;
    size_t size;
    size_t pad[4];
};
struct aaudio_buffer_struct_stream {
    u8 num_buffers;
    struct aaudio_buffer_struct_buffer buffers[100];
    char filler[32];
};
struct aaudio_buffer_struct_device {
    char name[128];
    u8 num_input_streams;
    u8 num_output_streams;
    struct aaudio_buffer_struct_stream input_streams[5];
    struct aaudio_buffer_struct_stream output_streams[5];
    char filler[128];
};
struct aaudio_buffer_struct {
    u32 version;
    u32 signature;
    u32 flags;
    u8 num_devices;
    struct aaudio_buffer_struct_device devices[20];
};

struct aaudio_subdevice {
    struct list_head list;
    aaudio_device_id_t dev_id;
    u8 buf_id;
    char uid[AAUDIO_DEVICE_MAX_UID_LEN + 1];

    dma_addr_t buf_in_dma_addr;
    void *buf_in_ptr;
    size_t buf_in_size;
};

struct aaudio_device {
    struct pci_dev *pci;
    dev_t devt;
    struct device *dev;
    void __iomem *reg_mem_bs;
    dma_addr_t reg_mem_bs_dma;
    void __iomem *reg_mem_cfg;

    u32 __iomem *reg_mem_gpr;

    struct aaudio_buffer_struct *bs;

    struct bce_device *bce;
    struct aaudio_bce bcem;

    struct snd_card *card;

    struct list_head subdevice_list;

    struct completion remote_alive;
};

void aaudio_handle_notification(struct aaudio_device *a, struct aaudio_msg *msg);
void aaudio_handle_command(struct aaudio_device *a, struct aaudio_msg *msg);

int aaudio_module_init(void);
void aaudio_module_exit(void);

#endif //AAUDIO_H
