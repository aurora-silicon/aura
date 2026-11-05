# Speaker tuning identity

Aura exports the Mac's acoustic ID independently of its tuning bundle as `/chosen/asahi,acoustic-id`, one big-endian FDT u32. It reads exactly one ADT node named `audio` carrying `acoustic-id`, a little-endian u32 in the range 1–99999. Missing, malformed, or ambiguous input removes a stale DT property and exports nothing. The tree walk is bounded to 64 levels and 65536 nodes. The existing root `serial-number` export is unchanged.

The installer records this acoustic ID, a canonical model from root `target-type` (`J616sAP` → `j616s`), and `unit_id_sha256`, SHA-256 of root serial-number bytes excluding trailing NUL bytes. The serial is 1–128 printable non-space ASCII bytes, without case conversion. Linux compares the model to its first root compatible, the acoustic ID to this export, and the serial hash to its root serial-number. Missing or mismatched model/acoustic identity refuses production compilation; missing or different unit identity disables SpTS calibration and compiles open loop.

No calibration data, serial hash, or amplifier configuration is exported by this helper. A caller must check its return value: a failed removal or write must not let a stale value authorize tuning. This check is integrity binding, not a signature.

`sh tests/run-kboot-audio.sh` exercises valid, missing, malformed, wrong-endian and ambiguous synthetic inputs alongside the existing amplifier tests, using the host ADT reader and libfdt under ASan/UBSan.
