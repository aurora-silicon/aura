# SPDX-License-Identifier: GPL-2.0-only
"""Compare real AArch64 aura code against its pre-M5/M6 integration baseline.

This is instruction/MMIO replay, not a hardware qualification. External PMGR
features, poll completion and architectural registers are explicit inputs.
"""
import argparse
import gc
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import capstone
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import *

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('baseline', type=Path, help='Baseline source tree with build/aura-raw.elf')
parser.add_argument('candidate', type=Path, help='Candidate source tree with build/aura-raw.elf')
parser.add_argument('--output', type=Path, required=True, help='New directory for the result receipt')
args = parser.parse_args()
BASE = args.baseline.resolve()
CURRENT = args.candidate.resolve()
HERE = args.output.resolve()
HERE.mkdir(parents=True, exist_ok=False)
STOP = 0x18000000
HEAP = 0x10000000

class Binary:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.elf = ELFFile(io.BytesIO(self.data))
        self.symbols = {s.name: s.entry.st_value for s in self.elf.get_section_by_name('.symtab').iter_symbols() if s.name}
        self.sysops = {}
        decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        decoder.skipdata = True
        for section in self.elf.iter_sections():
            if section['sh_flags'] & 4:
                for i in decoder.disasm(section.data(), section['sh_addr']):
                    if i.mnemonic in ('mrs', 'msr', 'sys'):
                        self.sysops[i.address] = (i.mnemonic, i.op_str)
        self.types = {}
        for cu in self.elf.get_dwarf_info().iter_CUs():
            for d in cu.iter_DIEs():
                n = d.attributes.get('DW_AT_name')
                if n and d.tag == 'DW_TAG_structure_type' and 'DW_AT_byte_size' in d.attributes:
                    self.types[n.value.decode()] = {x.attributes['DW_AT_name'].value.decode(): x.attributes['DW_AT_data_member_location'].value for x in d.iter_children() if x.tag == 'DW_TAG_member' and 'DW_AT_name' in x.attributes and 'DW_AT_data_member_location' in x.attributes}

