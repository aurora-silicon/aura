// SPDX-License-Identifier: MIT

use crate::{
    c_size_t,
    gpt::GPT,
    nvme::{
        alloc_sector_buf, nvme_dma_uncertain, nvme_read, NVMEStorage, SectorBuffer, SECTOR_SIZE,
    },
    println,
};
use alloc::{boxed::Box, slice, string::String, vec::Vec};
use core::{cmp, ffi::c_void, mem, ptr};
use uuid::Uuid;

/*
 * Everything here is read from the disk, so every field is read with a bounds check and every
 * offset, length and count from the disk is checked before it is used: a malformed or hostile
 * container makes the scan fail, never panic.
 */

struct NxSuperblock([u8; 4096]);

macro_rules! obj_field {
    ($name:ident, $typ:ident, $offs:expr) => {
        #[allow(dead_code)]
        fn $name(&self) -> Option<$typ> {
            let bytes = self.0.get($offs..$offs + mem::size_of::<$typ>())?;
            Some($typ::from_le_bytes(bytes.try_into().ok()?))
        }
    };
}

macro_rules! obj_header {
    () => {
        obj_field!(cksum, u64, 0);
        obj_field!(oid, u64, 8);
        obj_field!(xid, u64, 16);
        obj_field!(type_, u32, 24);
        obj_field!(subtype, u32, 28);
    };
}

impl NxSuperblock {
    const MAGIC: u32 = 1112758350; //'BSXN'
    const MAX_FILE_SYSTEMS: usize = 100;
    /// nx_xp_desc_blocks: the high bit marks a non-contiguous checkpoint descriptor area
    const XP_DESC_NONCONTIGUOUS: u32 = 1 << 31;
    fn get_buf(&mut self) -> &mut [u8] {
        &mut self.0
    }
    fn new() -> Self {
        NxSuperblock([0; 4096])
    }
    obj_header!();
    obj_field!(magic, u32, 32);
    obj_field!(block_size, u32, 36);
    obj_field!(omap_oid, u64, 160);
    obj_field!(xp_desc_blocks, u32, 104);
    obj_field!(xp_desc_base, u64, 112);
    fn fs_oid(&self, i: usize) -> Option<u64> {
        let at = 184 + 8 * i;
        Some(u64::from_le_bytes(self.0.get(at..at + 8)?.try_into().ok()?))
    }
}

struct OmapPhys<'a>(&'a [u8]);
impl OmapPhys<'_> {
    const SIZE: usize = 88;
    obj_header!();
    obj_field!(tree_oid, u64, 48);
}

struct NLoc<'a>(&'a [u8]);

impl NLoc<'_> {
    obj_field!(off, u16, 0);
    obj_field!(len, u16, 2);
}

struct KVLoc<'a>(&'a [u8]);
impl KVLoc<'_> {
    const SIZE: usize = 8;
    fn k(&self) -> NLoc<'_> {
        NLoc(&self.0[0..4])
    }
    fn v(&self) -> NLoc<'_> {
        NLoc(&self.0[4..8])
    }
}

struct KVOff<'a>(&'a [u8]);
impl KVOff<'_> {
    const SIZE: usize = 4;
    obj_field!(k, u16, 0);
    obj_field!(v, u16, 2);
}

struct OmapKey<'a>(&'a [u8]);
impl OmapKey<'_> {
    const SIZE: usize = 16;
    obj_field!(oid, u64, 0);
    obj_field!(xid, u64, 8);
}

struct OmapVal<'a>(&'a [u8]);
impl OmapVal<'_> {
    const SIZE: usize = 16;
    obj_field!(size, u32, 4);
    obj_field!(paddr, u64, 8);
}

