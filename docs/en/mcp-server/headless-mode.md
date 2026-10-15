# Headless mode

The emulator can be started in **headless mode** via the CLI option
`--headless`. In this mode no graphics window is created and no audio
device is opened, but the emulation loop runs normally.

## Usage

```
mz800emu --headless
mz800emu --headless --run-mzf <path-to-program.mzf>
mz800emu --headless --no-first-run-windows
```

The process keeps running until externally terminated (`Ctrl+C` on
Linux, `taskkill /F /IM mz800emu.exe` on Windows, or an SDL quit
event).

## When to use it

- **CI / batch test runner** - automated tests without requiring a
  display / X server / audio device
- **MCP subprocess** - the MCP client (Claude Code, Claude Desktop)
  spawns the emulator as a child process communicating over a named
  pipe
- **Headless benchmark** - performance measurement without GUI render
  overhead

## What is available in headless mode

| Component | State |
|-----------|-------|
| Z80 CPU emulation | runs normally |
| Peripherals (GDG, CTC, PPI, PSG, FDC, ...) | run normally |
| Framebuffer (in memory, BGRA 928×288) | rendered |
| Snapshot save/load | works |
| CMT hack (`--run-mzf`) | works |
| CMT hack load started by the ROM (boot menu C, `LOAD`) | **no** file dialog: the load is cancelled at once (the ROM reports Break) and a warning goes to stderr. Load the program with `media_load_mzf` or `--run-mzf`, or switch the CMT hack off (`cmt_hack_set`) and use the virtual tape |
| Console stdout/stderr | only in `FORCE_CONSOLE=1` builds (see below) |
| GUI window | **no** (skipped) |
| Audio output | **no** (no-op SDL audio device) |
| Emulation pace | system clock: 100 % = real time (MZ-800: 50 frames/s), custom speed and MAX SPEED as in windowed mode |
| ImGui debugger | **no** (context not created) |

Headless emulation therefore runs in real time. To run faster (tests, batches), set the speed at start with `--speed` (`--speed max` = MAX SPEED, `--speed 400` = four times real time; valid range 1-4000 %), or change it at runtime with the MCP `set_speed` tool. The `--speed` option applies to `mz800emu --headless`; with `--mcp-pipe` it is rejected (`Unknown option: --speed`, startup fails); in pipe mode set the speed with `set_speed`.

## Console-enabled build (Windows)

The standard `mz800emu.exe` on Windows is linked as a GUI subsystem =
stdout / stderr are not visible in a pipe. For headless workflow where
you need to see logs:

```
mingw32-make mz800emu FORCE_CONSOLE=1
```

(Equivalent to `cmake -DMZ_FORCE_CONSOLE=ON`.) A `FORCE_CONSOLE=1`
build allocates a console at startup and stdout / stderr are visible
in the terminal or can be redirected to a file via a shell pipe.

The default remains the GUI subsystem (for end users with the Windows
Explorer icon experience).

## Known limitations

- **Window error log at startup** - 2-3 lines of `getSDL_Window_by_name():
  Failed to get window: main_window` - **benign**, callers have a
  `NULL` window guard and continue without error. The message can be
  safely ignored.

## Screenshot in headless mode

Resource `emulator://frame/screenshot.raw` works **also in headless
mode** (= `--mcp-pipe`, `--headless`). The handler
primarily reads the SDL display-ready snapshot
(`g_iface_video->fbsnapshot_pixels`); if NULL, it falls back to the
GDG live buffer (`g_framebuffer.pixels`, statically allocated BSS).

The response contains a `fallback_source` string field:

| `fallback_source` | Meaning |
|-------------------|---------|
| `"sdl_snapshot"` | Primary SDL display path (= typical in GUI mode) |
| `"gdg_live"` | Fallback to GDG live buffer (= typical in headless / race condition) |

### Python client example (raw RGBA → PIL Image)

