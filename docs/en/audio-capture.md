# Audio capture

Records what the sound hardware of the emulated computer does into a folder,
in emulated time - for finding sound bugs of a program or of the emulator.
All platforms (MZ-800, MZ-700 PAL and NTSC, MZ-1500); the MZ-700 has only the
speaker.

## Starting and stopping

| Way | How |
|-----|-----|
| Menu | **Tools -> Audio Capture -> Record Audio Capture** (`Alt + Q`) starts the capture; the same item or key stops it. |
| Command line | `--audio-capture <folder>` starts it right after startup, `--audio-capture-frames <count>` stops it after the given number of frames; with `--headless` the emulator then exits. |
| MCP | `emu_audio_capture_start`, `emu_audio_capture_stop`, `emu_audio_capture_status` (see [`mcp-server/tools-overview.md`](mcp-server/tools-overview.md)). |

From the menu each capture creates a new folder `<platform>_audio_YYYYMMDD_HHMMSS`
in the output folder (default: `audio_captures` next to the emulator; MCP
without `dir` uses it too). While it runs, the top left corner of the picture
shows **AUDIO** and the captured time; when it ends, a notification shows the
folder. **Open Output Folder** opens the output folder in the file manager.

The capture starts at the end of the next frame; when `sdl.wav` is written,
after six more frames (about 0.1 s) that let the audio output settle. It
ends with stop, after the given number of frames, after one hour, or when
the emulated time starts again (reset, loading a snapshot).

## Settings

**Tools -> Audio Capture -> Settings...** sets the output folder and selects
the files to write; saved in the configuration (section `[AUDIO_CAPTURE]`).
The command line uses the same file selection.

| Setting | INI key | Default |
|---------|---------|---------|
| Output folder | `output_dir` | empty = `audio_captures` next to the emulator |

| File | INI key | Content (all on by default) |
|------|---------|---------|
| `events.csv` | `events` | every change of the output level of a sound channel |
| `psg_writes.csv` | `psg_writes` | every write to the PSG, decoded |
| `reference.wav` | `reference_wav` | the sound computed exactly in emulated time |
| `sdl.wav` | `sdl_wav` | the sound of the audio output - what you hear |

`capture.json` with the description of the capture is always written.

## Files

Times in the CSV files: `ticks` = clock ticks of the video chip since the
start of the capture (the frequency is `gdg_clk_hz` in capture.json),
`time_us` = the same in microseconds, `frame` = the frame since the start
(20 ms PAL, 16.7 ms NTSC).

**events.csv** - `ticks,time_us,frame,channel,level`. Channels: `ctc0` (the
speaker, level 0 or 15), `psg0_0` .. `psg0_3` (PSG, channel 3 = noise) and on
the MZ-1500 or an MZ-800 with the second PSG `psg1_0` .. `psg1_3`. Level 0-15
is the output of the channel (for the PSG 15 minus the attenuation while the
generator output is high, otherwise 0). The first rows (`ticks` 0) are the
levels at the start.

**psg_writes.csv** - `ticks,time_us,frame,psg,byte,channel,reg,value`:
`psg` = target (1 = PSG0, 2 = PSG1, 3 = both), `byte` = written value (hex),
`channel` 0-3, `reg` and `value` from the state of the PSG after the write:

| reg | value |
|-----|-------|
| `tone_lo` | latch byte of a tone: the low 4 bits of the divider (the divider changes only with the next data byte) |
| `tone` | the complete 10-bit divider |
| `volume` | attenuation 0-15 (15 = off) |
| `noise` | `(type << 2) \| rate` |

**reference.wav** and **sdl.wav** - 44100 Hz, 16 bit, stereo, the same
format, so they can be compared directly. reference.wav is computed exactly
in emulated time; sdl.wav is what the emulator's audio output produced. Their
difference shows defects of the audio output. sdl.wav comes later: without
an audio device (`--headless`) by exactly one frame, with a device by its
buffering. With an audio device at MAX SPEED the audio output skips frames -
capture.json then has `sdl_blocks` lower than `frames`.

**capture.json** - platform, `gdg_clk_hz`, `frames_per_sec`, `wav_rate`,
channel names, counts (`frames`, `events`, `psg_writes`, `reference_samples`,
`sdl_blocks`, `sdl_samples`) and `stop_reason`.

## Example

```
mz800emu --headless --run-mzf game.mzf --audio-capture C:/tmp/snd --audio-capture-frames 500
```

Captures 10 seconds of the game (PAL) into `C:/tmp/snd` and exits.
