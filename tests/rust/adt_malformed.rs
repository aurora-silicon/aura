// SPDX-License-Identifier: GPL-2.0-only
// Host harness for the production Rust ADT parser.
#![allow(non_camel_case_types)]

use std::ffi::{c_void, CStr, CString};

type c_size_t = usize;

#[path = "../../rust/src/adt.rs"]
mod parser;

#[no_mangle]
static mut adt: *const c_void = std::ptr::null();
static mut ADT_SIZE: u32 = 0;

#[no_mangle]
extern "C" fn adt_get_size() -> u32 {
    unsafe { ADT_SIZE }
}

fn property(blob: &mut Vec<u8>, name: &str, value: &[u8]) {
    let start = blob.len();
    blob.resize(start + 36, 0);
    blob[start..start + name.len()].copy_from_slice(name.as_bytes());
    blob[start + 32..start + 36].copy_from_slice(&(value.len() as u32).to_le_bytes());
    blob.extend_from_slice(value);
    while blob.len() % 4 != 0 {
        blob.push(0);
    }
}

/// A node header and its properties; its children, if any, follow in the blob.
fn node(blob: &mut Vec<u8>, props: &[(&str, &[u8])], children: u32) -> i32 {
    let offset = blob.len() as i32;
    blob.extend_from_slice(&(props.len() as u32).to_le_bytes());
    blob.extend_from_slice(&children.to_le_bytes());
    for (name, value) in props {
        property(blob, name, value);
    }
    offset
}

/// Point the parser at `blob`, of which the ADT is the first `size` bytes.
/// The blob is leaked: the parser hands out 'static references into it.
fn install(blob: Vec<u8>, size: usize) -> *const c_void {
    assert!(size <= blob.len());
    let words: &'static mut [u32] =
        Box::leak(vec![0u32; blob.len().div_ceil(4)].into_boxed_slice());
    // SAFETY: the u32 slice is leaked and holds at least blob.len() bytes.
    let bytes =
        unsafe { std::slice::from_raw_parts_mut(words.as_mut_ptr().cast::<u8>(), blob.len()) };
    bytes.copy_from_slice(&blob);
    unsafe {
        adt = bytes.as_ptr().cast();
        ADT_SIZE = size as u32;
        adt
    }
}

/// The C entry points return their error value for an offset that is not a
/// node or property, or a node they cannot read, instead of panicking (which
/// reboots aura), so that the ADT_FOREACH_* loops can stop on it.
fn c_api_errors_instead_of_panics() {
    let mut blob = Vec::new();
    // The root's second property claims more bytes than the image has.
    node(&mut blob, &[("name", b"device-tree\0"), ("x", b"1234")], 2);
    let size = blob.len();
    blob[size - 8..size - 4].copy_from_slice(&64u32.to_le_bytes());
    let adt_ptr = install(blob, size);
    let name = CString::new("name").unwrap();
    let compat = CString::new("apple,x").unwrap();
    unsafe {
        for bad in [-1, -4, 2, size as i32, i32::MAX] {
            assert!(parser::adt_get_child_count(adt_ptr, bad) < 0, "{bad}");
            assert!(parser::adt_get_property_count(adt_ptr, bad) < 0, "{bad}");
            assert!(parser::adt_first_property_offset(adt_ptr, bad) < 0, "{bad}");
            assert!(parser::adt_next_property_offset(adt_ptr, bad) < 0, "{bad}");
            assert!(parser::adt_get_name(adt_ptr, bad).is_null(), "{bad}");
            assert!(
                parser::adt_getprop(adt_ptr, bad, name.as_ptr(), std::ptr::null_mut()).is_null()
            );
            assert!(parser::adt_get_property(adt_ptr, bad, name.as_ptr()).is_null());
            assert!(!parser::adt_is_compatible(adt_ptr, bad, compat.as_ptr()));
            assert!(!parser::adt_is_compatible_at(
                adt_ptr,
                bad,
                compat.as_ptr(),
                0
            ));
            assert!(parser::adt_subnode_offset(adt_ptr, bad, name.as_ptr()) < 0);
        }
        let first = parser::adt_first_property_offset(adt_ptr, 0);
        assert_eq!(first, 8);
        // The broken second property is not handed out as an offset.
        assert!(parser::adt_next_property_offset(adt_ptr, first) < 0);
        // A lookup that has to pass it cannot read the compatible list.
        assert!(!parser::adt_is_compatible(adt_ptr, 0, compat.as_ptr()));
        // The children the header claims are not there.
        assert!(parser::adt_first_child_offset(adt_ptr, 0) < 0);
    }

    // A node without a name has no name, rather than halting aura.
    let mut blob = Vec::new();
    node(&mut blob, &[("compatible", b"apple,x\0")], 0);
    let len = blob.len();
    let adt_ptr = install(blob, len);
    unsafe {
        assert!(parser::adt_get_name(adt_ptr, 0).is_null());
        assert!(parser::adt_is_compatible(adt_ptr, 0, compat.as_ptr()));
    }
}

