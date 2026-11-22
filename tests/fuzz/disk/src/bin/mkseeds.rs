// SPDX-License-Identifier: MIT
//! Synthetic seed disks for the fuzz targets, written to <out>/{gpt,chainload,apfs,secboot}/.
//!
//!   mkseeds <out>
//!
//! Every seed is built here from scratch (no captured disk): a GPT with one partition, and in it
//! a FAT12, FAT16 or small FAT32 ESP holding aurora/stage2.bin, an aura-try marker and its target,
//! or a minimal APFS container with an xART volume and one ".gl" file.

use aura_disk_fuzz::disk::{crc32, SECTOR};
use fatfs::{
    FatType, FileSystem, FormatVolumeOptions, FsOptions, IoBase, Read, Seek, SeekFrom, Write,
};
use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;

/// A volume in memory; clones share it, so it survives the FileSystem that owns one.
#[derive(Clone)]
struct Mem {
    buf: Rc<RefCell<Vec<u8>>>,
    pos: usize,
}

impl IoBase for Mem {
    type Error = ();
}

impl Read for Mem {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize, ()> {
        let v = self.buf.borrow();
        let n = buf.len().min(v.len().saturating_sub(self.pos));
        buf[..n].copy_from_slice(&v[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

impl Write for Mem {
    fn write(&mut self, buf: &[u8]) -> Result<usize, ()> {
        let mut v = self.buf.borrow_mut();
        let n = buf.len().min(v.len().saturating_sub(self.pos));
        v[self.pos..self.pos + n].copy_from_slice(&buf[..n]);
        self.pos += n;
        Ok(n)
    }
    fn flush(&mut self) -> Result<(), ()> {
        Ok(())
    }
}

impl Seek for Mem {
    fn seek(&mut self, pos: SeekFrom) -> Result<u64, ()> {
        self.pos = match pos {
            SeekFrom::Start(n) => n as usize,
            SeekFrom::End(n) => (self.buf.borrow().len() as i64 + n) as usize,
            SeekFrom::Current(n) => (self.pos as i64 + n) as usize,
        };
        Ok(self.pos as u64)
    }
}

const ESP_TYPE: [u8; 16] = guid(
    0xc12a7328,
    0xf81f,
    0x11d2,
    [0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b],
);
const APFS_TYPE: [u8; 16] = guid(
    0x69646961,
    0x6700,
    0x11aa,
    [0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac],
);
const PART_GUID: [u8; 16] = guid(
    0x5a1e0f2b,
    0x3c4d,
    0x4e5f,
    [0x8a, 0x9b, 0x0c, 0x1d, 0x2e, 0x3f, 0x40, 0x51],
);
/// first usable LBA: header at 1, 4 blocks of 128-byte entries at 2..6
const FIRST: u64 = 6;
const ENTRIES: u32 = 128;

/// a GUID in its on-disk (mixed-endian) byte order
const fn guid(a: u32, b: u16, c: u16, d: [u8; 8]) -> [u8; 16] {
    let a = a.to_le_bytes();
    let b = b.to_le_bytes();
    let c = c.to_le_bytes();
    [
        a[0], a[1], a[2], a[3], b[0], b[1], c[0], c[1], d[0], d[1], d[2], d[3], d[4], d[5], d[6],
        d[7],
    ]
}

/// A disk of `FIRST + part.len()/SECTOR` blocks: GPT with one partition holding `part`.
fn gpt_disk(type_guid: [u8; 16], part: &[u8]) -> Vec<u8> {
    let part_sectors = part.len().div_ceil(SECTOR) as u64;
    let last = FIRST + part_sectors - 1;
    let mut d = vec![0u8; (FIRST as usize + part_sectors as usize) * SECTOR];
    // the entry array
    let e = 2 * SECTOR;
    d[e..e + 16].copy_from_slice(&type_guid);
    d[e + 16..e + 32].copy_from_slice(&PART_GUID);
    d[e + 32..e + 40].copy_from_slice(&FIRST.to_le_bytes());
    d[e + 40..e + 48].copy_from_slice(&last.to_le_bytes());
    for (i, c) in "ESP".encode_utf16().enumerate() {
        d[e + 56 + 2 * i..e + 58 + 2 * i].copy_from_slice(&c.to_le_bytes());
    }
    let table_crc = crc32(&d[e..e + ENTRIES as usize * 128]);
    // the header
    let h = SECTOR;
    d[h..h + 8].copy_from_slice(b"EFI PART");
    d[h + 8..h + 12].copy_from_slice(&0x0001_0000u32.to_le_bytes());
    d[h + 12..h + 16].copy_from_slice(&92u32.to_le_bytes());
    d[h + 24..h + 32].copy_from_slice(&1u64.to_le_bytes());
    d[h + 32..h + 40].copy_from_slice(&(last + 1 + FIRST).to_le_bytes()); // alternate
    d[h + 40..h + 48].copy_from_slice(&FIRST.to_le_bytes());
    d[h + 48..h + 56].copy_from_slice(&last.to_le_bytes());
    d[h + 56..h + 72].copy_from_slice(&PART_GUID);
    d[h + 72..h + 80].copy_from_slice(&2u64.to_le_bytes());
    d[h + 80..h + 84].copy_from_slice(&ENTRIES.to_le_bytes());
    d[h + 84..h + 88].copy_from_slice(&128u32.to_le_bytes());
    d[h + 88..h + 92].copy_from_slice(&table_crc.to_le_bytes());
    let crc = crc32(&d[h..h + 92]);
    d[h + 16..h + 20].copy_from_slice(&crc.to_le_bytes());
    d[FIRST as usize * SECTOR..FIRST as usize * SECTOR + part.len()].copy_from_slice(part);
    d
}

/// A FAT volume of `sectors` 4K blocks with `files` (path, contents); directories are made.
fn fat(fat_type: FatType, sectors: u32, files: &[(&str, &[u8])]) -> Vec<u8> {
    let mut mem = Mem {
        buf: Rc::new(RefCell::new(vec![0u8; sectors as usize * SECTOR])),
        pos: 0,
    };
    let opts = FormatVolumeOptions::new()
        .bytes_per_sector(SECTOR as u16)
        .bytes_per_cluster(SECTOR as u32)
        .total_sectors(sectors)
        .fats(2)
        .fat_type(fat_type)
        .volume_label(*b"EFI        ");
    fatfs::format_volume(&mut mem, opts).expect("format");
    mem.pos = 0;
    {
        let fs = FileSystem::new(mem.clone(), FsOptions::new()).expect("mount");
        let root = fs.root_dir();
        for (path, data) in files {
            if let Some((dir, _)) = path.rsplit_once('/') {
                let _ = root.create_dir(dir);
            }
            let mut f = root.create_file(path).expect("create");
            f.truncate().unwrap();
            f.write_all(data).unwrap();
        }
        drop(root);
        fs.unmount().unwrap();
    }
    let v = mem.buf.borrow().clone();
    v
}

/// An explicitly marked FAT32 volume of `sectors` 4K blocks, below the usual FAT32 cluster count
/// (fatfs's explicit-fat32 feature, which aura builds with), formatted by hand: format_volume()
/// does not make one this small.
fn small_fat32(sectors: u32) -> Vec<u8> {
    let mut v = vec![0u8; sectors as usize * SECTOR];
    let b = &mut v[..SECTOR];
    b[..3].copy_from_slice(&[0xeb, 0x58, 0x90]);
    b[3..11].copy_from_slice(b"MSWIN4.1");
    le16(b, 11, SECTOR as u16); // bytes per sector
    b[13] = 1; // sectors per cluster
    le16(b, 14, 8); // reserved sectors
    b[16] = 2; // FATs
    b[21] = 0xf8;
    le32(b, 32, sectors);
    le32(b, 36, 1); // sectors per FAT
    le32(b, 44, 2); // root directory cluster
    le16(b, 48, 1); // FSInfo sector
    le16(b, 50, 6); // backup boot sector
    b[64] = 0x80;
    b[66] = 0x29;
    b[71..82].copy_from_slice(b"EFI        ");
    b[82..90].copy_from_slice(b"FAT32   ");
    b[510] = 0x55;
    b[511] = 0xaa;
    let boot = v[..SECTOR].to_vec();
    v[6 * SECTOR..7 * SECTOR].copy_from_slice(&boot);
    let fsinfo = &mut v[SECTOR..2 * SECTOR];
    le32(fsinfo, 0, 0x4161_5252);
    le32(fsinfo, 484, 0x6141_7272);
    le32(fsinfo, 488, u32::MAX);
    le32(fsinfo, 492, u32::MAX);
    le32(fsinfo, 508, 0xaa55_0000);
    for fat in [8, 9] {
        let f = fat * SECTOR;
        le32(&mut v, f, 0x0fff_fff8);
        le32(&mut v, f + 4, 0x0fff_ffff);
        le32(&mut v, f + 8, 0x0fff_ffff); // the root directory
    }
    v
}

/// Write `files` into the FAT volume `image`.
fn populate(image: Vec<u8>, files: &[(&str, &[u8])]) -> Vec<u8> {
    let mem = Mem {
        buf: Rc::new(RefCell::new(image)),
        pos: 0,
    };
    {
        let fs = FileSystem::new(mem.clone(), FsOptions::new()).expect("mount");
        let root = fs.root_dir();
        for (path, data) in files {
            if let Some((dir, _)) = path.rsplit_once('/') {
                let _ = root.create_dir(dir);
            }
            let mut f = root.create_file(path).expect("create");
            f.truncate().unwrap();
            f.write_all(data).unwrap();
        }
        drop(root);
        fs.unmount().unwrap();
    }
    let v = mem.buf.borrow().clone();
    v
}

/// Drop trailing all-zero blocks: the fuzz disk reads them back as zeroes anyway.
fn trim(mut d: Vec<u8>) -> Vec<u8> {
    while d.len() > 2 * SECTOR && d[d.len() - SECTOR..].iter().all(|&b| b == 0) {
        d.truncate(d.len() - SECTOR);
    }
    d
}

fn put(out: &Path, dir: &str, name: &str, data: &[u8]) {
    let dir = out.join(dir);
    std::fs::create_dir_all(&dir).unwrap();
    std::fs::write(dir.join(name), data).unwrap();
    println!("{}/{} {} bytes", dir.display(), name, data.len());
}

/// sel byte for the chainload target (see fuzz_targets/chainload.rs)
fn with_sel(mut d: Vec<u8>, sel: u8) -> Vec<u8> {
    d[0] = sel;
    d
}

// ---- a minimal APFS container: one xART volume whose root has "x.gl" with one extent ----

const NX_MAGIC: u32 = 0x4253584e; // 'NXSB'
const APSB_MAGIC: u32 = 0x42535041; // 'APSB'

fn le16(b: &mut [u8], o: usize, v: u16) {
    b[o..o + 2].copy_from_slice(&v.to_le_bytes());
}
fn le32(b: &mut [u8], o: usize, v: u32) {
    b[o..o + 4].copy_from_slice(&v.to_le_bytes());
}
fn le64(b: &mut [u8], o: usize, v: u64) {
    b[o..o + 8].copy_from_slice(&v.to_le_bytes());
}

/// A leaf root B-tree node with fixed-size omap entries (oid -> paddr).
fn omap_node(oid: u64, entries: &[(u64, u64)]) -> Vec<u8> {
    let mut b = vec![0u8; SECTOR];
    le64(&mut b, 8, oid);
    le16(&mut b, 32, 0x1 | 0x4); // ROOT | FIXED_KV_SIZE
    le16(&mut b, 34, 0);
    le32(&mut b, 36, entries.len() as u32);
    let toc_len = (entries.len() * 4) as u16;
    le16(&mut b, 40, 0); // table space off (from 56)
    le16(&mut b, 42, toc_len);
    let key_start = 56 + toc_len as usize;
    let val_end = SECTOR - 40;
    for (i, (k, paddr)) in entries.iter().enumerate() {
        let koff = i * 16;
        let voff = (i + 1) * 16;
        le16(&mut b, 56 + i * 4, koff as u16);
        le16(&mut b, 56 + i * 4 + 2, voff as u16);
        le64(&mut b, key_start + koff, *k);
        le64(&mut b, key_start + koff + 8, 1);
        le32(&mut b, val_end - voff + 4, SECTOR as u32);
        le64(&mut b, val_end - voff + 8, *paddr);
    }
    b
}

/// The volume's root file-system tree: a dir record "x.gl" in the root and its extent.
fn fs_root(oid: u64, file_id: u64, ext_block: u64, ext_len: u64) -> Vec<u8> {
    let mut b = vec![0u8; SECTOR];
    le64(&mut b, 8, oid);
    le16(&mut b, 32, 0x1); // ROOT, variable size
    le32(&mut b, 36, 2);
    le16(&mut b, 40, 0);
    le16(&mut b, 42, 16); // two 8-byte KVLoc entries
    let key_start = 56 + 16;
    let val_end = SECTOR - 40;
    // dir record key: obj_id_and_type, name_len_and_hash, name\0
    let name = b"x.gl\0";
    let drec_key_len = 12 + name.len();
    let mut k = 0usize;
    le64(&mut b, key_start + k, 2 | (9u64 << 60));
    le32(&mut b, key_start + k + 8, name.len() as u32);
    b[key_start + k + 12..key_start + k + 12 + name.len()].copy_from_slice(name);
    // value: file_id, date_added, flags
    let drec_val_len = 18;
    le64(&mut b, val_end - drec_val_len, file_id);
    le16(&mut b, 56, k as u16);
    le16(&mut b, 58, drec_key_len as u16);
    le16(&mut b, 60, drec_val_len as u16);
    le16(&mut b, 62, drec_val_len as u16);
    k += drec_key_len;
    // extent key: obj_id_and_type, logical addr; value: len_and_flags, phys block, crypto id
    le64(&mut b, key_start + k, file_id | (8u64 << 60));
    le64(&mut b, key_start + k + 8, 0);
    let ext_val_off = drec_val_len + 24;
    le64(&mut b, val_end - ext_val_off, ext_len);
    le64(&mut b, val_end - ext_val_off + 8, ext_block);
    le16(&mut b, 64, k as u16);
    le16(&mut b, 66, 16);
    le16(&mut b, 68, ext_val_off as u16);
    le16(&mut b, 70, 24);
    b
}

fn apfs_container() -> Vec<u8> {
    // blocks: 0 nx superblock, 1 container omap, 2 its tree, 3 volume superblock,
    // 4 volume omap, 5 its tree, 6 fs root tree, 7 the file's data
    let mut c = vec![0u8; 8 * SECTOR];
    let blk = |c: &mut Vec<u8>, i: usize, b: &[u8]| {
        c[i * SECTOR..i * SECTOR + b.len()].copy_from_slice(b)
    };
    let (fs_oid, root_oid, file_id) = (0x402u64, 0x404u64, 0x10u64);

    let mut nx = vec![0u8; SECTOR];
    le64(&mut nx, 8, 1);
    le64(&mut nx, 16, 1);
    le32(&mut nx, 32, NX_MAGIC);
    le32(&mut nx, 36, SECTOR as u32);
    le32(&mut nx, 104, 0); // no checkpoint descriptor blocks
    le64(&mut nx, 160, 1); // omap at block 1
    le64(&mut nx, 184, fs_oid);
    blk(&mut c, 0, &nx);

    let mut om = vec![0u8; SECTOR];
    le64(&mut om, 8, 1);
    le64(&mut om, 48, 2);
    blk(&mut c, 1, &om);
    blk(&mut c, 2, &omap_node(2, &[(fs_oid, 3)]));

    let mut apsb = vec![0u8; SECTOR];
    le64(&mut apsb, 8, fs_oid);
    le32(&mut apsb, 32, APSB_MAGIC);
    le64(&mut apsb, 128, 4);
    le64(&mut apsb, 136, root_oid);
    le16(&mut apsb, 964, 0x100); // xART role
    blk(&mut c, 3, &apsb);

    let mut vom = vec![0u8; SECTOR];
    le64(&mut vom, 8, 4);
    le64(&mut vom, 48, 5);
    blk(&mut c, 4, &vom);
    blk(&mut c, 5, &omap_node(5, &[(root_oid, 6)]));
    blk(&mut c, 6, &fs_root(root_oid, file_id, 7, 64));
    c[7 * SECTOR..7 * SECTOR + 64].copy_from_slice(&[0x5a; 64]);
    c
}

fn main() {
    let out = std::env::args().nth(1).expect("usage: mkseeds <out>");
    let out = Path::new(&out);
    let stage2 = b"aura stage 2 payload\n".repeat(300);
    let try_bin = b"trial image\n".repeat(10);

    let small = [
        ("aurora/stage2.bin", &stage2[..]),
        ("aura-try", b"armed\naurora/try.bin\n"),
        ("aurora/try.bin", &try_bin[..]),
        ("aura-proxy", b""),
    ];
    let boot_trial = [
        ("aurora/stage2.bin", &stage2[..]),
        ("aura-try", b"armed\nvmlinuz;dtb/t8103-j274.dtb;-\n"),
    ];
    for (name, fat_type, sectors, files) in [
        ("fat12", FatType::Fat12, 64u32, &small[..]),
        ("fat16", FatType::Fat16, 4200, &small[..]),
        ("fat12-boot-trial", FatType::Fat12, 64, &boot_trial[..]),
    ] {
        let disk = trim(gpt_disk(ESP_TYPE, &fat(fat_type, sectors, files)));
        put(out, "gpt", &format!("{name}.img"), &disk);
        for sel in [0u8, 2, 4, 6] {
            put(
                out,
                "chainload",
                &format!("{name}-sel{sel}.img"),
                &with_sel(disk.clone(), sel),
            );
        }
    }

    // the file system real ESPs use
    let disk = trim(gpt_disk(ESP_TYPE, &populate(small_fat32(256), &small)));
    put(out, "gpt", "fat32-small.img", &disk);
    for sel in [0u8, 2, 4, 6] {
        put(
            out,
            "chainload",
            &format!("fat32-small-sel{sel}.img"),
            &with_sel(disk.clone(), sel),
        );
    }

    let apfs = gpt_disk(APFS_TYPE, &apfs_container());
    put(out, "apfs", "xart.img", &apfs);

    // secboot: u16 body length, body, covered bytes (the record secboot.rs's interop test checks)
    const VECTOR: &str = "415552534947303101000000010000000500000000000000a00f0000000000006ef2bae2eee577cbafa4941675dfe6c78abc88f9a9475d7ca6837d24eacd1a7097ad9c9e2a7199e5be55967c7354932412513315b3ba59c1d578936c6272e52a12e4e4b99411830f";
    let body: Vec<u8> = (0..VECTOR.len() / 2)
        .map(|i| u8::from_str_radix(&VECTOR[2 * i..2 * i + 2], 16).unwrap())
        .collect();
    let mut sb = (body.len() as u16).to_le_bytes().to_vec();
    sb.extend_from_slice(&body);
    sb.extend_from_slice(&b"aura".repeat(1000));
    put(out, "secboot", "test-key-vector.bin", &sb);
}
