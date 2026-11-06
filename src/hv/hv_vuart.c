/* SPDX-License-Identifier: MIT */

#include "hv.h"
#include "aic.h"
#include "iodev.h"
#include "uart.h"
#include "uart_regs.h"
#include "usb.h"
#ifdef AURA_WINDOWS
#include "windows/serial_capture.h"
#endif

bool active = false;

u32 ucon = 0;
u32 utrstat = 0;
u32 ufstat = 0;

int vuart_irq = 0;
#ifdef AURA_WINDOWS
/* Rejected raw-CDC TX bytes when a guest ignores an attached full FIFO.
 * Independent from framed-capture loss accounting; inspect via the ELF symbol. */
u64 vuart_tx_overruns;
#endif

static void update_irq(void)
{
    ssize_t rx_queued;

#ifdef AURA_WINDOWS
    /* Generic I/O event dispatch also flushes every console, which can block
     * on an unrelated transport. Poll only the guest debug controller here. */
    iodev_lock(IODEV_USB_VUART);
    usb_dwc3_vuart_poll(iodev_get_opaque(IODEV_USB_VUART));
    iodev_unlock(IODEV_USB_VUART);
#else
    iodev_handle_events(IODEV_USB_VUART);
#endif

    bool tx_ready = true;
#ifdef AURA_WINDOWS
    tx_ready = iodev_can_write(IODEV_USB_VUART);
#endif
    if (tx_ready)
        utrstat |= UTRSTAT_TXBE | UTRSTAT_TXE;
    else
        utrstat &= ~(UTRSTAT_TXBE | UTRSTAT_TXE | UTRSTAT_TXTHRESH);
    utrstat &= ~UTRSTAT_RXD;

    u32 tx_status = tx_ready ? 0 : UFSTAT_TXFULL | FIELD_PREP(UFSTAT_TXCNT, 15);
    ufstat = tx_status;
    if ((rx_queued = iodev_can_read(IODEV_USB_VUART))) {
        utrstat |= UTRSTAT_RXD;
        if (rx_queued > 15)
            ufstat = tx_status | FIELD_PREP(UFSTAT_RXCNT, 15) | UFSTAT_RXFULL;
        else
            ufstat = tx_status | FIELD_PREP(UFSTAT_RXCNT, rx_queued);

        if (FIELD_GET(UCON_RXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_RXTO_ENA) {
            utrstat |= UTRSTAT_RXTO;
        }
    }

    if (tx_ready && FIELD_GET(UCON_TXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_TXTHRESH_ENA) {
        utrstat |= UTRSTAT_TXTHRESH;
    }

    if (vuart_irq) {
        uart_clear_irqs();
        if (utrstat & (UTRSTAT_TXTHRESH | UTRSTAT_RXTHRESH | UTRSTAT_RXTO)) {
            aic_set_sw(vuart_irq, true);
        } else {
            aic_set_sw(vuart_irq, false);
        }
    }

    //     printf("HV: vuart UTRSTAT=0x%x UFSTAT=0x%x UCON=0x%x\n", utrstat, ufstat, ucon);
}

static void handle_vuart_passthrough(uint8_t b)
{
    const char PREFIX[] = "HVLOG: ";
    static int state = 0;

    if (!PREFIX[state]) {
        if (b == '\r' || b == '\n') {
            printf("\n");
            state = 0;
            return;
        }
        printf("%c", b);
        return;
    }

    if (b == PREFIX[state])
        state++;
    else
        state = 0;

    if (!PREFIX[state])
        printf("%s", PREFIX);
}

static bool handle_vuart(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    UNUSED(width);

    addr &= 0xfff;

    update_irq();

    if (write) {
        //         printf("HV: vuart W 0x%lx <- 0x%lx (%d)\n", addr, *val, width);
        switch (addr) {
            case UCON:
                ucon = *val;
                break;
            case UTXH: {
                uint8_t b = *val;
#ifdef AURA_WINDOWS
                hv_windows_serial_putc(b);
                /* The Windows secondary CDC callback only enqueues once.
                 * Unbound/closed consoles consume and discard, as before. */
                if (iodev_write(IODEV_USB_VUART, &b, 1) != 1 && vuart_tx_overruns != ~0ULL)
                    vuart_tx_overruns++;
#else
                if (iodev_can_write(IODEV_USB_VUART))
                    iodev_write(IODEV_USB_VUART, &b, 1);
#endif
#ifdef AURA_WINDOWS
                if (!hv_windows_serial_capture_enabled())
#endif
                    handle_vuart_passthrough(b);
                break;
            }
            case UTRSTAT:
                utrstat &= ~(*val & (UTRSTAT_TXTHRESH | UTRSTAT_RXTHRESH | UTRSTAT_RXTO));
                break;
        }
    } else {
        switch (addr) {
            case UCON:
                *val = ucon;
                break;
            case URXH:
                if (iodev_can_read(IODEV_USB_VUART)) {
                    uint8_t c;
                    iodev_read(IODEV_USB_VUART, &c, 1);
                    *val = c;
                } else {
                    *val = 0;
                }
                break;
            case UTRSTAT:
                *val = utrstat;
                break;
            case UFSTAT:
                *val = ufstat;
                break;
            default:
                *val = 0;
                break;
        }
        //         printf("HV: vuart R 0x%lx -> 0x%lx (%d)\n", addr, *val, width);
    }

    return true;
}

void hv_vuart_poll(void)
{
    if (!active)
        return;

    update_irq();
}

void hv_map_vuart(u64 base, int irq, iodev_id_t iodev)
{
#ifdef AURA_WINDOWS
    vuart_tx_overruns = 0;
#endif
    hv_map_hook(base, handle_vuart, 0x1000);
    usb_iodev_vuart_setup(iodev);
    vuart_irq = irq;
    active = true;
}