struct JDrecHashedKey<'a>(&'a [u8]);
impl JDrecHashedKey<'_> {
    obj_field!(obj_id_and_type, u64, 0);
    fn name_len(&self) -> Option<u32> {
        Some(u32::from_le_bytes(self.0.get(8..12)?.try_into().ok()?) & 0x000003ff)
    }
    /// The name without its terminating NUL, which name_len counts.
    fn name(&self) -> Option<&[u8]> {
        let len = (self.name_len()? as usize).checked_sub(1)?;
        self.0.get(12..12 + len)
    }
}

struct JDrecVal<'a>(&'a [u8]);
impl JDrecVal<'_> {
    obj_field!(file_id, u64, 0);
}

struct JFileExtentKey<'a>(&'a [u8]);
impl JFileExtentKey<'_> {
    obj_field!(obj_id_and_type, u64, 0);
    obj_field!(logical_addr, u64, 8);
}

struct JFileExtentVal<'a>(&'a [u8]);
impl JFileExtentVal<'_> {
    /// len_and_flags: the length in bytes below the flags in the top byte
    const LEN_MASK: u64 = 0x00ff_ffff_ffff_ffff;
    obj_field!(len_and_flags, u64, 0);
    obj_field!(phys_block_num, u64, 8);
    fn len(&self) -> Option<u64> {
        Some(self.len_and_flags()? & Self::LEN_MASK)
    }
}

struct BTreeInfo;
impl BTreeInfo {
    const SIZE: usize = 40;
}

struct BTreeNodePhys<'a>(&'a [u8]);
impl BTreeNodePhys<'_> {
    const FIXED_KV_SIZE: u16 = 0x4;
    const ROOT: u16 = 0x1;
    const SIZE: usize = 56;
    obj_header!();
    obj_field!(flags, u16, 32);
    obj_field!(level, u16, 34);
    obj_field!(nkeys, u32, 36);
    fn table_space(&self) -> Option<NLoc<'_>> {
        Some(NLoc(self.0.get(40..44)?))
    }

    /// A leaf node's areas, checked against the node: (table of contents, keys start, values
    /// end), the TOC ending where the keys start.
    fn leaf_areas(&self, fixed: bool) -> Option<(usize, usize, usize)> {
        if self.level()? != 0 {
            println!("apfs: B-trees deeper than one node are not supported");
            return None;
        }
        if (self.flags()? & Self::FIXED_KV_SIZE != 0) != fixed {
            println!("apfs: unexpected B-tree key layout");
            return None;
        }
        let ts = self.table_space()?;
        let toc = Self::SIZE.checked_add(ts.off()? as usize)?;
        let keys = toc.checked_add(ts.len()? as usize)?;
        let vals = self
            .0
            .len()
            .checked_sub(if self.flags()? & Self::ROOT == 0 {
                0
            } else {
                BTreeInfo::SIZE
            })?;
        (keys <= vals).then_some((toc, keys, vals))
    }

    /// Entry `i` of a leaf: its key and value, each inside the node's key and value areas.
    fn leaf_entry(&self, fixed: bool, i: usize) -> Option<(&[u8], &[u8])> {
        let (toc, keys, vals) = self.leaf_areas(fixed)?;
        let esz = if fixed { KVOff::SIZE } else { KVLoc::SIZE };
        let at = toc.checked_add(i.checked_mul(esz)?)?;
        if at.checked_add(esz)? > keys {
            return None;
        }
        let entry = &self.0[at..at + esz];
        let (k_off, k_len, v_off, v_len) = if fixed {
            let e = KVOff(entry);
            (e.k()?, OmapKey::SIZE as u16, e.v()?, OmapVal::SIZE as u16)
        } else {
            let e = KVLoc(entry);
            (e.k().off()?, e.k().len()?, e.v().off()?, e.v().len()?)
        };
        let k_start = keys.checked_add(k_off as usize)?;
        let k_end = k_start.checked_add(k_len as usize)?;
        let v_start = vals.checked_sub(v_off as usize)?;
        let v_end = v_start.checked_add(v_len as usize)?;
        if k_end > vals || v_start < keys || v_end > vals {
            return None;
        }
        Some((&self.0[k_start..k_end], &self.0[v_start..v_end]))
    }

    /// The leaf's entries, at most as many as its table of contents holds.
    fn leaf_entries(&self, fixed: bool) -> impl Iterator<Item = (&[u8], &[u8])> {
        let n = self.nkeys().unwrap_or(0) as usize;
        (0..n).map_while(move |i| self.leaf_entry(fixed, i))
    }
}

