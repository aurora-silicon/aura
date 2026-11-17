// SPDX-License-Identifier: MIT
//! Signed aura payloads and boot measurements (docs/secure-boot.md).
//!
//! An `aura_sig` record covers the bytes that follow it. Its body is a fixed header, then an
//! Ed25519 signature over `DOMAIN || header || SHA-256(covered bytes)`. The stage that runs the
//! payload checks it before it parses anything else, with a public key built into that stage.

use ed25519_compact::{PublicKey, Signature};
use sha2::{Digest, Sha256};

/// Domain separation for every aura signature.
pub const DOMAIN: &[u8; 16] = b"AURA-SIG-V1\0\0\0\0\0";
pub const BODY_MAGIC: &[u8; 8] = b"AURSIG01";
pub const ALG_ED25519: u32 = 1;

/// Signed header, little endian, right after the `aura_sig` tag and its u32 record size.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Header {
    pub version: u32,
    pub alg: u32,
    pub security_version: u32,
    pub flags: u32,
    pub covered_len: u64,
    pub key_id: [u8; 8],
}

/// Header bytes: magic(8) version(4) alg(4) security_version(4) flags(4) covered_len(8) key_id(8)
pub const HEADER_LEN: usize = 40;
pub const SIG_LEN: usize = 64;
pub const BODY_LEN: usize = HEADER_LEN + SIG_LEN;

pub fn parse_header(body: &[u8]) -> Option<Header> {
    if body.len() < HEADER_LEN || &body[..8] != BODY_MAGIC {
        return None;
    }
    let u32_at = |o: usize| u32::from_le_bytes(body[o..o + 4].try_into().unwrap());
    let mut key_id = [0u8; 8];
    key_id.copy_from_slice(&body[32..40]);
    Some(Header {
        version: u32_at(8),
        alg: u32_at(12),
        security_version: u32_at(16),
        flags: u32_at(20),
        covered_len: u64::from_le_bytes(body[24..32].try_into().unwrap()),
        key_id,
    })
}

/// The first 8 bytes of SHA-256 over the raw public key.
pub fn key_id(pubkey: &[u8; 32]) -> [u8; 8] {
    let d = Sha256::digest(pubkey);
    let mut id = [0u8; 8];
    id.copy_from_slice(&d[..8]);
    id
}

pub fn message(header_bytes: &[u8], digest: &[u8; 32]) -> [u8; 16 + HEADER_LEN + 32] {
    let mut m = [0u8; 16 + HEADER_LEN + 32];
    m[..16].copy_from_slice(DOMAIN);
    m[16..16 + HEADER_LEN].copy_from_slice(&header_bytes[..HEADER_LEN]);
    m[16 + HEADER_LEN..].copy_from_slice(digest);
    m
}

#[derive(Debug, PartialEq, Eq)]
pub enum Verdict {
    Verified = 0,
    BadFormat = 1,
    UnknownKey = 2,
    BadSignature = 3,
    Rollback = 4,
}

/// Check a record body against the covered bytes. `digest` gets SHA-256 of the covered bytes
/// whenever the body is well formed, so a failed check is still measured.
pub fn verify(
    body: &[u8],
    covered: &[u8],
    pubkey: &[u8; 32],
    min_security_version: u32,
    digest: &mut [u8; 32],
) -> (Verdict, Option<Header>) {
    let Some(h) = parse_header(body) else {
        return (Verdict::BadFormat, None);
    };
    if body.len() < BODY_LEN || h.version != 1 || h.alg != ALG_ED25519 {
        return (Verdict::BadFormat, Some(h));
    }
    if h.covered_len != covered.len() as u64 {
        return (Verdict::BadFormat, Some(h));
    }
    digest.copy_from_slice(&Sha256::digest(covered));
    if h.key_id != key_id(pubkey) {
        return (Verdict::UnknownKey, Some(h));
    }
    let (Ok(pk), Ok(sig)) = (
        PublicKey::from_slice(pubkey),
        Signature::from_slice(&body[HEADER_LEN..BODY_LEN]),
    ) else {
        return (Verdict::BadFormat, Some(h));
    };
    if pk.verify(message(body, digest), &sig).is_err() {
        return (Verdict::BadSignature, Some(h));
    }
    if h.security_version < min_security_version {
        return (Verdict::Rollback, Some(h));
    }
    (Verdict::Verified, Some(h))
}

