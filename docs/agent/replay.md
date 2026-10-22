---
title: Replay - record inputs once, repeat the run exactly
description: How to record a run (keys, joysticks, resets, tape control, debugger writes, the mounted disks and tape) into a .mzr file and play it back exactly in the same or a new process - repeat a bug with other breakpoints, trace or ramlog, go back to a checkpoint while recording, jump to a frame while playing, find the first frame where two runs differ, check a run in CI (--replay-verify, exit code 4).
---
# Replay: record inputs once, repeat the run exactly

Recipe for an AI client that has found a situation worth repeating (a
bug, a crash, a glitch) and wants to repeat exactly the same run with
other observation tools, or to keep it as a regression check.

## What a replay is

A `.mzr` file = a complete start snapshot (always with the RAM disk) +
every input change at a frame boundary + SHA-256 hashes of the state
every N frames. Playback loads the snapshot, applies the same inputs at
the same places and compares the hashes; it stops at the first frame
whose state differs. The emulator core is deterministic from the same
state with the same inputs (also in a new process and at any speed), so
a replay repeats the run instruction for instruction.

Recorded: keyboard matrix (GUI keyboard, virtual keyboard, `emu_input_*`
and `emu_input_send_keys*` together), joysticks (host joystick read once
per frame, numeric keypad, MCP), resets, autotype, tape control
(`emu_cmt_play`, `_stop`, `_pause`, `_eject`, `emu_cmt_tape_seek`,
`emu_cmt_record`, the tape windows), files chosen in the CMT hack dialog,
debugger writes (`emu_mem_write`, `emu_mem_write_force`, region writes,
`emu_set_register`, CPU flags, `emu_io_write`, `emu_irq_inject`,
`emu_nmi_inject`). Stored at the start: contents of all mounted disks
(FDC, FDC1, Quick Disk, IDE8) and the tape with its block and speed
settings, up to 64 MB together (a larger image only as SHA-256: the
playback needs the same image mounted). The Unicard clock runs from the
emulated time. Hashed parts: `cpu` (registers and hidden Z80 state),
`ram`, `memext`, `vram`, `pcg`, `periph` (CTC, PIO 8255, Z80 PIO, PSG,
GDG, VRAM controller, floppy and Quick Disk controllers) and the emulated
time.

Playback works on copies of the media: the program may write to a disk
or record to tape, the original files and the INI do not change, after
the playback the media from the INI are mounted again.

Not recorded: Unicard SD directory (warning), Quick Disk in directory
mode (warning), INI settings. Snapshot load, inserting or ejecting media,
inserting another tape, tape speed and polarity, the CMT hack switch and
settings tools are refused while recording.

## Recipe: record a bug, then investigate it with other tools

1. Bring the machine to the start point (load a program, a snapshot),
   pause at a frame boundary: `emu_run(frames=1)`.
2. `emu_replay_record_start(path="bug.mzr")`.
3. Drive the program: `emu_input_send_keys_with_delays`, `emu_run`,
   writes. Until the bug shows.
4. `emu_replay_record_stop()` - `outcome: "recorded"`.
5. Repeat as often as needed, each time with other tools:
   ```
   emu_replay_play(path="bug.mzr", pause_after_load=true)
   emu_bp_add(...)            # or emu_ramlog_start, emu_trace_start ...
   emu_resume()
   emu_wait_stop()            # a breakpoint, or reason "replay_end"
   ```
   Breakpoints that only observe (`log`, `mark`, trace, ramlog, CDL)
   keep the replay valid. A breakpoint `poke` / `set`, `emu_mem_write`
   or Freeze Bytes during playback mark it `deviated` (it goes on, the
   hashes are no longer checked) - `emu_replay_status` shows it.

## Recipe: go back and try another way (rewind)

Checkpoints are on by default: a snapshot in memory every 250 frames,
the last 60 kept (`checkpoint_interval`, `checkpoint_count` of
`emu_replay_record_start`; 0 = off). `emu_replay_status().checkpoints`
shows `count`, `first_frame`, `last_frame`, `bytes`.

```
emu_replay_record_start(path="try.mzr")
emu_run(frames=600)              # play on ... a wrong move at about 560
emu_replay_rewind()              # back to the checkpoint 500, paused there
emu_replay_rewind()              # once more: 250
emu_replay_rewind(frames=300)    # or: the newest checkpoint >= 300 frames back
emu_input_send_keys_with_delays(...)   # another way from there
emu_replay_record_stop()         # the file holds only the final path, rerecord_count = number of rewinds
```

A rewind past a write to a disk (FDC, Quick Disk, IDE8), a recording to
tape or a tape eject is refused (`a disk or the tape was written or
ejected after frame N`): checkpoints do not hold media contents and the
user files are never overwritten. No checkpoint is taken while autotype
runs or the tape records.