class Run:
    def __init__(self, binary, chip, part, features=True, poll_failure=False):
        self.b = binary
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.trace = []
        self.features, self.poll_failure = features, poll_failure
        self.registers = {'midr_el1': 0x610f0001 | (part << 4), 'mpidr_el1': 0, 'currentel': 8}
        self.alloc = HEAP+0x10000
        pages = set()
        for s in binary.elf.iter_segments():
            if s['p_type'] == 'PT_LOAD':
                for addr in range(s['p_vaddr'] & ~4095, (s['p_vaddr']+s['p_memsz']+4095)&~4095, 4096):
                    if addr not in pages:
                        self.uc.mem_map(addr, 4096); pages.add(addr)
                self.uc.mem_write(s['p_vaddr'], s.data())
        # The image runs at its link address (0), so a RELR-packed word already holds its value;
        # only .rela.dyn words (zero in the file, --no-apply-dynamic-relocs) need their addend.
        rela = binary.elf.get_section_by_name('.rela.dyn')
        for relocation in rela.iter_relocations() if rela else ():
            if relocation['r_info_type'] == 0:
                continue
            assert relocation['r_info_type'] == 1027
            self.uc.mem_write(relocation['r_offset'], struct.pack('<Q', relocation['r_addend']))
        self.uc.mem_map(HEAP, 0x100000)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.reg_write(UC_ARM64_REG_SP, HEAP+0xff000)
        self.put('chip_id', chip, 4)
        self.put('boot_cpu_mpidr', 0, 8)
        self.put('boot_cpu_idx', 0, 4)
        self.stub = {}
        for name in ['printf','puts','uart_puts','debug_putc','pmgr_get_feature','pmgr_power_on','pmgr_set_voltage_ctl','poll32','poll64','aic_write','free','malloc','calloc']:
            if name in binary.symbols:
                self.stub[binary.symbols[name]] = name
        self.uc.hook_add(UC_HOOK_CODE, self.code)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.memory)
    def put(self, name, value, size):
        if name in self.b.symbols:
            self.uc.mem_write(self.b.symbols[name], value.to_bytes(size, 'little'))
    def string(self, address):
        return bytes(self.uc.mem_read(address, 200)).split(b'\0')[0].decode()
    def unmapped(self, uc, access, address, size, value, _):
        assert address >= 0x200000000, hex(address)
        uc.mem_map(address & ~4095, 4096)
        return True
    def memory(self, uc, access, address, size, value, _):
        from unicorn import UC_MEM_WRITE
        if address >= 0x200000000:
            self.trace.append(['write' if access == UC_MEM_WRITE else 'read', address, size, value if access == UC_MEM_WRITE else int.from_bytes(uc.mem_read(address,size),'little')])
    def code(self, uc, address, size, _):
        if address in self.stub:
            name = self.stub[address]
            args = [uc.reg_read(UC_ARM64_REG_X0+i) for i in range(4)]
            result = 0
            if name in ('init_cpu','exception_initialize','smp_secondary_entry','smp_secondary_prep_el3','aura_main'):
                self.trace.append(['entry_call',name])
            elif name == 'pmgr_get_feature':
                self.trace.append([name, self.string(args[0]), self.features]); result = int(self.features)
            elif name in ('pmgr_power_on','pmgr_set_voltage_ctl','aic_write'):
                self.trace.append([name, args[0], self.string(args[1]) if name == 'pmgr_power_on' else args[1] if name == 'aic_write' else None])
            elif name in ('poll32','poll64'):
                self.trace.append([name, *args]); result = -1 if self.poll_failure else 0
            elif name in ('malloc','calloc'):
                result = self.alloc; self.alloc += 0x1000
            uc.reg_write(UC_ARM64_REG_X0, result & ((1<<64)-1))
            uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
            return
        if address in self.b.sysops:
            kind, operands = self.b.sysops[address]
            if kind == 'sys':
                self.trace.append(['sys', operands]); uc.reg_write(UC_ARM64_REG_PC,address+4); return
            first, second = [x.strip() for x in operands.split(',',1)]
            reg, sysreg = (first, second) if kind == 'mrs' else (second, first)
            if reg == 'xzr': value = 0; regnum = None
            elif re.fullmatch(r'x\d+', reg):
                number=int(reg[1:]); regnum=UC_ARM64_REG_LR if number==30 else UC_ARM64_REG_FP if number==29 else UC_ARM64_REG_X0+number
                value=uc.reg_read(regnum)
            else:
                return
            if kind == 'mrs':
                value=self.registers.get(sysreg,0)
                if regnum is not None: uc.reg_write(regnum,value)
            else: self.registers[sysreg]=value
            self.trace.append([kind,sysreg,value]); uc.reg_write(UC_ARM64_REG_PC,address+4)
    def call(self, name, *args):
        for i,a in enumerate(args): self.uc.reg_write(UC_ARM64_REG_X0+i,a)
        self.uc.reg_write(UC_ARM64_REG_LR,STOP)
        try:
            self.uc.emu_start(self.b.symbols[name],STOP,count=500000)
        except Exception as error:
            pc=self.uc.reg_read(UC_ARM64_REG_PC)
            decoder=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
            detail=[(i.mnemonic,i.op_str) for i in decoder.disasm(bytes(self.uc.mem_read(pc,4)),pc)]
            raise RuntimeError((name,hex(pc),detail,self.trace[-4:])) from error
        assert self.uc.reg_read(UC_ARM64_REG_PC)==STOP, (name,hex(self.uc.reg_read(UC_ARM64_REG_PC)))
        return self.uc.reg_read(UC_ARM64_REG_X0)
    def scenario(self):
        entries=['init_cpu','exception_initialize','smp_secondary_entry','smp_secondary_prep_el3','aura_main']
        self.stub.update({self.b.symbols[n]:n for n in entries})
        for el in (4,8,12):
            for cpu in (0,1):
                self.registers.update(currentel=el,mpidr_el1=cpu)
                self.trace.append(['reset_entry',el,cpu])
                self.call('_cpu_reset_c',HEAP+0x4000)
        for n in entries: del self.stub[self.b.symbols[n]]
        startup_trace=list(self.trace);self.trace=[]
        self.registers.update(currentel=8,mpidr_el1=0)
        self.call('init_cpu')
        init_trace=list(self.trace); self.trace=[]
        result=self.call('cpufreq_init')
        freq_trace=list(self.trace); self.trace=[]
        table=self.b.symbols['spin_table']
        for cpu in range(4): self.uc.mem_write(table+cpu*64,struct.pack('<QQ',cpu,1))
        self.call('smp_set_wfe_mode',1)
        self.call('smp_set_wfe_mode',0)
        smp_trace=list(self.trace); self.trace=[]
        asc, msg = HEAP+0x100, HEAP+0x200
        layout=self.b.types['asc_dev']
        self.uc.mem_write(asc+layout['base'],struct.pack('<Q',0x230000000))
        self.uc.mem_write(msg,struct.pack('<QQ',0x123456789abcdef0,0xabcdef01))
        sent=self.call('asc_send',asc,msg)
        # Replay a normal legacy I2A message on the same mailbox.
        self.uc.mem_write(0x230000830,struct.pack('<QQ',0xfedcba9876543210,0x10abcdef01))
        received=self.call('asc_recv',asc,msg)
        return dict(startup=startup_trace,init=init_trace,frequency_return=result,frequency=freq_trace,smp=smp_trace,
                    asc=self.trace,sent=sent,received=received,message=bytes(self.uc.mem_read(msg,12)).hex())

