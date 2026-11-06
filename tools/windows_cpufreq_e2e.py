#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay the real host-to-Mu CPU-frequency handoff without accessing hardware."""
import argparse, hashlib, json, pathlib, shutil, struct, subprocess, sys
ROOT=pathlib.Path(__file__).resolve().parents[1]
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser();p.add_argument('--mu',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 report={'result':'failed','hardware_accessed':False,'cases':[],'sources':{}}
 try:
  sys.path.insert(0,str(ROOT/'proxyclient'))
  from aura.windows import plan_profile, cpufreq_handoff_marker
  frequencies=[[972000,1152000,1584000,1992000,2352000,2700000,2964000,3048000],[1308000,1608000,1932000,2220000,2508000,2820000,3072000,3300000,3516000,3720000,3876000,4020000,4176000,4248000,4272000,4332000,4428000,4416000,4464000]]
  tables=[[word for khz in ladder for word in (khz,900)] for ladder in frequencies]
  image=out/'fixture.fd';image.write_bytes(bytes(65536))
  base={'schema':'aura.windows.boot.v1','name':'cpu-handoff-replay','target':{'chip_id':'0x8142','model':'Mac17,3'},'mu':{'path':str(image),'sha256':sha(image),'entry_offset':0},'usb':'disabled'}
  for name,policy,chip,board,model,accepted in [('disabled','disabled',0x8142,0x24,'Mac17,3',True),('enabled','j813-manual-v1',0x8142,0x24,'Mac17,3',True),('wrong-chip','j813-manual-v1',0x8152,0x24,'Mac17,3',False),('wrong-board','j813-manual-v1',0x8142,0x23,'Mac17,3',False),('wrong-model','j813-manual-v1',0x8142,0x24,'Mac17,4',False),('unknown','auto',0x8142,0x24,'Mac17,3',False),('wrong-type',True,0x8142,0x24,'Mac17,3',False)]:
   profile=dict(base,cpufreq=policy,target={'chip_id':hex(chip),'model':model});f=out/(name+'.json');f.write_text(json.dumps(profile))
   try:
    plan=plan_profile(f);marker=cpufreq_handoff_marker(plan.cpufreq,chip,board,model,tables)
    assert marker==(struct.pack('<I',1) if policy=='j813-manual-v1' else None)
    ok=True
   except ValueError:ok=False
   assert ok==accepted,name
   report['cases'].append({'name':name,'passed':True})
  for mutation in ['missing','wrong-frequency','missing-word','extra-word','wrong-type']:
   changed=[list(t) for t in tables]
   if mutation=='missing':changed=None
   elif mutation=='wrong-frequency':changed[1][0]+=1
   elif mutation=='missing-word':changed[0].pop()
   elif mutation=='extra-word':changed[0].append(0)
   else:changed[0][1]=True
   try:cpufreq_handoff_marker('j813-manual-v1',0x8142,0x24,'Mac17,3',changed)
   except ValueError:pass
   else:raise AssertionError('accepted '+mutation)
   report['cases'].append({'name':mutation,'passed':True})
  f=out/'default.json';f.write_text(json.dumps(base));assert plan_profile(f).cpufreq=='disabled'
  mu=a.mu.resolve();source=mu/'Silicon/Apple/AppleSiliconPkg/Drivers/AcpiPlatformDxe/AcpiPlatform.c';asl=mu/'Platform/MacBookAir2026Pkg/AcpiTables/CPUF.asl';harness=ROOT/'tests/e2e/windows_cpufreq_gate.c'
  text=source.read_text();start=text.index('STATIC\nBOOLEAN\nAppleCpuFreqTableAdmitted (');end=text.index('\n}\n',start)+3
  (out/'cpufreq-gate.inc').write_text(text[start:end])
  assert 'if (!AppleCpuFreqTableAdmitted ((EFI_ACPI_DESCRIPTION_HEADER *)CurrentTable, Size))' in text
  for f in [source,asl,harness,pathlib.Path(__file__).resolve(),ROOT/'proxyclient/aura/windows.py',ROOT/'proxyclient/aura/hv/__init__.py']:
   report['sources'][str(f)]=sha(f)
  for cmd in [['iasl','-p',str(out/'CPUF'),str(asl)],['clang','-std=c17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(out),str(harness),'-o',str(out/'gate')],[str(out/'gate'),str(out/'CPUF.aml')]]:
   r=subprocess.run(cmd,text=True,capture_output=True,timeout=30);(out/('command-'+str(len(list(out.glob('command-*'))))+'.log')).write_text(r.stdout+r.stderr);r.check_returncode()
  aml=(out/'CPUF.aml').read_bytes();assert b'AURA8142' in aml
  for address in [0x210e20000,0x211e20000]:assert struct.pack('<Q',address) in aml and struct.pack('<Q',address+0xfff) in aml
  assert sum(aml)%256==0
  native=(ROOT/'src/hv/windows/hv.c').read_text()
  pieces=[]
  for signature in ['int hv_windows_configure(u64 flags)','bool hv_windows_cpufreq_owned(void)']:
   begin=native.index(signature);finish=native.index('\n}\n',begin)+3;pieces.append(native[begin:finish])
  (out/'cpufreq-native.inc').write_text('\n'.join(pieces))
  proxy=(ROOT/'src/proxy.c').read_text();begin=proxy.index('        case P_CPUFREQ_INIT:');finish=proxy.index('        case P_READ_GIGALOCKER:',begin)
  (out/'cpufreq-proxy.inc').write_text(proxy[begin:finish])
  native_harness=ROOT/'tests/e2e/windows_cpufreq_native.c'
  for file in [ROOT/'src/hv/windows/hv.c',ROOT/'src/proxy.c',native_harness]:report['sources'][str(file)]=sha(file)
  for cmd in [['clang','-std=c17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(out),str(native_harness),'-o',str(out/'native')],[str(out/'native')]]:
   result=subprocess.run(cmd,text=True,capture_output=True,timeout=30);(out/('native-'+str(len(list(out.glob('native-*'))))+'.log')).write_text(result.stdout+result.stderr);result.check_returncode()
  report.update(result='passed',aml_sha256=sha(out/'CPUF.aml'),scope='Actual profile/marker/compiled Mu gate/compiled resource table; no MMIO or Windows PEP qualification')
 except Exception as e:report['error']=type(e).__name__+': '+str(e)
 finally:(out/'receipt.json').write_text(json.dumps(report,indent=2)+'\n')
 print(report['result'],str(out/'receipt.json'));return report['result']!='passed'
if __name__=='__main__':raise SystemExit(main())
