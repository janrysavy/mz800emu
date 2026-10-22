#!/usr/bin/env python3
"""E2E test: identifikátory InsnPC a FrameCycle ve výrazech breakpointů (MCP pipe).

U MEM_W (a MEM_R, IORQ) breakpointu je ``PC`` ve výrazu živý registr PC,
tedy adresa za zapisující instrukcí. Adresu instrukce dává nově
``InsnPC``. ``Cycle`` (takty GDG od resetu) je 32bitový a po zhruba
121 s emulace přeteče; ``FrameCycle`` jsou takty GDG od začátku
aktuálního snímku. Ověřuje:

  - MEM_W BP s podmínkou ``InsnPC == 3009h`` zastaví na zápisu
    ``LD (HL),66h`` na 3009h (první zápis ``LD (4000h),A`` na 3003h
    podmínku nesplní),
  - MEM_W BP s podmínkou ``PC == 3009h`` nezastaví nikdy (PC je za
    instrukcí), s ``PC == 300Bh`` ano,
  - PC_EXEC BP s podmínkou ``FrameCycle == Cycle - Frame * 354432``
    zastaví (MZ-800 PAL: 354432 taktů GDG na snímek), opačná podmínka
    nezastaví.

Program (MZ-800, RAM 3000h):

    3000 F3        DI
    3001 3E 55     LD A,55h
    3003 32 00 40  LD (4000h),A
    3006 21 01 40  LD HL,4001h
    3009 36 66     LD (HL),66h
    300B 18 FE     JR $

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Adresa a bajty programu (viz docstring modulu).
PROG = 0x3000
PROG_HEX = "F33E553200402101403666" + "18FE"
#: Takty GDG na snímek MZ-800 PAL.
FRAME_TICKS = 354432
#: Jak dlouho čekat, zda breakpoint zastaví [s].
WAIT_S = 1.5


def _load(emu):
    """Nahraje program, vyplní 4000h-400Fh hodnotou AAh, PC = PROG."""
    emu.call("mem_write", {"addr": 0x4000, "data_hex": "AA" * 16})
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})


def _run_with_bp(emu, fields, values):
    """Založí BP, spustí program a vrátí (zastaveno, hits); BP pak smaže."""
    # Program a výplň hlídaného rozsahu nahrát před založením BP.
    _load(emu)
    data = {"fields": fields}
    data.update(values)
    res = emu.call("bp_create_with_init", data)
    check(res.get("created") is True, f"breakpoint created: {values}")
    bp_id = res["id"]
    try:
        emu.call("run")
        deadline = time.time() + WAIT_S
        paused = False
        while time.time() < deadline:
            if emu.call("get_state").get("paused"):
                paused = True
                break
            time.sleep(0.05)
        if not paused:
            emu.call("pause")
        hits = None
        for bp in emu.call("bp_list").get("breakpoints") or []:
            if bp.get("id") == bp_id:
                hits = bp.get("hits")
        return paused, hits
    finally:
        emu.call("bp_remove", {"id": bp_id})


def _memw(expr):
    """Pole a hodnoty MEM_W BP na 4000h-400Fh s podmínkou."""
    return (["type", "addr", "addr_end", "addr_match_mode", "expr"],
            {"type": "MEM_W", "addr": 0x4000, "addr_end": 0x400F,
             "addr_match_mode": "RANGE", "expr": expr})


def run(emu):
    """Ověří InsnPC, význam PC u MEM_W a FrameCycle."""
    emu.call("pause")

    paused, hits = _run_with_bp(emu, *_memw("InsnPC == 0x3009"))
    check(paused and hits == 1, f"InsnPC == 3009h stops once (paused={paused}, hits={hits})")
    first = base64.b64decode(emu.call("mem_read", {"addr": 0x4000, "len": 1})
                             .get("data_b64", ""))
    check(first == bytes([0x55]),f"the write at 3003h passed the condition ({first!r})")

    paused, hits = _run_with_bp(emu, *_memw("PC == 0x3009"))
    check(not paused and hits == 0, f"PC == 3009h never stops (paused={paused}, hits={hits})")

    paused, hits = _run_with_bp(emu, *_memw("PC == 0x300B"))
    check(paused and hits == 1, f"PC == 300Bh stops: PC is after the instruction (hits={hits})")

    exec_fields = ["type", "addr", "expr"]
    paused, hits = _run_with_bp(emu, exec_fields, {
        "type": "PC_EXEC", "addr": 0x3006,
        "expr": f"FrameCycle == Cycle - Frame * {FRAME_TICKS}"})
    check(paused and hits == 1, f"FrameCycle == Cycle - Frame * {FRAME_TICKS} (hits={hits})")

    paused, hits = _run_with_bp(emu, exec_fields, {
        "type": "PC_EXEC", "addr": 0x3006,
        "expr": f"(FrameCycle < 0) || (FrameCycle >= {FRAME_TICKS})"})
    check(not paused and hits == 0, f"FrameCycle stays in 0..{FRAME_TICKS - 1}")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_bp_insnpc_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu)
            ok = True
        finally:
            emu.close()
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