const APFS_FS_FLAGS_OFFSET: usize = 264;
struct ApfsSuperblock<'a>(&'a [u8]);
impl ApfsSuperblock<'_> {
    const MAGIC: u32 = 1112756289; //'BSPA'
    obj_header!();
    obj_field!(magic, u32, 32);
    obj_field!(role, u16, 964);
    obj_field!(fs_flags, u64, APFS_FS_FLAGS_OFFSET);
    obj_field!(omap_oid, u64, 128);
    obj_field!(root_tree_oid, u64, 136);
}

const VOL_ROLE_XART: u16 = 0x100;
const OBJ_TYPE_SHIFT: u32 = 60;
const APFS_TYPE_FILE_EXTENT: u64 = 8;
const APFS_TYPE_DIR_REC: u64 = 9;
const ROOT_DIR_INO_NUM: u64 = 2;

struct Partition {
    offset: u64,
    /// the partition's length in blocks: nothing past it is read
    sectors: u64,
    buf: Option<Box<SectorBuffer>>,
}

/// A zeroed buffer of `len` bytes read from the disk, or None if it does not fit the partition
/// or memory.
fn alloc_read_buf(disk: &Partition, len: u64) -> Option<Vec<u8>> {
    if len > disk.sectors.saturating_mul(SECTOR_SIZE as u64) {
        return None;
    }
    let len = usize::try_from(len).ok()?;
    let mut v = Vec::new();
    v.try_reserve_exact(len).ok()?;
    v.resize(len, 0);
    Some(v)
}

fn pread(disk: &mut Partition, pos: u64, target: &mut [u8]) -> Result<(), ()> {
    let blocks = (target.len().div_ceil(SECTOR_SIZE)) as u64;
    if pos > disk.sectors || blocks > disk.sectors - pos {
        println!(
            "apfs: read of {} blocks at {} is outside the partition",
            blocks, pos
        );
        return Err(());
    }
    unsafe {
        for i in 0..blocks as usize {
            let lba = disk.offset + i as u64 + pos;
            let buf = disk.buf.as_mut().ok_or(())?;
            if !nvme_read(1, lba, buf.0.as_mut_ptr() as *mut _) {
                if nvme_dma_uncertain() {
                    // The timed-out controller may still DMA into this exact allocation.
                    if let Some(buf) = disk.buf.take() {
                        mem::forget(buf);
                    }
                }
                println!("nvme_read({}, {}) failed", 1, lba);
                return Err(());
            }
            let off = i * SECTOR_SIZE;
            let copy_len = cmp::min(SECTOR_SIZE, target.len() - off);
            target[off..off + copy_len].copy_from_slice(&buf.0[..copy_len])
        }
    }
    Ok(())
}

// should probably fix xids here
fn lookup_omap<'a>(cur_node: &'a BTreeNodePhys, key: u64) -> Option<OmapVal<'a>> {
    cur_node
        .leaf_entries(true)
        .find(|(k, _)| OmapKey(k).oid() == Some(key))
        .map(|(_, v)| OmapVal(v))
}

fn lookup_gl(cur_node: &BTreeNodePhys) -> Option<u64> {
    let root_ino_key = ROOT_DIR_INO_NUM | (APFS_TYPE_DIR_REC << OBJ_TYPE_SHIFT);
    for (k, v) in cur_node.leaf_entries(false) {
        let map_key = JDrecHashedKey(k);
        if map_key.obj_id_and_type() != Some(root_ino_key) {
            continue;
        }
        match map_key.name() {
            Some(name) if String::from_utf8_lossy(name).ends_with(".gl") => {}
            _ => continue,
        }
        return JDrecVal(v).file_id();
    }
    None
}

