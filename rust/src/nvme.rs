// SPDX-License-Identifier: MIT AND GPL-2.0-only
use crate::println;
use alloc::boxed::Box;
use core::cmp::min;
use core::ffi::c_void;
use fatfs::SeekFrom;

extern "C" {
    pub(crate) fn nvme_read(nsid: u32, lba: u64, buffer: *mut c_void) -> bool;
    fn nvme_read_blocks(nsid: u32, lba: u64, buffer: *mut c_void, count: u32) -> bool;
    pub(crate) fn nvme_write(nsid: u32, lba: u64, buffer: *const c_void) -> bool;
    pub(crate) fn nvme_flush(nsid: u32) -> bool;
}

pub(crate) const SECTOR_SIZE: usize = 4096;
const BULK_SECTORS: usize = 256;

pub type Error = ();

#[repr(C, align(4096))]
pub(crate) struct SectorBuffer(pub(crate) [u8; SECTOR_SIZE]);

#[repr(C, align(4096))]
struct BulkBuffer([u8; SECTOR_SIZE * BULK_SECTORS]);

pub(crate) fn alloc_sector_buf() -> Box<SectorBuffer> {
    let p: Box<SectorBuffer> = unsafe { Box::new_zeroed().assume_init() };
    debug_assert_eq!(0, p.0.as_ptr().align_offset(SECTOR_SIZE));
    p
}

/// Blocks fetched by one read-ahead command; bounded by NVME_MAX_READ_BLOCKS in nvme.h.
const WINDOW_SECTORS: usize = 64;

#[repr(C, align(4096))]
struct WindowBuffer([u8; SECTOR_SIZE * WINDOW_SECTORS]);

struct Window {
    start: u64,
    sectors: u64,
    buf: Box<WindowBuffer>,
}

impl Window {
    fn new() -> Window {
        let buf: Box<WindowBuffer> = unsafe { Box::new_zeroed().assume_init() };
        debug_assert_eq!(0, buf.0.as_ptr().align_offset(4096));
        Window {
            start: 0,
            sectors: 0,
            buf,
        }
    }

    fn contains(&self, lba: u64) -> bool {
        lba >= self.start && lba - self.start < self.sectors
    }
}

/// FAT reads alternate between the allocation table and the file data, so keep two windows:
/// a sequential run is fetched 64 blocks at a time while the table block stays cached.
pub struct NVMEStorage {
    nsid: u32,
    offset: u64,
    extent_sectors: Option<u64>,
    read_budget: Option<u64>,
    bytes_budget: Option<u64>,
    op_budget: Option<u64>,
    bulk: Option<Box<BulkBuffer>>,
    windows: [Window; 2],
    mru: usize,
    last_miss: Option<u64>,
    multi_ok: bool,
    pos: u64,
}

impl NVMEStorage {
    pub fn new(nsid: u32, offset: u64) -> NVMEStorage {
        NVMEStorage {
            nsid,
            offset,
            extent_sectors: None,
            read_budget: None,
            bytes_budget: None,
            op_budget: None,
            bulk: None,
            windows: [Window::new(), Window::new()],
            mru: 0,
            last_miss: None,
            multi_ok: true,
            pos: 0,
        }
    }

    pub fn with_extent(mut self, sectors: u64) -> Result<Self, Error> {
        if sectors == 0
            || self.offset.checked_add(sectors - 1).is_none()
            || sectors.checked_mul(SECTOR_SIZE as u64).is_none()
        {
            return Err(());
        }
        self.extent_sectors = Some(sectors);
        Ok(self)
    }

    pub fn with_read_budget(mut self, sectors: u64) -> Self {
        self.read_budget = Some(sectors);
        self.bytes_budget = Some(sectors.saturating_mul(SECTOR_SIZE as u64));
        self.op_budget = Some((1 << 20) - 1);
        self
    }

    fn charge_op(&mut self) -> Result<(), Error> {
        if let Some(left) = self.op_budget.as_mut() {
            if *left == 0 {
                return Err(());
            }
            *left -= 1;
        }
        Ok(())
    }

    fn charge(&mut self, sectors: usize) -> Result<(), Error> {
        if let Some(left) = self.read_budget.as_mut() {
            if *left < sectors as u64 {
                return Err(());
            }
            *left -= sectors as u64;
        }
        Ok(())
    }

    fn available_sectors(&self, relative_lba: u64) -> u64 {
        self.extent_sectors
            .map(|extent| extent.saturating_sub(relative_lba))
            .unwrap_or(u64::MAX)
    }

