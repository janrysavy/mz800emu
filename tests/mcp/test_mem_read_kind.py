#!/usr/bin/env python3
"""E2E test: datové čtení hned za instrukcí je čtení dat, ne bajt instrukce.

Logovací čtecí callback rozlišuje bajty instrukce (X: M1, prefix, operand)
od čtení dat (R). Čtení, které leží hned za dosud načtenými bajty
instrukce, se dřív bralo jako operand (X): CDL ho počítalo jako vykonání
a MEM_R breakpoint na takové adrese nezastavil.

Případy (program na 3000h, cíl návratu 3100h: ``JR $``):

  A  ``RET`` na 3004h se SP = 3005h: POP čte 3005h a 3006h (hned za RET);
  B  kontrola: ``RET`` na 3004h se SP = 3010h;
  C  ``LD A,(3003h)`` na 3000h čte 3003h (hned za instrukcí).

Pro každý případ: CDL (export, oblast ``bus``) má u čtené buňky r = 1
a MEM_R breakpoint na ní zastaví (hits = 1). Na MZ-800, MZ-700 a MZ-1500
(pokud binárky existují). Procesy izolované, zabíjí se jen vlastní PID.
Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP.
"""

import json
import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)

#: Jméno -> (program na 3000h, další bloky {adresa: bajty}, čtená adresa).
CASES = {
    "A ret with SP right after it": (bytes.fromhex("F3" "310530" "C9" "0031"), {}, 0x3005),
    "B ret with SP elsewhere": (bytes.fromhex("F3" "311030" "C9"), {0x3010: bytes.fromhex("0031")}, 0x3010),
    "C ld a,(nn) right after it": (bytes.fromhex("F3" "3A0430" "18FE"), {}, 0x3004),
}
#: BPT_TYPE_MEM_R (breakpoints.h)
BP_MEM_R = 1


def bus_cell(meta_path, addr):
    """Čítače (r, w, x, s) buňky oblasti bus (CPU adresa) z exportu CDL."""
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    for reg in meta["regions"]:
        if "bus" in reg.get("file", ""):
            data = (meta_path.parent / reg["file"]).read_bytes()
            return struct.unpack_from("<4I", data, addr * 16)
    raise TestFailure(f"no bus region in the CDL export ({meta_path})")


def case(exe, tmp, name, prog, extra, addr):
    """Jeden případ: CDL a MEM_R breakpoint na čtené adrese."""
    d = tmp / (exe.stem + "_" + name[0])
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        emu.call("pause")
        emu.call("mem_write", {"addr": 0x3100, "data_hex": "18FE"})
        for a, data in extra.items():
            emu.call("mem_write", {"addr": a, "data_hex": data.hex()})
        emu.call("mem_write", {"addr": 0x3000, "data_hex": prog.hex()})
        emu.call("set_register", {"reg": "PC", "value": 0x3000})
        bp = emu.call("bp_create_with_init", {"addr": addr, "fields": ["type", "addr"], "type": BP_MEM_R})
        emu.call("cdl_start")
        emu.call("run", {"frames": 1})
        emu.call("pause")
        hits = [b for b in emu.call("bp_list")["breakpoints"] if b["id"] == bp["id"]][0]["hits"]
        meta = d / "cdl.json"
        emu.call("cdl_export", {"path": str(meta)})
        r, _, x, _ = bus_cell(meta, addr)
        check(r == 1 and hits == 1,
              f"{exe.stem} {name}: data read of {addr:04X}h is R (r={r}, x={x}) and MEM_R stops (hits={hits})")
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    tmp = Path(tempfile.mkdtemp(prefix="mz_readkind_"))
    ok = False
    try:
        for name in ("mz800emu", "mz700emu-pal", "mz1500emu"):
            e = _find_exe(name)
            if not e:
                print(f"  SKIP: {name} binary not found")
                continue
            for cname, (prog, extra, addr) in CASES.items():
                case(Path(e), tmp, cname, prog, extra, addr)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        shutil.rmtree(tmp, ignore_errors=True)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
