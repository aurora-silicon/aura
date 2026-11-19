#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('fill', root / 'tools/fill_stage1_config.py')
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp); (tmp / 'src/tinf').mkdir(parents=True); (tmp / 'build').mkdir()
    for name in ('stage1_config.c','stage1_config.h'):
        (tmp/'src'/name).write_bytes((root/'src'/name).read_bytes())
    (tmp/'build/build_cfg.h').write_text('#define CHAINLOADING\n')
    (tmp/'src/types.h').write_text('#include <stdbool.h>\n#include <stdint.h>\ntypedef uint32_t u32;\n')
    (tmp/'src/assert.h').write_text('#pragma once\n#include_next <assert.h>\n#define static_assert _Static_assert\n')
    (tmp/'src/string.h').write_text('#include_next <string.h>\n')
    (tmp/'src/utils.h').write_text('#include <stdio.h>\n')
    for name in ('crc32.c','tinf.h'):
        (tmp/'src/tinf'/name).write_bytes((root/'src/tinf'/name).read_bytes())
    blank = mod.MAGIC + (1).to_bytes(4, 'little') + bytes(mod.BLOCK_SIZE - 20) + b'STACKBOT'
    fixture = mod.fill(blank, '01234567-89ab-cdef-0123-456789abcdef', 'aurora/boot.bin', 15000)
    (tmp/'config').write_bytes(fixture[:mod.BLOCK_SIZE])
    for target in ('../boot.bin', '/boot.bin', 'x//y', 'x;y', 'x\\y'):
        try: mod.fill(blank, '01234567-89ab-cdef-0123-456789abcdef', target, 15000)
        except ValueError: pass
        else: raise AssertionError(target)
    subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-Wall','-Werror','-I'+str(tmp/'src'),str(root/'tests/stage1_config_host.c'),str(tmp/'src/tinf/crc32.c'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test'), str(tmp/'config')], check=True)
print('PASS: stage1 config CRC, factory proxy fallback, UUID/path/window validation')
