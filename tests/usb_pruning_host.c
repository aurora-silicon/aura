// SPDX-License-Identifier: MIT

#include <libfdt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef unsigned long u64;
_Static_assert(sizeof(u64)==8,"64-bit host required");
typedef uint32_t u32;
static void *dt, *adt;
#define bail_cleanup(...) do { fprintf(stderr,__VA_ARGS__); goto err; } while(0)
#define bail(...) do { fprintf(stderr,__VA_ARGS__); return -1; } while(0)
#define ADT_GETPROP(a,b,c,d) (-1)
#define ADT_FOREACH_CHILD(a,n) for ((n)=1;(n)<=2;(n)++)
static int adt_path_offset_trace(void *a,const char *s,int *p) { p[0]=1;p[1]=0;return 0; }
static const char *adt_get_name(void *a,int n) { return n==1?"usb-drd0":"usb-drd1"; }
static int adt_get_reg(void *a,int *p,const char *s,int r,u64 *v,void *x) { *v=p[1]==1?0x702280000ULL:0xb02280000ULL; return 0; }
#define PMGR_DIE_OFFSET 0x2000000000ULL
#define DT_MAX_RANGES 1
struct dt_ranges_tbl { int unused; };
static void dt_parse_ranges(void *d,int n,struct dt_ranges_tbl *r) {}
static u64 dt_translate(struct dt_ranges_tbl *r,const fdt64_t *p) { return fdt64_to_cpu(*p); }

/* PRODUCTION ROUTINE */

int main(int argc,char **argv) {
 FILE *f=fopen(argv[1],"rb"); if(!f)return 2;
 fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
 void *b=malloc(n); if(fread(b,1,n,f)!=n)return 3;fclose(f);
 dt=malloc(n+65536);if(fdt_open_into(b,dt,n+65536))return 4;
 if(dt_disable_missing_devs("usb-drd","usb@",8,0))return 5;
 if(fdt_pack(dt))return 6;
 f=fopen(argv[2],"wb");if(!f)return 7;
 fwrite(dt,1,fdt_totalsize(dt),f);fclose(f);return 0;
}
