#!/usr/bin/env python3
"""E2E test: MCP irq_inject a nmi_inject se přijmou i bez HW linky INT (pipe).

Přerušení z ``irq_inject`` / ``nmi_inject`` jen nastaví čekající IRQ / NMI
v jádře Z80. Kontrola přerušení v hlavní smyčce ale končila, když žádný
čip netáhl linku INT, a jádro volala jen při IFF1 = 1. Přerušení z MCP se
tak bez HW zdroje nepřijalo vůbec a NMI navíc záviselo na IFF1, přestože
dokumentace slibuje přijetí IRQ při IFF1 a NMI vždy. Ověřuje:

  - ``irq_inject`` s vektorem při EI (IM 2) skočí do obsluhy z tabulky,
  - ``nmi_inject`` po přijetí IRQ (IFF1 = 0) skočí na 0066h,
  - ``irq_inject`` při DI se nepřijme, dokud program neprovede EI.

Program (MZ-800, RAM 3000h): odmapuje dolní ROM, na 0066h zapíše ``JR $``,
nastaví IM 2 s tabulkou na 3100h, EI a čeká v ``JR $``. Obsluha IRQ na
3200h je ``JR $`` (IFF1 zůstane 0).

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

#: Začátek programu.
PROG = 0x3000
#: Program (viz docstring modulu); smyčka JR $ je na WAIT_EI.
PROG_HEX = ("D3E0"        # OUT (0E0h),A   dolní ROM -> RAM
            "216600"      # LD HL,0066h
            "3618"        # LD (HL),18h
            "23"          # INC HL
            "36FE"        # LD (HL),0FEh   0066h: JR $
            "ED5E"        # IM 2
            "3E31"        # LD A,31h
            "ED47"        # LD I,A
            "FB"          # EI
            "18FE")       # JR $
#: Adresa čekací smyčky po EI.
WAIT_EI = 0x3011
#: Vektor IM 2 a položka tabulky (I = 31h).
VECTOR = 0x5A
TABLE_ENTRY = 0x3100 + VECTOR
#: Obsluha IRQ (JR $).
ISR = 0x3200
#: Program 2 (3300h): DI, čekání, EI, čekání.
PROG_DI = 0x3300
PROG_DI_HEX = ("F3"       # 3300h DI
               "18FE")    # 3301h JR $
#: Smyčka při DI.
WAIT_DI = 0x3301


def _pc(emu):
    """Vrátí PC."""
    return emu.call("get_registers")["PC"]


def _setup(emu):
    """Zapíše program, tabulku a obsluhu, rozběhne program do smyčky po EI."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("mem_write", {"addr": TABLE_ENTRY, "data_hex": f"{ISR & 0xFF:02X}{ISR >> 8:02X}"})
    emu.call("mem_write", {"addr": ISR, "data_hex": "18FE"})
    emu.call("mem_write", {"addr": PROG_DI, "data_hex": PROG_DI_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 2})
    check(_pc(emu) == WAIT_EI, f"program waits in JR $ after EI (PC {_pc(emu):04X}h)")


def case_irq_then_nmi(emu):
    """IRQ při EI skočí do obsluhy, NMI při IFF1 = 0 na 0066h."""
    _setup(emu)
    emu.call("irq_inject", {"vector": VECTOR})
    emu.call("run", {"frames": 1})
    check(_pc(emu) == ISR, f"injected IRQ accepted, PC in the IM 2 handler ({_pc(emu):04X}h)")

    emu.call("nmi_inject")
    emu.call("run", {"frames": 1})
    check(_pc(emu) == 0x0066, f"injected NMI accepted with IFF1 = 0, PC 0066h ({_pc(emu):04X}h)")


def case_irq_waits_for_ei(emu):
    """IRQ při DI čeká; po EI se přijme."""
    _setup(emu)
    emu.call("set_register", {"reg": "PC", "value": PROG_DI})
    emu.call("run", {"frames": 1})
    check(_pc(emu) == WAIT_DI, f"program waits with DI ({_pc(emu):04X}h)")
    emu.call("irq_inject", {"vector": VECTOR})
    emu.call("run", {"frames": 2})
    check(_pc(emu) == WAIT_DI, f"IRQ with DI is not accepted ({_pc(emu):04X}h)")
    # EI na místo JR $ (EI; JR $): IRQ čeká a přijme se po instrukci za EI.
    emu.call("mem_write", {"addr": WAIT_DI, "data_hex": "FB18FE"})
    emu.call("run", {"frames": 1})
    check(_pc(emu) == ISR, f"pending IRQ accepted after EI ({_pc(emu):04X}h)")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_irq_nmi_inject_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            case_irq_then_nmi(emu)
            emu.call("reset")
            case_irq_waits_for_ei(emu)
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
