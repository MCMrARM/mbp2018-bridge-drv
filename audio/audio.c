#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/random.h>
#include "audio.h"

static dev_t aaudio_chrdev;
static struct class *aaudio_class;

static int aaudio_init(struct aaudio_device *a);

static int aaudio_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct aaudio_device *aaudio = NULL;
    int status = 0;

    pr_info("aaudio: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "aaudio")) {
        status = -ENODEV;
        goto fail;
    }

    aaudio = kzalloc(sizeof(struct aaudio_device), GFP_KERNEL);
    if (!aaudio) {
        status = -ENOMEM;
        goto fail;
    }

    aaudio->bce = global_bce;
    if (!aaudio->bce) {
        dev_warn(&dev->dev, "aaudio: No BCE available\n");
        status = -EINVAL;
        goto fail;
    }

    aaudio->pci = dev;
    pci_set_drvdata(dev, aaudio);

    aaudio->devt = aaudio_chrdev;
    aaudio->dev = device_create(aaudio_class, &dev->dev, aaudio->devt, NULL, "aaudio");
    if (IS_ERR_OR_NULL(aaudio->dev)) {
        status = PTR_ERR(aaudio_class);
        goto fail;
    }

    init_completion(&aaudio->remote_alive);

    dev_info(aaudio->dev, "aaudio: bs len = %llx\n", pci_resource_len(dev, 0));
    aaudio->reg_mem_bs = pci_iomap(dev, 0, 0);
    aaudio->reg_mem_cfg = pci_iomap(dev, 4, 0);

    aaudio->reg_mem_gpr = (u32 __iomem *) ((u8 __iomem *) aaudio->reg_mem_cfg + 0xC000);

    if (IS_ERR_OR_NULL(aaudio->reg_mem_bs) || IS_ERR_OR_NULL(aaudio->reg_mem_cfg)) {
        dev_warn(&dev->dev, "aaudio: Failed to pci_iomap required regions\n");
        goto fail;
    }

    if (aaudio_bce_init(aaudio)) {
        dev_warn(&dev->dev, "aaudio: Failed to init BCE command transport\n");
        goto fail;

    }
    aaudio_init(aaudio);

    return 0;

fail:
    if (aaudio && aaudio->dev)
        device_destroy(aaudio_class, aaudio->devt);
    kfree(aaudio);

    if (!IS_ERR_OR_NULL(aaudio->reg_mem_bs))
        pci_iounmap(dev, aaudio->reg_mem_bs);
    if (!IS_ERR_OR_NULL(aaudio->reg_mem_cfg))
        pci_iounmap(dev, aaudio->reg_mem_cfg);

    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static void aaudio_remove(struct pci_dev *dev)
{
    struct aaudio_device *aaudio = pci_get_drvdata(dev);

    pci_iounmap(dev, aaudio->reg_mem_bs);
    pci_iounmap(dev, aaudio->reg_mem_cfg);
    device_destroy(aaudio_class, aaudio->devt);
    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);
    kfree(aaudio);
}

static int aaudio_set_remote_access(struct aaudio_device *a, u64 mode);

