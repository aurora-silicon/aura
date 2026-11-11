"""Exercise USB reset, nested control progress and transfer retirement."""
from pathlib import Path
import os
import re
import subprocess


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|void) " + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_usb_reset(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get("USB_RESET_SOURCE", repo / "src/usb_dwc3_impl.inc")).read_text()
    reset = function(source, "usb_dwc3_handle_event_usbrst")
    dma = function((repo / "src/usb_cdc_state.c").read_text(), "usb_cdc_dma_may_release")
    harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX_ENDPOINTS 6
#define CDC_ACM_PIPE_MAX 2
#define XFER_BUFFER_BYTES_PER_EP 16
#define TRBS_PER_EP 2
#define USB_LEP_CTRL_OUT 0
#define USB_LEP_CTRL_IN 1
#define DWC3_DCFG 0
#define DWC3_DALEPENA 4
#define DWC3_DCFG_DEVADDR_MASK 127
#define DWC3_DCFG_DEVADDR(x) (x)
#define DWC3_DALEPENA_EP(x) (1U<<(x))
#define usb_debug_printf(...) ((void)0)
enum {USB_DWC3_EP0_STATE_IDLE,USB_DWC3_EP0_STATE_SETUP_HANDLE,USB_DWC3_EP0_STATE_DATA_SEND_DONE,USB_DWC3_EP0_STATE_DATA_RECV_DONE};
struct dwc3_trb {uint64_t a,b;};
struct endpoint {bool xfer_in_progress,zlp_pending,stalled;int resource_index,trb_count,completed_trbs,transfer_length;unsigned char xfer_buffer[16];struct dwc3_trb trb[2];};
struct rb {int read,write;};
typedef struct {unsigned regs;bool reset_in_progress,ep0_progressed_during_reset,configured_during_reset,cdc_reconfigure,cdc_ever_configured,primary_dtr_pending,failed;int ep0_state;struct endpoint endpoints[6];struct {bool ready;struct rb *host2device,*device2host;} pipe[2];} dwc3_dev_t;
static unsigned address,enabled;static int ends,stalls,setups,mode,writes;
static void mask32(unsigned reg,unsigned mask,unsigned value){assert(reg==0 && mask==127);address=value;writes++;}
static void write32(unsigned reg,unsigned value){assert(reg==4);enabled=value;writes++;}
static int usb_dwc3_end_transfer(dwc3_dev_t *d,int ep){
 assert(address==0 && enabled==3);ends++;
 if(mode==1)return -1;
 if(mode==2){d->ep0_progressed_during_reset=true;d->ep0_state=USB_DWC3_EP0_STATE_SETUP_HANDLE;d->endpoints[0].xfer_in_progress=true;address=37;}
 if(mode==3){d->configured_during_reset=true;d->ep0_progressed_during_reset=true;d->endpoints[ep].xfer_in_progress=true;d->endpoints[ep].resource_index=91;}
 return 0;
}
static void usb_dwc3_fail_session(dwc3_dev_t *d){d->failed=true;}
static void usb_dwc3_ep_set_stall(dwc3_dev_t *d,int ep,int value){assert(value==0);stalls++;d->endpoints[ep].stalled=false;}
static int usb_dwc3_start_setup_phase(dwc3_dev_t *d){setups++;d->endpoints[0].xfer_in_progress=true;return 0;}
''' + dma + reset + r'''
static dwc3_dev_t d;static struct rb rb[4];
static void setup(void){
 memset(&d,0,sizeof(d));mode=ends=stalls=setups=writes=0;address=29;enabled=63;
 d.cdc_ever_configured=true;d.primary_dtr_pending=true;d.ep0_state=USB_DWC3_EP0_STATE_IDLE;
 for(int i=0;i<2;i++){d.pipe[i].ready=true;d.pipe[i].host2device=&rb[2*i];d.pipe[i].device2host=&rb[2*i+1];rb[2*i]=(struct rb){1,9};rb[2*i+1]=(struct rb){2,8};}
 for(int i=0;i<6;i++){memset(d.endpoints[i].xfer_buffer,0x55,16);d.endpoints[i].resource_index=17;}
}
int main(void){
#if USB_DWC3_SUPERSPEED
 setup();d.endpoints[2].xfer_in_progress=true;d.endpoints[3].stalled=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(ends==1 && stalls==1 && setups==1 && !d.failed && !d.reset_in_progress && d.cdc_reconfigure);
 for(int i=2;i<6;i++)assert(!d.endpoints[i].xfer_in_progress && !d.endpoints[i].resource_index && !d.endpoints[i].xfer_buffer[0]);
 for(int i=0;i<2;i++)assert(!d.pipe[i].ready && d.pipe[i].host2device->read==9 && d.pipe[i].device2host->read==8);
 setup();d.reset_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);assert(!writes && !ends);
 setup();mode=1;d.endpoints[2].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(d.failed && !d.reset_in_progress && d.endpoints[2].xfer_in_progress && d.endpoints[2].xfer_buffer[0]==0x55 && !setups);
 setup();mode=2;d.endpoints[2].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(address==37 && d.endpoints[0].xfer_in_progress && d.endpoints[0].xfer_buffer[0]==0x55 && !setups && !d.reset_in_progress);
 setup();mode=3;d.endpoints[2].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(d.endpoints[2].xfer_in_progress && d.endpoints[2].resource_index==91 && d.endpoints[2].xfer_buffer[0]==0x55 && !setups);
 setup();d.ep0_state=USB_DWC3_EP0_STATE_SETUP_HANDLE;d.endpoints[0].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(d.endpoints[0].xfer_in_progress && !setups && d.endpoints[0].xfer_buffer[0]==0x55);
 setup();d.endpoints[2].stalled=true;usb_dwc3_handle_event_usbrst(&d,false);assert(!d.endpoints[2].stalled && !stalls && !setups);
