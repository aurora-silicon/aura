// SPDX-License-Identifier: GPL-2.0-only
// Host harness for the well-formed paths of the production Rust ADT parser:
// tree walking, path lookup, property access and reg/ranges translation.
// The fixture tree below is synthetic; it only mirrors the ADT layout.
#![allow(non_camel_case_types)]

use std::ffi::{c_char, c_void, CStr, CString};

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

const NOT_FOUND: i32 = -1;
const BAD_PATH: i32 = -5;
const BAD_NCELLS: i32 = -14;
const BAD_VALUE: i32 = -15;
const BAD_LENGTH: i32 = -20;

struct Node {
    props: Vec<(&'static str, Vec<u8>)>,
    children: Vec<Node>,
}

fn node(name: &str, mut props: Vec<(&'static str, Vec<u8>)>, children: Vec<Node>) -> Node {
    let mut value = name.as_bytes().to_vec();
    value.push(0);
    props.insert(0, ("name", value));
    Node { props, children }
}

fn u32s(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

fn strs(list: &[&str]) -> Vec<u8> {
    let mut out = Vec::new();
    for s in list {
        out.extend_from_slice(s.as_bytes());
        out.push(0);
    }
    out
}

fn emit(blob: &mut Vec<u8>, n: &Node) {
    blob.extend_from_slice(&(n.props.len() as u32).to_le_bytes());
    blob.extend_from_slice(&(n.children.len() as u32).to_le_bytes());
    for (name, value) in &n.props {
        let start = blob.len();
        blob.resize(start + 36, 0);
        blob[start..start + name.len()].copy_from_slice(name.as_bytes());
        blob[start + 32..start + 36].copy_from_slice(&(value.len() as u32).to_le_bytes());
        blob.extend_from_slice(value);
        while blob.len() % 4 != 0 {
            blob.push(0);
        }
    }
    for c in &n.children {
        emit(blob, c);
    }
}

/// The ADT image backing every call. It is leaked so the parser's
/// 'static references stay valid for the whole run.
fn install(root: &Node) -> &'static mut [u8] {
    let mut blob = Vec::new();
    emit(&mut blob, root);
    let blob: &'static mut [u8] = Box::leak(blob.into_boxed_slice());
    unsafe {
        adt = blob.as_ptr().cast();
        ADT_SIZE = blob.len() as u32;
    }
    blob
}

fn c(s: &str) -> CString {
    CString::new(s).unwrap()
}

fn path(p: &str) -> i32 {
    unsafe { parser::adt_path_offset(adt, c(p).as_ptr()) }
}

fn name_of(off: i32) -> String {
    unsafe {
        CStr::from_ptr(parser::adt_get_name(adt, off))
            .to_str()
            .unwrap()
            .to_owned()
    }
}

fn subnode(off: i32, name: &str) -> i32 {
    unsafe { parser::adt_subnode_offset(adt, off, c(name).as_ptr()) }
}

fn trace(p: &str) -> (i32, [i32; 8]) {
    let mut offsets = [0x5a5a_5a5ai32; 8];
    let ret = unsafe { parser::adt_path_offset_trace(adt, c(p).as_ptr(), offsets.as_mut_ptr()) };
    (ret, offsets)
}

fn reg(p: &str, prop: &str, idx: i32) -> Result<(u64, u64), i32> {
    let (ret, mut offsets) = trace(p);
    assert!(ret > 0, "trace {p} failed: {ret}");
    let mut a = 0;
    let mut s = 0;
    let r = unsafe {
        parser::adt_get_reg(
            adt,
            offsets.as_mut_ptr(),
            c(prop).as_ptr(),
            idx,
            &mut a,
            &mut s,
        )
    };
    if r == 0 {
        Ok((a, s))
    } else {
        Err(r)
    }
}

fn compat(off: i32, s: &str) -> bool {
    unsafe { parser::adt_is_compatible(adt, off, c(s).as_ptr()) }
}

fn compat_at(off: i32, s: &str, idx: usize) -> bool {
    unsafe { parser::adt_is_compatible_at(adt, off, c(s).as_ptr(), idx) }
}

fn getprop(off: i32, name: &str) -> Option<Vec<u8>> {
    let mut len = 0u32;
    let p = unsafe { parser::adt_getprop(adt, off, c(name).as_ptr(), &mut len) };
    if p.is_null() {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(p as *const u8, len as usize) }.to_vec())
}

/// A small tree in the shape of a real ADT: a root with 2 address cells, an
/// arm-io bus that remaps a 32-bit child window into the 64-bit physical
/// space, a nested bus with its own ranges, and a sibling after arm-io so the
/// sibling walk has to skip a whole subtree.
fn tree() -> Node {
    node(
        "device-tree",
        vec![
            ("#address-cells", u32s(&[2])),
            ("#size-cells", u32s(&[2])),
            ("compatible", strs(&["J000AP", "AppleARM"])),
        ],
        vec![
            node(
                "cpus",
                vec![("#address-cells", u32s(&[1])), ("#size-cells", u32s(&[0]))],
                vec![
                    node("cpu0", vec![("reg", u32s(&[0x000]))], vec![]),
                    node("cpu1", vec![("reg", u32s(&[0x001]))], vec![]),
                ],
            ),
            node(
                "arm-io",
                vec![
                    ("#address-cells", u32s(&[2])),
                    ("#size-cells", u32s(&[2])),
                    ("compatible", strs(&["arm-io,t0000", "arm-io"])),
                    // child 0x0_0000_0000 + 0x1_0000_0000 -> parent 0x2_0000_0000
                    // child 0x1_0000_0000 + 0x0_1000_0000 -> parent 0x5_0000_0000
                    (
                        "ranges",
                        u32s(&[
                            0x0,
                            0x0,
                            0x0,
                            0x2,
                            0x0,
                            0x1, //
                            0x0,
                            0x1,
                            0x0,
                            0x5,
                            0x1000_0000,
                            0x0,
                        ]),
                    ),
                    ("reg", u32s(&[0x0, 0x2, 0x0, 0x1])),
                ],
                vec![
                    node(
                        "dart-disp0",
                        vec![
                            ("compatible", strs(&["dart,t8110"])),
                            (
                                "reg",
                                u32s(&[
                                    0x0040_0000,
                                    0x0,
                                    0x4000,
                                    0x0, //
                                    0x0050_0000,
                                    0x0,
                                    0x8000,
                                    0x0, //
                                    0x0800_0000,
                                    0x1,
                                    0x100,
                                    0x0,
                                ]),
                            ),
                            ("unit-flags", u32s(&[0xdead_beef])),
                            ("u64-prop", 0x1122_3344_5566_7788u64.to_le_bytes().to_vec()),
                        ],
                        vec![],
                    ),
                    node(
                        "apcie",
                        vec![
                            ("#address-cells", u32s(&[1])),
                            ("#size-cells", u32s(&[1])),
                            // 32-bit child window 0x1000_0000+0x10_0000 maps to
                            // the arm-io address 0x0_0300_0000.
                            ("ranges", u32s(&[0x1000_0000, 0x0300_0000, 0x0, 0x10_0000])),
                            ("reg", u32s(&[0x0200_0000, 0x0, 0x1000, 0x0])),
                        ],
                        vec![
                            node(
                                "pcie-bridge0",
                                vec![("reg", u32s(&[0x1000_4000, 0x800]))],
                                vec![],
                            ),
                            // No range covers this address: it is passed through.
                            node(
                                "pcie-bridge1",
                                vec![("reg", u32s(&[0x7000_0000, 0x800]))],
                                vec![],
                            ),
                            // Ends exactly at the end of apcie's window.
                            node(
                                "pcie-bridge2",
                                vec![("reg", u32s(&[0x100f_f800, 0x800]))],
                                vec![],
                            ),
                        ],
                    ),
                    node(
                        "bad-cells",
                        vec![
                            ("#address-cells", u32s(&[3])),
                            ("#size-cells", u32s(&[1])),
                            ("reg", u32s(&[0x0060_0000, 0x0, 0x100, 0x0])),
                        ],
                        vec![node("leaf", vec![("reg", u32s(&[0, 0, 0, 0]))], vec![])],
                    ),
                    node("empty-reg", vec![("reg", vec![])], vec![]),
                ],
            ),
            node(
                "chosen",
                vec![
                    ("boot-args", strs(&["-v"])),
                    ("odd-size", vec![1, 2, 3]),
                    ("after-odd", u32s(&[0x1234_5678])),
                ],
                vec![],
            ),
        ],
    )
}

fn walk_tree() {
    let root = path("/");
    assert_eq!(root, 0);
    assert_eq!(unsafe { parser::adt_check_header(adt) }, 0);
    assert_eq!(name_of(root), "device-tree");
    assert_eq!(unsafe { parser::adt_get_child_count(adt, root) }, 3);
    assert_eq!(unsafe { parser::adt_get_property_count(adt, root) }, 4);

    // The sibling walk skips each node's whole subtree.
    let cpus = unsafe { parser::adt_first_child_offset(adt, root) };
    assert_eq!(name_of(cpus), "cpus");
    let arm_io = unsafe { parser::adt_next_sibling_offset(adt, cpus) };
    assert_eq!(name_of(arm_io), "arm-io");
    let chosen = unsafe { parser::adt_next_sibling_offset(adt, arm_io) };
    assert_eq!(name_of(chosen), "chosen");
    assert_eq!(chosen, path("/chosen"));

    // A leaf has no first child.
    let cpu1 = path("/cpus/cpu1");
    assert_eq!(
        unsafe { parser::adt_first_child_offset(adt, cpu1) },
        NOT_FOUND
    );
    assert_eq!(unsafe { parser::adt_get_child_count(adt, cpu1) }, 0);

    // Property iteration visits every property in order, padding included.
    let mut names = Vec::new();
    let mut off = unsafe { parser::adt_first_property_offset(adt, chosen) };
    for _ in 0..unsafe { parser::adt_get_property_count(adt, chosen) } {
        let mut name: *const c_char = std::ptr::null();
        let mut len = 0u32;
        let v = unsafe { parser::adt_getprop_by_offset(adt, off, &mut name, &mut len) };
        assert!(!v.is_null());
        names.push((
            unsafe { CStr::from_ptr(name) }.to_str().unwrap().to_owned(),
            len,
        ));
        off = unsafe { parser::adt_next_property_offset(adt, off) };
    }
    assert_eq!(
        names,
        vec![
            ("name".to_owned(), 7),
            ("boot-args".to_owned(), 3),
            ("odd-size".to_owned(), 3),
            ("after-odd".to_owned(), 4),
        ]
    );
    let p = unsafe { parser::adt_get_property(adt, chosen, c("after-odd").as_ptr()) };
    assert!(!p.is_null());
    let p = unsafe { parser::adt_get_property_by_offset(adt, (p as usize - adt as usize) as i32) };
    assert!(!p.is_null());
    assert!(unsafe { parser::adt_get_property(adt, chosen, c("missing").as_ptr()) }.is_null());
}

fn lookup_paths() {
    // Leading, doubled and trailing slashes are ignored.
    let dart = path("/arm-io/dart-disp0");
    assert!(dart > 0);
    assert_eq!(path("arm-io/dart-disp0"), dart);
    assert_eq!(path("//arm-io//dart-disp0/"), dart);
    assert_eq!(name_of(dart), "dart-disp0");

    let arm_io = path("/arm-io");
    assert_eq!(subnode(arm_io, "dart-disp0"), dart);
    assert_eq!(subnode(arm_io, "apcie"), path("/arm-io/apcie"));
    assert_eq!(subnode(arm_io, "missing"), NOT_FOUND);
    // Names match whole, not by prefix, in both directions.
    assert_eq!(subnode(arm_io, "dart"), NOT_FOUND);
    assert_eq!(subnode(arm_io, "dart-disp0x"), NOT_FOUND);
    assert_eq!(path("/arm-io/nope"), NOT_FOUND);
    assert_eq!(path("/arm-io/dart-disp0/deeper"), NOT_FOUND);

    // The trace records one offset per path element and ends in zero.
    let (ret, offsets) = trace("/arm-io/apcie/pcie-bridge1");
    assert_eq!(ret, path("/arm-io/apcie/pcie-bridge1"));
    assert_eq!(offsets[0], arm_io);
    assert_eq!(offsets[1], path("/arm-io/apcie"));
    assert_eq!(offsets[2], ret);
    assert_eq!(offsets[3], 0);
    // Nothing past the terminator is written.
    assert_eq!(offsets[4], 0x5a5a_5a5a);

    let (ret, _) = trace("/arm-io/apcie/missing");
    assert_eq!(ret, NOT_FOUND);

    // A path deeper than the eight-entry trace is refused, not overrun.
    let mut deep = node("leaf9", vec![], vec![]);
    for d in (1..=8).rev() {
        deep = node(&format!("n{d}"), vec![], vec![deep]);
    }
    install(&node("device-tree", vec![], vec![deep]));
    let (ret, offsets) = trace("/n1/n2/n3/n4/n5/n6/n7/n8");
    assert!(ret > 0);
    assert!(offsets.iter().all(|&o| o > 0));
    assert_eq!(trace("/n1/n2/n3/n4/n5/n6/n7/n8/leaf9").0, BAD_PATH);
    assert!(path("/n1/n2/n3/n4/n5/n6/n7/n8/leaf9") > 0);
    install(&tree());
}

fn properties() {
    let root = path("/");
    let dart = path("/arm-io/dart-disp0");
    let chosen = path("/chosen");

    assert_eq!(getprop(chosen, "boot-args").unwrap(), b"-v\0");
    assert_eq!(getprop(chosen, "odd-size").unwrap(), [1, 2, 3]);
    // The property after an unaligned one starts on the next 4-byte boundary.
    assert_eq!(
        getprop(chosen, "after-odd").unwrap(),
        0x1234_5678u32.to_le_bytes()
    );
    assert!(getprop(chosen, "missing").is_none());

    let mut word = [0u8; 4];
    assert_eq!(
        unsafe {
            parser::adt_getprop_copy(
                adt,
                dart,
                c("unit-flags").as_ptr(),
                word.as_mut_ptr().cast(),
                4,
            )
        },
        4
    );
    assert_eq!(u32::from_le_bytes(word), 0xdead_beef);
    let mut dword = [0u8; 8];
    assert_eq!(
        unsafe {
            parser::adt_getprop_copy(
                adt,
                dart,
                c("u64-prop").as_ptr(),
                dword.as_mut_ptr().cast(),
                8,
            )
        },
        8
    );
    assert_eq!(u64::from_le_bytes(dword), 0x1122_3344_5566_7788);
    assert_eq!(
        unsafe {
            parser::adt_getprop_copy(
                adt,
                dart,
                c("missing").as_ptr(),
                dword.as_mut_ptr().cast(),
                8,
            )
        },
        NOT_FOUND
    );

    // setprop overwrites in place and only at the property's own size.
    let new = 0x0bad_f00du32.to_le_bytes();
    assert_eq!(
        unsafe {
            parser::adt_setprop(
                adt as *mut c_void,
                dart,
                c("unit-flags").as_ptr(),
                new.as_ptr() as *mut c_void,
                4,
            )
        },
        4
    );
    assert_eq!(getprop(dart, "unit-flags").unwrap(), new);
    let big = [0u8; 8];
    assert_eq!(
        unsafe {
            parser::adt_setprop(
                adt as *mut c_void,
                dart,
                c("unit-flags").as_ptr(),
                big.as_ptr() as *mut c_void,
                8,
            )
        },
        BAD_LENGTH
    );
    let short = [0u8; 2];
    assert_eq!(
        unsafe {
            parser::adt_setprop(
                adt as *mut c_void,
                dart,
                c("unit-flags").as_ptr(),
                short.as_ptr() as *mut c_void,
                2,
            )
        },
        BAD_LENGTH
    );
    assert_eq!(
        unsafe {
            parser::adt_setprop(
                adt as *mut c_void,
                dart,
                c("missing").as_ptr(),
                big.as_ptr() as *mut c_void,
                8,
            )
        },
        NOT_FOUND
    );
    assert_eq!(getprop(dart, "unit-flags").unwrap(), new);
    // The next property is untouched.
    assert_eq!(
        getprop(dart, "u64-prop").unwrap(),
        0x1122_3344_5566_7788u64.to_le_bytes()
    );

    // compatible is a NUL-separated list; every entry matches, prefixes do not.
    let arm_io = path("/arm-io");
    assert!(compat(root, "J000AP"));
    assert!(compat(root, "AppleARM"));
    assert!(!compat(root, "Apple"));
    assert!(!compat(root, "J000APx"));
    assert!(compat(arm_io, "arm-io"));
    assert!(!compat(path("/cpus"), "cpus"));
    assert!(compat_at(arm_io, "arm-io,t0000", 0));
    assert!(compat_at(arm_io, "arm-io", 1));
    assert!(!compat_at(arm_io, "arm-io", 0));
    assert!(!compat_at(arm_io, "arm-io,t0000", 1));
}

fn registers() {
    // Root-level node: 2+2 cells, no translation above it.
    assert_eq!(reg("/arm-io", "reg", 0), Ok((0x2_0000_0000, 0x1_0000_0000)));

    // Each reg entry is selected by index and translated through arm-io.
    assert_eq!(
        reg("/arm-io/dart-disp0", "reg", 0),
        Ok((0x2_0040_0000, 0x4000))
    );
    assert_eq!(
        reg("/arm-io/dart-disp0", "reg", 1),
        Ok((0x2_0050_0000, 0x8000))
    );
    // The second range entry (child 0x1_0000_0000) maps to 0x5_0000_0000.
    assert_eq!(
        reg("/arm-io/dart-disp0", "reg", 2),
        Ok((0x5_0800_0000, 0x100))
    );
    assert_eq!(reg("/arm-io/dart-disp0", "reg", 3), Err(BAD_VALUE));
    assert_eq!(reg("/arm-io/dart-disp0", "missing", 0), Err(NOT_FOUND));
    assert_eq!(reg("/arm-io/empty-reg", "reg", 0), Err(NOT_FOUND));

    // Two levels of ranges: 1+1 cells under apcie, 2+2 under arm-io.
    assert_eq!(reg("/arm-io/apcie", "reg", 0), Ok((0x2_0200_0000, 0x1000)));
    assert_eq!(
        reg("/arm-io/apcie/pcie-bridge0", "reg", 0),
        Ok((0x2_0300_4000, 0x800))
    );
    // Outside apcie's window the address is passed up unchanged, then arm-io
    // still translates it.
    assert_eq!(
        reg("/arm-io/apcie/pcie-bridge1", "reg", 0),
        Ok((0x2_7000_0000, 0x800))
    );

    // A reg that ends exactly at the end of a range window is still inside it.
    assert_eq!(
        reg("/arm-io/apcie/pcie-bridge2", "reg", 0),
        Ok((0x2_030f_f800, 0x800))
    );

    // A 1-cell address with 0 size cells (cpus).
    assert_eq!(reg("/cpus/cpu1", "reg", 0), Ok((0x1, 0)));

    // More than two address cells is refused.
    assert_eq!(reg("/arm-io/bad-cells/leaf", "reg", 0), Err(BAD_NCELLS));
}

fn edge_cases() {
    // An out-of-range compatible index, or a node without compatible, is
    // "not compatible" rather than a panic.
    assert!(!compat_at(path("/arm-io"), "arm-io", 2));
    assert!(!compat_at(path("/cpus"), "cpus", 0));

    // A reg index whose byte offset does not fit in 32 bits is refused instead
    // of wrapping the bounds check onto another entry.
    assert_eq!(
        reg("/arm-io/dart-disp0", "reg", 0x4000_0000),
        Err(BAD_VALUE)
    );
    assert_eq!(
        reg("/arm-io/dart-disp0", "reg", 0x3fff_ffff),
        Err(BAD_VALUE)
    );
    assert_eq!(reg("/arm-io/dart-disp0", "reg", i32::MAX), Err(BAD_VALUE));

    install(&node(
        "device-tree",
        vec![],
        vec![
            node("abcdefg@9", vec![], vec![]),
            node("dcp@0", vec![], vec![]),
            node("dcpext", vec![], vec![]),
            node("last", vec![], vec![node("kid", vec![], vec![])]),
        ],
    ));
    // A unit address in the query must match the node's whole name.
    assert_eq!(path("/zzzzz@1"), NOT_FOUND);
    assert_eq!(path("/dcp@1"), NOT_FOUND);
    assert_eq!(name_of(path("/dcp@0")), "dcp@0");
    // A query without a unit address matches a node name that has one, but
    // never a longer name.
    assert_eq!(name_of(path("/dcp")), "dcp@0");
    assert_eq!(name_of(path("/dcpext")), "dcpext");
    assert_eq!(path("/dcpex"), NOT_FOUND);
    // A miss under the last node of the ADT is NotFound, not a walk past the
    // end of the image.
    assert_eq!(path("/missing"), NOT_FOUND);
    assert_eq!(path("/last/missing"), NOT_FOUND);
    assert_eq!(name_of(path("/last/kid")), "kid");
    install(&tree());
}

fn main() {
    install(&tree());
    walk_tree();
    lookup_paths();
    properties();
    registers();
    edge_cases();
    println!("adt_semantics: ok");
}