fn lookup_extents<'a>(cur_node: &'a BTreeNodePhys, key: u64) -> Option<JFileExtentVal<'a>> {
    cur_node
        .leaf_entries(false)
        .find(|(k, _)| JFileExtentKey(k).obj_id_and_type() == Some(key))
        .map(|(_, v)| JFileExtentVal(v))
}

/// Read the object map at block `oid` and the root node of its tree, which must say it is
/// object `oid`'s tree.
fn read_omap_tree(disk: &mut Partition, oid: u64, block_size: usize) -> Option<Vec<u8>> {
    let mut omap_bytes = vec![0; OmapPhys::SIZE];
    pread(disk, oid, &mut omap_bytes).ok()?;
    let omap = OmapPhys(&omap_bytes);
    if omap.oid()? != oid {
        println!("apfs: object map {} is not at its block", oid);
        return None;
    }
    let tree_oid = omap.tree_oid()?;
    let mut node_bytes = vec![0; block_size];
    pread(disk, tree_oid, &mut node_bytes).ok()?;
    if BTreeNodePhys(&node_bytes).oid()? != tree_oid {
        println!("apfs: object map tree {} is not at its block", tree_oid);
        return None;
    }
    Some(node_bytes)
}

/// The gigalocker of the xART volume `fs_id`, None if `fs_id` is no readable xART volume.
fn read_volume(
    disk: &mut Partition,
    cont_node: &BTreeNodePhys,
    fs_id: u64,
    block_size: usize,
) -> Option<Vec<u8>> {
    let asb_offset = lookup_omap(cont_node, fs_id)?.paddr()?;
    let mut asb_bytes = vec![0; block_size];
    pread(disk, asb_offset, &mut asb_bytes).ok()?;
    let asb = ApfsSuperblock(&asb_bytes);
    if asb.oid()? != fs_id || asb.magic()? != ApfsSuperblock::MAGIC {
        println!("apfs: volume {} has no valid superblock", fs_id);
        return None;
    }
    if asb.role()? != VOL_ROLE_XART {
        return None;
    }
    let vol_node_bytes = read_omap_tree(disk, asb.omap_oid()?, block_size)?;
    let vol_node = BTreeNodePhys(&vol_node_bytes);
    let root_tree_oid = asb.root_tree_oid()?;
    let fs_root = lookup_omap(&vol_node, root_tree_oid)?;
    // a B-tree node is one block
    if fs_root.size()? as usize != block_size {
        println!("apfs: root tree node of {} bytes", fs_root.size()?);
        return None;
    }
    let mut fs_root_bytes = vec![0; block_size];
    pread(disk, fs_root.paddr()?, &mut fs_root_bytes).ok()?;
    let fs_root_node = BTreeNodePhys(&fs_root_bytes);
    if fs_root_node.oid()? != root_tree_oid {
        println!("apfs: root tree {} is not at its block", root_tree_oid);
        return None;
    }
    let gl_file_id = lookup_gl(&fs_root_node)?;
    let extent = lookup_extents(
        &fs_root_node,
        gl_file_id | (APFS_TYPE_FILE_EXTENT << OBJ_TYPE_SHIFT),
    )?;
    let mut data = alloc_read_buf(disk, extent.len()?)?;
    pread(disk, extent.phys_block_num()?, &mut data).ok()?;
    Some(data)
}

