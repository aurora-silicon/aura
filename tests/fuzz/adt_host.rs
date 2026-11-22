// SPDX-License-Identifier: MIT
// aura's ADT reader as a host staticlib for the fuzz targets (tests/fuzz/build.sh).
#![allow(non_camel_case_types, dead_code, unused_imports)]

type c_size_t = usize;

#[path = "../../rust/src/adt.rs"]
mod adt;
