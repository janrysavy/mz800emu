# Unattended runs (CI without an MCP client)

Recipe for an AI client that prepares a test run for CI or a script:
the emulator in `--headless` ends by itself when a condition is met,
writes `result.json` and returns an exit code. No MCP session is needed.

For the full option reference see the user documentation
`docs/en/mcp-server/headless-mode.md`, section "Unattended runs".

## Recipe

```
mz800emu --headless --speed max --run-mzf test.mzf \
         --exit-on-pc 2000h --fail-on-pc 2100h --timeout-seconds 30 \
         --automation-dir out
echo $?            # 0 pass, 1 fail, 2 timeout, 3 error
```

1. Decide how the program reports the result:
   - it jumps to a "pass" / "fail" address -> `--exit-on-pc ADDR` /
     `--fail-on-pc ADDR` (add `,skip=N` to ignore the first N passes);
   - it writes a result code to a port or memory cell ->
     `--exit-on-out PORT` / `--exit-on-write ADDR` (exit code = the
     written value; `ADDR=VAL` waits for one value);
   - it only runs for some time -> `--exit-after-frames N`;
   - anything else -> a breakpoint set with the action `exit` loaded by
     `--bpt FILE` (condition and action as in `emulator://docs/bp_dsl`
     and `emulator://docs/action_dsl`).
2. Always add a safety timeout (`--timeout-frames` / `--timeout-seconds`,
   emulated time, exit code 2).
3. Add `--speed max` - headless otherwise runs in real time.
4. Read `out/result.json` (or `--result-json FILE`): `reason`,
   `exit_code`, `message`, `trigger`, `time`, `cpu`, `hashes`,
   `artifacts`. `out/screen.png` is the final screen.

## Things to know

- First condition met wins; on the same frame `--exit-after-frames`
  beats the timeout.
- Exit codes: 0 success, 1 test failed, 2 timeout, 3 error of the
  emulator or the options (invalid option, missing `--run-mzf` file), 4
  a played-back replay differs (`--replay FILE --replay-verify`, see
  `emulator://docs/replay`), 64-255 free for program codes. Code 3 for
  invalid options only when an unattended-run option is given (otherwise
  1 as before).
- `result.json` is written also on timeout and errors. After an option
  error it has no `time` / `cpu` / `hashes`.
- `hashes` (SHA-256 of RAM, MemExt, VRAM, PCG, screen and `cpu` =
  registers with the hidden Z80 state) are deterministic: two runs with
  the same options and the same files give the same values, so a script
  can compare them with a reference run.
- Code coverage of the run: `--coverage branch` (or `detail`) collects
  from the start, `result.json` then has `coverage` (status: `complete`,
  `units`, `branches` with `both` / `taken_only` / `not_taken_only`;
  `null` without coverage) and `--coverage-save-on-exit cov.json` (or
  `.csv`) writes all rows - see `emulator://docs/coverage`.
- `trigger.hits` counts the breakpoint hits after the skipped ones
  (`skip=2` -> `hits` 1 on the third pass).
- In an unattended run the saved default breakpoint set (`.bpt`) and
  `$` variables (`.vars`) are neither loaded nor saved: breakpoints come
  only from `--bpt` / `--exit-on-*` / `--fail-on-pc`, leftovers from a GUI
  session cannot change the result.
- `--exit-on-*`, `--fail-on-pc` and `--bpt` need a build with the
  debugger; time conditions and `result.json` work in every build.
- Numbers: decimal, `0x2000` or `2000h`. The format string of an `exit`
  message supports `%X`, `%d` ... without a width (`%04X` is not
  supported).
- Video (`--record`) and audio capture (`--audio-capture`) are finished
  before the process ends, whatever the exit code; their paths are in
  `artifacts`.

## The action `exit` in an MCP session

With `--mcp-pipe`, in the GUI, and in `--headless` with an MCP TCP server
but without an unattended-run option, `exit` (and the time conditions)
only **pause** the emulation:

- `emu_run` returns `stopped_by: "exit"`;
- the `paused` event (topic `paused`, `emu_event_subscribe`) has
  `reason: "exit"`, `exit_code`, `exit_reason`, `message` and `pc`;
- the process keeps running; the client decides what to do (read state,
  `emu_stop` in pipe mode, continue).

`stop ["fmt", args]` in an action stops like a breakpoint with an empty
action (`stopped_by: "breakpoint"`), with an optional message.
