#!/usr/bin/env python3
"""Krok a rozběh z pauzy přes MCP pipe bez zbytečného čekání.

Smyčka pauzy (mzzarch_main_do_emulator_paused) po zpracování příkazu
z fronty dbgapi čeká na další příkaz nejvýš 20 ms. Když právě zpracovaný
příkaz pauzu zrušil (run) nebo vyžádal krok (step_into), smí smyčka hned
skončit - jinak se každý krok a každý rozběh odloží o 20 ms, pokud klient
čeká jen na výsledek a nic dalšího neposílá.

Test proto čeká na výsledek přes ``event_poll`` (blokující poll ve vlákně
dispatch, nejde přes frontu emu vlákna, čekání smyčky pauzy neprobudí):

  1. step_into -> událost step_done, 30x,
  2. run s breakpointem na adrese ve smyčce ROM -> událost paused, 30x.

Kontroluje medián doby na jeden cyklus pod LIMIT_MS (před opravou byl
kolem 21 ms, po ní pod 1 ms; limit nechává rezervu pro zatížený stroj).

Binárka: kořen repa nebo proměnná ``MZ_EMU``. Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL.
"""

import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Počet měřených cyklů v každé části.
CYCLES = 30
#: Horní mez mediánu jednoho cyklu [ms].
LIMIT_MS = 10.0


class TestFailure(Exception):
    """Selhání kontroly testu (zpráva je pro uživatele, anglicky)."""


def _find_exe():
    """Najde binárku mz800emu (přednost má proměnná MZ_EMU)."""
    env = os.environ.get("MZ_EMU")
    if env and Path(env).is_file():
        return Path(env)
    for c in _EXE_CANDIDATES:
        if c.is_file():
            return c
    print("ERROR: mz800emu binary not found", file=sys.stderr)
    sys.exit(1)


class Pipe:
    """Emulátor v režimu --mcp-pipe a synchronní volání JSONL příkazů."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, work):
        self.rid = 0
        self.proc = subprocess.Popen(
            [str(exe), "--mcp-pipe", f"--cfg-dir={work}", f"--work-dir={work}",
             "--no-save-ini", "--no-first-run-windows"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            text=True, encoding="utf-8", errors="replace", cwd=str(work))

    def call(self, cmd, data=None):
        """Pošle příkaz a vrátí jeho odpověď (ostatní řádky přeskočí)."""
        self.rid += 1
        msg = {"type": "request", "req_id": self.rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise TestFailure(f"emulator closed the pipe during '{cmd}'")
            line = line.strip()
            if line.startswith("{"):
                m = json.loads(line)
                if m.get("req_id") == self.rid:
                    if not m.get("success"):
                        raise TestFailure(f"'{cmd}' failed: {m}")
                    return m.get("data") or {}

    def wait_event(self, topic):
        """Čeká přes event_poll na událost `topic` (nejvýš 2 s na jeden poll)."""
        while True:
            d = self.call("event_poll", {"timeout_ms": 2000, "max_events": 10})
            if any(e["topic"] == topic for e in d["events"]):
                return
            if not d["events"]:
                raise TestFailure(f"no '{topic}' event within 2 s")


def measure(fn):
    """Spustí `fn` CYCLES-krát, vrátí medián doby jednoho běhu [ms]."""
    times = []
    for _ in range(CYCLES):
        t = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t) * 1000.0)
    return statistics.median(times)


def check(cond, what):
    """Vyhodnotí jednu kontrolu a vypíše PASS/FAIL."""
    print(f"  {'PASS' if cond else 'FAIL'}: {what}")
    if not cond:
        raise TestFailure(what)


def main():
    """Spustí obě měření; 0 = PASS, 1 = FAIL."""
    emu_test_proc.install(deadline_s=50)
    exe = _find_exe()
    work = Path(tempfile.mkdtemp(prefix="mztest-resume-"))
    pipe = None
    try:
        pipe = Pipe(exe, work)
        time.sleep(1.0)
        pipe.call("pause")
        pipe.call("event_subscribe", {"topics": ["step_done", "paused"]})
        pipe.call("event_poll", {"timeout_ms": 0, "max_events": 100})

        def step():
            pipe.call("step_into")
            pipe.wait_event("step_done")

        ms = measure(step)
        check(ms < LIMIT_MS, f"step_into -> step_done median {ms:.2f} ms < {LIMIT_MS} ms")

        pc = pipe.call("get_registers")["PC"]
        pipe.call("bp_add", {"addr": pc})

        def run_to_bp():
            pipe.call("run")
            pipe.wait_event("paused")

        ms = measure(run_to_bp)
        check(ms < LIMIT_MS, f"run -> breakpoint {pc:04X}h -> paused median {ms:.2f} ms < {LIMIT_MS} ms")
        pipe.call("bp_clear")
        pipe.call("shutdown")
        pipe.proc.wait(timeout=20)
    except TestFailure as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        if pipe:
            emu_test_proc.kill_proc(pipe.proc)
        emu_test_proc.rmtree_retry(work)
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
