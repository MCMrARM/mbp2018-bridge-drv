#include "vhci.h"
#include "../pci.h"
#include "command.h"
#include <linux/usb.h>
#include <linux/usb/hcd.h>

static dev_t bce_vhci_chrdev;
static struct class *bce_vhci_class;
static const struct hc_driver bce_vhci_driver;

static int bce_vhci_create_event_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci);
static int bce_vhci_create_message_queues(struct bce_vhci *vhci);
static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci);

int bce_vhci_create(struct bce_device *dev, struct bce_vhci *vhci)
{
    int status;

    spin_lock_init(&vhci->hcd_spinlock);

    vhci->dev = dev;

    vhci->vdevt = bce_vhci_chrdev;
    vhci->vdev = device_create(bce_vhci_class, dev->dev, vhci->vdevt, NULL, "bce-vhci");
    if (IS_ERR_OR_NULL(vhci->vdev)) {
        status = PTR_ERR(vhci->vdev);
        goto fail_dev;
    }

    if ((status = bce_vhci_create_message_queues(vhci)))
        goto fail_mq;
    if ((status = bce_vhci_create_event_queues(vhci)))
        goto fail_eq;

    vhci->hcd = usb_create_hcd(&bce_vhci_driver, vhci->vdev, "bce-vhci");
    if (!vhci->hcd) {
        status = -ENOMEM;
        goto fail_hcd;
    }
    vhci->hcd->self.sysdev = &dev->pci->dev;
    vhci->hcd->self.uses_dma = 1;
    *((struct bce_vhci **) vhci->hcd->hcd_priv) = vhci;
    vhci->hcd->speed = HCD_USB2;

    if ((status = usb_add_hcd(vhci->hcd, 0, 0)))
        goto fail_hcd;

    return 0;

fail_hcd:
    bce_vhci_destroy_event_queues(vhci);
fail_eq:
    bce_vhci_destroy_message_queues(vhci);
fail_mq:
    device_destroy(bce_vhci_class, vhci->vdevt);
fail_dev:
    if (!status)
        status = -EINVAL;
    return status;
}

void bce_vhci_destroy(struct bce_vhci *vhci)
{
    usb_remove_hcd(vhci->hcd);
    bce_vhci_destroy_event_queues(vhci);
    bce_vhci_destroy_message_queues(vhci);
    device_destroy(bce_vhci_class, vhci->vdevt);
}

struct bce_vhci *bce_vhci_from_hcd(struct usb_hcd *hcd)
{
    return *((struct bce_vhci **) hcd->hcd_priv);
}

int bce_vhci_start(struct usb_hcd *hcd)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;
    u16 port_mask = 0;
    bce_vhci_port_t port_no = 0;
    if ((status = bce_vhci_cmd_controller_enable(&vhci->cq, 1, &port_mask)))
        return status;
    vhci->port_mask = port_mask;
    vhci->port_power_mask = 0;
    if ((status = bce_vhci_cmd_controller_start(&vhci->cq)))
        return status;
    port_mask = vhci->port_mask;
    while (port_mask) {
        port_no += 1;
        port_mask >>= 1;
    }
    vhci->port_count = port_no;
    return 0;
}

static int bce_vhci_hub_status_data(struct usb_hcd *hcd, char *buf)
{
    return 0;
}

