#!/usr/bin/env python3
"""E2E test voleb ``screenshot_save_to_file`` (area, scale, crop), headless + MCP TCP.

Program (``--run-mzf``, MZ-800 mód) je jen ``JR $``; po zastavení na jeho
adrese se uloží několik PNG a z hlavičky IHDR se ověří rozměry:

  * výchozí (full, framebuffer)          -> 928x288 (dřívější chování),
  * canvas + native (režim 320 bodů)      -> 320x200, hires = false,
  * full + scale 2 (okno na 100 %)        -> 928x576,
  * canvas + native + crop 32,4 256x192   -> 256x192,
  * crop mimo obraz                       -> chyba parametrů.

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
from test_run_mzf_state import (TcpEmu, TestFailure, _EXEC, _find_exe,  # noqa: E402
                                _make_mzf, check)


def _png_size(path):
    """Vrátí (šířka, výška) z hlavičky IHDR PNG souboru."""
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise TestFailure(f"{path} is not a PNG file")
    return struct.unpack(">II", data[16:24])


def main():
    emu_test_proc.install(deadline_s=60)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    work = Path(tempfile.mkdtemp(prefix="mz_screenshot_opts_"))
    ok = False
    try:
        mzf = work / "loop.mzf"
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

            cases = [
                ("default", {}, (928, 288)),
                ("canvas_native", {"area": "canvas", "scale": "native"}, (320, 200)),
                ("full_x2", {"scale": 2}, (928, 576)),
                ("crop", {"area": "canvas", "scale": "native",
                          "crop": {"x": 32, "y": 4, "width": 256, "height": 192}}, (256, 192)),
            ]
            for name, opts, size in cases:
                path = work / f"{name}.png"
                resp = emu.call("screenshot_save_to_file", {"path": str(path), **opts})
                check(resp.get("available") is True, f"[{name}] screenshot available ({resp})")
                check((resp.get("width"), resp.get("height")) == size,
                      f"[{name}] response size {size} (got {resp.get('width')}x{resp.get('height')})")
                check(_png_size(path) == size, f"[{name}] PNG IHDR size {size} (got {_png_size(path)})")
                if opts.get("scale") == "native":
                    check(resp.get("hires") is False, f"[{name}] 320-dot mode detected")

            try:
                emu.call("screenshot_save_to_file", {
                    "path": str(work / "bad.png"), "area": "canvas", "scale": "native",
                    "crop": {"x": 300, "y": 0, "width": 100, "height": 5}})
                raise TestFailure("crop outside the image was accepted")
            except TestFailure as e:
                check("Crop rectangle" in str(e), f"crop outside the image is rejected ({e})")
            check(not (work / "bad.png").exists(), "no file written for a rejected crop")
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
