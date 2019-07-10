#include "pci.h"
#include <linux/module.h>

static dev_t bce_chrdev;
static struct class *bce_class;

static irqreturn_t bce_handle_mb_irq(int irq, void *dev);
static int bce_fw_version_handshake(struct bce_device *bce);

static int bce_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct bce_device *bce = NULL;
    int status = 0;

    pr_info("bce: capturing our device");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "bce")) {
        status = -ENODEV;
        goto fail;
    }

    bce = kzalloc(sizeof(struct bce_device), GFP_KERNEL);
    if (!bce) {
        status = -ENOMEM;
        goto fail;
    }

    bce->pci = dev;
    pci_set_drvdata(dev, bce);

    bce->devt = bce_chrdev;
    bce->dev = device_create(bce_class, &dev->dev, bce->devt, NULL, "bce");
    if (IS_ERR_OR_NULL(bce->dev)) {
        status = PTR_ERR(bce_class);
        goto fail;
    }

    bce->reg_mem_mb = pci_iomap(dev, 4, 0);
    bce->reg_mem_dma = pci_iomap(dev, 2, 0);

    if (IS_ERR_OR_NULL(bce->reg_mem_mb) || IS_ERR_OR_NULL(bce->reg_mem_dma)) {
        pr_err("bce: Failed to pci_iomap required regions");
        goto fail;
    }

    bce_mailbox_init(&bce->mbox, bce->reg_mem_mb);

    request_irq(0, bce_handle_mb_irq, 0, "mailbox interrupt", dev);

    if ((status = bce_fw_version_handshake(bce)))
        goto fail;

    pr_info("bce: device probe success");

fail:
    if (bce && bce->dev)
        device_destroy(bce_class, bce->devt);
    kfree(bce);

    if (!IS_ERR_OR_NULL(bce->reg_mem_mb))
        pci_iounmap(dev, bce->reg_mem_mb);
    if (!IS_ERR_OR_NULL(bce->reg_mem_dma))
        pci_iounmap(dev, bce->reg_mem_dma);

    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static irqreturn_t bce_handle_mb_irq(int irq, void *dev)
{
    struct bce_device *bce = pci_get_drvdata(dev);
    bce_mailbox_handle_interrupt(&bce->mbox);
    return IRQ_HANDLED;
}

static int bce_fw_version_handshake(struct bce_device *bce) {
    u64 result;
    int status;

    if ((status = bce_mailbox_send(&bce->mbox, BCE_MB_MSG(BCE_MB_SET_FW_PROTOCOL_VERSION, BC_PROTOCOL_VERSION),
            &result)))
        return status;
    if (BCE_MB_TYPE(result) != BCE_MB_SET_FW_PROTOCOL_VERSION ||
        BCE_MB_VALUE(result) != BC_PROTOCOL_VERSION) {
        pr_info("bce: FW version handshake failed %x:%llx", BCE_MB_TYPE(result), BCE_MB_VALUE(result));
        return -EINVAL;
    }
    return 0;
}

static void bce_remove(struct pci_dev *dev)
{
    struct bce_device *bce = pci_get_drvdata(dev);

    pci_iounmap(dev, bce->reg_mem_mb);
    pci_iounmap(dev, bce->reg_mem_dma);
    device_destroy(bce_class, bce->devt);
    pci_release_regions(dev);
    pci_disable_device(dev);
    kfree(bce);
}

static struct pci_device_id bce_ids[  ] = {
        { PCI_DEVICE(PCI_VENDOR_ID_APPLE, 0x1801) },
        { 0, },
};

struct pci_driver bce_pci_driver = {
        .name = "bce",
        .id_table = bce_ids,
        .probe = bce_probe,
        .remove = bce_remove
};


static int __init bce_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&bce_chrdev, 0, 1, "bce")))
        goto fail_chrdev;
    bce_class = class_create(THIS_MODULE, "bce");
    if (IS_ERR(bce_class)) {
        result = PTR_ERR(bce_class);
        goto fail_class;
    }
    result = pci_register_driver(&bce_pci_driver);
    if (result)
        goto fail_drv;

fail_drv:
    pci_unregister_driver(&bce_pci_driver);
fail_class:
    class_destroy(bce_class);
fail_chrdev:
    unregister_chrdev_region(bce_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}
static void __exit bce_module_exit(void)
{
    class_destroy(bce_class);
    unregister_chrdev_region(bce_chrdev, 1);
    pci_unregister_driver(&bce_pci_driver);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("MrARM");
MODULE_DESCRIPTION("BCE Driver");
MODULE_VERSION("0.01");
module_init(bce_module_init);
module_exit(bce_module_exit);