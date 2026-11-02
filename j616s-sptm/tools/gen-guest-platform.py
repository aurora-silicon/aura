#!/usr/bin/python3 -I
# SPDX-License-Identifier: GPL-2.0-only
"""Writes guest-platform.dtsi from tools/guest-platform.dtsi.in: the Wi-Fi and Bluetooth calibration, the Bluetooth address,
the antenna SKU and the OPP tables come from this Mac's DT (raw-boot Linux on the daily kernel, loader-filled nodes).
Domains, DART, PCIe and pinctrl are in the template: the board DT has no apcie_gp, apcie_phy_sw labels, the ADT clock-gates are the source.
Local build input; never published (holds the Wi-Fi calibration blob). DT_ROOT overrides the running DT; board values need m4.163, a loader-filled wifi node."""
import os,struct,sys
R=os.environ.get('DT_ROOT','/sys/firmware/devicetree/base')
def rd(p): return open(R+p,'rb').read()
def cells(b): return struct.unpack('>%dI'%(len(b)//4),b)
o=[]
# OPP tables + cpufreq, all clusters. No apple,fast-die-effort: PMP does not run in the guest.
for c,addr in (('e',0x210e20000),('p0',0x211e20000),('p1',0x212e20000)):
    o.append(f'\tcpu_opp_{c}: opp-table-{c} {{\n\t\tcompatible = "operating-points-v2"; opp-shared;\n')
    for n in sorted(os.listdir(R+'/opp-table-'+c)):
        q=f'/opp-table-{c}/'+n
        if not os.path.isdir(R+q): continue
        hz=cells(rd(q+'/opp-hz')); lvl=cells(rd(q+'/opp-level'))[0]; lat=cells(rd(q+'/clock-latency-ns'))[0] if os.path.exists(R+q+'/clock-latency-ns') else 0
        o.append(f'\t\t{n} {{ opp-hz = /bits/ 64 <{(hz[0]<<32)|hz[1]}>; opp-level = <{lvl}>; clock-latency-ns = <{lat}>; }};\n')
    o.append('\t};\n')
    o.append(f"""	cpufreq_{c}: cpufreq@{addr:x} {{
		compatible = "apple,t6041-cluster-cpufreq", "apple,t8112-cluster-cpufreq";
		reg = <{addr>>32:#x} {addr&0xffffffff:#x} 0x0 0x1000>;
		nonposted-mmio;
		#performance-domain-cells = <0>;
	}};
""")
w='/pcie@1cb0000000/pci@0,0/wifi@0,0'
hexb=lambda b:' '.join('%02x'%x for x in b)
bt=[d for d in os.listdir(R+'/pcie@1cb0000000/pci@0,0') if d.startswith('bluetooth@')]
if len(bt)!=1: raise SystemExit('no single bluetooth node under pci@0,0 in '+R)
b='/pcie@1cb0000000/pci@0,0/'+bt[0]
t=open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'guest-platform.dtsi.in')).read()
t=t.replace('@OPP@',''.join(o)).replace('@CAL@',hexb(rd(w+'/brcm,cal-blob'))).replace('@SKU@',rd(w+'/apple,antenna-sku').rstrip(b'\0').decode())\
 .replace('@BTADDR@',hexb(rd(b+'/local-bd-address'))).replace('@BTBFCAL@',hexb(rd(b+'/brcm,taurus-bf-cal-blob'))).replace('@BTCAL@',hexb(rd(b+'/brcm,taurus-cal-blob')))
open(sys.argv[1],'w').write(t)
