# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import zlib

import pytest


TOOL = Path(__file__).resolve().parents[2] / "tools/fill_stage1_config.py"
spec = importlib.util.spec_from_file_location("stage1_config", TOOL)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_fill_preserves_bare_image_and_checksum():
    generic = b"prefix" + module.MAGIC + bytes(module.BLOCK_SIZE - len(module.MAGIC))
    generic += b"suffixSTACKBOT"
    generic = bytearray(generic)
    struct.pack_into("<I", generic, len(b"prefix") + len(module.MAGIC), 1)
    image = module.fill(bytes(generic), "12345678-abcd-1234-abcd-1234567890ab",
                        "aurora/stage2.bin", 15000)
    assert len(image) == len(generic)
    assert image.endswith(b"STACKBOT")
    body_start = len(b"prefix") + len(module.MAGIC)
    body = image[body_start:body_start + module.BODY.size]
    assert struct.unpack_from("<II", body) == (1, 15000)
    assert struct.unpack_from("<I", image, body_start + module.BODY.size)[0] == zlib.crc32(body)
    assert image.count(b";aurora/stage2.bin") == 1


def test_fill_takes_the_padded_stage1_path():
    generic = bytearray(module.MAGIC + bytes(module.BLOCK_SIZE - len(module.MAGIC)) + b"STACKBOT")
    struct.pack_into("<I", generic, len(module.MAGIC), 1)
    image = module.fill(bytes(generic), "12345678-abcd-1234-abcd-1234567890ab",
                        "m1n1/boot-ctxr.bin", 0)
    assert image.count(b";m1n1/boot-ctxr.bin\0") == 1


@pytest.mark.parametrize("uuid,path", [
    ("1234", "aurora/stage2.bin"),
    ("12345678-abcd-1234-abcd-1234567890ab", "/absolute.bin"),
    ("12345678-abcd-1234-abcd-1234567890ab", "../escape.bin"),
])
def test_fill_rejects_invalid_target(uuid, path):
    with pytest.raises(ValueError):
        module.fill(b"STACKBOT", uuid, path, 15000)


@pytest.mark.parametrize("path", [
    "aurora/stage\0two.bin",  # embedded NUL
    "aurora/stage\x1ftwo.bin",  # control character
    "aurora//stage2.bin",  # empty component
    "aurora/./stage2.bin",  # dot component
    "aurora/../stage2.bin",
])
def test_installer_rejects_invalid_path_grammar(path):
    assert not module.valid_path(path)
    with pytest.raises(ValueError):
        module.fill(b"STACKBOT", "12345678-abcd-1234-abcd-1234567890ab", path, 15000)


def test_c_stage1_parser_uses_same_path_grammar(tmp_path):
    source = (TOOL.parents[1] / "src/stage1_config.c").read_text()
    start = source.index("static bool valid_path(")
    end = source.index("static bool stage1_config_valid(", start)
    helper = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
extern struct { char stage2_path[192]; } config;
''' + helper + r'''
int main(void)
{
    char path[192] = {0};
    strcpy(path, "aurora/stage2.bin");
    assert(valid_path(path));
    strcpy(path, ";aurora/stage2.bin");
    assert(valid_path(path));
    strcpy(path, ";m1n1/boot-ctxr.bin");
    assert(valid_path(path));
    strcpy(path, ";;aurora/stage2.bin");
    assert(!valid_path(path));
    memset(path, 0, sizeof(path));
    memcpy(path, "aurora/a\0b", 10);
    assert(!valid_path(path));
    memset(path, 0, sizeof(path));
    strcpy(path, "aurora/a\037b");
    assert(!valid_path(path));
    strcpy(path, "aurora//stage2.bin");
    assert(!valid_path(path));
    strcpy(path, "aurora/./stage2.bin");
    assert(!valid_path(path));
    strcpy(path, "aurora/../stage2.bin");
    assert(!valid_path(path));
    return 0;
}
'''
    binary = tmp_path / "stage1-path"
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_rust_fat_parser_uses_same_path_grammar(tmp_path):
    source = (TOOL.parents[1] / "rust/src/chainload.rs").read_text()
    start = source.index("fn valid_fat_path(")
    end = source.index("#[cfg(test)]", start)
    helper = source[start:end]
    harness = helper + r'''
fn main() {
    assert!(valid_fat_path("aurora/stage2.bin"));
    assert!(valid_fat_path("m1n1/boot-ctxr.bin"));
    for path in ["aurora/a\0b", "aurora/a\x1fb", "aurora//b",
                 "aurora/./b", "aurora/../b"] {
        assert!(!valid_fat_path(path), "accepted {path:?}");
    }
}
'''
    binary = tmp_path / "rust-stage1-path"
    subprocess.run(["rustc", "--edition=2021", "-Dwarnings", "-o", str(binary), "-"],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


@pytest.mark.parametrize("options,expected", [([], 0), (["--window-ms", "15000"], 15000)])
def test_cli_normal_boot_does_not_wait(tmp_path, options, expected):
    image = bytearray(module.MAGIC + bytes(module.BLOCK_SIZE - len(module.MAGIC)) + b"STACKBOT")
    struct.pack_into("<I", image, len(module.MAGIC), 1)
    source, output = tmp_path / "generic.bin", tmp_path / "configured.bin"
    source.write_bytes(image)
    subprocess.run([sys.executable, str(TOOL), "--input", str(source), "--output", str(output),
                    "--esp-partuuid", "12345678-abcd-1234-abcd-1234567890ab", *options], check=True)
    configured = output.read_bytes()
    assert struct.unpack_from("<II", configured, len(module.MAGIC)) == (1, expected)
    assert source.read_bytes() == image
