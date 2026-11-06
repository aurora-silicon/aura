/* SPDX-License-Identifier: MIT AND GPL-2.0-only */
/* One image carries both USB protocol engines; the PHY selects one at runtime. */
#include "usb_dwc3.h"
#include "malloc.h"

typedef struct dwc3_hs_dev dwc3_hs_dev_t;
dwc3_hs_dev_t *usb_dwc3_hs_init(uintptr_t regs, dart_dev_t *dart);
void usb_dwc3_hs_shutdown(dwc3_hs_dev_t *dev);
void usb_dwc3_hs_handle_events(dwc3_hs_dev_t *dev);
ssize_t usb_dwc3_hs_can_read(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc3_hs_can_write(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe);
u8 usb_dwc3_hs_getbyte(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc3_hs_putbyte(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);
size_t usb_dwc3_hs_read(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc3_hs_write(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc3_hs_queue(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc3_hs_flush(dwc3_hs_dev_t *dev, cdc_acm_pipe_id_t pipe);
#ifdef AURA_WINDOWS
ssize_t usb_dwc3_hs_vuart_capacity(dwc3_hs_dev_t *dev);
size_t usb_dwc3_hs_vuart_write(dwc3_hs_dev_t *dev, const void *buf, size_t count);
void usb_dwc3_hs_vuart_poll(dwc3_hs_dev_t *dev);
#endif
typedef struct dwc3_ss_dev dwc3_ss_dev_t;
dwc3_ss_dev_t *usb_dwc3_ss_init(uintptr_t regs, dart_dev_t *dart);
void usb_dwc3_ss_shutdown(dwc3_ss_dev_t *dev);
void usb_dwc3_ss_handle_events(dwc3_ss_dev_t *dev);
ssize_t usb_dwc3_ss_can_read(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc3_ss_can_write(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe);
u8 usb_dwc3_ss_getbyte(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc3_ss_putbyte(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);
size_t usb_dwc3_ss_read(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc3_ss_write(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc3_ss_queue(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc3_ss_flush(dwc3_ss_dev_t *dev, cdc_acm_pipe_id_t pipe);
#ifdef AURA_WINDOWS
ssize_t usb_dwc3_ss_vuart_capacity(dwc3_ss_dev_t *dev);
size_t usb_dwc3_ss_vuart_write(dwc3_ss_dev_t *dev, const void *buf, size_t count);
void usb_dwc3_ss_vuart_poll(dwc3_ss_dev_t *dev);
#endif

struct dwc3_dev {
    bool superspeed;
    union {
        dwc3_hs_dev_t *hs;
        dwc3_ss_dev_t *ss;
    } engine;
};

#ifdef AURA_WINDOWS
ssize_t usb_dwc3_vuart_capacity(dwc3_dev_t *dev)
{
    if (!dev)
        return -1;
    return dev->superspeed ? usb_dwc3_ss_vuart_capacity(dev->engine.ss)
                           : usb_dwc3_hs_vuart_capacity(dev->engine.hs);
}

size_t usb_dwc3_vuart_write(dwc3_dev_t *dev, const void *buf, size_t count)
{
    if (!dev)
        return count;
    return dev->superspeed ? usb_dwc3_ss_vuart_write(dev->engine.ss, buf, count)
                           : usb_dwc3_hs_vuart_write(dev->engine.hs, buf, count);
}

void usb_dwc3_vuart_poll(dwc3_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->superspeed)
        usb_dwc3_ss_vuart_poll(dev->engine.ss);
    else
        usb_dwc3_hs_vuart_poll(dev->engine.hs);
}
#endif

dwc3_dev_t *usb_dwc3_init_mode(uintptr_t regs, dart_dev_t *dart, bool superspeed)
{
    dwc3_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;
    dev->superspeed = superspeed;
    if (superspeed) {
        dev->engine.ss = usb_dwc3_ss_init(regs, dart);
        if (dev->engine.ss)
            return dev;
    } else {
        dev->engine.hs = usb_dwc3_hs_init(regs, dart);
        if (dev->engine.hs)
            return dev;
    }
    free(dev);
    return NULL;
}

dwc3_dev_t *usb_dwc3_init(uintptr_t regs, dart_dev_t *dart)
{
    return usb_dwc3_init_mode(regs, dart, false);
}

void usb_dwc3_shutdown(dwc3_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->superspeed)
        usb_dwc3_ss_shutdown(dev->engine.ss);
    else
        usb_dwc3_hs_shutdown(dev->engine.hs);
    free(dev);
}

void usb_dwc3_handle_events(dwc3_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->superspeed)
        usb_dwc3_ss_handle_events(dev->engine.ss);
    else
        usb_dwc3_hs_handle_events(dev->engine.hs);
}

ssize_t usb_dwc3_can_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_can_read(dev->engine.ss, pipe)
                           : usb_dwc3_hs_can_read(dev->engine.hs, pipe);
}

bool usb_dwc3_can_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_can_write(dev->engine.ss, pipe)
                           : usb_dwc3_hs_can_write(dev->engine.hs, pipe);
}

u8 usb_dwc3_getbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_getbyte(dev->engine.ss, pipe)
                           : usb_dwc3_hs_getbyte(dev->engine.hs, pipe);
}

void usb_dwc3_putbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte)
{
    if (!dev)
        return;
    if (dev->superspeed)
        usb_dwc3_ss_putbyte(dev->engine.ss, pipe, byte);
    else
        usb_dwc3_hs_putbyte(dev->engine.hs, pipe, byte);
}

size_t usb_dwc3_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_read(dev->engine.ss, pipe, buf, count)
                           : usb_dwc3_hs_read(dev->engine.hs, pipe, buf, count);
}

size_t usb_dwc3_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_write(dev->engine.ss, pipe, buf, count)
                           : usb_dwc3_hs_write(dev->engine.hs, pipe, buf, count);
}

size_t usb_dwc3_queue(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    if (!dev)
        return 0;
    return dev->superspeed ? usb_dwc3_ss_queue(dev->engine.ss, pipe, buf, count)
                           : usb_dwc3_hs_queue(dev->engine.hs, pipe, buf, count);
}

void usb_dwc3_flush(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev)
        return;
    if (dev->superspeed)
        usb_dwc3_ss_flush(dev->engine.ss, pipe);
    else
        usb_dwc3_hs_flush(dev->engine.hs, pipe);
}
