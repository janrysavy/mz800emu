# UI renderer selection

The default main UI renderer is SDL software. Select an explicit alternative:

```
mz800emu --renderer=software
mz800emu --renderer=direct3d11
mz800emu --renderer=warp
mz800emu --renderer=opengl
```

Direct3D11 requires Windows. WARP uses SDL's Direct3D11 CPU rasterizer and
requires SDL3.4+. Creation failures are reported; no alternative renderer is
selected. Explicit CLI settings override inherited `SDL_RENDER_DRIVER` and
`SDL_RENDER_DIRECT3D11_WARP` hints. Startup logs the actual SDL renderer name.
Headless mode continues to create no main window or rendering context.

Screen, image/virtual-keyboard and snapshot-preview textures use pointer-width
backend-neutral IDs. SDL textures have an explicit RGBA32 streaming format to
keep updates consistent, opaque indexed screen pixels and preserved RGBA image
alpha. Screen updates reuse their texture. SDL backends use SDL VSync control.
UI textures and ImGui are destroyed before renderer/video teardown.

## Checks without OpenGL

```
python tests/ui/check_sdl_renderers.py --sdl path/to/SDL3.dll --renderer software --output software.json
python tests/ui/check_sdl_renderers.py --sdl path/to/SDL3.dll --renderer direct3d11 --output d3d11.json
python tests/ui/check_sdl_renderers.py --sdl path/to/SDL3.dll --renderer warp --output warp.json
```

These create hidden windows and verify exact RGB readback. Software uses SDL's
dummy display. They test the SDL device, not the full application or host
stability. The C `video-textures` test exercises production helpers for padded
indexed rows, opacity, in-place RGB updates and RGBA alpha in dummy software
mode. `sdlapp` includes a stale queued-timer regression check.

For bounded full UI checks, `--ui-hidden --ui-exit-after-ms=10000` creates a
hidden window, pumps real ImGui rendering on the main thread at10Hz, and then
requests normal application exit. Hidden OpenGL checks are rejected before
initialization. UI timers use host wall time; guest CPU/device clocks are
unchanged. These flags are for scripted checks, not ordinary play.

The Windows integration has hidden-window boot/input/frame/normal-exit checks
for software, Direct3D11 and WARP. No OpenGL context was launched during their
qualification. This is not proof of long-session stability or a diagnosis of
a reported OpenGL driver/system freeze.
