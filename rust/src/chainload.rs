// SPDX-License-Identifier: MIT AND GPL-2.0-only
#![deny(unsafe_op_in_unsafe_fn)]

use crate::c_size_t;
use crate::gpt;
use crate::nvme;
use crate::println;
use crate::trial;
use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::{c_char, c_int, c_void, CStr};
use fatfs::{FileSystem, FsOptions, Read};
use uuid::Uuid;

#[derive(Debug)]
pub enum Error {
    FATError(fatfs::Error<nvme::Error>),
    GPTError(gpt::Error<nvme::Error>),
    BadArgs,
    PartitionNotFound,
    FileNotFound,
    AmbiguousFile,
    InvalidFile,
    OutOfMemory,
    UnexpectedEof,
}

impl From<fatfs::Error<nvme::Error>> for Error {
    fn from(err: fatfs::Error<nvme::Error>) -> Error {
        Error::FATError(err)
    }
}

fn valid_fat_path(path: &str) -> bool {
    !path.is_empty()
        && path
            .bytes()
            .all(|byte| (0x20..0x7f).contains(&byte) && byte != b';' && byte != b'\\')
        && path
            .split('/')
            .all(|component| !component.is_empty() && component != "." && component != "..")
}

#[cfg(test)]
mod path_tests {
    use super::valid_fat_path;

    #[test]
    fn stage1_path_grammar() {
        assert!(valid_fat_path("aurora/stage2.bin"));
        for path in [
            "aurora/a\0b",
            "aurora/a\x1fb",
            "aurora//b",
            "aurora/./b",
            "aurora/../b",
        ] {
            assert!(!valid_fat_path(path), "accepted {path:?}");
        }
    }
}

impl From<gpt::Error<nvme::Error>> for Error {
    fn from(err: gpt::Error<nvme::Error>) -> Error {
        Error::GPTError(err)
    }
}

type Root<'a> =
    fatfs::Dir<'a, nvme::NVMEStorage, fatfs::NullTimeProvider, fatfs::LossyOemCpConverter>;

fn read_file(root: &Root, path: &str) -> Result<Vec<u8>, Error> {
    let (parent, file_name) = path.rsplit_once('/').unwrap_or(("", path));
    let dir = if parent.is_empty() {
        root.clone()
    } else {
        root.open_dir(parent)?
    };
    let mut matched = None;
    for result in dir.iter() {
        let entry = result?;
        if entry.eq_name(file_name) {
            if matched.is_some() {
                return Err(Error::AmbiguousFile);
            }
            matched = Some(entry);
        }
    }
    let entry = matched.ok_or(Error::FileNotFound)?;
    if !entry.is_file() {
        return Err(Error::InvalidFile);
    }
    let size = entry.len() as usize;
    if size == 0 || size > 256 * 1024 * 1024 {
        return Err(Error::InvalidFile);
    }
    let mut file = entry.to_file();

    println!("File size: {}", size);

    let mut buf = Vec::new();
    buf.try_reserve_exact(size)
        .map_err(|_| Error::OutOfMemory)?;
    buf.resize(size, 0);
    let mut slice = &mut buf[..];
    while !slice.is_empty() {
        let read = file.read(slice)?;
        if read == 0 {
            return Err(Error::UnexpectedEof);
        }
        slice = &mut slice[read..];
    }
    println!("File read successfully");

    Ok(buf)
}

/// Spend an armed aura-try marker at the ESP root and return the path it names (trial.rs).
fn take_trial(root: &Root, offset: u64, sectors: u64) -> Option<String> {
    let entry = root
        .iter()
        .filter_map(|r| r.ok())
        .find(|e| e.is_file() && e.eq_name(trial::MARKER))?;
    let len = entry.len() as usize;
    if len < trial::ARMED.len() + 2 || len > nvme::SECTOR_SIZE {
        println!("aura-try: ignoring a marker of {} bytes", len);
        return None;
    }
    let mut contents = vec![0u8; len];
    let mut file = entry.to_file();
    let mut slice = &mut contents[..];
    while !slice.is_empty() {
        match file.read(slice) {
            Ok(0) | Err(_) => return None,
            Ok(n) => slice = &mut slice[n..],
        }
    }
    let Some(path) = trial::armed_path(&contents) else {
        println!("aura-try: marker is spent");
        return None;
    };
    if !valid_fat_path(path) {
        println!("aura-try: ignoring an invalid path");
        return None;
    }
    let first = file.extents().next()?.ok()?;
    let block = first.offset / nvme::SECTOR_SIZE as u64;
    if first.offset % nvme::SECTOR_SIZE as u64 != 0 || block >= sectors {
        println!("aura-try: marker data is not block aligned");
        return None;
    }
    if !trial::spend(&mut trial::Nvme(1), offset + block, &contents) {
        println!("aura-try: could not spend the marker, booting the configured image");
        return None;
    }
    println!("aura-try: marker spent, trying {}", path);
    Some(String::from(path))
}