static int bce_vhci_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue, u16 wIndex, char *buf, u16 wLength)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    int status;
    struct usb_hub_descriptor *hd;
    struct usb_hub_status *hs;
    struct usb_port_status *ps;
    u32 port_status;
    // pr_info("bce-vhci: bce_vhci_hub_control %x %i %i [bufl=%i]\n", typeReq, wValue, wIndex, wLength);
    if (typeReq == GetHubDescriptor && wLength >= sizeof(struct usb_hub_descriptor)) {
        hd = (struct usb_hub_descriptor *) buf;
        memset(hd, 0, sizeof(*hd));
        hd->bDescLength = sizeof(struct usb_hub_descriptor);
        hd->bDescriptorType = USB_DT_HUB;
        hd->bNbrPorts = (u8) vhci->port_count;
        hd->wHubCharacteristics = HUB_CHAR_INDV_PORT_LPSM | HUB_CHAR_INDV_PORT_OCPM;
        hd->bPwrOn2PwrGood = 0;
        hd->bHubContrCurrent = 0;
        return 0;
    } else if (typeReq == GetHubStatus && wLength >= sizeof(struct usb_hub_status)) {
        hs = (struct usb_hub_status *) buf;
        memset(hs, 0, sizeof(*hs));
        hs->wHubStatus = 0;
        hs->wHubChange = 0;
        return 0;
    } else if (typeReq == GetPortStatus && wLength >= 4 /* usb 2.0 */) {
        ps = (struct usb_port_status *) buf;
        ps->wPortStatus = 0;
        ps->wPortChange = 0;

        if ((status = bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0, &port_status)))
            return status;

        if (vhci->port_power_mask & BIT(wIndex))
            ps->wPortStatus |= USB_PORT_STAT_POWER;

        if (port_status & 16)
            ps->wPortStatus |= USB_PORT_STAT_ENABLE | USB_PORT_STAT_HIGH_SPEED;
        if (port_status & 4)
            ps->wPortStatus |= USB_PORT_STAT_CONNECTION;
        if (port_status & 2)
            ps->wPortStatus |= USB_PORT_STAT_OVERCURRENT;
        if (port_status & 8)
            ps->wPortStatus |= USB_PORT_STAT_RESET;
        if (port_status & 0x60)
            ps->wPortStatus |= USB_PORT_STAT_SUSPEND;

        if (port_status & 0x40000)
            ps->wPortChange |= USB_PORT_STAT_C_CONNECTION;

        pr_info("bce-vhci: Translated status %x to %x:%x\n", port_status, ps->wPortStatus, ps->wPortChange);
        return 0;
    } else if (typeReq == SetPortFeature) {
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_on(&vhci->cq, (u8) wIndex);
            /* As far as I am aware, power status is not part of the port status so store it separately */
            if (!status)
                vhci->port_power_mask |= BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_RESET) {
            /* The device does not support being reset more than once, so workaround it */
            if (vhci->port_reset_mask & BIT(wIndex))
                return 0;
            vhci->port_reset_mask |= BIT(wIndex);
            return bce_vhci_cmd_port_reset(&vhci->cq, (u8) wIndex, wValue);
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            /* TODO: Am I supposed to also suspend the endpoints? */
            pr_info("bce-vhci: Suspending port %i\n", wIndex);
            return bce_vhci_cmd_port_suspend(&vhci->cq, (u8) wIndex);
        }
    } else if (typeReq == ClearPortFeature) {
        if (wValue == USB_PORT_FEAT_ENABLE)
            return bce_vhci_cmd_port_disable(&vhci->cq, (u8) wIndex);
        if (wValue == USB_PORT_FEAT_POWER) {
            status = bce_vhci_cmd_port_power_off(&vhci->cq, (u8) wIndex);
            if (!status)
                vhci->port_power_mask &= ~BIT(wIndex);
            return status;
        }
        if (wValue == USB_PORT_FEAT_C_CONNECTION)
            return bce_vhci_cmd_port_status(&vhci->cq, (u8) wIndex, 0x40000, &port_status);
        if (wValue == USB_PORT_FEAT_C_RESET) { /* I don't think I can transfer it in any way */
            return 0;
        }
        if (wValue == USB_PORT_FEAT_SUSPEND) {
            pr_info("bce-vhci: Resuming port %i\n", wIndex);
            return bce_vhci_cmd_port_resume(&vhci->cq, (u8) wIndex);
        }
    }
    pr_err("bce-vhci: bce_vhci_hub_control unhandled request: %x %i %i [bufl=%i]\n", typeReq, wValue, wIndex, wLength);
    dump_stack();
    return -EIO;
}

static int bce_vhci_enable_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    struct bce_vhci_device *vdev;
    bce_vhci_device_t devid;
    pr_info("bce_vhci_enable_device\n");

    /* We need to early address the device */
    if (bce_vhci_cmd_device_create(&vhci->cq, udev->portnum, &devid))
        return -EIO;

    pr_info("bce_vhci_cmd_device_create %i -> %i\n", udev->portnum, devid);

    vdev = kzalloc(sizeof(struct bce_vhci_device), GFP_KERNEL);
    vhci->port_to_device[udev->portnum] = devid;
    vhci->devices[devid] = vdev;

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[0], &udev->ep0, devid, DMA_BIDIRECTIONAL);
    udev->ep0.hcpriv = &vdev->tq[0];
    vdev->tq_mask |= BIT(0);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &udev->ep0.desc);
    return 0;
}

static int bce_vhci_address_device(struct usb_hcd *hcd, struct usb_device *udev)
{
    return 0;
}

