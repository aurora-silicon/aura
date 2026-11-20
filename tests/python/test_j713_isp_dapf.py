# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the production T8132 DAPF range export on the J713 ISP DART's ADT records."""
import json
from pathlib import Path
import re
import subprocess

from test_j713_aop_dapf import PREFIX
from test_j713_dapf_instance import block

TESTS = r'''
static unsigned filter_writes_at_start;
static void reset(void) {
 memcpy(nodes,original,sizeof(nodes));chip_id=T8132;writable=false;filter_writes=0;
}
static int ranges(u32 r0,u32 r4,u32 r20,u64 *out,int max) {
 return dapf_t8132_ranges("/arm-io/dart-isp",r0,r4,r20,out,max);
}
int main(void) {
 u64 r[32];
 int n;
 /* Every slice, in record order, with the words the kernel writes. */
 reset();n=ranges(0x31,0,1,r,16);
 assert(n==13);for(int i=0;i<n;i++)printf("R %lx %lx\n",r[2*i],r[2*i+1]);
 /* Refused: too little room, other words, another SoC. */
 reset();assert(ranges(0x31,0,1,r,12)<0);
 reset();assert(ranges(0x31,0,2,r,16)<0);
 reset();assert(ranges(0x31,4,1,r,16)<0);
 reset();assert(ranges(0x30,0,1,r,16)<0);
 /* The bank is found by its type, wherever it sits in the instance list. */
 static u8 inst[7*16];static u8 tmp[16];u64 t;
 reset();memcpy(inst,nodes[2].instance,sizeof(inst));
 memcpy(tmp,inst+4*16,16);memcpy(inst+4*16,inst+5*16,16);memcpy(inst+5*16,tmp,16);
 t=nodes[2].base[4];nodes[2].base[4]=nodes[2].base[5];nodes[2].base[5]=t;
 t=nodes[2].size[4];nodes[2].size[4]=nodes[2].size[5];nodes[2].size[5]=t;
 nodes[2].instance=inst;assert(ranges(0x31,0,1,r,16)==13);
 /* No DAPF bank (it reads as GAPF), or two of them: no bank to pick. */
 reset();memcpy(inst,nodes[2].instance,sizeof(inst));memcpy(inst+5*16,inst+6*16,4);
 nodes[2].instance=inst;assert(ranges(0x31,0,1,r,16)<0);
 reset();memcpy(inst,nodes[2].instance,sizeof(inst));memcpy(inst+4*16,inst+5*16,4);inst[4*16+4]=1;
 nodes[2].instance=inst;assert(ranges(0x31,0,1,r,16)<0);
 reset();chip_id=T8140;assert(ranges(0x31,0,1,r,16)<0);
 /* A slice that ends before it starts. */
 static u8 config[13*52];
 reset();memcpy(config,nodes[2].config,sizeof(config));
 u64 start=~0UL;memcpy(config+4*52,&start,8);nodes[2].config=config;
 assert(ranges(0x31,0,1,r,16)<0);
 /* It only reads the ADT. */
 assert(!filter_writes);(void)filter_writes_at_start;
 puts("ISP DAPF: ranges, refusals and read-only pass");
 return 0;
}
'''


def expected_ranges(config):
    return [(int.from_bytes(config[i:i + 8], "little"),
             int.from_bytes(config[i + 8:i + 16], "little"))
            for i in range(0, len(config), 52)]


def test_j713_isp_dapf(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/dapf.c").read_text()
    production = source[source.index("struct dapf_t8020_config {"):]
    window = block((repo / "src/dapf.h").read_text(), "struct dapf_window {") + ";\n"
    aop = json.loads((repo / "tests/data/j713-dapf-instance.json").read_text())["nodes"]
    isp = json.loads((repo / "tests/data/j713-isp-dapf.json").read_text())
    # The AOP and MTP from the shared fixture, and the ISP in the third slot.
    nodes = [aop[0], aop[1], isp]
    declarations, entries = [], []
    for i, node in enumerate(nodes):
        for label, key in [("inst", "instance_hex"), ("cfg", "filter_hex")]:
            raw = bytes.fromhex(node[key])
            declarations.append(f'static const u8 {label}{i}[] = {{{",".join(map(str, raw))}}};')
        bases = ",".join(r["base"] for r in node["reg"])
        sizes = ",".join(r["size"] for r in node["reg"])
        entries.append(f'{{"{node["name"]}",inst{i},cfg{i},sizeof(inst{i}),sizeof(cfg{i}),'
                       f'{{{bases}}},{{{sizes}}},{len(node["reg"])}}}')
    inputs = "\n".join(declarations) + '\nstatic const struct node original[3]={' + ",".join(entries) + '};\n'
    # The ISP DART has seven register ranges.
    prefix = PREFIX.replace("u64 base[3], size[3];", "u64 base[8], size[8];", 1)
    assert prefix != PREFIX
    harness = prefix + window + production + inputs + TESTS
    expected = expected_ranges(bytes.fromhex(isp["filter_hex"]))
    cfile, binary = tmp_path / "isp-dapf.c", tmp_path / "isp-dapf"

    def run(text):
        cfile.write_text(text)
        if subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                           "-Wno-unused-parameter", "-Wno-unused-function",
                           str(cfile), "-o", str(binary)]).returncode:
            return False
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        got = [(int(a, 16), int(b, 16))
               for a, b in re.findall(r"^R ([0-9a-f]+) ([0-9a-f]+)$", result.stdout, re.M)]
        return result.returncode == 0 and got == expected

    assert run(harness)
    controls = [
        ("words", "config[i].r4 != r4 ||", "false ||"),
        ("room", "count > (u32)max", "false"),
        ("order", "ranges[2 * i] = config[i].start;", "ranges[2 * i] = config[count - 1 - i].start;"),
        ("slice order", "entry.end < entry.start", "false"),
        ("smmu", "case DAPF_INSTANCE_SMMU:", ""),
        ("one bank", "if (found >= 0) {", "if (false) {"),
    ]
    for title, old, new in controls:
        bad = harness.replace(old, new, 1)
        assert bad != harness, title
        assert not run(bad), title