baseline=Binary(BASE/'build/aura-raw.elf')
current=Binary(CURRENT/'build/aura-raw.elf')
headers='\n'.join(p.read_text() for p in (BASE/'src').glob('*.h'))
parts={k:int(v,16) for k,v in re.findall(r'#define\s+(MIDR_PART_\w+)\s+(0x[0-9a-fA-F]+)',headers)}
cases=[]
for name,label in re.findall(r'\{(MIDR_PART_\w+),\s*"([^"]+)"',(BASE/'src/chickens.c').read_text()):
    chipname=name[len('MIDR_PART_'):].split('_')[0]
    chip=int(chipname[1:],16) if chipname.startswith('T') else int(chipname[1:],16) if chipname.startswith('S') and chipname[1:].isalnum() and not chipname.startswith('S5L') else 0x8960
    if chip in (0x8142,0x8152,0x6050,0x6051): continue
    cases.append((label,chip,parts[name]))
results=[]
for label,chip,part in cases:
    for features,fail in [(False,False),(True,False),(True,True)]:
        a=Run(baseline,chip,part,features,fail); before=a.scenario();del a
        b=Run(current,chip,part,features,fail); after=b.scenario();del b;gc.collect()
        if before!=after:
            (HERE/'legacy-replay-mismatch.json').write_text(json.dumps(dict(label=label,features=features,fail=fail,before=before,after=after),indent=2))
            raise AssertionError((label,features,fail,'see legacy-replay-mismatch.json'))
        results.append(dict(cpu=label,chip=hex(chip),part=hex(part),features=features,poll_failure=fail,
                            trace_sha256=hashlib.sha256(json.dumps(before,sort_keys=True).encode()).hexdigest(),
                            register_operations=len(before['init']),mmio_operations=len(before['frequency'])))
    print('PASS',label,flush=True)
output=dict(result='PASS',scope='compiled AArch64 instruction replay; not physical older-chip proof',
            baseline_sha256=hashlib.sha256(baseline.data).hexdigest(),candidate_sha256=hashlib.sha256(current.data).hexdigest(),cases=results)
(HERE/'legacy-replay-result.json').write_text(json.dumps(output,indent=2)+'\n')
print('LEGACY_REPLAY_PASS',len(results),flush=True)
