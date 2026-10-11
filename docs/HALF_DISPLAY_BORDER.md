# Half-size MZ-800 presentation border

Keep the emulated 928x288 raster and 640x200 canvas intact. Crop only the outer
half of each border in UI texture coordinates: source origin 77,23 and 784x244
visible samples. Border strips 154/134/46/42 become 77/67/23/21. The active
320-pixel graphics retain doubled horizontal samples; no VRAM or mode change.

Shared constants drive ImGui texture UVs, fallback SDL blit, window scale and
aspect lock. Default 1.5x UI changes 1392x864 to 1176x732; the active canvas
remains 960x600. Full-raster/native API captures, recordings and guest clocks
retain their original representation. Other architectures retain full borders.
OpenGL uses the same source coordinates but is never launched in qualification.

`test_sdl-border` checks every displayed pixel at 1x and 2x scale against a
coordinate-coloured full-raster source, including every active canvas edge.
Hidden native probes passed on software, Direct3D11 and WARP. The game and
single debugger workspace remain independently resized.

## Superseded by fixed50px request

The later request uses fixed50 physical pixels on all four sides. Nine source
regions retain the complete canvas and full emulated border colour raster,
with framebuffer-scale compensation. Canvas-relative scale/aspect calculations
add100 pixels to each client dimension: default1060x700, canvas960x600.
Other architectures retain full-raster sizing. No guest data or clocks change.

Three-backend probes check every distinct640 sample at integer1x/2x scales and
real320-mode doubled pairs at default1.5x. Retain the earlier fractional640
probe: exact nearest-sampling ties selected different adjacent samples across
backends. The320-mode probe has no such distinct-colour ties and checks exact
3x guest pixels, every canvas edge and all50-pixel border strips.
