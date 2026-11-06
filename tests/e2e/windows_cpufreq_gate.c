/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STATIC static
#define IN
#define CONST const
#define VOID void
#define TRUE true
#define FALSE false
#define DEBUG(x) ((void)0)
#define CompareMem memcmp
#define CopyMem memcpy
#define SIGNATURE_32(a,b,c,d) ((uint32_t)(a)|((uint32_t)(b)<<8)|((uint32_t)(c)<<16)|((uint32_t)(d)<<24))
typedef bool BOOLEAN; typedef size_t UINTN; typedef uint32_t UINT32;
typedef struct __attribute__((packed)) {uint32_t Signature,Length;uint8_t Revision,Checksum,OemId[6];uint64_t OemTableId;uint32_t OemRevision,CreatorId,CreatorRevision;} EFI_ACPI_DESCRIPTION_HEADER;
static uint32_t marker,chip,board;static size_t length;static int missing;
static const void *dt_get_prop(const char *path,const char *name,size_t *len){
 assert(!strcmp(path,"/chosen"));*len=4;
 if(!strcmp(name,"aura,windows-cpufreq-v1")){*len=length;return missing==1?NULL:&marker;}
 if(!strcmp(name,"chip-id"))return missing==2?NULL:&chip;
 if(!strcmp(name,"board-id"))return missing==3?NULL:&board;
 abort();
}
#include "cpufreq-gate.inc"
int main(int argc,char **argv){assert(argc==2);unsigned char data[4096];FILE*f=fopen(argv[1],"rb");assert(f);size_t size=fread(data,1,sizeof(data),f);fclose(f);EFI_ACPI_DESCRIPTION_HEADER*h=(void*)data;assert(size==h->Length);
 for(int test=0;test<10;test++){marker=1;chip=0x8142;board=0x24;length=4;missing=0;
  if(test==1)missing=1;if(test==2)marker=0;if(test==3)marker=2;if(test==4)length=1;if(test==5)length=8;if(test==6)chip=0x8152;if(test==7)board=0x23;if(test==8)missing=2;if(test==9)missing=3;
  assert(AppleCpuFreqTableAdmitted(h,size)==(test==0));
 }
 assert(!AppleCpuFreqTableAdmitted(h,sizeof(*h)-1));memcpy(&h->OemTableId,"OTHERCPU",8);assert(AppleCpuFreqTableAdmitted(h,size));puts("12 admission cases passed");return 0;
}
