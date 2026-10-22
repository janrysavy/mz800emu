# GDG State - video subsystem panel

A separate debug window that shows the current internal state of the
GDG chip (Sharp's custom video LSI). The GDG is the video signal
generator, it drives raster timing, the palette, the border, hardware
scrolling, and on the MZ-1500 also the PCG (Programmable Character
Generator).

The panel is strictly **observational** - it only reads and displays,
it never modifies the running emulation. You can leave it open all
the time.

| Panel | Shortcut | Available on |
|-------|----------|--------------|
| GDG State | Alt+Shift+V | MZ-700, MZ-800, MZ-1500 |

The window's contents differ by architecture - each platform has its
own set of specifics. The **Raster** section at the bottom is shared
and looks the same on all three platforms.


## Activation and persistence

Open the panel via any of these:

- Menu **Debugger -> GDG State**.
- Keyboard shortcut **Alt+Shift+V** (V = Video).
- From DBG Workplace - the **Workplace -> GDG** submenu lets you
  enable the panel to open automatically together with the main
  debugger window. Off by default.

Window position, size and the expanded/collapsed state of sections
are remembered between emulator runs. State polling only runs while
the window is open - a closed window does not load the emulator.


## Common UI elements

- Sections inside the panel are inside collapsing headers - the
  collapsed state is remembered.
- Values are shown in decimal or hex depending on what makes sense
  (registers in hex, counters in decimal).
- Clicking a colour swatch in the palette opens a popup with the
  index and the RGB value.
- A click never modifies any value - the window is read-only.


## Sections on the MZ-800

### Video mode

The current graphics mode decoded from the DMD register (bits D2-D0)
and the MZ-700 compat flag (bit D3).

| Value | Meaning |
|-------|---------|
| 320x200 4-color VBANK A | 320x200 graphics, 4 colours, VRAM bank A |
| 320x200 4-color VBANK B | 320x200 graphics, 4 colours, VRAM bank B |
| 320x200 16-color | 16-colour graphics (4-plane VRAM) |
| 640x200 2-color VBANK A | 640x200 monochrome, VRAM bank A |
| 640x200 2-color VBANK B | 640x200 monochrome, VRAM bank B |
| 640x200 4-color | 640x200, 4 colours |
| MZ-700 compat (text 40x25) | MZ-700 compatibility text mode |

Below the decoded mode the panel also shows the raw DMD register
value and a breakdown of its bits (MZ700, SCRW640, HICOLOR, VBANK).

### RF / WF

The state of the VRAM controller for Read Format (RF) and Write
Format (WF). It controls how the CPU accesses the 4-plane VRAM
organization in the 16-colour mode.

| Field | Meaning |
|-------|---------|
| `WF plane` | Mask of planes to write to (I / II / III / IV) |
| `WF mode` | Write operation (SINGLE / EXOR / OR / RESET / REPLACE / PSET) |
| `RF plane` | Mask of planes to read from |
| `RF search` | Mask used for the bit-pattern search |
| `VBANK (WF/RF)` | The VRAM bank currently active for CPU access |
| `WRITE FF` | `set` until the GDG has written the last CPU write from its buffer into the VRAM; until then the next CPU access to the VRAM waits (MZ-800 and MZ-700 mode) |

### HW scroll

Hardware vertical scrolling of the displayed area.

| Field | Meaning |
|-------|---------|
| `Enabled` | Whether HW scroll is currently active |
| `SSA` | Scroll Start Address |
| `SEA` | Scroll End Address |
| `SW` | Scroll Width |
| `SOF` | Scroll Offset |

### CG-RAM (MZ-700 mode)

Informational section. CG-RAM (user-defined characters for MZ-700
text mode) lives in the first 4 KB of VRAM plane I. The data itself
is not shown here - that belongs in the **Memory Map** window. The
panel only tells you whether the MZ-700 compatibility mode is
currently active.

### Palette

16 IGRB colour swatches in a 4x4 grid. They represent all 16
physical colours of the MZ-800 as you see them on screen with the
current colour scheme (Normal / Grayscale / Green).

Clicking a swatch opens a popup with the index (0-15) and the RGB
value. Below the grid the panel also shows the raw palette register
values PALGRP and PAL0..3.

### Border

The colour of the border (the frame around the active area)
according to the BOR register. The index 0-15 and a small
ColorButton preview are shown next to each other.


## Sections on the MZ-700

The MZ-700 GDG has no platform-specific registers worth showing
here - no palette (only 8 fixed IGRB colours per cell through the
attribute RAM), no HW scroll, no border port. The attribute RAM and
the character generator are not part of the GDG.

For the MZ-700, the panel only shows the shared **Raster** section
at the bottom.


## Sections on the MZ-1500

### Mode & Priority

The current GDG mode and the layer priority for the PCG overlay.

| Field | Meaning |
|-------|---------|
| `DMD` | Raw value of the Display Mode Descriptor register |
| `Mode` | MZ-700 compat (PCG off) or MZ-1500 PCG overlay active |
| `Layer priority` | BPF (background -> PCG -> foreground) or BFP (background -> foreground -> PCG) |

### PCG bank mapping

The mapping of PCG banks into the CPU address space (D000-EFFF).
The PCG has three 8 KB banks; at any given moment only one of them
is visible to the CPU (through SPEC mapping). The GDG itself always
reads all three banks for rendering - the mapping only affects CPU
access.

| Field | Meaning |
|-------|---------|
| `CPU view of D000-EFFF` | What is currently mapped (PCG bank 1/2/3, CGROM, or VRAM/RAM) |
| `SPEC id` | Raw value of the SPEC field |
| `PCG bank size` | Size of one PCG bank (bytes) |
| `PCG bank count` | Number of PCG banks |

### PCG usage

Number of character cells that have the PCG overlay bit set in the
attribute RAM. A useful indicator of whether the application uses
PCG at all.

- `0 / 1024` = the application uses only CGROM characters.
- `> 0 / 1024` with the PCG mode active = the PCG overlay is
  actually being used.
- `> 0 / 1024` in MZ-700 compat mode = the PCG bits in attributes
  are set, but the GDG ignores them (overlay turned off by the DMD
  bit).

### PCG palette

8 IGRB colour swatches in a single row. They represent the PCG
overlay palette (port 0xF1). Index 0 means "PCG pixel is
transparent" in the renderer - the colour underneath (background or
foreground layer) remains visible. Indices 1-7 are regular colours.

