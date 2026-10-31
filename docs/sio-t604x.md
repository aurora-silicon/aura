# T604x SIO loader handoff

T6040 and T6041 select an explicit protocol-10 table layout. T6030 keeps its
existing layout, and older chips keep the generic layout.

The keyed ADT records populate fixed output slots:

| Parameter | Slots | Bytes |
| --- | --- | ---: |
| `0x1a` map ranges | MINI, AOPA, AOPG, MISC | `0x40` |
| `0x22` DMA shims | SSPI, SUAR, SAUD, SQSP, ADMA, AAUD | `0xd8` |
| `0x1c` device types | dSPI, dUAR, dMCA, dDPA, dPDM, dALE, dAMC, dAPD, dQSP | `0x48` |

Unused payload slots are zero. Each shim appends a 32-bit absent value
`0xffffffff` to its 32-byte ADT payload. Unknown keys, duplicate keys, missing
properties and partial records refuse the handoff. Allocations must fit in
42-bit IOVA space. Parameters remain `(key, IOVA >> 12)` and `(key + 1, size)`.

The loader resolves the `sio` alias, or one unambiguous `apple,t6041-sio` node
when that alias is absent. It first disables that SIO, then reserves firmware
and auxiliary mappings. Only a complete handoff enables the referenced
mailbox, DART, power-domain chain and SIO. An FDT transaction restores the
disabled tree if reservation, parameter publication or provider enablement
fails. Allocator-owned backing memory is retained on refusal, as for existing
SIO setup; it is not reused while firmware ownership is uncertain.

Run the source-only refusal tests on a build host:

```
python3 -m pytest -v tests/python/test_sio_reservation_full.py \
  tests/python/test_sio_t604x.py tests/python/test_sio_t604x_handoff.py
make USE_CLANG=1 -j8 build/sio.o build/kboot.o
```

These changes do not qualify a hardware boot or stream. A candidate must be
checked with the matching Linux driver and a fallback before merge.

Implementation reuses the MIT-licensed m1n1 SIO table builder, its public
T6030 protocol-10 profile and its existing FDT reservation helpers. Only scalar
interface and topology facts are added; no proprietary implementation code
or captured device data is included.
