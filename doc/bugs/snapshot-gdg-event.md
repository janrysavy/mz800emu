# Snapshot restore leaves a stale GDG event and crashes mz800emu

Loading a snapshot restores the GDG raster position but leaves the pending `g_gdg.event` from the machine being replaced. Resuming can process a screen-row event in the top border, causing an out-of-bounds VRAM read and a host access violation (`0xC0000005`).

Reproduced on clean upstream [`95ca032ca789d24dffcd39827da1ef8cbbbe135a`](https://github.com/michalhucik/mz800emu/tree/95ca032ca789d24dffcd39827da1ef8cbbbe135a): Windows x64, MSYS2 UCRT64 GCC/CMake Release build, headless MCP pipe transport.

## Reproduction

Save the standalone, MIT-licensed [repro_gdg_event.py](../../tests/snapshot/repro_gdg_event.py) and run it against an unmodified upstream executable. It needs only Python's standard library and the emulator's existing JSONL/MCP API.

```powershell
python -X utf8 repro_gdg_event.py `
  --exe C:/path/to/mz800emu.exe `
  --output C:/temp/mz800-snapshot-repro-1
```

Use a new output directory. If DLLs are elsewhere, add `--dependency-dir C:/path/to/dependencies`.

The script supplies an independently written JP/NOP program in ordinary RAM, with interrupts disabled and `DMD=00h` (320×200, four colours). It:

1. Saves a native snapshot near the start of a frame.
2. Runs to a visible scanline with `AFTER_LAST_SCREEN_PIXEL` pending.
3. Restores the snapshot, writes the same DMD value and runs one frame.

In the recorded failing run, the snapshot was saved at **frame 1, row 0, column 193**, after which execution reached **frame 2, row 103, column 793**. Restoring returned to the saved raster position, but the next run disconnected and the process exited with **3221225477 / 0xC0000005**.

**Expected:** the restored machine runs normally with the pending GDG event appropriate to its saved raster position.

The script supplies no game or ROM payload and does not edit the snapshot in the crash reproduction. Generated snapshot archives may contain installed firmware and should not be attached to the issue.

## Cause

`snap_gdg.c` saves `beam_row` and `total_elapsed` but omits the pending GDG event name and timestamp. The event restored by `snap_mzarch.c` belongs to the separate main scheduler.

Source inspection traces the stale `MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL` through `gdg_process_events` to `framebuffer_MZ800_current_screen_row_fill` and `framebuffer_MZ800_screen_row_fill`. At row 0, `(beam_row - 46) * 40` wraps to **0xfffff8d0**, an invalid index into an 8192-byte VRAM plane.

CDB independently confirms an access violation at `movzx esi,byte ptr [rbx+rdx]` with **RDX=00000000fffff8d0**. Its Release-build stack contains module offsets; the function names above come from source inspection.

## Fix and checks

The fix in this commit saves, validates and restores the pending event. For older snapshots without the optional element, it reconstructs the next event from the saved raster. It does not change CPU clocks, palettes, WAIT behaviour or raster timing.

The focused fixed build passes **13 cases**: new and synthetic legacy-format snapshots at rows **0, 45, 46, 245, 246 and 311**, plus one `DMD=02h` case. The legacy fixture removes only the optional event element and updates the archive checksum. All **five existing snapshot CTest suites** also pass.

The fixture's display mode is documented: [GDG graphics modes](https://www.ordoz.com/gdg-documented/10-mz800-video-system.html) lists `DMD=00h` as 320×200, four colours without expanded VRAM; the [Sharp Service Manual, printed page 20 / PDF page 21](https://www.idealine.info/sharpmz/mz-800/download/sm800.pdf#page=21) defines the display-mode register at CEh. Snapshot event restoration itself is an emulator operation.

Created by **GPT-6.1 Sol**, 2026-10-09. Reviewed by **Jan Rysavy**.
