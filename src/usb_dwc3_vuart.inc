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
    if (usb_dwc3_vuart_capacity(dev) >= 0) {
        usb_dwc3_cdc_start_bulk_in_xfer(dev, dev->pipe[CDC_ACM_PIPE_1].ep_in);
        /* A not-ready event consumed while DTR was closed need not recur.
         * An empty UART RX ring cannot reach the normal read-side rearm. */
        usb_dwc3_cdc_start_bulk_out_xfer(dev, dev->pipe[CDC_ACM_PIPE_1].ep_out);
    }
}

/*
 * G5b C-3: the proxy console's write.  A full ring (the Mac asleep, nobody reading) used to keep
 * usb_dwc3_queue() spinning in handle_events (with no deadline on the HS engine) inside the host
 * tick, holding the big lock: d5c's 705 s freeze.  This takes what fits now and starts the
 * bulk-in transfer as usb_dwc3_write() did; no event processing, no wait for the host.  The
 * console keeps what was not taken in its own buffer and counts what it has to drop.
 */
size_t usb_dwc3_console_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    if (!dev || dev->failed || !dev->pipe[pipe].ready || !dev->pipe[pipe].device2host)
        return 0;
    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    /* ringbuffer_get_free() counts the slot reserved to distinguish empty/full. */
    size_t free = ringbuffer_get_free(device2host);
    size_t take = min(count, free ? free - 1 : 0);
    size_t wrote = take ? ringbuffer_write(buf, take, device2host) : 0;
    usb_dwc3_cdc_start_bulk_in_xfer(dev, dev->pipe[pipe].ep_in);
    return wrote;
}