    /// Returns the window index holding `lba` (relative to the partition), loading it if needed.
    fn fetch(&mut self, lba: u64) -> Result<usize, Error> {
        for i in 0..2 {
            if self.windows[i].contains(lba) {
                self.mru = i;
                return Ok(i);
            }
        }

        let victim = 1 - self.mru;
        let continues = |w: &Window| w.sectors > 0 && w.start.checked_add(w.sectors) == Some(lba);
        let sequential = self.last_miss.and_then(|l| l.checked_add(1)) == Some(lba)
            || continues(&self.windows[0])
            || continues(&self.windows[1]);
        self.last_miss = Some(lba);

        let abs = lba.checked_add(self.offset).ok_or(())?;
        let mut count = if sequential && self.multi_ok {
            WINDOW_SECTORS as u32
        } else {
            1
        };
        count = min(count as u64, self.available_sectors(lba)) as u32;
        if count == 0 {
            return Err(());
        }
        count = (min(count as u64 - 1, u64::MAX - abs) + 1) as u32;
        if let Some(left) = self.read_budget {
            count = min(count as u64, left) as u32;
        }
        if count == 0 {
            return Err(());
        }
        self.charge(count as usize)?;
        self.windows[victim].sectors = 0;
        let ptr = self.windows[victim].buf.0.as_mut_ptr() as *mut c_void;
        if count > 1 && !unsafe { nvme_read_blocks(self.nsid, abs, ptr, count) } {
            println!(
                "nvme_read_blocks({}, {}, {}) failed, using single-block reads",
                self.nsid, abs, count
            );
            self.multi_ok = false;
            self.charge(1)?;
            count = 1;
        }
        if count == 1 && !unsafe { nvme_read(self.nsid, abs, ptr) } {
            println!("nvme_read({}, {}) failed", self.nsid, abs);
            return Err(());
        }
        self.windows[victim].start = lba;
        self.windows[victim].sectors = count as u64;
        self.mru = victim;
        Ok(victim)
    }
}

impl fatfs::IoBase for NVMEStorage {
    type Error = Error;
}

impl fatfs::Read for NVMEStorage {
    fn read(&mut self, mut buf: &mut [u8]) -> Result<usize, Self::Error> {
        self.charge_op()?;
        if let Some(left) = self.bytes_budget.as_mut() {
            if *left < buf.len() as u64 {
                return Err(());
            }
            *left -= buf.len() as u64;
        }
        let mut read = 0;
        while !buf.is_empty() {
            let relative_lba = self.pos / SECTOR_SIZE as u64;
            if self.available_sectors(relative_lba) == 0 {
                break;
            }
            let off = self.pos as usize % SECTOR_SIZE;
            let max_bytes = self
                .available_sectors(relative_lba)
                .saturating_mul(SECTOR_SIZE as u64)
                .saturating_sub(off as u64);
            let requested = min(buf.len(), min(max_bytes, usize::MAX as u64) as usize);
            if requested == 0 {
                break;
            }

            if requested > SECTOR_SIZE - off && self.available_sectors(relative_lba) > 1 {
                let sectors = min(
                    BULK_SECTORS,
                    min(
                        self.available_sectors(relative_lba),
                        (off.saturating_add(requested)
                            .saturating_add(SECTOR_SIZE - 1)
                            / SECTOR_SIZE) as u64,
                    ) as usize,
                );
                self.charge(sectors)?;
                let lba = self.offset.checked_add(relative_lba).ok_or(())?;
                if self.bulk.is_none() {
                    self.bulk = Some(unsafe { Box::new_zeroed().assume_init() });
                }
                let bulk = self.bulk.as_mut().ok_or(())?;
                if !unsafe {
                    nvme_read_blocks(self.nsid, lba, bulk.0.as_mut_ptr().cast(), sectors as u32)
                } {
                    return Err(());
                }
                let copy_len = min(requested, sectors * SECTOR_SIZE - off);
                buf[..copy_len].copy_from_slice(&bulk.0[off..off + copy_len]);
                buf = &mut buf[copy_len..];
                read += copy_len;
                self.pos = self.pos.checked_add(copy_len as u64).ok_or(())?;
                continue;
            }

            let lba = relative_lba;
            let i = self.fetch(lba)?;
            let w = &self.windows[i];
            let off = (self.pos - w.start * SECTOR_SIZE as u64) as usize;
            let avail = w.sectors as usize * SECTOR_SIZE - off;
            let copy_len = min(avail, requested);
            buf[..copy_len].copy_from_slice(&w.buf.0[off..off + copy_len]);
            buf = &mut buf[copy_len..];
            read += copy_len;
            self.pos = self.pos.checked_add(copy_len as u64).ok_or(())?;
        }
        Ok(read)
    }
}

impl fatfs::Write for NVMEStorage {
    fn write(&mut self, _buf: &[u8]) -> Result<usize, Self::Error> {
        Err(())
    }
    fn flush(&mut self) -> Result<(), Self::Error> {
        Err(())
    }
}

