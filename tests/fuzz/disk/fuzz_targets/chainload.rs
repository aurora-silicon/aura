// SPDX-License-Identifier: MIT
//! rust/src/chainload.rs on a disk image: the GPT, aura's NVMe read path (nvme.rs), the FAT
//! file system of the ESP (the fatfs crate aura builds) and the aura-try marker (trial.rs).
//!
//! The image's first byte (protective MBR, never read by aura) picks the call:
//!   bits 1-2: 0 rust_load_image() of aurora/stage2.bin with a trial buffer,
//!             1 the same taking no trial, 2 rust_take_boot_trial(), 3 rust_read_into()
//!   bit 0:    leave the GPT CRCs as they are (otherwise they are fixed up)
//! The ESP is the first partition of the image's GPT.
//!
//! Checked: no panic; a loaded image is 1..=256 MiB; the only write is one block of the ESP
//! that held an armed marker and now reads `spent`, and it is flushed.
#![no_main]

use aura_disk_fuzz::{chainload, disk};
use libfuzzer_sys::fuzz_target;
use std::ffi::{c_char, c_void, CString};

fuzz_target!(|data: &[u8]| {
    let mut image = data.to_vec();
    let sel = image.first().copied().unwrap_or(0);
    if sel & 1 == 0 {
        disk::fix_gpt_crcs(&mut image);
    }
    disk::set(&image);
    let Some((uuid, first, last)) = disk::first_partition(&image) else {
        return;
    };

    // what a block the run writes held before
    let before = |lba: u64| {
        let mut b = vec![0u8; disk::SECTOR];
        let start = lba as usize * disk::SECTOR;
        if start < image.len() {
            let end = (start + disk::SECTOR).min(image.len());
            b[..end - start].copy_from_slice(&image[start..end]);
        }
        b
    };

    match (sel >> 1) & 3 {
        0 | 1 => {
            let spec = CString::new(format!("{uuid};aurora/stage2.bin")).unwrap();
            let mut img: *mut c_void = std::ptr::null_mut();
            let mut size = 0usize;
            let mut proxy = false;
            let mut trial_buf = [0u8; 256];
            let (trial_ptr, trial_cap) = if (sel >> 1) & 3 == 0 {
                (trial_buf.as_mut_ptr() as *mut c_char, trial_buf.len())
            } else {
                (std::ptr::null_mut(), 0)
            };
            let ret = unsafe {
                chainload::rust_load_image(
                    spec.as_ptr(),
                    &mut img,
                    &mut size,
                    &mut proxy,
                    trial_ptr,
                    trial_cap,
                )
            };
            if ret == 0 {
                assert!(
                    !img.is_null() && size > 0 && size <= 256 << 20,
                    "image of {size} bytes"
                );
                unsafe { chainload::rust_free_image(img, size) };
            }
            if trial_cap > 0 {
                assert!(trial_buf.contains(&0), "trial path not terminated");
            }
        }
        2 => {
            let raw = CString::new(uuid.clone()).unwrap();
            let mut line = [0u8; 512];
            let ret = unsafe {
                chainload::rust_take_boot_trial(raw.as_ptr(), line.as_mut_ptr().cast(), line.len())
            };
            if ret == 0 {
                let end = line
                    .iter()
                    .position(|&b| b == 0)
                    .expect("unterminated line");
                assert_eq!(
                    std::str::from_utf8(&line[..end])
                        .unwrap()
                        .split(';')
                        .count(),
                    3
                );
            }
        }
        _ => {
            let spec = CString::new(format!("{uuid};aurora/stage2.bin")).unwrap();
            let mut dst = vec![0u8; 1 << 20];
            let mut size = 0usize;
            let ret = unsafe {
                chainload::rust_read_into(
                    spec.as_ptr(),
                    dst.as_mut_ptr().cast(),
                    dst.len(),
                    &mut size,
                )
            };
            if ret == 0 {
                assert!(size <= dst.len());
            }
        }
    }

    disk::with(|d| {
        assert!(d.write_log.len() <= 1, "{} writes", d.write_log.len());
        if let Some(&lba) = d.write_log.first() {
            assert!(
                lba >= first && lba <= last,
                "write to block {lba} outside the ESP"
            );
            let old = before(lba);
            let new = &d.writes[&lba];
            assert!(
                old.starts_with(aura_disk_fuzz::TRIAL_ARMED),
                "overwrote a block that held no armed marker"
            );
            assert!(new.starts_with(aura_disk_fuzz::TRIAL_SPENT));
            assert_eq!(
                old[aura_disk_fuzz::TRIAL_SPENT.len()..],
                new[aura_disk_fuzz::TRIAL_SPENT.len()..]
            );
            assert!(d.flushes >= 1, "marker write not flushed");
        }
    });
});