fn register_paths() {
    let mut blob = Vec::new();
    let mut offsets = [0i32; 8];
    for depth in 0..=8 {
        if depth > 0 {
            offsets[depth - 1] = blob.len() as i32;
        }
        blob.extend_from_slice(&4u32.to_le_bytes());
        blob.extend_from_slice(&u32::from(depth < 8).to_le_bytes());
        property(&mut blob, "name", b"node\0");
        property(&mut blob, "#address-cells", &1u32.to_le_bytes());
        property(&mut blob, "#size-cells", &1u32.to_le_bytes());
        let (addr, size): (u32, u32) = if depth == 8 {
            (0x8000, 0x800)
        } else {
            (0x1000, 0x100)
        };
        let mut reg = addr.to_le_bytes().to_vec();
        reg.extend_from_slice(&size.to_le_bytes());
        property(&mut blob, "reg", &reg);
    }
    let reg = CString::new("reg").unwrap();
    let mut addr = 0;
    let mut size = 0;
    unsafe {
        adt = blob.as_ptr().cast();
        ADT_SIZE = blob.len() as u32;
        // A full eight-node path has no terminator inside the C ABI array.
        assert_eq!(
            parser::adt_get_reg(
                adt,
                offsets.as_mut_ptr(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            0
        );
        assert_eq!((addr, size), (0x8000, 0x800));
        offsets[1..].fill(0);
        assert_eq!(
            parser::adt_get_reg(
                adt,
                offsets.as_mut_ptr(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            0
        );
        assert_eq!((addr, size), (0x1000, 0x100));
        assert_eq!(
            parser::adt_get_reg(
                adt,
                offsets.as_mut_ptr(),
                reg.as_ptr(),
                1,
                &mut addr,
                &mut size
            ),
            -15
        );
    }
}

fn main() {
    let mut blob = vec![0u8; 344];
    blob[0..4].copy_from_slice(&1u32.to_le_bytes());
    blob[8..13].copy_from_slice(b"name\0");
    blob[40..44].copy_from_slice(&300u32.to_le_bytes());
    let name = CString::new("name").unwrap();
    let mut out = [0xa5u8; 300];

    unsafe {
        adt = blob.as_ptr().cast();
        ADT_SIZE = blob.len() as u32;

        assert_eq!(
            parser::adt_getprop_copy(adt, 0, name.as_ptr(), out.as_mut_ptr().cast(), 299),
            -20
        );
        assert!(out.iter().all(|&byte| byte == 0xa5));
        assert_eq!(
            parser::adt_getprop_copy(adt, 0, name.as_ptr(), out.as_mut_ptr().cast(), 300),
            300
        );
        assert!(out.iter().all(|&byte| byte == 0));

        blob[40..44].copy_from_slice(&301u32.to_le_bytes());
        assert_eq!(
            parser::adt_getprop_copy(adt, 0, name.as_ptr(), out.as_mut_ptr().cast(), 300),
            -4
        );
        assert_eq!(
            parser::adt_getprop_copy(adt, -1, name.as_ptr(), out.as_mut_ptr().cast(), 300),
            -4
        );
        assert_eq!(parser::adt_first_child_offset(adt, -1), -4);
        assert_eq!(parser::adt_next_sibling_offset(adt, -1), -4);

        let reg = CString::new("reg").unwrap();
        let mut path = [0i32; 8];
        let mut addr = 0xa5a5u64;
        let mut size = 0x5a5au64;
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            -4
        );
        path[0] = -1;
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            -4
        );
        path[0] = i32::MAX;
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            -4
        );
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                reg.as_ptr(),
                -1,
                &mut addr,
                &mut size
            ),
            -15
        );
        assert_eq!(
            parser::adt_get_reg(
                adt,
                std::ptr::null_mut(),
                reg.as_ptr(),
                0,
                &mut addr,
                &mut size
            ),
            -15
        );
        assert_eq!((addr, size), (0xa5a5, 0x5a5a));
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                std::ptr::null(),
                0,
                &mut addr,
                &mut size
            ),
            -15
        );
        let invalid_name = [0xffu8, 0];
        assert_eq!(
            parser::adt_get_reg(
                adt,
                path.as_mut_ptr(),
                invalid_name.as_ptr().cast(),
                0,
                &mut addr,
                &mut size
            ),
            -15
        );
        assert_eq!((addr, size), (0xa5a5, 0x5a5a));
    }
    register_paths();
    c_api_errors_instead_of_panics();
}