```python
import base64
from PIL import Image

resp = emu.call('get_frame_screenshot_raw')
data = resp['data']
if not data.get('available'):
    raise RuntimeError(f"Screenshot unavailable: {data.get('reason')}")
print(f"Source: {data['fallback_source']}, "
      f"frame {data['source_screen_id']}")

raw = base64.b64decode(data['data_b64'])
img = Image.frombytes('RGBA', (data['width'], data['height']), raw)
img.save('mz_screen.png')
```

For INDEX8 processing (= without auto expand into RGBA) the
Resource `emulator://frame/framebuffer/info` exposes a 16-entry
palette; the raw pixels would have to be obtained via a different
Tool (= not via screenshot.raw, which performs the RGBA expand on
the backend).

### PNG to a file with area, scale and crop

`emu_screenshot_save_to_file` (command `screenshot_save_to_file`)
writes a PNG straight to a file on the server host. Optional parameters
select what is saved:

| Parameter | Values | Meaning |
|-----------|--------|---------|
| `area` | `full` (default), `canvas` | whole frame with the border, or the picture only |
| `scale` | `framebuffer` (default), `native`, `1` to `4` | see below |
| `crop` | `{"x", "y", "width", "height"}` | cut-out in output pixels, only with `framebuffer` or `native` |

- `framebuffer` - framebuffer pixels 1:1, one pixel per GDG clock
  horizontally and per picture line vertically (MZ-800 full frame
  928x288, canvas 640x200). This is the behaviour without parameters.
- `native` - one image pixel per dot of the computer. When all pairs of
  neighbouring framebuffer pixels in the area are equal (320-dot modes),
  the width is halved (MZ-800 canvas 320x200), otherwise it is kept
  (640x200); lines 1:1. The reply contains `hires`.
- `1` to `4` - the aspect ratio of the emulator window; `2` equals the
  window at 100 % (MZ-800 full frame 928x576). Scale `1` drops every
  second column in the 640-dot modes.

Example - the 256x192 middle of a 320x200 picture:

```python
emu_screenshot_save_to_file(path="C:/tmp/viewport.png", area="canvas",
                            scale="native",
                            crop={"x": 32, "y": 4, "width": 256, "height": 192})
```

An unknown value or a crop outside the image is an error, no file is
written.

### Frame series - every changed frame as PNG

For finding animation bugs, `emu_screenshot_series_start` saves a PNG of
every emulated frame that differs from the last saved one (commands
`screenshot_series_start`, `screenshot_series_stop`,
`screenshot_series_status`).

- Frames are taken in emulated time at the end of each frame, so no frame
  is skipped even at MAX SPEED. When writing cannot keep up, the emulation
  waits. The series only captures while the emulation runs.
- `area`, `scale` and `crop` work as with a single screenshot (default
  scale `native`). Only changes inside them count - with a crop you can
  follow one sprite and ignore the rest of the screen.
- Files: `frame_NNNNNNN.png`, where NNNNNNN is the emulated frame since
  the start of the series (gaps = unchanged frames), and `index.csv` with
  `frame,file,gdg_screen`.
- `only_changed` (default true), `frames` (stop after N emulated frames,
  0 = until stop), `max_files` (safety limit, default 10000), `dir`
  (default: a new folder in the screenshot output folder).

```python
emu_screenshot_series_start(area="canvas", frames=250)   # 5 s of PAL frames
emu_run(frames=250)
emu_screenshot_series_status()   # files, unchanged, dir, stop_reason
```

`emu_screenshot_series_stop(wait=True)` ends a series early and waits until
all frames are written.

### Known limitations

- **Race condition in GUI mode** - if the client requests
  screenshots very often (= at a frequency close to the 50 Hz
  frame rate), the handler may occasionally return
  `fallback_source: "gdg_live"` instead of `"sdl_snapshot"` in
  GUI mode (= the SDL render thread consumed between publish
  windows). The content is byte-identical, so clients typically
  do not need to special-case this.
- **Pre-emu screenshot** - before the first `frame_done` (= just
  after reset / power-on), the screenshot may return a zeroed
  buffer with `source_screen_id: 0`. Clients can detect this case
  via the `source_screen_id == 0` test.

## Related

- [MCP server overview](README.md)