static int bce_vhci_check_bandwidth(struct usb_hcd *hcd, struct usb_device *udev)
{
    return 0;
}

static int bce_vhci_get_frame_number(struct usb_hcd *hcd)
{
    return 0;
}

static int bce_vhci_urb_enqueue(struct usb_hcd *hcd, struct urb *urb, gfp_t mem_flags)
{
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    pr_debug("bce_vhci_urb_enqueue %x\n", urb->ep->desc.bEndpointAddress);
    if (!q)
        return -ENOENT;
    return bce_vhci_urb_create(q, urb);
}

static int bce_vhci_urb_dequeue(struct usb_hcd *hcd, struct urb *urb, int status)
{
    int retval;
    struct bce_vhci_transfer_queue *q = urb->ep->hcpriv;
    if (!q)
        return -ENOENT;
    pr_info("bce_vhci_urb_dequeue %x\n", urb->ep->desc.bEndpointAddress);
    mutex_lock(&q->state_change_mutex);
    bce_vhci_transfer_queue_pause(q);
    retval = bce_vhci_urb_cancel(q, urb, status);
    bce_vhci_transfer_queue_resume(q);
    mutex_unlock(&q->state_change_mutex);
    return retval;
}

static u8 bce_vhci_endpoint_index(u8 addr)
{
    if (addr & 0x80)
        return (u8) (0x10 + (addr & 0xf));
    return (u8) (addr & 0xf);
}

static int bce_vhci_add_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_device *vdev = vhci->devices[devid];
    pr_info("bce_vhci_add_endpoint %x/%x:%x\n", udev->portnum, devid, endp_index);

    if (udev->bus->root_hub == udev) /* The USB hub */
        return 0;
    if (vdev == NULL)
        return -ENODEV;
    if (vdev->tq_mask & BIT(endp_index)) {
        endp->hcpriv = &vdev->tq[endp_index];
        return 0;
    }

    bce_vhci_create_transfer_queue(vhci, &vdev->tq[endp_index], endp, devid,
            usb_endpoint_dir_in(&endp->desc) ? DMA_FROM_DEVICE : DMA_TO_DEVICE);
    endp->hcpriv = &vdev->tq[endp_index];
    vdev->tq_mask |= BIT(endp_index);

    bce_vhci_cmd_endpoint_create(&vhci->cq, devid, &endp->desc);
    return 0;
}

static int bce_vhci_drop_endpoint(struct usb_hcd *hcd, struct usb_device *udev, struct usb_host_endpoint *endp)
{
    u8 endp_index = bce_vhci_endpoint_index(endp->desc.bEndpointAddress);
    struct bce_vhci *vhci = bce_vhci_from_hcd(hcd);
    bce_vhci_device_t devid = vhci->port_to_device[udev->portnum];
    struct bce_vhci_transfer_queue *q = endp->hcpriv;
    struct bce_vhci_device *vdev = vhci->devices[devid];
    pr_info("bce_vhci_drop_endpoint %x:%x\n", udev->portnum, endp_index);
    if (!q) {
        if (vdev && vdev->tq_mask & BIT(endp_index)) {
            pr_err("something deleted the hcpriv?\n");
            q = &vdev->tq[endp_index];
        } else {
            return 0;
        }
    }
    /*
    bce_vhci_cmd_endpoint_destroy(&vhci->cq, devid, (u8) (endp->desc.bEndpointAddress & 0x8F));
    vhci->devices[devid]->tq_mask &= ~BIT(endp_index);
    bce_vhci_destroy_transfer_queue(vhci, q);
    */
    return 0;
}

static int bce_vhci_create_message_queues(struct bce_vhci *vhci)
{
    if (bce_vhci_message_queue_create(vhci, &vhci->msg_commands, "VHC1HostCommands") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_system, "VHC1HostSystemEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_isochronous, "VHC1HostIsochronousEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_interrupt, "VHC1HostInterruptEvents") ||
        bce_vhci_message_queue_create(vhci, &vhci->msg_asynchronous, "VHC1HostAsynchronousEvents")) {
        bce_vhci_destroy_message_queues(vhci);
        return -EINVAL;
    }
    spin_lock_init(&vhci->msg_asynchronous_lock);
    bce_vhci_command_queue_create(&vhci->cq, &vhci->msg_commands);
    return 0;
}

static void bce_vhci_destroy_message_queues(struct bce_vhci *vhci)
{
    bce_vhci_command_queue_destroy(&vhci->cq);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_commands);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_system);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_isochronous);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_interrupt);
    bce_vhci_message_queue_destroy(vhci, &vhci->msg_asynchronous);
}

