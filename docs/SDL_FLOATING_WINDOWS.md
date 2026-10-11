# Floating debugger windows with the SDL renderer

The software/Direct3D11/WARP frontend previously rendered ImGui panels in the
main game client area. The user reported clipped registers, breakpoints and
memory-map panels. The OpenGL path enabled platform viewports; the SDL path
intentionally did not. [Upstream SDL renderer backend](https://github.com/ocornut/imgui/blob/docking/backends/imgui_impl_sdlrenderer3.cpp)
lacks renderer support for separate platform windows; enabling the flag alone
is insufficient. [SDL textures](https://wiki.libsdl.org/SDL3/SDL_CreateTexture)
belong to a particular renderer.

## Local extension

Enable ImGui platform viewports for the SDL frontend. The user selected one
separate debugger workspace containing all debugger panels. Only this host
uses NoAutoMerge; debugger panels dock inside it and preserve rearrangement
within its dock tree. Registers, memory map and breakpoints start in separate
split regions; other panels use the central region with tabs. Each debugger
Begin routes through one common helper, including secondary browser/disassembly
and hardware inspectors. Closing the host clears its panel requests; reopening
a panel reopens the workspace. The full-screen game stays on the main viewport.
If the main debugger was open, closing the host uses its existing emulator-thread
state recompute request as well, so hidden windows do not leave tracing active.
The SDL frontend enables docking for this host; OpenGL keeps its existing layout.
The existing SDL platform backend owns window creation, movement, resizing,
focus and close events. New renderer callbacks create one explicitly selected
SDL renderer per viewport and destroy textures before their owning renderer.
No OpenGL contexts are created. The application renders platform windows from
its normal UI loop; child close events already distinguish sdlapp-owned main
windows from ImGui windows.

Subtract each draw-data DisplayPos and apply/restore framebuffer scaling. Keep
the software SDL_Vertex stride workaround. Font textures use CPU atlas pixels
when available. Generic images are read back once per source per UI frame from
their owning renderer, with texture modifiers, target, logical presentation,
scale, viewport and clip restored. Each child holds its own texture replica,
updated for the current frame; idle replicas are retired. Mutable images within
one draw frame are not a separately qualified use case. Generic-image readback
is a potential host cost; ordinary debugger panels chiefly use the font atlas.
Guest memory, clocks, device behavior and gameplay are unchanged.

## Qualification

`test_sdl-viewports` (Windows CMake target) keeps every native window hidden.
It checks two independent renderers, font pixels, red-to-green image updates,
movement and enlargement, parent pixels/state, closing one child and cleanup.
Standalone software, Direct3D11 and CPU WARP checks pass. Software geometry
resize-through4K remains passing. No OpenGL execution.

The bounded actual-emulator check `--ui-test-floating-windows` requires
`--ui-hidden` plus `--ui-exit-after-ms`; headless and OpenGL modes are rejected.
It opens the real CPU-register, breakpoint and memory-map widgets, hides all
platform windows and writes the single workspace surface at UI frame20 in cfg-dir.
Normal visible launches do not open debugger panels automatically.

The final grouped feature passes all five focused CMake targets (SDL lifecycle,
production video textures, geometry, native viewports, single workspace), and
actual hidden software/Direct3D11/WARP native boot plus SPACE/N/P/O,16 guest
frames and normal exit0. Each real-widget capture contains exactly one host
with three docked panels. The labelled cadence-v2 Spectrum prefix passes105
endpoints;221 complete paused CPU-panel response hashes match the pre-feature
runtime. This covers bounded boot/input/display, not campaign or hardware proof.

The first real-widget tests exposed a pre-existing startup race: software and
WARP opened Breakpoints before emulator-thread debugger initialization. GDB
resolved the null `g_breakpoints.breakpoints` access in
`bpt_render_events_for_parent`; DbgEngMCP independently captured c0000005 at
`mz800emu+0x2624d`, `mov eax,[rdx+8]` with RDX=0. Direct3D11 happened to pass.
Publish debugger initialization with a GLib atomic flag after initialization
finishes. The breakpoint panel keeps its open request and displays an
initializing message until that flag is set. This also avoids racing the
emulator's `bpt_state_init`. It does not establish general concurrent BP CRUD
or shutdown safety. Failed checks and both debugger traces are retained in
the port workspace before rerunning the same early-panel test.
The first guard build failed because the new readiness declaration was not
included by the panel; add its explicit debugger header and retain that build
failure separately.

Implementation does not prove long UI/host stability or physical hardware.
Interactive multi-monitor, DPI, native drag/focus and menu-dialog behavior need
bounded manual review; automated movement/resize/pixels/close cover named scope.
