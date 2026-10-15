#!/usr/bin/env python3
"""E2E test série screenshotů (``screenshot_series_start`` / ``_status`` /
``_stop``), headless + MCP TCP.

Program (``--run-mzf``, MZ-800 mód): 1200h ``JR 1202h``, 1202h ``JR $`` -
obraz se nemění. Po zastavení na PC_EXEC breakpointu 1200h:

  * only_changed, frames 10, ``run`` 12 snímků -> 1 soubor (první snímek),
    9 nezměněných, stop_reason "Frame count reached", index.csv 1 řádek,
  * všechny snímky (only_changed false), frames 5 -> 5 souborů
    frame_0000000.png až frame_0000004.png v rozměru native 464x288,
  * druhý start během běžící série -> chyba; stop -> writing false.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka).
"""

import json
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty
from test_run_mzf_state import TcpEmu, TestFailure, _EXEC, _find_exe, check  # noqa: E402


def _make_mzf(path):
    """MZF: 1200h JR 1202h, 1202h JR $ (breakpoint na 1200h zastaví jen jednou)."""
    body = bytes([0x18, 0x00, 0x18, 0xFE])
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"SERIES TEST\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


def _png_size(path):
    """Vrátí (šířka, výška) z hlavičky IHDR PNG souboru."""
    data = Path(path).read_bytes()
    return struct.unpack(">II", data[16:24])


def _wait_done(emu, timeout=20.0):
    """Počká, až série skončí a dopíše; vrátí stav."""
    deadline = time.monotonic() + timeout
    while True:
        st = emu.call("screenshot_series_status")
        if not st.get("active") and not st.get("writing"):
            return st
        if time.monotonic() > deadline:
            raise TestFailure(f"series did not finish ({st})")
        time.sleep(0.1)


def main():
    emu_test_proc.install(deadline_s=90)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    work = Path(tempfile.mkdtemp(prefix="mz_screenshot_series_"))
    ok = False
    try:
        mzf = work / "series.mzf"
        _make_mzf(mzf)
        ini = work / "emu.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n", encoding="utf-8")
        (work / "mz800-breakpoints.bpt").write_text(json.dumps({
            "breakpoints": [{"id": 1, "name": "exec", "addr": _EXEC, "type": "PC_EXEC",
                             "enabled": True, "skip_count": 0, "hit_count": 0,
                             "expr": None, "action": None, "edge_triggered": False}],
            "groups": []}), encoding="utf-8")
        port = emu_test_proc.free_tcp_port()
        args = [str(exe), "--headless", "--no-save-ini", "--no-first-run-windows",
                f"--cfg-dir={work}", f"--work-dir={work}", f"--config={ini}",
                f"--mcp-tcp-port={port}", "--mode-switch=800", "--run-mzf", str(mzf)]
        emu = TcpEmu(args, work, port)
        try:
            deadline = time.monotonic() + 20.0
            while not emu.call("get_state").get("paused"):
                if time.monotonic() > deadline:
                    raise TestFailure("exec breakpoint was not hit")
                time.sleep(0.1)

            # 1) jen změněné snímky na statickém obrazu
            d1 = work / "changed"
            st = emu.call("screenshot_series_start", {"dir": str(d1), "frames": 10})
            check(st.get("active") is True and st.get("scale") == "native",
                  f"series started (native, active) ({st})")
            try:
                emu.call("screenshot_series_start", {"dir": str(work / "other")})
                raise TestFailure("second start was accepted")
            except TestFailure as e:
                check("already running" in str(e), f"second start is rejected ({e})")
            emu.call("run", {"frames": 12})
            st = _wait_done(emu)
            check(st.get("frames_seen") == 10, f"10 frames observed (got {st.get('frames_seen')})")
            check(st.get("files") == 1, f"static picture -> 1 file (got {st.get('files')})")
            check(st.get("unchanged") == 9, f"9 unchanged frames (got {st.get('unchanged')})")
            check(st.get("stop_reason") == "Frame count reached", f"stop reason ({st.get('stop_reason')})")
            csv = (d1 / "index.csv").read_text(encoding="utf-8").splitlines()
            check(csv[0] == "frame,file,gdg_screen" and len(csv) == 2 and csv[1].startswith("0,frame_0000000.png,"),
                  f"index.csv header + 1 row ({csv})")

            # 2) všechny snímky
            d2 = work / "all"
            emu.call("screenshot_series_start", {"dir": str(d2), "frames": 5, "only_changed": False})
            emu.call("run", {"frames": 6})
            st = _wait_done(emu)
            check(st.get("files") == 5, f"all frames -> 5 files (got {st.get('files')})")
            names = sorted(p.name for p in d2.glob("frame_*.png"))
            check(names == [f"frame_{i:07d}.png" for i in range(5)], f"frame file names ({names})")
            check(_png_size(d2 / "frame_0000000.png") == (464, 288), "native full frame 464x288")

            # 3) stop bez běžící série
            try:
                emu.call("screenshot_series_stop")
                raise TestFailure("stop without a series was accepted")
            except TestFailure as e:
                check("No frame series is running" in str(e), f"stop without a series is an error ({e})")
        finally:
            emu.close()
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(work)
        print("PASS")
        return 0
    print(f"Work dir kept: {work}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