static int aaudio_init(struct aaudio_device *a)
{
    int status;
    int i, j;
    u32 ver, sig, bs_base;
    struct aaudio_send_ctx sctx;

    ver = ioread32(&a->reg_mem_gpr[0]);
    if (ver < 3) {
        dev_err(a->dev, "aaudio: Bad GPR version (%u)", ver);
        return -EINVAL;
    }
    sig = ioread32(&a->reg_mem_gpr[1]);
    if (sig != AAUDIO_SIG) {
        dev_err(a->dev, "aaudio: Bad GPR sig (%x)", sig);
        return -EINVAL;
    }
    bs_base = ioread32(&a->reg_mem_gpr[2]);
    a->bs = (struct aaudio_buffer_struct *) ((u8 *) a->reg_mem_bs + bs_base);
    if (a->bs->signature != AAUDIO_SIG) {
        dev_err(a->dev, "aaudio: Bad BS sig (%x)", a->bs->signature);
        return -EINVAL;
    }
    dev_info(a->dev, "aaudio: BS ver = %i\n", a->bs->version);
    dev_info(a->dev, "aaudio: Num devices = %i\n", a->bs->num_devices);
    for (i = 0; i < a->bs->num_devices; i++) {
        dev_info(a->dev, "aaudio: Device %i %s\n", i, a->bs->devices[i].name);
        for (j = 0; j < a->bs->devices[i].num_input_streams; j++) {
            dev_info(a->dev, "aaudio: Device %i Stream %i: Input; Buffer Count = %i\n", i, j,
                    a->bs->devices[i].input_streams[j].num_buffers);
        }
        for (j = 0; j < a->bs->devices[i].num_output_streams; j++) {
            dev_info(a->dev, "aaudio: Device %i Stream %i: Output; Buffer Count = %i\n", i, j,
                     a->bs->devices[i].output_streams[j].num_buffers);
        }
    }

    if ((status = aaudio_send(a, &sctx, 500,
            aaudio_msg_set_alive_notification, 1, 3))) {
        pr_err("Sending alive notification failed\n");
        return status;
    }

    if (wait_for_completion_timeout(&a->remote_alive, msecs_to_jiffies(500)) == 0) {
        pr_err("Timed out waiting for remote\n");
        return -ETIMEDOUT;
    }
    dev_info(a->dev, "aaudio: Continuing init\n");

    if ((status = aaudio_set_remote_access(a, AAUDIO_REMOTE_ACCESS_ON))) {
        pr_err("Failed to set remote access\n");
        return status;
    }


    return 0;
}

static int aaudio_set_remote_access(struct aaudio_device *a, u64 mode)
{
    int status = 0;
    struct aaudio_send_ctx sctx;
    struct aaudio_msg reply;
    aaudio_reply_alloc(&reply);
    if ((status = aaudio_send_cmd_sync(a, &sctx, &reply, 500,
            aaudio_msg_set_remote_access, mode)))
        goto finish;
    status = aaudio_msg_get_remote_access_response(&reply);
finish:
    aaudio_reply_free(&reply);
    return status;
}

void aaudio_handle_notification(struct aaudio_device *a, struct aaudio_msg *msg)
{
    struct aaudio_send_ctx sctx;
    struct aaudio_msg_base base;
    if (aaudio_msg_get_base(msg, &base))
        return;
    switch (base.msg) {
        case AAUDIO_MSG_NOTIFICATION_BOOT:
            dev_info(a->dev, "Received boot notification from remote\n");

            /* Resend the alive notify */
            if (aaudio_send(a, &sctx, 500, aaudio_msg_set_alive_notification, 1, 3)) {
                pr_err("Sending alive notification failed\n");
            }
            break;
        case AAUDIO_MSG_NOTIFICATION_ALIVE:
            dev_info(a->dev, "Received alive notification from remote\n");
            complete_all(&a->remote_alive);
            break;
        default:
            dev_info(a->dev, "Unhandled notification %i", base.msg);
            break;
    }
}

static struct pci_device_id aaudio_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1803) },
        { 0, },
};

struct pci_driver aaudio_pci_driver = {
        .name = "aaudio",
        .id_table = aaudio_ids,
        .probe = aaudio_probe,
        .remove = aaudio_remove
};


int aaudio_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&aaudio_chrdev, 0, 1, "aaudio")))
        goto fail_chrdev;
    aaudio_class = class_create(THIS_MODULE, "aaudio");
    if (IS_ERR(aaudio_class)) {
        result = PTR_ERR(aaudio_class);
        goto fail_class;
    }
    
    result = pci_register_driver(&aaudio_pci_driver);
    if (result)
        goto fail_drv;
    return 0;

fail_drv:
    pci_unregister_driver(&aaudio_pci_driver);
fail_class:
    class_destroy(aaudio_class);
fail_chrdev:
    unregister_chrdev_region(aaudio_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}

void aaudio_module_exit(void)
{
    pci_unregister_driver(&aaudio_pci_driver);
    class_destroy(aaudio_class);
    unregister_chrdev_region(aaudio_chrdev, 1);
}