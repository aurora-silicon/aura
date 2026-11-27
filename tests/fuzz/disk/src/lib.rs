// SPDX-License-Identifier: MIT
//! aura's disk parsers as they are in rust/src, built for the host over an in-memory NVMe
//! namespace (disk.rs), for the libFuzzer targets in fuzz_targets/.
#![allow(dead_code, non_camel_case_types, unused_imports, unused_macros)]

extern crate alloc;

/// rust/src/lib.rs's c_size_t
pub type c_size_t = usize;

#[path = "../../../../rust/src/print.rs"]
pub mod print;

#[path = "../../../../rust/src/nvme.rs"]
pub mod nvme;

#[path = "../../../../rust/src/gpt.rs"]
pub mod gpt;

#[path = "../../../../rust/src/trial.rs"]
pub mod trial;

#[path = "../../../../rust/src/apfs.rs"]
pub mod apfs;

#[path = "../../../../rust/src/chainload.rs"]
pub mod chainload;

#[path = "../../../../rust/src/secboot.rs"]
pub mod secboot;

pub mod disk;

/// trial.rs's marker states, for the targets' checks
pub const TRIAL_ARMED: &[u8] = trial::ARMED;
pub const TRIAL_SPENT: &[u8] = trial::SPENT;
