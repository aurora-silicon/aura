#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# gen-vgic-cpuif.sh <kernel tree> > vgic_cpuif.c : lines of hyp/vgic-v3-sr.c on the shim above the marker of vgic_cpuif.head
F=$1/arch/arm64/kvm/hyp/vgic-v3-sr.c
D=$(dirname "$(readlink -f "$0")")
cat "$D/vgic_cpuif.head"
sed -n 19,21p "$F"
sed -n '/^u64 __gic_v3_get_lr/,/^}/p;/^void __gic_v3_set_lr/,/^}/p' "$F"
sed -n '/^static void __vgic_v3_write_ap0rn/,/^}/p;/^static void __vgic_v3_write_ap1rn/,/^}/p;/^static u32 __vgic_v3_read_ap0rn/,/^}/p;/^static u32 __vgic_v3_read_ap1rn/,/^}/p' "$F"
sed -n '/^static u64 __vgic_v3_read_vmcr/,/^}/p;/^static void __vgic_v3_write_vmcr/,/^}/p' "$F"
A=$(grep -n '^static int __vgic_v3_bpr_min' "$F" | cut -d: -f1)
B=$(grep -n '^static bool __vgic_v3_check_trap_forwarding' "$F" | cut -d: -f1)
sed -n "$A,$((B-1))p" "$F"
sed -n '/^int __vgic_v3_perform_cpuif_access/,/^}/p' "$F" | python3 -I -c '
import sys,re
s=sys.stdin.read()
s=re.sub(r"\tif \(kern_hyp_va\(vcpu->kvm\).*?return 0;\n\n", "", s, flags=re.S)
s=re.sub(r"\tif \(vcpu_mode_is_32bit\(vcpu\)\) \{.*?\} else \{\n\t\tsysreg = esr_sys64_to_sysreg\(esr\);\n\t\}\n", "\tsysreg = esr & 0x3ffc1e;\n\tsysreg = (((sysreg >> 20) & 3) << 19) | (((sysreg >> 14) & 7) << 16) | (((sysreg >> 10) & 15) << 12) | (((sysreg >> 1) & 15) << 8) | (((sysreg >> 17) & 7) << 5);\n", s, flags=re.S)
s=s.replace("is_read = (esr & ESR_ELx_SYS64_ISS_DIR_MASK) == ESR_ELx_SYS64_ISS_DIR_READ;","is_read = esr & 1;")
s=re.sub(r"\tif \(__vgic_v3_check_trap_forwarding\(vcpu, sysreg, is_read\)\)\n\t\treturn 0;\n\n","",s)
print(s)'
cat "$D/vgic_cpuif.tail"
