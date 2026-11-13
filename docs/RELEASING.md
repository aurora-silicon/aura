# Releasing Aura

Aura has its own version series, starting at `v0.1.0`. Keep feature work in the internal repository separate from the public release branch.

1. Update `docs/releases/<version>.md` with changes, known limits, and evidence for any hardware qualification.
2. Commit coherent changes to public `main`. Require green build, format-check, test, and release artifact jobs for that exact source revision.
3. Create an annotated `v<version>` tag on that revision and push it to `aurora-silicon/aura`.
4. The release workflow builds both stages, packages binaries and matching debug ELF files with licenses and source/build manifests, checks SHA-256 sums, and creates a **draft** GitHub release.
5. Review/edit the draft notes and qualify the exact artifacts on the intended hardware before publishing. The workflow does not publish drafts and refuses to alter an already published release.

6. Pin the tag downstream, all in the same release:
   - `sources.json` in fedora-aurora, alarm-aurora and nixos-aurora: commit, version and tarball hash.
   - aurora-installer: move the `aura` submodule to the tag, and record the raw Stage 1 size for the tagged commit under `pinned_stage1_bytes` in `macOS/test/conformance/fixtures/constraints.json`. Take the size and SHA-256 of `/usr/share/aura/stage1/aura-raw.bin` from the packaged build. The installer's conformance matrix compares this pin with the distribution pins and fails every cell while they differ.
   - The signed installer catalogs, once the distributions' testing feeds carry the release: build the images, publish their payloads and adopt the set in aurora-distributions (see its `docs/RELEASING.md`). Check that every payload's Stage 1 and Stage 2 carry the release's version marker.

Main-branch builds produce Actions artifacts but do not create GitHub releases. A manual dispatch also builds artifacts without publishing or creating a release. Re-running a tag build may replace assets on its existing draft; it preserves edited release notes.

Local packaging uses `tools/package_release.py` after a clean-source `RELEASE=1 CHAINLOADING=1` build, with `ESP_STAGE2=0` for `stage1` or `ESP_STAGE2=1` for `stage2`. Set `AURA_VERSION_TAG` to the release tag and pass the same value to `--version`. Use `--compiler` to record the actual compiler when cross-compiling.

The release archives contain Aura components. A complete UEFI boot bundle additionally needs board-matching DTBs and a matching U-Boot payload; those versions and their hashes must be recorded separately. No deployment or firmware installation is performed by release CI.
