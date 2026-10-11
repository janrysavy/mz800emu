# Enlarged ImGui images disappearing with SDL software rendering

The software UI on SDL 3.4.18 could draw a grey diagonal triangle, or lose the
entire game image after enlargement. Native guest VRAM was unaffected.

## Evidence and upstream source

Our hidden final-pixel reproduction uses the real ImGui backend, an856x600
four-colour texture and foreground debugger overlay.640x480 passes;1280x800,
2128x1328 and3840x2160 lose all four sampled image quadrants and report
`triangle area overflow`. Resizing back to640x480 passes again.

SDL's [software rectangle detection](https://github.com/libsdl-org/SDL/blob/main/src/render/SDL_render.c)
reads UVs with `color_stride` when checking the four vertices' range, although
its later copy uses `uv_stride`. ImGui's backend provides position/UV arrays
with20-byte stride and its converted float colours with16-byte stride.
The [documented API](https://wiki.libsdl.org/SDL3/SDL_RenderGeometryRaw) permits
these independent strides. Misread UVs can defeat rectangle detection; large
textured triangles then hit the [overflow guard](https://github.com/libsdl-org/SDL/blob/main/src/render/software/SDL_triangle.c).
This wrong stride remains in upstream main as inspected on11 October2026.

## Local workaround

Only for the software renderer, convert each draw command's vertices to ordinary
interleaved `SDL_Vertex` records. Supply position, colour and UV pointers with
the same32-byte stride to the existing `SDL_RenderGeometryRaw` call. SDL can
recognize quads and use its own texture-copy implementation. Original indices,
VtxOffset, draw ordering, clipping and arbitrary meshes are retained. Direct3D11
and WARP keep the original backend. No private SDL code or game-image special
path is added. A reused ImVector holds temporary host vertices; guest memory,
clocks and renderer selection are unchanged.

A prototype manually copying six-index image rectangles also passed, but was
replaced with this general layout workaround. Setting CLAMP alone still failed.
A future SDL update fixing the UV stride should permit removing the conversion,
subject to this regression test. This does not fix every conceivable oversized
arbitrary gradient triangle in SDL, and does not diagnose the original GPU freeze.

## Regression target

Build `test_sdl-geometry` with BUILD_TESTING enabled, then run
`ctest --test-dir <build> -R '^sdl-geometry$' --output-on-failure`.
It forces the dummy software display by default; it never initializes OpenGL.
It checks five sizes including4K and resize-back, image quadrants and foreground
overlay, source crop, tint, alpha, clipping, texture-state restoration and a draw
list over65536 vertices. Compile all ImGui sources with `ImDrawIdx=unsigned int`
to exercise the32-bit index configuration separately. The standalone executable
accepts `software`, `direct3d11` or `warp` and an optional BMP output directory.
GPU selections create only hidden windows and require a Windows display.

Do not run the unrelated OpenGL `ui-smoke` target on a host where OpenGL freezes.
These checks establish bounded rendering behavior, not long host stability.