/*
 * trial: None to leave any aura-try marker alone. Otherwise a NUL-terminated buffer: empty to
 * take an armed marker (and receive the path it names), or the path of a trial already taken.
 */
fn load_image(
    spec: &str,
    proxy_request: Option<&mut bool>,
    trial: Option<&mut [u8]>,
) -> Result<Vec<u8>, Error> {
    println!("Chainloading from the ESP");

    let mut args = spec.split(';');

    let uuid = Uuid::parse_str(args.next().ok_or(Error::BadArgs)?).or(Err(Error::BadArgs))?;
    let path = args.next().ok_or(Error::BadArgs)?;
    if args.next().is_some() || !valid_fat_path(path) {
        return Err(Error::BadArgs);
    }

    let part = {
        let storage = nvme::NVMEStorage::new(1, 0);
        let mut pt = gpt::GPT::new(storage)?;

        println!("Searching for the requested partition");
        pt.find_by_partuuid(uuid)?.ok_or(Error::PartitionNotFound)?
    };

    let offset = part.get_starting_lba();
    let sectors = part
        .get_ending_lba()
        .checked_sub(offset)
        .and_then(|n| n.checked_add(1))
        .ok_or(Error::BadArgs)?;

    println!("Partition offset: {}", offset);

    let storage = nvme::NVMEStorage::new(1, offset)
        .with_extent(sectors)
        .map_err(|_| Error::BadArgs)?
        .with_read_budget(512 * 1024 * 1024 / 4096);
    let opts = FsOptions::new().update_accessed_date(false);

    let fs = FileSystem::new(storage, opts)?;
    let root = fs.root_dir();
    // An "aura-proxy" file at the ESP root asks Stage 1 to wait for a host before booting.
    if let Some(marker) = proxy_request {
        *marker = false;
        for result in root.iter() {
            let entry = result?;
            if entry.is_file() && entry.eq_name("aura-proxy") {
                *marker = true;
            }
        }
    }

    if let Some(buf) = trial {
        let taken = match buf.iter().position(|&b| b == 0) {
            Some(0) => take_trial(&root, offset, sectors),
            Some(end) => core::str::from_utf8(&buf[..end]).ok().map(String::from),
            None => None,
        };
        buf[0] = 0;
        if let Some(try_path) = taken {
            if try_path.len() < buf.len() {
                match read_file(&root, &try_path) {
                    Ok(image) => {
                        buf[..try_path.len()].copy_from_slice(try_path.as_bytes());
                        buf[try_path.len()] = 0;
                        return Ok(image);
                    }
                    Err(err) => {
                        println!("aura-try: {:?}, booting the configured image", err);
                    }
                }
            }
        }
    }

    read_file(&root, path)
}

#[no_mangle]
pub unsafe extern "C" fn rust_load_image(
    raw_spec: *const c_char,
    image: *mut *mut c_void,
    size: *mut c_size_t,
    proxy_request: *mut bool,
    trial: *mut c_char,
    trial_capacity: c_size_t,
) -> c_int {
    if raw_spec.is_null() || image.is_null() || size.is_null() {
        return -1;
    }
    let Ok(spec) = (unsafe { CStr::from_ptr(raw_spec).to_str() }) else {
        return -1;
    };

    let marker = unsafe { proxy_request.as_mut() };
    let trial = if trial.is_null() || trial_capacity == 0 {
        None
    } else {
        Some(unsafe { core::slice::from_raw_parts_mut(trial as *mut u8, trial_capacity) })
    };
    match load_image(spec, marker, trial) {
        Ok(buf) => {
            let buf = Box::leak(buf.into_boxed_slice());
            unsafe {
                *size = buf.len();
                *image = buf.as_mut_ptr() as *mut c_void;
            }
            0
        }
        Err(err) => {
            println!("Chainload failed: {:?}", err);
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_free_image(image: *mut c_void, size: c_size_t) {
    if !image.is_null() {
        let slice = core::ptr::slice_from_raw_parts_mut(image as *mut u8, size);
        drop(unsafe { Box::from_raw(slice) });
    }
}
