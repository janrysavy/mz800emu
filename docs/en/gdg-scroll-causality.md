# Mid-line hardware scroll writes and raster causality

The GDG framebuffer is filled incrementally. Before changing the hardware
scroll mapping, flush elapsed pixels using the old SOF/SSA/SEA/SW and enabled
state. Flushing only after changing registers can retroactively repaint the
current scanline with the new offset. Final VRAM screenshots do not expose it.

This correction follows the existing pre-mutation VRAM, palette and border
write pattern. It changes neither guest CPU/device clocks nor pixel-fetch
granularity. The existing shifted-byte latch remains in the renderer.
Direct debugger setters use the same ordering. MZ-700 skips native raster fill.

`test_gdg_scroll_causality` checks actual renderer pixels for low/high SOF,
enable/disable transitions, band registers and direct setters, with unchanged
device timestamps. It is a component check, not a physical GDG latch measurement.

A native MZF reproducer in the native conversion project fills all four planes
with a static colour pattern, waits several complete frames, writes SOF in the
middle of one canvas scanline, and captures the published frame. No host memory
or register writes, forced refresh or mid-line breakpoint is used. Cases cover
SOF 0->1, 1->0, 1->2 and 2->1 with full-screen and 168-line scroll bands, and
with/without a preceding harmless write to the unused VRAM tail. Before the
correction, all eight ordinary writes repaint 128 already-scanned pixels;
the preceding write reduces, but does not eliminate, the wrong prefix. Static
before/final pictures and unaffected rows are correct.

Physical register-latch timing, CRT appearance, interrupts during guest line
counting, and superimpose clock changes remain separate qualification scopes.
