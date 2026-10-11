# Windows DPI manifest

The previous executable had an embedded manifest containing asInvoker and OS
compatibility declarations, but no DPI awareness. Preserve those declarations
and add PerMonitorV2 with PerMonitor fallback plus legacy true/pm. Embed resource
ID 1, type 24 through the shared Windows resource helper for every emulator.
The manifest dependency ensures edits rebuild the resource. Non-Windows builds
are unchanged. Windows applies the policy before any SDL/native debugger window
is created, preventing Windows DPI bitmap virtualization.

Reference: https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process

Qualification must extract the manifest from the final executable and query
GetWindowDpiAwarenessContext/AreDpiAwarenessContextsEqual for both the hidden
main and debugger windows. This is not proof of mixed-DPI monitor transitions
or optimal font/style sizing; those require a separate manual review.

## Renderer default after manual review

The user accepted software, WARP and Direct3D11 previews, then requested
Direct3D11 as Windows default. Omitted --renderer selects direct3d11 on Windows
and software elsewhere. Explicit --renderer=software / warp remain available.
The existing override disables inherited WARP for ordinary direct3d11 and
prevents inherited OpenGL hints from replacing the selected renderer.
Headless still creates no renderer; no guest clocks or framebuffer change.
