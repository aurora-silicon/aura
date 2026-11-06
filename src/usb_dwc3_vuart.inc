/* SPDX-License-Identifier: MIT */
/* Secondary CDC FIFO operations. Included in both DWC3 protocol engines. */
ssize_t usb_dwc3_vuart_capacity(dwc3_dev_t *dev)
{
    if (!dev || dev->failed || !dev->pipe[CDC_ACM_PIPE_1].ready ||
        !dev->pipe[CDC_ACM_PIPE_1].device2host)
        return -1;
    /* ringbuffer_get_free() counts the slot reserved to distinguish empty/full. */
    size_t free = ringbuffer_get_free(dev->pipe[CDC_ACM_PIPE_1].device2host);
    return free ? (ssize_t)(free - 1) : 0;
}

size_t usb_dwc3_vuart_write(dwc3_dev_t *dev, const void *buf, size_t count)
{
    ssize_t capacity = usb_dwc3_vuart_capacity(dev);
    if (capacity < 0)
        return count; /* An absent debug console must not stall the guest. */
    if (count > (size_t)capacity)
        return 0;
    /* No host wait, event processing or endpoint command in the enqueue path. */
    return ringbuffer_write(buf, count, dev->pipe[CDC_ACM_PIPE_1].device2host);
}

void usb_dwc3_vuart_poll(dwc3_dev_t *dev)
{
    if (!dev)
        return;
    usb_dwc3_handle_events(dev);
    if (usb_dwc3_vuart_capacity(dev) >= 0)
        usb_dwc3_cdc_start_bulk_in_xfer(dev, dev->pipe[CDC_ACM_PIPE_1].ep_in);
}