fn scan_volume(disk: &mut Partition) -> Result<Vec<u8>, ()> {
    let mut sb = NxSuperblock::new();
    pread(disk, 0, sb.get_buf())?;
    if sb.magic() != Some(NxSuperblock::MAGIC) {
        return Err(());
    }
    // blocks are read as NVMe sectors
    if sb.block_size() != Some(SECTOR_SIZE as u32) {
        println!("apfs: unsupported block size {:?}", sb.block_size());
        return Err(());
    }
    let xp_desc_blocks = sb.xp_desc_blocks().ok_or(())?;
    if xp_desc_blocks & NxSuperblock::XP_DESC_NONCONTIGUOUS != 0 {
        println!("apfs: non-contiguous checkpoint areas are not supported");
        return Err(());
    }
    let xp_desc_base = sb.xp_desc_base().ok_or(())?;
    for i in 0..xp_desc_blocks {
        let mut sbc = NxSuperblock::new();
        pread(
            disk,
            xp_desc_base.checked_add(i as u64).ok_or(())?,
            sbc.get_buf(),
        )?;
        if sbc.magic() == Some(NxSuperblock::MAGIC)
            && sbc.block_size() == Some(SECTOR_SIZE as u32)
            && sbc.xid() > sb.xid()
        {
            sb = sbc;
        }
    }
    let cont_node_bytes = read_omap_tree(disk, sb.omap_oid().ok_or(())?, SECTOR_SIZE).ok_or(())?;
    let cont_node = BTreeNodePhys(&cont_node_bytes);
    for i in 0..NxSuperblock::MAX_FILE_SYSTEMS {
        let fs_id = sb.fs_oid(i).ok_or(())?;
        if fs_id == 0 {
            continue;
        }
        if let Some(data) = read_volume(disk, &cont_node, fs_id, SECTOR_SIZE) {
            return Ok(data);
        }
    }
    Err(())
}

fn scan_disks() -> Result<Vec<u8>, ()> {
    let storage = NVMEStorage::new(1, 0);
    let mut pt = GPT::new(storage).map_err(|_| ())?;
    for i in 0..pt.count() {
        let v = pt.index(i).map_err(|_| ())?;
        if v.get_type_guid() != Uuid::parse_str("69646961-6700-11aa-aa11-00306543ecac").unwrap() {
            continue;
        }
        // GPT::index() checked that the partition does not end before it starts
        let mut part = Partition {
            offset: v.get_starting_lba(),
            sectors: v.get_ending_lba() - v.get_starting_lba() + 1,
            buf: Some(alloc_sector_buf()),
        };
        return scan_volume(&mut part);
    }
    Err(())
}