## Recipe: jump to a frame while playing (seek)

```
emu_replay_play(path="bug.mzr", pause_after_load=true)
emu_replay_seek(frame=12000)     # runs at MAX SPEED, seeking: true
emu_wait_stop()                  # reason "replay_seek", paused at frame 12000
emu_bp_add(...); emu_resume()    # investigate from there
emu_replay_seek(frame=11500)     # back: from a checkpoint of this playback
```

A seek back without a usable checkpoint (before the first one, or past
a media write) starts the playback from the beginning with clean media
copies and runs to the frame - always correct, only slower. The hashes
are checked during the seek; breakpoints stay active and may stop it
early (then `seeking` stays true and the seek goes on after
`emu_resume`). `emu_replay_rewind` while playing = a seek to the
previous checkpoint. Checkpoints are not in the `.mzr` file: a new
process collects its own while it plays.

Typical use after a desync: the playback ends at the desync frame N;
play again with `pause_after_load`, `emu_replay_seek(frame=N-50)`, then
step with trace or ramlog to the difference.

## Rules while recording

| Situation | What happens |
|-----------|--------------|
| write while running | taken at the frame boundary, recorded with position `d` |
| write while paused at a frame boundary (after `emu_pause` sent while running, `emu_run(frames=N)`, a deadline, or right after the start) | recorded with position `p` |
| write while paused **between** boundaries (after a step, a breakpoint, `emu_run_until_addr`) | refused: "changes the machine in a pause between frame boundaries" - run to a boundary first (`emu_run(frames=1)`) |
| keys (`emu_input_*`, HID scripts) | taken from the key matrix at each frame boundary; in a pause only at a boundary |
| tape control (`emu_cmt_play`, `_play_paused`, `_stop`, `_pause`, `_eject`, `emu_cmt_tape_seek`, `emu_cmt_record`) | recorded as a `cmt` event like a debugger write (running: `d`, pause at a boundary: `p`, refused between boundaries) |
| `emu_snapshot_load*`, `emu_media_*` (except `emu_media_state`), `emu_cmt_open`, `emu_cmt_set_*` (speed, pulses, polarity, boost), `emu_cmt_hack_set`, block speed, `emu_settings_set`, `emu_platform_set`, `emu_periph_*`, `emu_memmap_set`, `emu_io_read`, `emu_freeze_add` | refused: "is not allowed while recording a replay" |
| reset (`emu_reset`, F12) | taken at the next frame boundary (in a pause at a boundary at once) |
| breakpoint action `poke` / `set` | not recorded; warning in the manifest |

The refusal text comes inside the error of the tool
(`... [MEM_WRITE failed]`). Steps, breakpoints, trace and reads work as
usual while recording.

## Finding where two runs differ

Playback reports the first frame with a hash difference: `emu_wait_stop`
returns `reason: "replay_desync"`, `emu_replay_status` gives
`outcome_frame` and `desync_parts` (`cpu`, `ram`, `memext`, `vram`,
`pcg`, `periph`, `time`). The emulation is paused at that frame boundary - read
memory and registers there. For a finer search record with a smaller
`hash_interval` (down to 1 = every frame).

`inputs.jsonl` inside the `.mzr` (ZIP) is text: removing or moving one
event and playing back shows what that input changed. Lines:
`{"f":frame,"at":"b"|"d"|"p","k":"kbd","m":[10 column bytes]}`,
`"k":"joy","p":port,"s":state`, `"k":"autotype"`, `"k":"reset"`,
`"k":"cmt","op":"play"|"pause"|...,"v":value`, `"k":"cmthack","media":K`,
`"k":"dbg","cmd":"mem_write_checked","args":{...}`, last
`{"f":N,"k":"end"}`.

## CI check without an MCP client

```
mz800emu --headless --replay bug.mzr --replay-verify --result-json result.json
```

Exit code 0 = every hash matched (`reason: "replay_end"`), 4 = the state
differs (`reason: "replay_desync"`, `replay.outcome_frame`,
`replay.desync_parts`), 3 = the replay cannot start (other platform or TV
system, a joystick port or the CMT hack set differently, an image that
is not stored in the replay and differs). `--replay-record
FILE --exit-after-frames N` records a run of the command line. See
`emulator://docs/unattended_runs`.

## Status fields

`emu_replay_status()`: `mode` (`off` / `recording` / `playing`),
`frame`, `frames` (length of the played / last played replay), `events`,
`hashes`, `hash_interval`, `warnings`, `deviated`, `deviation`, `path`,
`outcome` (`none`, `recorded`, `ended`, `desync`, `stopped`, `error`),
`outcome_frame`, `outcome_message`, `desync_parts`, `rerecord_count`,
`seeking`, `seek_target`, `checkpoints` (`interval`, `capacity`,
`count`, `first_frame`, `last_frame`, `bytes`).