/// C entry: verify an `aura_sig` body over `covered`. Returns a `Verdict` value; fills
/// `digest` (32 bytes), `security_version` and `key_id` (8 bytes) when the body parses.
///
/// # Safety
/// Every pointer must be valid for the length given with it (pubkey 32, digest 32, key_id 8).
#[no_mangle]
pub unsafe extern "C" fn aura_secboot_verify(
    body: *const u8,
    body_len: usize,
    covered: *const u8,
    covered_len: usize,
    pubkey: *const u8,
    min_security_version: u32,
    digest: *mut u8,
    security_version: *mut u32,
    key_id_out: *mut u8,
) -> u32 {
    if body.is_null() || covered.is_null() || pubkey.is_null() || digest.is_null() {
        return Verdict::BadFormat as u32;
    }
    let body = unsafe { core::slice::from_raw_parts(body, body_len) };
    let covered = unsafe { core::slice::from_raw_parts(covered, covered_len) };
    let pubkey: &[u8; 32] = unsafe { &*(pubkey as *const [u8; 32]) };
    let mut d = [0u8; 32];
    let (v, h) = verify(body, covered, pubkey, min_security_version, &mut d);
    unsafe { core::ptr::copy_nonoverlapping(d.as_ptr(), digest, 32) };
    if let Some(h) = h {
        if !security_version.is_null() {
            unsafe { *security_version = h.security_version };
        }
        if !key_id_out.is_null() {
            unsafe { core::ptr::copy_nonoverlapping(h.key_id.as_ptr(), key_id_out, 8) };
        }
    }
    v as u32
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_compact::{KeyPair, Seed};

    fn sign(kp: &KeyPair, covered: &[u8], secver: u32) -> Vec<u8> {
        let mut body = Vec::new();
        body.extend_from_slice(BODY_MAGIC);
        body.extend_from_slice(&1u32.to_le_bytes());
        body.extend_from_slice(&ALG_ED25519.to_le_bytes());
        body.extend_from_slice(&secver.to_le_bytes());
        body.extend_from_slice(&0u32.to_le_bytes());
        body.extend_from_slice(&(covered.len() as u64).to_le_bytes());
        body.extend_from_slice(&key_id(&kp.pk));
        let mut d = [0u8; 32];
        d.copy_from_slice(&Sha256::digest(covered));
        let sig = kp.sk.sign(message(&body, &d), None);
        body.extend_from_slice(sig.as_ref());
        body
    }

    #[test]
    fn verdicts() {
        let kp = KeyPair::from_seed(Seed::new([7u8; 32]));
        let other = KeyPair::from_seed(Seed::new([9u8; 32]));
        let data = b"dtb and u-boot bytes";
        let body = sign(&kp, data, 3);
        let pk: [u8; 32] = *kp.pk;
        let mut d = [0u8; 32];
        assert_eq!(verify(&body, data, &pk, 0, &mut d).0, Verdict::Verified);
        assert_eq!(verify(&body, data, &pk, 4, &mut d).0, Verdict::Rollback);
        assert_eq!(
            verify(&body, b"dtb and u-boot bytez", &pk, 0, &mut d).0,
            Verdict::BadSignature
        );
        assert_eq!(
            verify(&body, &data[1..], &pk, 0, &mut d).0,
            Verdict::BadFormat
        );
        let opk: [u8; 32] = *other.pk;
        assert_eq!(verify(&body, data, &opk, 0, &mut d).0, Verdict::UnknownKey);
        let mut bad = body.clone();
        bad[20] ^= 1; // the signed flags word
        assert_eq!(verify(&bad, data, &pk, 0, &mut d).0, Verdict::BadSignature);
    }
    /// A record tools/secboot_sign.py made with the published test key over "aura" * 1000.
    #[test]
    fn python_signer_interop() {
        let body: Vec<u8> = (0..VECTOR.len() / 2)
            .map(|i| u8::from_str_radix(&VECTOR[2 * i..2 * i + 2], 16).unwrap())
            .collect();
        let pk: &[u8; 32] = include_bytes!("../../data/secboot/test-ed25519.pub");
        let covered = b"aura".repeat(1000);
        let mut d = [0u8; 32];
        let (v, h) = verify(&body, &covered, pk, 0, &mut d);
        assert_eq!(v, Verdict::Verified);
        assert_eq!(h.unwrap().security_version, 5);
    }

    const VECTOR: &str = "415552534947303101000000010000000500000000000000a00f0000000000006ef2bae2eee577cbafa4941675dfe6c78abc88f9a9475d7ca6837d24eacd1a7097ad9c9e2a7199e5be55967c7354932412513315b3ba59c1d578936c6272e52a12e4e4b99411830f";
}
