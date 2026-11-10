# J613 native26 external handoff

`chosen.asahi,j613-25g83-dcpext=1` is a separate, default-off switch for
J613 firmware 26.6.2. The existing `asahi,t8122-dcpext` switch retains its
classic internal-marker requirement and does not admit native26.

The native reservation transaction requires the internal mapping handoff,
the exact external image UUID, matching DT/ADT processor and DART registers,
and the ADT DART window. It reads power ancestry before touching the ASC or
DART and refuses an inactive or unknown ancestor. This initial implementation
also refuses an unlocked firmware DART; it never locks or resets hardware.

The public locked-DART walker must account for every non-log firmware page
exactly at its remap address. Inherited root/leaf pages are reserved. Firmware
text and the OSLog buffer may reuse an exact no-map internal reservation; any
other overlap, including writable data and heap, is refused. The physical OSLog buffer is retained without an IOMMU mapping.

Only a completed reservation transaction publishes both
`apple,j613-25g83-dcpext-handoff = <1>` and the existing firmware-ready marker.
The four boot-state cells are powered ancestry, ASC CPU control, ASC CPU status
and firmware-DART protection. All processor nodes stay disabled. The kernel
owns optional gate admission and PMP startup/request ordering.

This handoff does not establish a running RTKit session, a working display,
DMA cessation, hotplug or suspend qualification. Kernel and loader tests and
an owner-attended boot remain distinct checks.
