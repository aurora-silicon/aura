// SPDX-License-Identifier: GPL-2.0-only
//! aura-try: boot an image once in place of the configured one.
//!
//! A file named aura-try at the root of the ESP holds `armed\n<path>\n`. The first aura stage of
//! a boot that finds it rewrites the first line to `spent` on the disk, reads that back, and only
//! then boots `<path>`. A trial that never reaches Linux therefore leaves the next boot
//! on the configured image. To arm another trial, write the file again.
//!
//! This is the only write aura makes to a disk. It goes to the one block that holds the start of
//! the marker, and only once that block has been read back holding exactly what the file system
//! says the marker holds.

use crate::nvme::{self, alloc_sector_buf, SectorBuffer, SECTOR_SIZE};
use core::ffi::c_void;

pub(crate) const MARKER: &str = "aura-try";
pub(crate) const ARMED: &[u8] = b"armed\n";
pub(crate) const SPENT: &[u8] = b"spent\n";
const _: () = assert!(ARMED.len() == SPENT.len());

pub(crate) trait Blocks {
    fn read(&mut self, lba: u64, buf: &mut SectorBuffer) -> bool;
    fn write(&mut self, lba: u64, buf: &SectorBuffer) -> bool;
    fn flush(&mut self) -> bool;
}

/// The ESP's namespace on the boot NVMe.
pub(crate) struct Nvme(pub(crate) u32);

impl Blocks for Nvme {
    fn read(&mut self, lba: u64, buf: &mut SectorBuffer) -> bool {
        unsafe { nvme::nvme_read(self.0, lba, buf.0.as_mut_ptr() as *mut c_void) }
    }
    fn write(&mut self, lba: u64, buf: &SectorBuffer) -> bool {
        unsafe { nvme::nvme_write(self.0, lba, buf.0.as_ptr() as *const c_void) }
    }
    fn flush(&mut self) -> bool {
        unsafe { nvme::nvme_flush(self.0) }
    }
}

/// The path an armed marker names: its second line.
pub(crate) fn armed_path(contents: &[u8]) -> Option<&str> {
    let rest = contents.strip_prefix(ARMED)?;
    let end = rest.iter().position(|&b| b == b'\n')?;
    core::str::from_utf8(&rest[..end]).ok()
}

/// Spend the marker whose `contents` start block `lba`. True once the block reads back spent.
pub(crate) fn spend(dev: &mut impl Blocks, lba: u64, contents: &[u8]) -> bool {
    if contents.len() > SECTOR_SIZE || !contents.starts_with(ARMED) {
        return false;
    }
    let mut block = alloc_sector_buf();
    if !dev.read(lba, &mut block) || block.0[..contents.len()] != *contents {
        return false;
    }
    block.0[..SPENT.len()].copy_from_slice(SPENT);
    if !dev.write(lba, &block) || !dev.flush() {
        return false;
    }
    let mut check = alloc_sector_buf();
    dev.read(lba, &mut check) && check.0 == block.0
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Disk {
        blocks: Vec<[u8; SECTOR_SIZE]>,
        writes: Vec<u64>,
        fail_write: bool,
        fail_flush: bool,
        drop_write: bool,
    }

    impl Disk {
        fn new(marker: &[u8]) -> Disk {
            let mut blocks = vec![[0x5au8; SECTOR_SIZE]; 4];
            blocks[2][..marker.len()].copy_from_slice(marker);
            Disk {
                blocks,
                writes: Vec::new(),
                fail_write: false,
                fail_flush: false,
                drop_write: false,
            }
        }
    }

    impl Blocks for Disk {
        fn read(&mut self, lba: u64, buf: &mut SectorBuffer) -> bool {
            match self.blocks.get(lba as usize) {
                Some(b) => {
                    buf.0.copy_from_slice(b);
                    true
                }
                None => false,
            }
        }
        fn write(&mut self, lba: u64, buf: &SectorBuffer) -> bool {
            self.writes.push(lba);
            if self.fail_write || lba as usize >= self.blocks.len() {
                return false;
            }
            if !self.drop_write {
                self.blocks[lba as usize].copy_from_slice(&buf.0);
            }
            true
        }
        fn flush(&mut self) -> bool {
            !self.fail_flush
        }
    }

    const MARKER_FILE: &[u8] = b"armed\nm1n1/boot-try.bin\n";

    #[test]
    fn armed_marker_names_a_path() {
        assert_eq!(armed_path(MARKER_FILE), Some("m1n1/boot-try.bin"));
        assert_eq!(armed_path(b"armed\nx\ntrailing"), Some("x"));
        assert_eq!(armed_path(b"spent\nm1n1/boot-try.bin\n"), None);
        assert_eq!(armed_path(b"armed\nno-newline"), None);
        assert_eq!(armed_path(b"armed"), None);
        assert_eq!(armed_path(b"armed\n\xff\n"), None);
    }

    #[test]
    fn marker_is_spent_in_place() {
        let mut disk = Disk::new(MARKER_FILE);
        assert!(spend(&mut disk, 2, MARKER_FILE));
        assert_eq!(disk.writes, [2]);
        assert!(disk.blocks[2].starts_with(b"spent\nm1n1/boot-try.bin\n"));
        assert!(disk.blocks[2][MARKER_FILE.len()..]
            .iter()
            .all(|&b| b == 0x5a));
        for i in [0, 1, 3] {
            assert!(disk.blocks[i].iter().all(|&b| b == 0x5a));
        }
        // Spent: never again
        let spent = disk.blocks[2];
        assert!(!spend(&mut disk, 2, &spent[..MARKER_FILE.len()]));
        assert_eq!(disk.writes, [2]);
    }

    #[test]
    fn nothing_is_written_to_a_block_that_does_not_hold_the_marker() {
        let mut disk = Disk::new(MARKER_FILE);
        assert!(!spend(&mut disk, 1, MARKER_FILE));
        assert!(!spend(&mut disk, 9, MARKER_FILE));
        let mut other = MARKER_FILE.to_vec();
        other[10] ^= 1;
        assert!(!spend(&mut disk, 2, &other));
        assert!(!spend(&mut disk, 2, &vec![b'a'; SECTOR_SIZE + 1]));
        assert!(disk.writes.is_empty());
    }

    #[test]
    fn a_marker_that_is_not_spent_on_disk_is_not_taken() {
        for (fail_write, fail_flush, drop_write) in [
            (true, false, false),
            (false, true, false),
            (false, false, true),
        ] {
            let mut disk = Disk::new(MARKER_FILE);
            disk.fail_write = fail_write;
            disk.fail_flush = fail_flush;
            disk.drop_write = drop_write;
            assert!(!spend(&mut disk, 2, MARKER_FILE));
        }
    }
}