#[no_mangle]
pub unsafe extern "C" fn rust_read_gigalocker(size: *mut c_size_t) -> *mut c_void {
    unsafe {
        let gl = match scan_disks() {
            Ok(g) => g,
            Err(_) => {
                *size = 0;
                return ptr::null_mut();
            }
        };
        let gl = gl.into_boxed_slice();
        *size = gl.len();
        Box::into_raw(gl) as *mut _
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_free_gigalocker(data: *mut c_void, size: c_size_t) {
    unsafe {
        mem::drop(Box::from_raw(slice::from_raw_parts_mut(
            data as *mut u8,
            size,
        )));
    }
}

#[cfg(test)]
pub(crate) fn test_scan_volume(offset: u64, sectors: u64) -> Result<Vec<u8>, ()> {
    let mut part = Partition {
        offset,
        sectors,
        buf: Some(alloc_sector_buf()),
    };
    scan_volume(&mut part)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::nvme::tests::with_image;

    fn le16(b: &mut [u8], o: usize, v: u16) {
        b[o..o + 2].copy_from_slice(&v.to_le_bytes());
    }
    fn le32(b: &mut [u8], o: usize, v: u32) {
        b[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }
    fn le64(b: &mut [u8], o: usize, v: u64) {
        b[o..o + 8].copy_from_slice(&v.to_le_bytes());
    }

    const B: usize = SECTOR_SIZE;
    const FS_OID: u64 = 0x402;
    const ROOT_OID: u64 = 0x404;
    const FILE_ID: u64 = 0x10;
    /// where the root tree's TOC, keys and values start in a test node
    const TOC: usize = BTreeNodePhys::SIZE;
    const VALS: usize = B - BTreeInfo::SIZE;

    /// A root leaf of fixed-size object map entries oid -> (size, paddr).
    fn omap_node(oid: u64, entries: &[(u64, u32, u64)]) -> Vec<u8> {
        let mut b = vec![0u8; B];
        le64(&mut b, 8, oid);
        le16(
            &mut b,
            32,
            BTreeNodePhys::ROOT | BTreeNodePhys::FIXED_KV_SIZE,
        );
        le32(&mut b, 36, entries.len() as u32);
        let toc_len = entries.len() * KVOff::SIZE;
        le16(&mut b, 42, toc_len as u16);
        let keys = TOC + toc_len;
        for (i, &(k, size, paddr)) in entries.iter().enumerate() {
            let (koff, voff) = (i * 16, (i + 1) * 16);
            le16(&mut b, TOC + i * 4, koff as u16);
            le16(&mut b, TOC + i * 4 + 2, voff as u16);
            le64(&mut b, keys + koff, k);
            le64(&mut b, keys + koff + 8, 1);
            le32(&mut b, VALS - voff + 4, size);
            le64(&mut b, VALS - voff + 8, paddr);
        }
        b
    }

    /// The volume's root tree: a root directory record "x.gl" and the file's one extent.
    fn fs_root(ext_len: u64) -> Vec<u8> {
        let mut b = vec![0u8; B];
        le64(&mut b, 8, ROOT_OID);
        le16(&mut b, 32, BTreeNodePhys::ROOT);
        le32(&mut b, 36, 2);
        le16(&mut b, 42, 16);
        let keys = TOC + 16;
        let name = b"x.gl\0";
        let drec_key = 12 + name.len();
        le64(
            &mut b,
            keys,
            ROOT_DIR_INO_NUM | (APFS_TYPE_DIR_REC << OBJ_TYPE_SHIFT),
        );
        le32(&mut b, keys + 8, name.len() as u32);
        b[keys + 12..keys + 12 + name.len()].copy_from_slice(name);
        le64(&mut b, VALS - 18, FILE_ID);
        for (o, v) in [(0, 0), (2, drec_key as u16), (4, 18), (6, 18)] {
            le16(&mut b, TOC + o, v);
        }
        le64(
            &mut b,
            keys + drec_key,
            FILE_ID | (APFS_TYPE_FILE_EXTENT << OBJ_TYPE_SHIFT),
        );
        le64(&mut b, VALS - 42, ext_len);
        le64(&mut b, VALS - 42 + 8, 7);
        for (o, v) in [(8, drec_key as u16), (10, 16), (12, 42), (14, 24)] {
            le16(&mut b, TOC + o, v);
        }
        b
    }

    /// Blocks: 0 container superblock, 1 its object map, 2 that map's tree, 3 the xART volume
    /// superblock, 4 its object map, 5 that map's tree, 6 the root tree, 7 the gigalocker.
    fn container() -> Vec<u8> {
        let mut c = vec![0u8; 8 * B];
        let nx = &mut c[..B];
        le64(nx, 8, 1);
        le64(nx, 16, 1);
        le32(nx, 32, NxSuperblock::MAGIC);
        le32(nx, 36, B as u32);
        le64(nx, 160, 1);
        le64(nx, 184, FS_OID);
        le64(&mut c[B..], 8, 1);
        le64(&mut c[B..], 48, 2);
        c[2 * B..3 * B].copy_from_slice(&omap_node(2, &[(FS_OID, B as u32, 3)]));
        let apsb = &mut c[3 * B..4 * B];
        le64(apsb, 8, FS_OID);
        le32(apsb, 32, ApfsSuperblock::MAGIC);
        le64(apsb, 128, 4);
        le64(apsb, 136, ROOT_OID);
        le16(apsb, 964, VOL_ROLE_XART);
        le64(&mut c[4 * B..], 8, 4);
        le64(&mut c[4 * B..], 48, 5);
        c[5 * B..6 * B].copy_from_slice(&omap_node(5, &[(ROOT_OID, B as u32, 6)]));
        c[6 * B..7 * B].copy_from_slice(&fs_root(64));
        c[7 * B..7 * B + 64].fill(0x5a);
        c
    }

    fn scan(image: &[u8]) -> Result<Vec<u8>, ()> {
        with_image(100, image, || {
            test_scan_volume(100, (image.len() / B) as u64)
        })
    }

    #[test]
    fn reads_the_gigalocker() {
        assert_eq!(scan(&container()), Ok(vec![0x5a; 64]));
    }

    #[test]
    fn extent_flags_are_not_length() {
        let mut c = container();
        // a flag in the top byte of len_and_flags
        le64(&mut c[6 * B..], VALS - 42, (1 << 56) | 64);
        assert_eq!(scan(&c), Ok(vec![0x5a; 64]));
    }

    /// Scan each corrupted container: every one must fail, none may panic.
    fn all_fail(cases: &[(&str, fn(&mut Vec<u8>))]) {
        let mut failed = Vec::new();
        for (what, corrupt) in cases {
            let mut c = container();
            corrupt(&mut c);
            match std::panic::catch_unwind(|| scan(&c)) {
                Ok(Err(())) => {}
                Ok(Ok(_)) => failed.push(format!("{what}: accepted")),
                Err(_) => failed.push(format!("{what}: panicked")),
            }
        }
        assert!(failed.is_empty(), "{failed:#?}");
    }

    #[test]
    fn malformed_containers_fail() {
        all_fail(&[
            ("block size", |c| le32(c, 36, 2 * B as u32)),
            ("index node", |c| le16(&mut c[2 * B..], 34, 1)),
            ("variable-size object map", |c| {
                le16(&mut c[2 * B..], 32, BTreeNodePhys::ROOT)
            }),
            ("more keys than the TOC holds", |c| {
                le32(&mut c[2 * B..], 36, 0xffff_ffff);
                le64(c, 184, FS_OID + 1);
            }),
            ("TOC past the node", |c| le16(&mut c[2 * B..], 40, 0xffff)),
            ("value offset past the value area", |c| {
                le16(&mut c[2 * B..], TOC + 2, 0xffff)
            }),
            ("key offset past the node", |c| {
                le16(&mut c[2 * B..], TOC, 0xffff)
            }),
            ("object map at another block", |c| le64(&mut c[B..], 8, 9)),
            ("volume superblock of another volume", |c| {
                le64(&mut c[3 * B..], 8, FS_OID + 1)
            }),
            ("volume superblock magic", |c| le32(&mut c[3 * B..], 32, 0)),
            ("root tree node of 0 bytes", |c| {
                le32(&mut c[5 * B..], VALS - 16 + 4, 0)
            }),
            ("root tree node at another block", |c| {
                le64(&mut c[6 * B..], 8, ROOT_OID + 1)
            }),
            ("empty file name", |c| {
                le32(&mut c[6 * B..], TOC + 16 + 8, 0)
            }),
            ("key length past the node", |c| {
                le16(&mut c[6 * B..], TOC + 2, 0xffff)
            }),
        ]);
    }

    #[test]
    fn reads_outside_the_partition_fail() {
        all_fail(&[
            ("non-contiguous checkpoint area", |c| {
                // the high bit of nx_xp_desc_blocks
                le32(c, 104, (1 << 31) | 4)
            }),
            ("checkpoint area past the partition", |c| {
                le32(c, 104, 4);
                le64(c, 112, u64::MAX - 1);
            }),
            ("object map past the partition", |c| le64(c, 160, u64::MAX)),
            ("gigalocker longer than the partition", |c| {
                le64(&mut c[6 * B..], VALS - 42, 9 * B as u64)
            }),
            ("gigalocker of 2^56 - 1 bytes", |c| {
                le64(&mut c[6 * B..], VALS - 42, (1 << 56) - 1)
            }),
            ("gigalocker past the partition", |c| {
                le64(&mut c[6 * B..], VALS - 42 + 8, u64::MAX)
            }),
        ]);
    }
}
