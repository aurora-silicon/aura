// SPDX-License-Identifier: MIT
use sha2::{Digest, Sha256};

/// Hash a validated, readable byte range into a caller-owned 32-byte buffer.
///
/// # Safety
/// `data` must be readable for `size` bytes; `out` must be writable for 32.
#[no_mangle]
pub unsafe extern "C" fn aura_sha256(data: *const u8, size: usize, out: *mut u8) {
    let bytes = unsafe { core::slice::from_raw_parts(data, size) };
    let digest = Sha256::digest(bytes);
    unsafe { core::ptr::copy_nonoverlapping(digest.as_ptr(), out, 32) };
}
