# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_embedded_target_applies_across_chip_generations(tmp_path):
    source = (ROOT / 'src/payload.c').read_text()
    start = source.index('#ifndef ESP_STAGE2', source.index('int payload_run(void)'))
    end = source.index('#endif', start)
    block = source[start:end] + '\n#endif\n'
    c = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define T8140 0x8140
#define MAX_CHOSEN_VARS 4
static unsigned chip_id;
static bool stage1_config_applied;
static char *chainload_spec;
static const char *configured;
static char stage1_esp_chosen[128];
static char *chosen[MAX_CHOSEN_VARS];
static size_t chosen_cnt;
static const char *stage1_config_target(void) {return configured;}
static const char *stage1_config_esp_uuid(void) {return "12345678-abcd-1234-abcd-1234567890ab";}
static int apply(void) {
''' + block + r'''
return 0;
}
int main(void) {
 unsigned chips[]={0x8103,0x6000,0x6001,0x6002,0x8112,0x6020,0x6021,0x6022,0x8122,0x6030,0x6031,0x6034,0x8140};
 for (unsigned i=0;i<sizeof(chips)/sizeof(*chips);i++) {
  chip_id=chips[i];stage1_config_applied=false;chainload_spec=NULL;chosen_cnt=0;
  configured="12345678-abcd-1234-abcd-1234567890ab;m1n1/boot.bin";
  assert(apply()==0);assert(chainload_spec && !strcmp(chainload_spec,configured));
  assert(chosen_cnt==1 && strstr(chosen[0],stage1_config_esp_uuid()));
  assert(apply()==0 && chosen_cnt==1);
  stage1_config_applied=false;chainload_spec="different;boot.bin";
  assert(apply()==-1 && chosen_cnt==1);
 }
 configured=NULL;chainload_spec=NULL;stage1_config_applied=false;chosen_cnt=0;
 assert(apply()==0 && !chainload_spec && !chosen_cnt);
 return 0;
}
'''
    f = tmp_path / 'config.c'
    f.write_text(c)
    binary = tmp_path / 'config'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(f),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_runtime_usb_engine_dispatch(tmp_path):
    # Compile the production dispatcher with fake hardware engines. Exercise
    # independent HS and SS devices together, including failed initialization.
    code = (ROOT / 'src/usb_dwc3.c').read_text()
    code = code.replace('#include "usb_dwc3.h"', '').replace('#include "malloc.h"', '')
    pre = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef void dart_dev_t;
typedef int cdc_acm_pipe_id_t;
typedef struct dwc3_dev dwc3_dev_t;
typedef int (*usb_dwc3_readvertise_hook_t)(void *);
'''
    fake = ''
    for speed, marker in [('hs',17),('ss',29)]:
        fake += f'''
struct dwc3_{speed}_dev {{ int marker; }};
static struct dwc3_{speed}_dev {speed}_instance = {{{marker}}};
static int {speed}_shutdown;
dwc3_{speed}_dev_t *usb_dwc3_{speed}_init(uintptr_t regs, dart_dev_t *dart) {{ (void)dart; return regs ? &{speed}_instance : NULL; }}
bool usb_dwc3_{speed}_shutdown(dwc3_{speed}_dev_t *d) {{ assert(d==&{speed}_instance);{speed}_shutdown++; return {speed}_shutdown == 1; }}
void usb_dwc3_{speed}_handle_events(dwc3_{speed}_dev_t *d) {{assert(d->marker=={marker});}}
ssize_t usb_dwc3_{speed}_can_read(dwc3_{speed}_dev_t *d,int p) {{return d->marker+p;}}
bool usb_dwc3_{speed}_can_write(dwc3_{speed}_dev_t *d,int p) {{return d->marker=={marker} && p==1;}}
u8 usb_dwc3_{speed}_getbyte(dwc3_{speed}_dev_t *d,int p) {{return d->marker+p;}}
void usb_dwc3_{speed}_putbyte(dwc3_{speed}_dev_t *d,int p,u8 v) {{assert(v==d->marker+p);}}
void usb_dwc3_{speed}_flush(dwc3_{speed}_dev_t *d,int p) {{assert(d->marker=={marker} && p==1);}}
'''
        for func in ('read','write','queue'):
            const = '' if func=='read' else 'const '
            fake += f'size_t usb_dwc3_{speed}_{func}(dwc3_{speed}_dev_t *d,int p,{const}void *b,size_t n) {{(void)b;return d->marker+p+n;}}\n'
    fake += r'''
int main(void) {
 dwc3_dev_t *hs=usb_dwc3_init(1,NULL), *ss=usb_dwc3_init_mode(1,NULL,true);
 assert(hs && ss && hs!=ss);assert(!usb_dwc3_init_mode(0,NULL,true));
 assert(!usb_dwc3_init_mode(0,NULL,false));
 dwc3_dev_t *both[]={hs,ss};int values[]={17,29};
 for (unsigned i=0;i<2;i++) {
  dwc3_dev_t *d=both[i];int v=values[i];
  usb_dwc3_handle_events(d);assert(usb_dwc3_can_read(d,1)==v+1);
  assert(usb_dwc3_can_write(d,1));assert(usb_dwc3_getbyte(d,1)==v+1);
  usb_dwc3_putbyte(d,1,v+1);usb_dwc3_flush(d,1);
  assert(usb_dwc3_read(d,1,NULL,5)==(size_t)v+6);
  assert(usb_dwc3_write(d,1,NULL,5)==(size_t)v+6);
  assert(usb_dwc3_queue(d,1,NULL,5)==(size_t)v+6);
 }
 assert(usb_dwc3_shutdown(hs) && usb_dwc3_shutdown(ss) && usb_dwc3_shutdown(NULL));
 assert(hs_shutdown==1 && ss_shutdown==1);
 return 0;
}
'''
    f = tmp_path / 'dispatch.c'
    f.write_text(pre + code + fake)
    binary = tmp_path / 'dispatch'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(f),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
