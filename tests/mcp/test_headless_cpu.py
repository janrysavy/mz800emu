#!/usr/bin/env python3
"""Hlavní vlákno v režimu --headless se netočí naprázdno.

Bez okna nemá hlavní smyčka sdlapp nic, co by ji brzdilo (vsync), a ve
výchozím režimu SDLAPP_LOOP_POLL zabrala celé jedno jádro (5,3 s CPU za
5 s). V headless proto běží v režimu SDLAPP_LOOP_WAIT_TIMEOUT.

Test spustí emulátor s ``--headless`` bez programu (běží ROM rychlostí
skutečného stroje) a bez klienta, počká na rozběh a změří spotřebu CPU
celého procesu za MEASURE_S sekund. Emulace sama potřebuje jen zlomek
jádra; limit LIMIT_RATIO nechává rezervu pro pomalý stroj.

Spotřebu CPU čte na Windows přes GetProcessTimes, na Linuxu z /proc;
jinde se test přeskočí (návratový kód 77).

MCP TCP port: volný od systému (23800 je vyhrazen pro běžné instance).
Binárka: kořen repa nebo proměnná ``MZ_EMU``. Výstup anglicky.
Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty

_TESTS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _TESTS_DIR.parent.parent
_EXE_CANDIDATES = [_REPO_ROOT / "mz800emu.exe", _REPO_ROOT / "mz800emu"]

#: Návratový kód pro přeskočení (SKIP_RETURN_CODE v CMakeLists).
SKIP_EXIT = 77
#: Doba rozběhu před měřením [s]; první asi 3 s po startu bere proces víc CPU.
WARMUP_S = 4.0
#: Doba měření [s].
MEASURE_S = 3.0
#: Nejvyšší dovolený podíl CPU času procesu na uplynulém čase.
LIMIT_RATIO = 0.5


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


def _cpu_seconds(proc):
    """Součet uživatelského a systémového CPU času procesu [s], nebo None.

    @param proc Běžící ``subprocess.Popen``.
    @return CPU čas, None když platforma neumí (pak se test přeskočí).
    """
    if sys.platform == "win32":
        import ctypes
        from ctypes import wintypes
        ft = [wintypes.FILETIME() for _ in range(4)]
        ok = ctypes.windll.kernel32.GetProcessTimes(
            wintypes.HANDLE(int(proc._handle)), *[ctypes.byref(f) for f in ft])
        if not ok:
            return None
        # FILETIME je v jednotkách 100 ns; ft[2] = kernel, ft[3] = user.
        return sum((f.dwHighDateTime << 32 | f.dwLowDateTime) for f in ft[2:]) / 1e7
    stat = Path(f"/proc/{proc.pid}/stat")
    if stat.is_file():
        # Pole za názvem procesu (v závorkách): utime a stime jsou 14. a 15. pole.
        fields = stat.read_text().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    return None


def main():
    """Spustí měření; 0 = PASS, 1 = FAIL, 77 = SKIP."""
    emu_test_proc.install(deadline_s=50)
    exe = _find_exe()
    tmp = Path(tempfile.mkdtemp(prefix="mztest-headless-cpu-"))
    proc = None
    try:
        port = emu_test_proc.free_tcp_port()
        proc = subprocess.Popen(
            [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
             f"--cfg-dir={tmp}", f"--work-dir={tmp}", f"--mcp-tcp-port={port}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=str(tmp))
        time.sleep(WARMUP_S)
        if proc.poll() is not None:
            print(f"FAIL: emulator exited early with code {proc.returncode}")
            return 1
        c0, t0 = _cpu_seconds(proc), time.perf_counter()
        if c0 is None:
            print("SKIP: process CPU time not available on this platform")
            return SKIP_EXIT
        time.sleep(MEASURE_S)
        c1, t1 = _cpu_seconds(proc), time.perf_counter()
        ratio = (c1 - c0) / (t1 - t0)
        ok = ratio < LIMIT_RATIO
        print(f"  {'PASS' if ok else 'FAIL'}: headless process CPU {c1 - c0:.2f} s "
              f"in {t1 - t0:.2f} s (ratio {ratio:.2f} < {LIMIT_RATIO})")
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        if proc:
            emu_test_proc.kill_proc(proc)
        emu_test_proc.rmtree_retry(tmp)


if __name__ == "__main__":
    sys.exit(main())
