// SPDX-License-Identifier: MIT
//! rust/src/gpt.rs on a disk image: the header and entry-array checks, every entry and the
//! PARTUUID lookup chainload.rs does. The first byte of the image (in the protective MBR,
//! which aura never reads) selects whether the CRCs are fixed up first.
#![no_main]

use aura_disk_fuzz::{disk, gpt::GPT, nvme::NVMEStorage};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut image = data.to_vec();
    if image.first().is_some_and(|b| b & 1 == 0) {
        disk::fix_gpt_crcs(&mut image);
    }
    disk::set(&image);
    let Ok(mut pt) = GPT::new(NVMEStorage::new(1, 0)) else {
        return;
    };
    for i in 0..pt.count() {
        if let Ok(part) = pt.index(i) {
            if !part.get_type_guid().is_nil() {
                assert!(part.get_starting_lba() <= part.get_ending_lba());
            }
        }
    }
    assert!(pt.index(pt.count()).is_err());
    if let Some((uuid, _, _)) = disk::first_partition(&image) {
        let _ = pt.find_by_partuuid(uuid::Uuid::parse_str(&uuid).unwrap());
    }
});
