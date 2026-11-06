/* Offline endpoint/ADT model. Written before the producer implementation. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TYPES_H
#define UTILS_H
#define HV_H
static bool hv_windows_can_prepare_device(void) { return true; }
#define __EXCEPTION_H__
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(x) (1ULL << (x))
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#define PACKED __attribute__((packed))
#define UNUSED(x) ((void)(x))
#define T8142 0x8142
#define sysop(x) ((void)0)
enum exc_guard_t { GUARD_OFF, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_SILENT=0x100 };
volatile enum exc_guard_t exc_guard; volatile int exc_count;
u32 chip_id=0x8142, board_id=0x24; void *adt;
static const char *scenario; static unsigned trains,writes; static bool trained,held;
struct cell {u64 a;u32 v;}; static struct cell cells[300]; static unsigned count;
static u32 get(u64 a) {for(unsigned i=0;i<count;i++)if(cells[i].a==a)return cells[i].v;return 0;}
static void put(u64 a,u32 v){for(unsigned i=0;i<count;i++)if(cells[i].a==a){cells[i].v=v;return;}assert(count<300);cells[count++]=(struct cell){a,v};}
static bool is(const char*s){return !strcmp(scenario,s);}
static u32 read32(u64 a){if(a==0x1cb0100010ULL && get(a)==0xffffffff)return is("bar-mask")?0xfffe0001:0xffff0004;if(a==0x1cb0100014ULL && get(a)==0xffffffff)return is("bar-high")?0xfffffffe:0xffffffff;return get(a);}
static u16 read16(u64 a){return read32(a&~3ULL)>>((a&2)*8);}
static void write32(u64 a,u32 v){writes++;if(is("bar-drop")&&a==0x1cb0100010ULL&&v==0x80000004)return;put(a,v);if(a==0x39a000300ULL&&(v&15)==2){held=true;if(!is("clamp-fail")){put(0x488028208ULL,0);put(0x488028804ULL,0);put(0x488028820ULL,0);put(0x1cb0000728ULL,0x14);}}}
static void write16(u64 a,u16 v){u64 b=a&~3ULL;unsigned sh=(a&2)*8;write32(b,(get(b)&~(0xffffU<<sh))|((u32)v<<sh));}
static void udelay(unsigned u){(void)u;}
#include "../../../src/adt.h"
static const char *paths[]={"/","/chosen","/arm-io/apcie","/arm-io/apcie/pci-bridge0","/arm-io/dart-apcie0","/arm-io/gpio0","/arm-io/smc"};
int adt_path_offset(const void*d,const char*p){(void)d;for(unsigned i=0;i<ARRAY_SIZE(paths);i++)if(!strcmp(paths[i],p))return i;return -1;}
int adt_path_offset_trace(const void*d,const char*p,int*out){int n=adt_path_offset(d,p);out[0]=n;out[1]=-1;return n;}
bool adt_is_compatible(const void*d,int n,const char*c){(void)d;return (n==2&&!strcmp(c,"apcie,t8142"))||(n==4&&!strcmp(c,"dart,t8110"))||(n==5&&!strcmp(c,"gpio,t8101"));}
int adt_get_reg(const void*d,int*p,const char*prop,int i,u64*a,u64*z){(void)d;(void)prop;u64 b=0,l=0;if(p[0]==2){u64 bs[]={0x1cb0000000,0x48c000000,0x48f000000,0x48f020000,0x48f040000,0x48e000000,0,0,0x488028000,0,0x48f010000};u64 ls[]={0x10000000,0x4000,0x40000,0x4000,0x20000,0x1000000,0,0,0x8000,0,0x4000};if(i<0||i>10)return -1;b=bs[i];l=ls[i];}else if(p[0]==4){b=0x488000000;l=0x20000;}else if(p[0]==5){b=0x39a000000;l=0x100000;}else return -1;if(is("resource")&&p[0]==4)b+=0x1000;*a=b;if(z)*z=l;return 0;}
const void*adt_getprop(const void*d,int n,const char*key,u32*len){(void)d;static u32 vals[7];memset(vals,0,sizeof(vals));const void*r=vals;*len=4;if(n==0&&!strcmp(key,"model")){r=is("model")?"Mac17,4":"Mac17,3";*len=8;}else if(n==1&&!strcmp(key,"chip-id"))vals[0]=chip_id;else if(n==1&&!strcmp(key,"board-id"))vals[0]=board_id;else if((n==5||n==6)&&!strcmp(key,"AAPL,phandle"))vals[0]=n==5?0xa0:0xd9;else if(n==3&&!strcmp(key,"function-perst")){*len=16;vals[0]=0xa0;vals[1]=0x4750494f;vals[2]=is("perst")?193:192;}else if(n==3&&!strcmp(key,"function-reset")){*len=16;vals[0]=0xd9;vals[1]=0x704b5734;vals[2]=0x67503133;vals[3]=0x800000;}else if(n==2&&!strcmp(key,"ranges")){*len=28;vals[0]=0x02000000;vals[1]=0x80000000;vals[3]=0x80000000;vals[4]=0xb;vals[5]=0x40000000;}else {*len=0;r=NULL;}return r;}
int adt_getprop_copy(const void*d,int n,const char*k,void*out,size_t size){u32 len;const void*p=adt_getprop(d,n,k,&len);if(!p||len!=size)return -1;memcpy(out,p,size);return len;}
int j813_n1_train_windows(void){trains++;trained=true;put(0x488028208ULL,0x1001);put(0x1cb0000000ULL,0x100c106b);put(0x1cb0100000ULL,is("identity")?0x1234106b:0x1900106b);put(0x1cb0101000ULL,0xffff);put(0x1cb0102000ULL,0xffff);put(0x1cb0100010ULL,4);put(0x1cb0100004ULL,is("post-bme")?4:0);put(0xb80008000ULL,is("rom")?1:0);return is("train")||is("clamp-fail")?-1:0;}
#include "../../../src/hv/windows/windows_n1.c"
int main(int argc,char**argv){assert(argc==2);scenario=argv[1];if(is("chip"))chip_id=0x8112;if(is("board"))board_id=0x25;put(0x488000000ULL,14U<<24);put(0x488000008ULL,(42U<<24)|(36U<<16)|0x202);put(0x48800000cULL,16);put(0x488000200ULL,is("locked")?1:0);put(0x488001000ULL,is("dart")?1:0);put(0x488000080ULL,is("busy")?0x80000000:0);put(0x39a000300ULL,3);if(is("pre-bme")){put(0x488028208ULL,1);put(0x1cb0000000ULL,0x100c106b);put(0x1cb0100000ULL,0x1900106b);put(0x1cb0100004ULL,4);}
unsigned char result[128];memset(result,0xaa,sizeof(result));int rc=hv_windows_n1_prepare(result,sizeof(result));bool success=is("success");if(success){assert(rc==0&&trains==1&&!held);u32 magic;memcpy(&magic,result,4);assert(magic==0x4831574e);FILE*f=fopen("descriptor.bin","wb");assert(f);assert(fwrite(result,1,128,f)==128);fclose(f);}else{assert(rc!=0);for(unsigned i=0;i<128;i++)assert(!result[i]);if(trained)assert(held);else assert(!writes);if(is("clamp-fail"))assert(rc==-2);}printf("%s rc=%d trains=%u writes=%u held=%d PASS\n",scenario,rc,trains,writes,held);}