impl fatfs::Seek for NVMEStorage {
    fn seek(&mut self, from: SeekFrom) -> Result<u64, Self::Error> {
        self.charge_op()?;
        self.pos = match from {
            SeekFrom::Start(n) => n,
            SeekFrom::End(_n) => return Err(()),
            SeekFrom::Current(n) => self.pos.checked_add_signed(n).ok_or(())?,
        };
        Ok(self.pos)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fatfs::{Read, Seek};
    use std::sync::Mutex;

    struct Mock {
        offset: u64,
        sectors: u64,
        reads: Vec<(u64, u32)>,
    }

    static MOCK: Mutex<Mock> = Mutex::new(Mock {
        offset: 0,
        sectors: 0,
        reads: Vec::new(),
    });

    fn reset(offset: u64, sectors: u64) {
        *MOCK.lock().unwrap() = Mock {
            offset,
            sectors,
            reads: Vec::new(),
        };
    }

    #[no_mangle]
    extern "C" fn iodev_console_write(_buffer: *const c_void, _len: u64) {}

    #[no_mangle]
    extern "C" fn nvme_read_blocks(_nsid: u32, lba: u64, buffer: *mut c_void, count: u32) -> bool {
        let mut mock = MOCK.lock().unwrap();
        assert!(lba >= mock.offset);
        let relative = lba - mock.offset;
        assert!(relative < mock.sectors);
        assert!(count > 0 && count as u64 <= mock.sectors - relative);
        mock.reads.push((lba, count));
        for i in 0..count as usize {
            unsafe {
                core::ptr::write_bytes(
                    buffer.cast::<u8>().add(i * SECTOR_SIZE),
                    (relative + i as u64) as u8,
                    SECTOR_SIZE,
                );
            }
        }
        true
    }

    #[no_mangle]
    extern "C" fn nvme_read(nsid: u32, lba: u64, buffer: *mut c_void) -> bool {
        nvme_read_blocks(nsid, lba, buffer, 1)
    }

    /* Only the aura-try marker writes (trial.rs), and its tests use their own block device. */
    #[no_mangle]
    extern "C" fn nvme_write(_nsid: u32, _lba: u64, _buffer: *const c_void) -> bool {
        unreachable!()
    }

    #[no_mangle]
    extern "C" fn nvme_flush(_nsid: u32) -> bool {
        unreachable!()
    }

    #[test]
    fn bounded_read_ahead_and_bulk_reads() {
        reset(100, 3);
        let mut storage = NVMEStorage::new(1, 100)
            .with_extent(3)
            .unwrap()
            .with_read_budget(3);
        let mut byte = [0];
        assert_eq!(storage.read(&mut byte), Ok(1));
        storage.seek(SeekFrom::Start(SECTOR_SIZE as u64)).unwrap();
        assert_eq!(storage.read(&mut byte), Ok(1));
        assert_eq!(byte, [1]);
        storage
            .seek(SeekFrom::Start(2 * SECTOR_SIZE as u64))
            .unwrap();
        assert_eq!(storage.read(&mut byte), Ok(1));
        assert_eq!(byte, [2]);
        assert_eq!(MOCK.lock().unwrap().reads, [(100, 1), (101, 2)]);
        storage
            .seek(SeekFrom::Start(3 * SECTOR_SIZE as u64))
            .unwrap();
        assert_eq!(storage.read(&mut byte), Ok(0));

        reset(100, 3);
        let mut storage = NVMEStorage::new(1, 100)
            .with_extent(3)
            .unwrap()
            .with_read_budget(3);
        let mut bytes = vec![0; 4 * SECTOR_SIZE];
        assert_eq!(storage.read(&mut bytes), Err(()));
        assert!(MOCK.lock().unwrap().reads.is_empty());
        bytes.resize(3 * SECTOR_SIZE, 0);
        assert_eq!(storage.read(&mut bytes), Ok(3 * SECTOR_SIZE));
        assert!(bytes[..SECTOR_SIZE].iter().all(|&b| b == 0));
        assert!(bytes[SECTOR_SIZE..2 * SECTOR_SIZE].iter().all(|&b| b == 1));
        assert!(bytes[2 * SECTOR_SIZE..].iter().all(|&b| b == 2));
        assert_eq!(MOCK.lock().unwrap().reads, [(100, 3)]);

        reset(100, 3);
        let mut storage = NVMEStorage::new(1, 100)
            .with_extent(3)
            .unwrap()
            .with_read_budget(1);
        assert_eq!(storage.read(&mut byte), Ok(1));
        storage.seek(SeekFrom::Start(SECTOR_SIZE as u64)).unwrap();
        assert_eq!(storage.read(&mut byte), Err(()));
        assert_eq!(MOCK.lock().unwrap().reads, [(100, 1)]);

        reset(u64::MAX, 1);
        let mut storage = NVMEStorage::new(1, u64::MAX).with_extent(1).unwrap();
        assert_eq!(storage.read(&mut byte), Ok(1));
        assert_eq!(MOCK.lock().unwrap().reads, [(u64::MAX, 1)]);
        assert!(NVMEStorage::new(1, u64::MAX).with_extent(2).is_err());
    }
}
