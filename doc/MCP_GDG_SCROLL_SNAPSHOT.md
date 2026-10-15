# Read-only hardware-scroll snapshot

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