Clicking a swatch opens a popup with the index, the IGRB code and
the RGB value.


## Shared section - Raster

This section is the same on all three architectures. It shows the
current raster state and the GDG tempo generator.

### Beam position and frame counter

| Field | Meaning |
|-------|---------|
| `Scanline` | Current beam row (0 to the full frame line count) |
| `Pixel clock` | Position within the current row (clock ticks since the start of the frame) |
| `VBLN count` | Frames since the emulator started / snapshot load (frame counter; a machine reset does not restart it) |

### Blanking and sync flags

| Field | Meaning |
|-------|---------|
| `HBLN flag` | Horizontal blanking - 1 = outside the visible part of the line |
| `VBLN flag` | Vertical blanking - 1 = outside the visible part of the frame |
| `HSYNC` | Current state of the horizontal sync signal |
| `VSYNC` | Current state of the vertical sync signal |

### Tempo signal

The GDG generates a slow "tempo" signal derived from the master
clock through a divider of 229. It is used as the source of the
periodic interrupt (typically 50 / 60 Hz depending on PAL / NTSC).

| Field | Meaning |
|-------|---------|
| `Tempo` | Current state of the tempo bit (0 / 1) |
| `Tempo divider` | Live counter of the divider, 0..228 |
| `CTC0 divider` | How many master GDG clock ticks equal one CTC0 tick (depends on the platform and PAL/NTSC) |


## Tips

### Tracking raster effects

If you are debugging a program that switches the palette or border
colour in the middle of a frame, watch the `Scanline` value in the
Raster section. Together with the **CTC State** panel (channel CTC2)
it lets you quickly determine on which line the interrupt fires.

### Silent screen

If the emulator draws nothing at all, look at `VBLN count` and
`Scanline` in the GDG State window. If `VBLN count` does not grow,
the GDG is not running (typically a problem in the emulator thread).
If `Scanline` stays frozen at the same value, raster timing has
failed somewhere.

### MZ-1500 and PCG

If you see a black screen on the MZ-1500 where you expected PCG
graphics, check in the **PCG usage** section how many cells have the
PCG bit set. If it is `0 / 1024`, the application never produced any
PCG cells. If it is > 0 and you still see nothing, look in **Mode &
Priority** whether the DMD bit for PCG mode is enabled - without it
the GDG ignores the PCG overlay.


## Relation to other windows

| What you need | Where to find it |
|---------------|------------------|
| GDG internal state, palette, border, PCG | **GDG State** (this window) |
| Mapping of VRAM / ROM / RAM into the CPU address space | **Memory Map** |
| Raw data of VRAM, CGROM, PCG banks | **Memory Map** (regions) |
| Per-line timing measurement (frame analysis) | **Measuring GDG** (different window, different purpose) |
| Periodic interrupt derived from the GDG tempo signal | **CTC State** (channel CTC2) |
