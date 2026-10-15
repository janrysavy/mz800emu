#!/usr/bin/env python3
"""E2E test ``get_periph_gdg`` (``emulator://periph/gdg``) - řadič VRAM (WF/RF)
a HW scroll MZ-800, headless + MCP TCP.

Program (``--run-mzf``, MZ-800 mód) zapíše:

  * WF (0CCh) = 95h: REPLACE (bit 7-5 = 100), VBANK B (bit 4), roviny I + III,
  * RF (0CDh) = 92h: search (bit 7), VBANK B, rovina II,
  * HW scroll přes OUT (C),A: SW (03CFh) = 78h, SSA (04CFh) = 05h,
    SEA (05CFh) = 7Dh, SOF_HI (02CFh) = 0, SOF_LO (01CFh) = 01h
    (SW = SEA - SSA, scroll se tedy zapne),

a zastaví se na PC_EXEC breakpointu za posledním OUT. Test ověří, že odpověď
obsahuje objekty ``vramctrl`` a ``hwscroll`` s těmito hodnotami - scroll
v jednotkách portu, ne interní bajtové adresy plane.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka).
"""

import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid procesů, volné TCP porty
from test_run_mzf_state import TcpEmu, TestFailure, _EXEC, _find_exe, check  # noqa: E402

#: Program na 1200h (viz docstring), končí ``JR $`` na adrese _DONE.
_PROGRAM = bytes([
    0x3E, 0x95, 0xD3, 0xCC,                     # LD A,95h / OUT (0CCh),A  WF
    0x3E, 0x92, 0xD3, 0xCD,                     # LD A,92h / OUT (0CDh),A  RF
    0x01, 0xCF, 0x03, 0x3E, 0x78, 0xED, 0x79,   # SW  = 78h
    0x01, 0xCF, 0x04, 0x3E, 0x05, 0xED, 0x79,   # SSA = 05h
    0x01, 0xCF, 0x05, 0x3E, 0x7D, 0xED, 0x79,   # SEA = 7Dh
    0x01, 0xCF, 0x02, 0x3E, 0x00, 0xED, 0x79,   # SOF_HI = 0
    0x01, 0xCF, 0x01, 0x3E, 0x01, 0xED, 0x79,   # SOF_LO = 1
])
_DONE = _EXEC + len(_PROGRAM)


def _make_mzf(path):
    """MZF: atribut 01h, fstrt = fexec = 1200h, tělo _PROGRAM + ``JR $``."""
    body = _PROGRAM + bytes([0x18, 0xFE])
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"GDG VRAMCTRL\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = _EXEC.to_bytes(2, "little")
    hdr[0x16:0x18] = _EXEC.to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


def _run(exe, work):
    """Spustí program a vrátí get_periph_gdg po zastavení na _DONE."""
    mzf = work / "vramctrl.mzf"
    _make_mzf(mzf)
    ini = work / "emu.ini"
    ini.write_text("[BREAKPOINTS]\nauto_load=1\nauto_save=0\n", encoding="utf-8")
    (work / "mz800-breakpoints.bpt").write_text(json.dumps({
        "breakpoints": [{"id": 1, "name": "done", "addr": _DONE, "type": "PC_EXEC",
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
                raise TestFailure("breakpoint after the last OUT was not hit")
            time.sleep(0.1)
        return emu.call("get_periph_gdg")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=60)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_gdg_vramctrl_"))
    ok = False
    try:
        gdg = _run(exe, tmp)
        vc = gdg.get("vramctrl") or {}
        hs = gdg.get("hwscroll") or {}
        check(vc.get("wf_mode") == "replace" and vc.get("wf_mode_code") == 4,
              f"WF mode REPLACE (got {vc.get('wf_mode')}, {vc.get('wf_mode_code')})")
        check(vc.get("wf_planes") == 0x05, f"WF planes I+III = 05h (got {vc.get('wf_planes')})")
        check(vc.get("rf_planes") == 0x02, f"RF planes II = 02h (got {vc.get('rf_planes')})")
        check(vc.get("rf_search") is True, f"RF search (got {vc.get('rf_search')})")
        check(vc.get("vbank") == 1, f"VBANK B (got {vc.get('vbank')})")
        check(hs.get("enabled") is True, f"HW scroll enabled (got {hs.get('enabled')})")
        check((hs.get("sof"), hs.get("ssa"), hs.get("sea"), hs.get("sw")) == (1, 0x05, 0x7D, 0x78),
              f"scroll SOF/SSA/SEA/SW in port units = 1/05h/7Dh/78h (got {hs})")
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Work dir kept: {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
