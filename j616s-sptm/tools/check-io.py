#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-only
"""check-io.py platform-io.h: refuse any region that hits SPTM Table A (type 0x1b); print the MISS and Table B pages.
The two io-range tables are runtime data of the user's own SPTM (16-byte records, dumped from memory at the
first SPTM call, e.g. under a debugger); they are not part of this repository. Set SPTM_IO_TABLES to the
directory holding tableA.bin and tableB.bin; without it the check is skipped."""
import os,re,struct,sys
T=os.environ.get('SPTM_IO_TABLES','')
if not T or not os.path.exists(os.path.join(T,'tableA.bin')):
    print('check-io: SPTM_IO_TABLES not set, skipped'); sys.exit(0)
T=T.rstrip('/')+'/'
def load(f):
    d=open(T+f,'rb').read(); r=[]
    for i in range(0,len(d),16):
        a,fl,s,w=struct.unpack_from('<IIII',d,i); r.append((s<<14,(s+(w&0xffffff))<<14,(a>>16)&0xff,w>>24))
    return r
A,B=load('tableA.bin'),load('tableB.bin'); bad=0; total=0
for m in re.finditer(r'\{(0x[0-9a-f]+)UL, (0x[0-9a-f]+)UL\},\s*/\*\s*(.*?)\s*\*/',open(sys.argv[1]).read()):
    a,s,n=int(m[1],16),int(m[2],16),m[3]; total+=s>>14
    ha=[e for e in A if e[0]<a+s and a<e[1]]
    miss=[p for p in range(a,a+s,0x4000) if not [e for e in B if e[0]<=p<e[1]]]
    print(f'{a:#013x} {s:#10x} A={len(ha)} misspages={len(miss)}/{s>>14} {n}')
    bad+=len(ha)
print('pages',total); sys.exit(1 if bad else 0)
