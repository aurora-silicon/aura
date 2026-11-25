// SPDX-License-Identifier: MIT
//! rust/src/secboot.rs: aura_secboot_verify() on an arbitrary record body and covered bytes.
//! Input: body length (u16 LE), body, covered bytes; checked with the published test key and
//! with a key the input names.
#![no_main]

use aura_disk_fuzz::secboot::{self, Verdict};
use libfuzzer_sys::fuzz_target;

const TEST_KEY: &[u8; 32] = include_bytes!("../../../../data/secboot/test-ed25519.pub");

fuzz_target!(|data: &[u8]| {
    if data.len() < 2 {
        return;
    }
    let blen = (u16::from_le_bytes([data[0], data[1]]) as usize).min(data.len() - 2);
    let (body, covered) = data[2..].split_at(blen);
    let mut own = [0u8; 32];
    let n = covered.len().min(32);
    own[..n].copy_from_slice(&covered[..n]);
    for key in [TEST_KEY, &own] {
        let mut digest = [0u8; 32];
        let mut secver = 0u32;
        let mut key_id = [0u8; 8];
        let v = unsafe {
            secboot::aura_secboot_verify(
                body.as_ptr(),
                body.len(),
                covered.as_ptr(),
                covered.len(),
                key.as_ptr(),
                1,
                digest.as_mut_ptr(),
                &mut secver,
                key_id.as_mut_ptr(),
            )
        };
        assert!(v <= Verdict::Rollback as u32);
        let (verdict, header) = secboot::verify(body, covered, key, 1, &mut [0u8; 32]);
        assert_eq!(verdict as u32, v);
        if v == Verdict::Verified as u32 {
            let h = header.unwrap();
            assert_eq!(h.covered_len, covered.len() as u64);
            assert_eq!(h.key_id, secboot::key_id(key));
            assert!(h.security_version >= 1);
        }
    }
});
