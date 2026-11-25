// SPDX-License-Identifier: MIT
//! rust/src/apfs.rs on a disk image, through rust_read_gigalocker(): the GPT, the APFS
//! container superblock, object maps, B-tree nodes and the volume's file extent, all read from
//! the disk. GPT CRCs are fixed up unless the image's first byte is odd.
#![no_main]

use aura_disk_fuzz::{apfs, disk};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut image = data.to_vec();
    if image.first().is_some_and(|b| b & 1 == 0) {
        disk::fix_gpt_crcs(&mut image);
    }
    disk::set(&image);
    let mut size = 0;
    let gl = unsafe { apfs::rust_read_gigalocker(&mut size) };
    if gl.is_null() {
        assert_eq!(size, 0);
    } else {
        unsafe { apfs::rust_free_gigalocker(gl, size) };
    }
    disk::with(|d| assert!(d.write_log.is_empty(), "the APFS reader wrote to the disk"));
});
