#!/usr/bin/env python3
"""Actual-source end-to-end assist/context replay; no guest/hardware access."""
import argparse,hashlib,json,pathlib,re,subprocess,sys
sys.dont_write_bytecode=True
H=pathlib.Path(__file__).resolve().parent;R=H.parents[1]
def sha(p):return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
def function(text,name):
 m=re.search(r'^(?:static )?(?:inline )?(?:void|bool|u64|int) '+re.escape(name)+r'\([^;]*?\)\s*\{',text,re.M)
 if not m:raise ValueError('Missing function '+name)
 start=m.start();pos=text.index('{',start);depth=0;state='code'
 while pos<len(text):
  c=text[pos];pair=text[pos:pos+2]
  if state=='line':
   if c=='\n':state='code'
  elif state=='block':
   if pair=='*/':state='code';pos+=1
  elif state in ['"',"'"]:
   if c=='\\':pos+=1
   elif c==state:state='code'
  elif pair=='//':state='line';pos+=1
  elif pair=='/*':state='block';pos+=1
  elif c in ['"',"'"]:state=c
  elif c=='{':depth+=1
  elif c=='}':
   depth-=1
   if depth==0:return text[start:pos+1]
  pos+=1
 raise ValueError('Unterminated '+name)
def main():
 p=argparse.ArgumentParser();p.add_argument('--out',type=pathlib.Path,required=True);p.add_argument('--expect-fast',action='store_true');p.add_argument('--baseline',type=pathlib.Path);a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True)
 sources={'src/utils.h':['timer_scale_alias','get_ticks'],'src/hv/windows/hv.c':['hv_host_counter'],'src/hv/windows/hv_exc.c':['hv_handle_software_pmu','hv_t8142_sysreg_assist_impl','hv_handle_t8142_sysreg_assist','hv_t8142_insn_is_absent_reg','hv_emulate_t8142_pending_undef','hv_handle_t8142_pmu_site','hv_handle_t8142_undef_trampoline']}
 layout=re.search(r'struct exc_info \{.*?^\};',(R/'src/exception.h').read_text(),re.M|re.S)[0]
 code=(H/'host.h').read_text().replace('/* ACTUAL_EXC_INFO */',layout)+'\n';extracted={'exc_info':hashlib.sha256(layout.encode()).hexdigest()}
 for path,names in sources.items():
  text=(R/path).read_text()
  for name in names:
   f=function(text,name);extracted[name]=hashlib.sha256(f.encode()).hexdigest()
   if name=='hv_t8142_sysreg_assist_impl':f=f.replace('{','{\n    generic_entries++; /* E2E-only observer; branch/body unchanged. */',1)
   code+=f+'\n\n'
 code+=(H/'e2e.c').read_text();cc=pathlib.Path(subprocess.check_output(['/usr/bin/xcrun','--find','clang'],text=True).strip()).resolve();sdk=subprocess.check_output(['/usr/bin/xcrun','--show-sdk-path'],text=True).strip()
 inputs=[R/x for x in sources]+[R/'src/arm_cpu_regs.h',R/'src/hv/windows/windows_regs.h',R/'src/exception.h',R/'src/types.h',H/'host.h',H/'e2e.c',pathlib.Path(__file__),R/'docs/windows-pmu-assist-fastpath.md']
 before={str(p):sha(p) for p in inputs};receipt={'scope':'Actual extracted clock, software PMU, generic/wrapper, indexed-site, pending-UNDEF and trampoline bodies; one E2E-only generic-entry observer. Hardware registers/translation/GIC carrier are modeled; complete GIC and surrounding production bodies remain source-pinned. No hardware or elapsed-speed claim.','source_sha256':before,'function_sha256':extracted,'compiler':{'path':str(cc),'sha256':sha(cc),'sdk':sdk},'expect_fast':a.expect_fast,'mutations':[],'commands':[]}
 mutations=[]
 if a.expect_fast:
  patterns=[('wrong-register-mask','instruction & 0xffffffe0U','instruction & 0xffffff00U'),('missing-chip-guard','chip_id == T8142 &&','true &&'),('missing-feature-guard','!cpu_features->apple_sysregs_unlocked &&','true &&'),('raw-unscaled-clock','u64 value = hv_host_counter();','u64 value = mrs(SYS_IMP_APL_CNTVCT_ALIAS_EL0);'),('wrong-output','*output = value;','*output = value + 1;'),('missing-fallback','return hv_t8142_sysreg_assist_impl(instruction, input, output, false);','return false;'),('lost-lr-write','&& rt < 31) /* MRS Xt, sysreg */','&& rt < 30) /* MRS Xt, sysreg */'),('uninitialized-generic-scratch','u64 regs[32] = {0};','u64 regs[32];'),('wrong-pending-return','ctx->elr = fault_pc + 4;','ctx->elr = fault_pc + 8;')]
  for name,old,new in patterns:
   assert old in code,name;mutations.append((name,code.replace(old,new,1)))
 for name,text in [('actual',code)]+mutations:
  d=a.out/name;d.mkdir();(d/'production.c').write_text(text);cmd=[str(cc),'-isysroot',sdk,'-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-sign-compare','-Wno-format','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ftrivial-auto-var-init=pattern','-DAURA_WINDOWS','-DEXPECT_FAST='+str(int(a.expect_fast)),'-I',str(R/'src'),str(d/'production.c'),'-o',str(d/'replay')]
  b=subprocess.run(cmd,capture_output=True,text=True,timeout=60);(d/'build.log').write_text(b.stdout+b.stderr);receipt['commands'].append({'argv':cmd,'returncode':b.returncode});assert b.returncode==0,(name,'build',b.stderr[-2500:])
  run=subprocess.run([str(d/'replay')],capture_output=True,text=True,timeout=60);(d/'run.log').write_text(run.stdout+run.stderr)
  if name=='actual':
   assert run.returncode==0,run.stderr;checks=[s[5:] for s in run.stdout.splitlines() if s.startswith('PASS ')];trace=[json.loads(s[6:]) for s in run.stdout.splitlines() if s.startswith('TRACE ')];receipt.update(checks=checks,assertions=int(re.search(r'CHECKS (\d+)',run.stdout)[1]),trace_events=len(trace));(a.out/'trace.json').write_text(json.dumps(trace,indent=2)+'\n')
  else:
   assert run.returncode!=0 and 'FAIL ' in run.stderr,(name,'mutant survived',run.stdout[-500:]);receipt['mutations'].append({'name':name,'returncode':run.returncode,'failure':run.stderr.splitlines()[0]})
 if a.baseline:
  assert (a.out/'trace.json').read_bytes()==(a.baseline/'trace.json').read_bytes(),'semantic event trace changed';receipt['baseline_trace_sha256']=sha(a.baseline/'trace.json');receipt['trace_matches_base']=True
 assert all(sha(p)==v for p,v in before.items()) and sha(cc)==receipt['compiler']['sha256']
 receipt['artifacts_sha256']={str(p.relative_to(a.out)):sha(p) for p in a.out.rglob('*') if p.is_file()};receipt['verdict']='PASS';(a.out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({'verdict':'PASS','checks':len(receipt['checks']),'assertions':receipt['assertions'],'trace_events':receipt['trace_events'],'mutants':len(receipt['mutations'])}))
if __name__=='__main__':main()
