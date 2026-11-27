// SPDX-License-Identifier: MIT
//! An in-memory NVMe namespace behind the C functions rust/src/nvme.rs and trial.rs call.
//!
//! The fuzz input is the start of the disk, 4K sectors; the rest of the namespace reads as
//! zeroes. Writes go to an overlay and are recorded so a target can check them.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::OnceLock;

pub const SECTOR: usize = 4096;
/// The namespace: 4 GiB, far more than any input.
pub const SECTORS: u64 = 1 << 20;
/// NVME_MAX_READ_BLOCKS in src/nvme.h
const MAX_READ_BLOCKS: u32 = 256;

pub struct Disk {
    image: Vec<u8>,
    pub writes: BTreeMap<u64, Vec<u8>>,
    pub write_log: Vec<u64>,
    pub flushes: u32,
    pub sectors_read: u64,
}

thread_local! {
    static DISK: RefCell<Disk> = RefCell::new(Disk {
        image: Vec::new(),
        writes: BTreeMap::new(),
        write_log: Vec::new(),
        flushes: 0,
        sectors_read: 0,
    });
}

pub fn set(image: &[u8]) {
    DISK.with(|d| {
        let mut d = d.borrow_mut();
        d.image.clear();
        d.image.extend_from_slice(image);
        d.writes.clear();
        d.write_log.clear();
        d.flushes = 0;
        d.sectors_read = 0;
    });
}

pub fn with<R>(f: impl FnOnce(&Disk) -> R) -> R {
    DISK.with(|d| f(&d.borrow()))
}

impl Disk {
    pub fn sector(&self, lba: u64, out: &mut [u8]) {
        if let Some(w) = self.writes.get(&lba) {
            out.copy_from_slice(w);
            return;
        }
        out.fill(0);
        let start = lba as usize * SECTOR;
        if start < self.image.len() {
            let end = (start + SECTOR).min(self.image.len());
            out[..end - start].copy_from_slice(&self.image[start..end]);
        }
    }
}

fn verbose() -> bool {
    static V: OnceLock<bool> = OnceLock::new();
    *V.get_or_init(|| std::env::var_os("AURA_FUZZ_VERBOSE").is_some())
}

#[no_mangle]
extern "C" fn iodev_console_write(buf: *const c_void, len: u64) {
    if verbose() {
        let s = unsafe { std::slice::from_raw_parts(buf as *const u8, len as usize) };
        eprint!("{}", String::from_utf8_lossy(s));
    }
}

#[no_mangle]
extern "C" fn nvme_read_blocks(nsid: u32, lba: u64, buffer: *mut c_void, count: u32) -> bool {
    // the C driver's contract: one namespace, 1..=NVME_MAX_READ_BLOCKS blocks per command
    assert_eq!(nsid, 1, "read from namespace {nsid}");
    assert!(
        count > 0 && count <= MAX_READ_BLOCKS,
        "nvme_read_blocks of {count} blocks"
    );
    if lba >= SECTORS || SECTORS - lba < count as u64 {
        return false;
    }
    DISK.with(|d| {
        let mut d = d.borrow_mut();
        d.sectors_read += count as u64;
        for i in 0..count as usize {
            let out = unsafe {
                std::slice::from_raw_parts_mut(buffer.cast::<u8>().add(i * SECTOR), SECTOR)
            };
            d.sector(lba + i as u64, out);
        }
    });
    true
}

#[no_mangle]
extern "C" fn nvme_read(nsid: u32, lba: u64, buffer: *mut c_void) -> bool {
    nvme_read_blocks(nsid, lba, buffer, 1)
}

#[no_mangle]
extern "C" fn nvme_write(nsid: u32, lba: u64, buffer: *const c_void) -> bool {
    assert_eq!(nsid, 1, "write to namespace {nsid}");
    if lba >= SECTORS {
        return false;
    }
    let data = unsafe { std::slice::from_raw_parts(buffer as *const u8, SECTOR) }.to_vec();
    DISK.with(|d| {
        let mut d = d.borrow_mut();
        d.writes.insert(lba, data);
        d.write_log.push(lba);
    });
    true
}

#[no_mangle]
extern "C" fn nvme_flush(nsid: u32) -> bool {
    assert_eq!(nsid, 1);
    DISK.with(|d| d.borrow_mut().flushes += 1);
    true
}

#[no_mangle]
extern "C" fn nvme_dma_uncertain() -> bool {
    false
}

pub fn crc32(bytes: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in bytes {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    !crc
}

fn u32_at(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

fn u64_at(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// Recompute the primary GPT's entry-array and header CRCs, so mutations of the fields behind
/// them reach the parsers. Only an entry array that lies inside the image is summed.
pub fn fix_gpt_crcs(image: &mut [u8]) {
    let h = SECTOR;
    if image.len() < h + 92 || &image[h..h + 8] != b"EFI PART" {
        return;
    }
    let table = u64_at(image, h + 72);
    let count = u32_at(image, h + 80) as u64;
    let esz = u32_at(image, h + 84) as u64;
    if let Some(start) = table.checked_mul(SECTOR as u64) {
        if let Some(end) = count.checked_mul(esz).and_then(|n| n.checked_add(start)) {
            if end <= image.len() as u64 {
                let crc = crc32(&image[start as usize..end as usize]);
                image[h + 88..h + 92].copy_from_slice(&crc.to_le_bytes());
            }
        }
    }
    image[h + 16..h + 20].fill(0);
    let crc = crc32(&image[h..h + 92]);
    image[h + 16..h + 20].copy_from_slice(&crc.to_le_bytes());
}

/// The first used GPT entry of the image: (partition GUID as text, first LBA, last LBA).
pub fn first_partition(image: &[u8]) -> Option<(String, u64, u64)> {
    let h = SECTOR;
    if image.len() < h + 92 {
        return None;
    }
    let table = (u64_at(image, h + 72) as usize).checked_mul(SECTOR)?;
    let esz = u32_at(image, h + 84) as usize;
    let count = u32_at(image, h + 80) as usize;
    for i in 0..count.min(128) {
        let e = table.checked_add(i.checked_mul(esz)?)?;
        if image.len() < e.checked_add(128)? {
            return None;
        }
        if image[e..e + 16].iter().all(|&b| b == 0) {
            continue;
        }
        let g = &image[e + 16..e + 32];
        let uuid = uuid::Uuid::from_bytes_le(g.try_into().unwrap());
        return Some((
            uuid.hyphenated().to_string(),
            u64_at(image, e + 32),
            u64_at(image, e + 40),
        ));
    }
    None
}