#else
 /* Every active transfer is ended, EP0 included, and SETUP is re-armed. */
 setup();for(int i=0;i<6;i++)d.endpoints[i].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(address==0 && enabled==3 && ends==6 && setups==1 && stalls==5);
 assert(d.endpoints[0].xfer_in_progress && d.ep0_state==USB_DWC3_EP0_STATE_SETUP_HANDLE);
 for(int i=1;i<6;i++)assert(!d.endpoints[i].xfer_in_progress && !d.endpoints[i].resource_index);
 for(int i=2;i<6;i++)assert(!d.endpoints[i].xfer_buffer[0]);
 assert(d.endpoints[0].xfer_buffer[0]==0x55);
 for(int i=0;i<2;i++)assert(!d.pipe[i].ready && d.pipe[i].host2device->read==9 && d.pipe[i].device2host->read==8);
 /* A live SETUP TRB is kept: ending it would leave no TRB for SET_ADDRESS. */
 setup();d.ep0_state=USB_DWC3_EP0_STATE_SETUP_HANDLE;d.endpoints[0].xfer_in_progress=true;
 d.endpoints[3].xfer_in_progress=true;usb_dwc3_handle_event_usbrst(&d,true);
 assert(ends==1 && !setups && d.endpoints[0].xfer_in_progress && d.endpoints[0].resource_index==17);
 assert(d.ep0_state==USB_DWC3_EP0_STATE_SETUP_HANDLE && !d.endpoints[3].xfer_in_progress);
 /* A data or status phase in flight belongs to the old session. */
 setup();d.ep0_state=USB_DWC3_EP0_STATE_DATA_SEND_DONE;d.endpoints[1].xfer_in_progress=true;
 usb_dwc3_handle_event_usbrst(&d,true);
 assert(ends==1 && setups==1 && d.endpoints[0].xfer_in_progress && !d.endpoints[1].xfer_in_progress);
 /* Nothing armed: no ENDTRANSFER, SETUP armed afresh. */
 setup();usb_dwc3_handle_event_usbrst(&d,true);
 assert(!ends && setups==1 && d.ep0_state==USB_DWC3_EP0_STATE_SETUP_HANDLE);
#endif
 puts("USB reset: transfer retirement and nested state PASS");return 0;
}
'''
    for mode in ("standard", "cdc"):
        code = tmp_path / ("usb-reset-" + mode + ".c")
        code.write_text(harness)
        binary = tmp_path / ("usb-reset-" + mode)
        args = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(code), "-o", str(binary)]
        args.insert(1, "-DUSB_DWC3_SUPERSPEED=" + ("1" if mode == "cdc" else "0"))
        subprocess.run(args, check=True)
        subprocess.run([str(binary)], check=True)
