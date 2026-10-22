#!/usr/bin/env python3
"""E2E test: krokování přes hranici snímku neztratí krok (MCP pipe).

Krok, jehož instrukce dokončí snímek, prochází v hlavní smyčce blokem
konce snímku. Ten dřív vybíral frontu dbgapi i během kroku: další
``STEP_INTO``, který klient poslal po dokončení příkazu kroku, se zpracoval
ještě před koncem kroku a ztratil se (``debugger_step_call(1)`` při
nastaveném kroku nic neudělá). ``step_n`` pak hlásil všechny kroky, ale PC
se posunulo o jeden méně, na každé hranici snímku.

Program: 4 KB instrukcí NOP od 2000h (MZ-800). Test 20x nastaví PC=2000h,
udělá ``step_n`` 1000 a ověří PC = 2000h + 1000. 20 000 NOPů po 4 T-stavech
je víc než jeden snímek MZ-800, takže aspoň jedno kolo hranici snímku
přejde (ověřeno čítačem snímků). Ověří i jednotlivé ``step_into`` přes
hranici snímku.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Začátek bloku NOPů v RAM.
PROG = 0x2000
#: Délka bloku NOPů [B].
PROG_LEN = 0x1000
#: Kroků v jednom kole step_n.
STEPS = 1000
#: Počet kol step_n.
ROUNDS = 20


def _pc(emu):
    """Vrátí aktuální PC."""
    regs = emu.call("get_registers")
    return regs.get("PC", regs.get("pc"))


def _frame(emu):
    """Vrátí čítač dokončených snímků (get_raster_pos)."""
    return emu.call("get_raster_pos")["frame_number"]


def case_step_n(emu):
    """step_n přes hranici snímku posune PC přesně o počet kroků."""
    print("Case: step_n across frame boundaries")
    frame0 = _frame(emu)
    for rnd in range(ROUNDS):
        emu.call("set_register", {"reg": "PC", "value": PROG})
        res = emu.call("step_n", {"count": STEPS})
        check(res.get("count") == STEPS, f"round {rnd}: step_n reports {STEPS} steps ({res})")
        pc = _pc(emu)
        if pc != PROG + STEPS:
            check(False, f"round {rnd}: PC is {PROG + STEPS:04X}h, got {pc:04X}h")
    crossed = _frame(emu) - frame0
    check(crossed >= 1, f"steps crossed at least one frame boundary ({crossed} frames)")
    check(True, f"{ROUNDS} x step_n {STEPS}: PC exact in every round")


def case_step_into(emu):
    """Jednotlivé step_into přes hranici snímku: každý krok o 1 bajt."""
    print("Case: step_into across a frame boundary")
    frame0 = _frame(emu)
    steps = 0
    pc = PROG
    emu.call("set_register", {"reg": "PC", "value": pc})
    # Nejvýš 2 snímky NOPů (MZ-800 PAL: asi 17 700 NOPů na snímek); čítač
    # snímků se kontroluje po 256 krocích.
    while steps < 2 * 17800:
        if pc >= PROG + PROG_LEN - 2:
            pc = PROG
            emu.call("set_register", {"reg": "PC", "value": pc})
        emu.call("step_into")
        after = _pc(emu)
        if after != pc + 1:
            check(False, f"step_into at {pc:04X}h moved PC to {after:04X}h")
        pc = after
        steps += 1
        if steps % 256 == 0 and _frame(emu) != frame0:
            break
    check(_frame(emu) != frame0, f"frame boundary crossed after {steps} single steps, PC exact")


def run(emu):
    """Spustí všechny případy nad jedním procesem emulátoru."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": "00" * PROG_LEN})
    case_step_n(emu)
    case_step_into(emu)


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_step_frame_"))
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
