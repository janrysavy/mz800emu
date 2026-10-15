# Read-only hardware-scroll snapshot

**Historical API, superseded by upstream 2.0.6.** The integration now uses
upstream `get_periph_gdg.hwscroll` with `sof/ssa/sea/sw/enabled` in port units.
Convert SOF by 8 and SSA/SEA/SW by 64 for plane-byte addresses. The old fields
below describe the prior qualified fork; they are no longer exported. Use
`docs/en/mcp-server/resources-overview.md` for the current API. This avoids
keeping a second GDG snapshot implementation.

`get_periph_gdg` adds `has_hw_scroll` and, on MZ-800, `hw_scroll`:

```json
{
  "has_hw_scroll": true,
  "hw_scroll": {
    "ssa": 0,
    "sea": 6720,
    "width": 6720,
    "offset": 8,
    "enabled": true,
    "units": "plane-bytes"
  }
}
```

The example values are illustrative. All values come from actual `g_hwscroll`
fields copied on the emulator thread in the same GDG command as beam/palette
state. Reading does not access an emulated I/O port, advance guest clocks, or
write device/CPU/RAM state. Addresses and sizes are plane bytes, already decoded
by the existing hardware-scroll implementation. `enabled` is the actual cached
device flag. Clients must not infer scroll state from game RAM.

Other platform builds return `has_hw_scroll: false` and omit `hw_scroll`.
Existing GDG response fields and their meaning remain unchanged. This is an
additive snapshot capability, not a new scrolling implementation or a claim of
raster-exact scanout. A paused snapshot describes the device at that boundary.

Long headless gameplay comparisons can map physical VRAM to displayed addresses
using this bounded response. Previously they needed to flush and reparse the
complete growing I/O trace after every update. I/O traces remain useful as an
independent bounded cross-check and for intermediate publication diagnostics.

## Bounded qualification, 15 October 2026

The native conversion project compared independent I/O-trace decoding with this
snapshot over the same 1,773 keyboard updates, five actual offset values and
16 camera changes. Full source crops, native display reconstructions, endpoint
CPU flags/clocks, four complete RAM/VRAM captures and fetched-code tracking
agree. Coverage was started at the same verified normal-game entry in both
lanes; asynchronous loader-start coverage is a different scope. Eleven invalid
host snapshot controls are rejected. SOF=0 correctly reports `enabled: false`
and represents identity mapping. This is API/observer qualification, not full
game campaign, acoustic or physical-hardware proof. Tested implementation:
`e1f4cfcb426dc480a9ab7ccbfd20f73c9d43276b` (subsequent change is documentation).