static void bce_vhci_handle_firmware_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);
static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);
static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg);

static int bce_vhci_create_event_queues(struct bce_vhci *vhci)
{
    vhci->ev_cq = bce_create_cq(vhci->dev, 0x100);
    if (!vhci->ev_cq)
        return -EINVAL;
#define CREATE_EVENT_QUEUE(field, name, cb) bce_vhci_event_queue_create(vhci, &vhci->field, name, cb)
    if (CREATE_EVENT_QUEUE(ev_commands,     "VHC1FirmwareCommands",           bce_vhci_handle_firmware_event) ||
        CREATE_EVENT_QUEUE(ev_system,       "VHC1FirmwareSystemEvents",       bce_vhci_handle_system_event) ||
        CREATE_EVENT_QUEUE(ev_isochronous,  "VHC1FirmwareIsochronousEvents",  bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_interrupt,    "VHC1FirmwareInterruptEvents",    bce_vhci_handle_usb_event) ||
        CREATE_EVENT_QUEUE(ev_asynchronous, "VHC1FirmwareAsynchronousEvents", bce_vhci_handle_usb_event)) {
        bce_vhci_destroy_event_queues(vhci);
        return -EINVAL;
    }
#undef CREATE_EVENT_QUEUE
    return 0;
}

static void bce_vhci_destroy_event_queues(struct bce_vhci *vhci)
{
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_commands);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_system);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_isochronous);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_interrupt);
    bce_vhci_event_queue_destroy(vhci, &vhci->ev_asynchronous);
    if (vhci->ev_cq)
        bce_destroy_cq(vhci->dev, vhci->ev_cq);
}

static void bce_vhci_handle_firmware_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    //
}

static void bce_vhci_handle_system_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    if (msg->cmd & 0x8000)
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
}

static void bce_vhci_handle_usb_event(struct bce_vhci_event_queue *q, struct bce_vhci_message *msg)
{
    bce_vhci_device_t devid;
    u8 endp;
    struct bce_vhci_device *dev;
    if (msg->cmd & 0x8000)
        bce_vhci_command_queue_deliver_completion(&q->vhci->cq, msg);
    if (msg->cmd == 0x1000 || msg->cmd == 0x1005) {
        devid = (bce_vhci_device_t) (msg->param1 & 0xff);
        endp = bce_vhci_endpoint_index((u8) ((msg->param1 >> 8) & 0xf));
        dev = q->vhci->devices[devid];
        if (!dev || (dev->tq_mask & BIT(endp)) == 0) {
            pr_err("bce-vhci: Didn't find destination for transfer queue event\n");
            return;
        }
        bce_vhci_transfer_queue_event(&dev->tq[endp], msg);
    }
}



static const struct hc_driver bce_vhci_driver = {
        .description = "bce-vhci",
        .product_desc = "BCE VHCI Host Controller",
        .hcd_priv_size = sizeof(struct bce_vhci *),

        .flags = HCD_USB2,

        .start = bce_vhci_start,
        .hub_status_data = bce_vhci_hub_status_data,
        .hub_control = bce_vhci_hub_control,
        .urb_enqueue = bce_vhci_urb_enqueue,
        .urb_dequeue = bce_vhci_urb_dequeue,
        .enable_device = bce_vhci_enable_device,
        .address_device = bce_vhci_address_device,
        .add_endpoint = bce_vhci_add_endpoint,
        .drop_endpoint = bce_vhci_drop_endpoint,
        .check_bandwidth = bce_vhci_check_bandwidth,
        .get_frame_number = bce_vhci_get_frame_number
};


int __init bce_vhci_module_init(void)
{
    int result;
    if ((result = alloc_chrdev_region(&bce_vhci_chrdev, 0, 1, "bce-vhci")))
        goto fail_chrdev;
    bce_vhci_class = class_create(THIS_MODULE, "bce-vhci");
    if (IS_ERR(bce_vhci_class)) {
        result = PTR_ERR(bce_vhci_class);
        goto fail_class;
    }
    return 0;

fail_class:
    class_destroy(bce_vhci_class);
fail_chrdev:
    unregister_chrdev_region(bce_vhci_chrdev, 1);
    if (!result)
        result = -EINVAL;
    return result;
}
void __exit bce_vhci_module_exit(void)
{
    class_destroy(bce_vhci_class);
    unregister_chrdev_region(bce_vhci_chrdev, 1);
}
