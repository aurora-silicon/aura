/* SPDX-License-Identifier: MIT */

#include "hv.h"
#include "aic.h"
#include "iodev.h"
#include "uart.h"
#include "uart_regs.h"
#include "usb.h"

bool active = false;

u32 ucon = 0;
u32 utrstat = 0;
u32 ufstat = 0;

int vuart_irq = 0;

/* Over KIS the proxy rides the DockChannel, not a DWC3 CDC pair: mirror guest TX onto that iodev
 * and keep RX off (reading the shared channel would steal the proxy's framed replies). */
static iodev_id_t vuart_tx_iodev = IODEV_USB_VUART;
static bool vuart_tx_only = false;
/* Set at a suspend-to-RAM commit: the real UART powers off with the SoC and is clock-gated when the
 * AP wakes, so from then on the vuart never touches it. */
static bool vuart_hw_off = false;
/* With the proxy on CDC, XNU's TX is also copied to DockChannel (always-on, bounded writes): if
 * the CDC link goes away, `macvdmtool debugusb` brings XNU's console back on KIS. */
static iodev_id_t vuart_tx_mirror = IODEV_MAX;

static void update_irq(void)
{
    ssize_t rx_queued;

    if (!vuart_tx_only)
        iodev_handle_events(IODEV_USB_VUART);

    utrstat |= UTRSTAT_TXBE | UTRSTAT_TXE;
    utrstat &= ~UTRSTAT_RXD;

    ufstat = 0;
    if (!vuart_tx_only && (rx_queued = iodev_can_read(IODEV_USB_VUART))) {
        utrstat |= UTRSTAT_RXD;
        if (rx_queued > 15)
            ufstat = FIELD_PREP(UFSTAT_RXCNT, 15) | UFSTAT_RXFULL;
        else
            ufstat = FIELD_PREP(UFSTAT_RXCNT, rx_queued);

        if (FIELD_GET(UCON_RXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_RXTO_ENA) {
            utrstat |= UTRSTAT_RXTO;
        }
    }

    if (FIELD_GET(UCON_TXMODE, ucon) == UCON_MODE_IRQ && ucon & UCON_TXTHRESH_ENA) {
        utrstat |= UTRSTAT_TXTHRESH;
    }

    if (vuart_irq) {
        if (!vuart_hw_off)
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
                if (iodev_can_write(vuart_tx_iodev))
                    iodev_write(vuart_tx_iodev, &b, 1);
                if (vuart_tx_mirror != IODEV_MAX && vuart_tx_mirror != vuart_tx_iodev &&
                    iodev_can_write(vuart_tx_mirror))
                    iodev_write(vuart_tx_mirror, &b, 1);
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
                if (!vuart_tx_only && iodev_can_read(IODEV_USB_VUART)) {
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
    hv_map_hook(base, handle_vuart, 0x1000);
    if (iodev >= IODEV_USB0 && iodev < IODEV_USB0 + USB_IODEV_COUNT) {
        usb_iodev_vuart_setup(iodev);
        vuart_tx_iodev = IODEV_USB_VUART;
        vuart_tx_only = false;
        vuart_tx_mirror = IODEV_DOCKCHANNEL_UART;
    } else {
        vuart_tx_iodev = iodev;
        vuart_tx_only = true;
        printf("HV: VUART TX mirrored to iodev %d (RX reserved for proxy)\n", iodev);
    }
    vuart_irq = irq;
    active = true;
}

/*
 * Suspend-to-RAM commit (sptm_virt_s2r.c): the guest's TX goes to an always-on iodev (DockChannel)
 * from now on, and the real UART is left alone. A DWC3 CDC pair is a SoC-domain device too, so the
 * mirror leaves it as well.
 */
void hv_vuart_sleep_commit(iodev_id_t tx)
{
    vuart_hw_off = true;
    vuart_tx_iodev = tx;
    vuart_tx_only = true;
}
