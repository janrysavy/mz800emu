# Snapshot restore leaves a stale GDG event and crashes mz800emu

Created by **GPT-6.1 Sol** on 2026-10-09. Human reviewer: **Jan Rysavy**;
review is pending. This report has not been submitted as an upstream issue or PR.

## Summary

Loading a valid snapshot restores the GDG beam position but leaves
`g_gdg.event` from the machine being replaced. Resuming can consequently process
`MZEVENT_GDG_AFTER_LAST_SCREEN_PIXEL` while the beam is in the top border.
The framebuffer then subtracts the canvas origin from an unsigned row number
and reads far beyond the VRAM allocation. On Windows this produces an access
violation, `0xC0000005`.

This reproduces with a tiny, independently written JP/NOP program in documented
320×200 four-colour mode. No game code, sprites, sound samples, ROM payload,
proprietary source or external application code is supplied by the test.
An ordinary emulator installation supplies its own dependencies/firmware;
none are included with this report. Generated snapshots are local test artifacts
and must not be attached to the public report.

## Affected source and environment

- Repository: <https://github.com/michalhucik/mz800emu>.
- Reproduced on a clean build of upstream `95ca032ca789d24dffcd39827da1ef8cbbbe135a`.
- Also reproduced with our earlier native inspection build based on
  `3cbaddc8f1fa6dfb2bbdd90bab3d159c56ab446d`, with and without our callback-clock correction.
- Windows x64; MSYS2 UCRT64 GCC/CMake Release build; MCP pipe transport,
  headless, maximum host speed. Physical guest clocks are unchanged.
- Relevant snapshot and framebuffer source files are identical between those
  upstream revisions. No defect in our renderer is required to reproduce it.

## Minimal reproduction

Use the standalone, MIT-licensed Python script
`tests/snapshot/repro_gdg_event.py` in the fix branch. It uses only the Python
standard library and the emulator's existing JSONL/MCP API.

```powershell
python -X utf8 tests/snapshot/repro_gdg_event.py `
  --exe C:/path/to/mz800emu.exe `
  --output C:/temp/mz800-snapshot-repro-1
```

The output directory must be new. If dependency DLLs are not beside the executable,
add `--dependency-dir C:/path/to/dependencies`. Optional `--cdb C:/path/to/cdb.exe`
captures a host-only exception/register/stack log; it does not create a memory dump.

The script:

1. Starts `--mcp-pipe --no-save-ini --no-first-run-windows` and pauses.
2. Selects documented graphics mode `DMD=00h`, maps RAM for the test and disables
   maskable interrupts. Supplies `JP $3000` at `$3000` and NOP bytes at `$3100`.
3. Saves a native snapshot near the beginning of a frame.
4. Executes only those test bytes until a visible scanline at horizontal tick
   790–793, where the next emulator GDG event is `AFTER_LAST_SCREEN_PIXEL`.
5. Restores the saved snapshot, writes the same documented DMD value, and runs one frame.

The JSON report records every request/response, executable/script SHA-256,
raster positions, clock configuration and spontaneous process exit before cleanup.
No snapshot fields or guest state are repaired to produce the crash.

### Observed and expected

One clean upstream run saved **frame 1, row 0, column 193**, advanced to
**frame 2, row 103, column 793**, then restored **frame 1, row 0, column 193**.
The next bounded `run` lost the connection; the process exited spontaneously
with decimal **3221225477**, Windows **0xC0000005**.

Expected: restoring the snapshot also restores its pending GDG event, so a
one-frame run completes normally. A snapshot without the new event element
should reconstruct the next event from its saved raster rather than retaining
an unrelated event from the current machine.

## Cause and debugger evidence

`src/emulator/snapshot/handlers/snap_gdg.c` serializes `beam_row` and
`total_elapsed`, but originally omitted `g_gdg.event.event_name` and
`g_gdg.event.ticks`. `snap_mzarch.c` restores the separate main scheduler event;
that does not restore the GDG scheduler's pending event.

The stale screen-row event eventually calls:

```text
gdg_process_events
  -> framebuffer_MZ800_current_screen_row_fill
    -> framebuffer_MZ800_screen_row_fill
      -> g_memoryVRAM_I[real_vram_addr]
```

With `beam_row=0` and canvas origin 46, the calculation
`(beam_row - 46) * 40` wraps to **0xfffff8d0**. CDB captured that value in `RDX`
at the failing byte read. The real VRAM plane has 8192 bytes; this index is
4,294,965,456. The separate missing-window message in headless logs is not the
cause demonstrated by this stack.

## Hardware documentation check

This does not depend on an undocumented graphics mode:

- [GDG graphics modes](https://www.ordoz.com/gdg-documented/10-mz800-video-system.html)
  identifies `DMD=00h` as 320×200, four colours, planes I/II, without expanded VRAM.
  `DMD=02h` is also documented but requires expanded VRAM; the crash reproduces
  in both modes.
- [Sharp Service Manual, printed page 20 / PDF page 21](https://www.idealine.info/sharpmz/mz-800/download/sm800.pdf#page=21)
  defines the display-mode register at port CEh and the corresponding bit settings.
- [GDG memory mapping](https://www.ordoz.com/gdg-documented/06-memory-mapping.html)
  describes the RAM mapping operations used by the fixture. Code executes in
  ordinary RAM at `$3000`.
- [GDG video timing](https://www.ordoz.com/gdg-documented/02-video-timing.html)
  and [DMD propagation](https://www.ordoz.com/gdg-documented/19-dmd-propagation.html)
  describe hardware raster behaviour. Native snapshot loading and its host-side
  event queue are emulator operations, not hardware programming requirements.

The numbers 46, 790 and 794 above describe this emulator's existing PAL event
coordinates; they are not proposed changes to hardware timing. The fix changes
saved-state restoration only, with no CPU-clock, palette, WAIT or raster-timing changes.

## Fix and validation

The focused patch saves/restores the pending GDG event for MZ-800 snapshots,
validates its index/raster timestamp, and reconstructs the next event for older
snapshots that lack the optional element. It does not merely suppress the
out-of-bounds read or skip display work.

Initial isolated validation: the reproduction crashes before the fix and
completes after it. New and explicitly constructed legacy-format snapshots
pass at saved rows **0, 45, 46, 245, 246 and 311**, covering the canvas boundaries
and the frame end. The compatibility fixture removes only the new optional
element and recalculates the archive checksum; it is separate from the unmodified
crash reproduction.

Current-upstream fix-build results and a permanent fix-commit link will be added
after that build has been checked. These tests do not certify every snapshot
subsystem, audio waveform parity or real-hardware behaviour.
