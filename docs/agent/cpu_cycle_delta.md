# Defined per-step cycle deltas across clock boundaries

`z80_step()` previously converted the absolute `uint32_t total_cycles` values
to `int` before subtracting them. A four-cycle NOP starting at `0x7ffffffe`
therefore attempted `-2147483646 - 2147483646`, which exceeds signed `int` on
the supported 32-bit-int targets. Typical builds happened to return four,
but signed overflow is undefined. A GCC `-ftrapv` build confirms the defect.

The fix retains the before-clock and elapsed delta as `uint32_t`, then converts
only the small per-step result to the existing `int` return type. It preserves
the modulo-`2^32` guest clock, timing, register state, and instruction processing.
It does not add a new clock, alter device clocks, or change reset behavior.

`cpu_z80_cycle_delta` is a registered standalone CTest. Its four controls cover
seven starting clocks each, around both `2^31` and `2^32`, using NOP, OUT with
five callback WAIT states, a post-instruction IM1 interrupt, and HALT refresh.
The assertions check returned cycles, the wrapped clock, PC, IRQ stack, and I/O.
They use synthetic fixtures and make no gameplay or hardware certification.

For a regression check that detects the old undefined arithmetic, compile both
the test and CPU with signed-overflow trapping (GCC):

```sh
gcc -std=gnu11 -O0 -ftrapv -Isrc -Itests/framework tests/libs/cpu_z80/test_cycle_delta.c tests/framework/unity.c src/libs/cpu-z80/z80.c -o cycle-delta-test
./cycle-delta-test
```

All 28 cases pass with the fix. The same test linked to parent CPU source
`aa76124967b37e397ef066b438a06bfbf9a17cd2` exits 77 at the first signed overflow;
its SIGABRT handler suppresses native
crash UI for this headless test. An ordinary optimized build can mask this
defect, so its passing assertions alone do not establish defined arithmetic.

GCC 16.2.0 on Windows UCRT64 also passed all six registered CPU test executables
in a Release build, and all 316 enabled/disabled fetched-coverage controls against
that optimized CPU library (including callback and fast RAM paths). These checks
do not substitute for any separately required native replay qualification.
