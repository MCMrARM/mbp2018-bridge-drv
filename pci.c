#include "pci.h"
#include <linux/module.h>

static dev_t bce_chrdev;
static struct class *bce_class;

static int bce_create_command_queues(struct bce_device *bce);
static void bce_free_command_queues(struct bce_device *bce);
static irqreturn_t bce_handle_mb_irq(int irq, void *dev);
static irqreturn_t bce_handle_dma_irq(int irq, void *dev);
static int bce_fw_version_handshake(struct bce_device *bce);
static int bce_register_command_queue(struct bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq);

static int bce_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    struct bce_device *bce = NULL;
    int status = 0;
    int nvec;

    pr_info("bce: capturing our device\n");

    if (pci_enable_device(dev))
        return -ENODEV;
    if (pci_request_regions(dev, "bce")) {
        status = -ENODEV;
        goto fail;
    }
    nvec = pci_alloc_irq_vectors(dev, 1, 8, PCI_IRQ_MSI);
    if (nvec < 5) {
        status = -EINVAL;
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
        dev_warn(&dev->dev, "bce: Failed to pci_iomap required regions\n");
        goto fail;
    }

    bce_mailbox_init(&bce->mbox, bce->reg_mem_mb);

    if ((status = pci_request_irq(dev, 0, bce_handle_mb_irq, NULL, dev, "bce_mbox")))
        goto fail;
    if ((status = pci_request_irq(dev, 4, bce_handle_dma_irq, NULL, dev, "bce_dma")))
        goto fail_interrupt_0;

    if ((status = dma_set_mask_and_coherent(&dev->dev, DMA_BIT_MASK(37)))) {
        dev_warn(&dev->dev, "dma: Setting mask failed\n");
        goto fail_interrupt;
    }

    if ((status = bce_fw_version_handshake(bce)))
        goto fail_interrupt;
    pr_info("bce: handshake done\n");

    if ((status = bce_create_command_queues(bce))) {
        pr_info("bce: Creating command queues failed\n");
        goto fail_interrupt;
    }

    return 0;

fail_interrupt:
    pci_free_irq(dev, 4, dev);
fail_interrupt_0:
    pci_free_irq(dev, 0, dev);
fail:
    if (bce && bce->dev)
        device_destroy(bce_class, bce->devt);
    kfree(bce);

    if (!IS_ERR_OR_NULL(bce->reg_mem_mb))
        pci_iounmap(dev, bce->reg_mem_mb);
    if (!IS_ERR_OR_NULL(bce->reg_mem_dma))
        pci_iounmap(dev, bce->reg_mem_dma);

    pci_free_irq_vectors(dev);
    pci_release_regions(dev);
    pci_disable_device(dev);

    if (!status)
        status = -EINVAL;
    return status;
}

static int bce_create_command_queues(struct bce_device *bce)
{
    int status;
    struct bce_queue_memcfg *cfg;

    bce->cmd_cq = bce_create_cq(bce, 0, 0x20);
    bce->cmd_sq = bce_create_sq(bce, 1, BCE_CMD_SIZE, 0x20, NULL);
    if (bce->cmd_cq == NULL || bce->cmd_sq == NULL) {
        status = -ENOMEM;
        goto err;
    }
    bce->queues[0] = (struct bce_queue *) bce->cmd_cq;
    bce->queues[1] = (struct bce_queue *) bce->cmd_sq;

    cfg = kzalloc(sizeof(struct bce_queue_memcfg), GFP_KERNEL);
    if (!cfg) {
        status = -ENOMEM;
        goto err;
    }
    bce_get_cq_memcfg(bce->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, false)))
        goto err;
    bce_get_sq_memcfg(bce->cmd_sq, bce->cmd_cq, cfg);
    if ((status = bce_register_command_queue(bce, cfg, true)))
        goto err;
    kfree(cfg);

    return 0;

err:
    if (bce->cmd_cq)
        bce_destroy_cq(bce, bce->cmd_cq);
    if (bce->cmd_sq)
        bce_destroy_sq(bce, bce->cmd_sq);
    return status;
}

static void bce_free_command_queues(struct bce_device *bce)
{
    bce_destroy_cq(bce, bce->cmd_cq);
    bce_destroy_sq(bce, bce->cmd_sq);
    bce->cmd_cq = NULL;
    bce->queues[0] = NULL;
}

static irqreturn_t bce_handle_mb_irq(int irq, void *dev)
{
    struct bce_device *bce = pci_get_drvdata(dev);
    bce_mailbox_handle_interrupt(&bce->mbox);
    return IRQ_HANDLED;
}

static irqreturn_t bce_handle_dma_irq(int irq, void *dev)
{
    int i;
    struct bce_device *bce = pci_get_drvdata(dev);
    for (i = 0; i < BCE_MAX_QUEUE_COUNT; i++)
        if (bce->queues[i] && bce->queues[i]->type == BCE_QUEUE_CQ)
            bce_handle_cq_completions(bce, (struct bce_queue_cq *) bce->queues[i]);
    return IRQ_HANDLED;
}

static int bce_fw_version_handshake(struct bce_device *bce)
{
    u64 result;
    int status;

    if ((status = bce_mailbox_send(&bce->mbox, BCE_MB_MSG(BCE_MB_SET_FW_PROTOCOL_VERSION, BC_PROTOCOL_VERSION),
            &result)))
        return status;
    if (BCE_MB_TYPE(result) != BCE_MB_SET_FW_PROTOCOL_VERSION ||
        BCE_MB_VALUE(result) != BC_PROTOCOL_VERSION) {
        pr_err("bce: FW version handshake failed %x:%llx\n", BCE_MB_TYPE(result), BCE_MB_VALUE(result));
        return -EINVAL;
    }
    return 0;
}

static int bce_register_command_queue(struct bce_device *bce, struct bce_queue_memcfg *cfg, int is_sq)
{
    int status;
    int cmd_type;
    u64 result;
    // OS X uses an bidirectional direction, but that's not really needed
    dma_addr_t a = dma_map_single(&bce->pci->dev, cfg, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (dma_mapping_error(&bce->pci->dev, a))
        return -ENOMEM;
    cmd_type = is_sq ? BCE_MB_REGISTER_COMMAND_SQ : BCE_MB_REGISTER_COMMAND_CQ;
    status = bce_mailbox_send(&bce->mbox, BCE_MB_MSG(cmd_type, a), &result);
    dma_unmap_single(&bce->pci->dev, a, sizeof(struct bce_queue_memcfg), DMA_TO_DEVICE);
    if (status)
        return status;
    if (BCE_MB_TYPE(result) != BCE_MB_SET_FW_PROTOCOL_VERSION)
        return -EINVAL;
    return 0;
}

static void bce_remove(struct pci_dev *dev)
{
    struct bce_device *bce = pci_get_drvdata(dev);

    pci_free_irq(dev, 0, dev);
    pci_free_irq(dev, 4, dev);
    bce_free_command_queues(bce);
    pci_iounmap(dev, bce->reg_mem_mb);
    pci_iounmap(dev, bce->reg_mem_dma);
    device_destroy(bce_class, bce->devt);
    pci_free_irq_vectors(dev);
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

    return 0;

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