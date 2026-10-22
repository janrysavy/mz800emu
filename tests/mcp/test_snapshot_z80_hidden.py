#!/usr/bin/env python3
"""E2E regresní test: snapshot uloží a obnoví skrytý stav Z80 (MCP pipe).

Chyba: snapshot ukládal jen viditelné registry. Skrytý stav Z80 (MEMPTR/WZ,
Q, čekající INT a NMI, ``total_cycles``) po ``snapshot_load`` zůstal ze
stroje, který se načtením přepisoval: WZ = 1234h uložené ve snapshotu se po
načtení vrátilo jako hodnota "špinavého" stroje.

Pro každou binárku v kořeni repa (nebo v ``MZ_EMU_DIR``):

  1  round-trip: program ``DI; LD A,0Fh; ADD A,1; JR $``,
     ``set_register WZ``, ``irq_inject`` a ``nmi_inject`` v pauze (INT
     i NMI zůstanou čekat, dokud emulace stojí), ``snapshot_save``; "špinavý" stroj (reset
     a 3 snímky), ``snapshot_load``; WZ, Q, čekající INT/NMI, EI delay
     a ``total_cycles`` se musí shodovat s uloženým stavem;
  2  starý formát: ze stejného snapshotu se odstraní prvek
     ``hidden_state`` z ``cpu/z80.xml`` (a kontrolní součet manifestu)
     a načte se do stroje s nenulovým skrytým stavem; skrytý stav musí
     být na výchozích hodnotách resetu, ``total_cycles`` zůstane ze stroje;
  3  EI s čekajícím INT: program nastaví trvalé INT z CTC2 (8253 a 8255
     mapované na 0E000h-0E007h, IM 2 s tabulkou vektorů), zastaví se hned
     po ``EI`` (EI delay = 1), ``snapshot_save``; souvislý běh a běh po
     načtení do resetovaného stroje musí dát stejných 12 kroků
     (instrukce po EI se vykoná před přijetím INT).

Scénář 3 prochází i na kódu před opravou (přerušení se testuje jen
v bloku událostí po kroku a po odpauzování jde vždy nejdřív krok), je to
pojistka, že obnova EI delay běh nezmění. Scénáře 1 a 2 dřív selhaly.

Každý proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, prázdné
INI, ``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (žádná binárka).
"""

import re
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Binárky, které test prochází.
_BINARIES = ["mz700emu-pal", "mz700emu-ntsc", "mz800emu", "mz1500emu"]

#: Adresa programů v RAM.
_PROG = 0x3000

#: DI; LD A,0Fh; ADD A,1; JR $.
_PROG_Q = bytes.fromhex("F3" "3E0F" "C601" "18FE")
#: Adresa JR $ v _PROG_Q (zastavení po ADD).
_PROG_Q_END = _PROG + 5

#: Trvalé INT z CTC2: DI; IM 2; I = 32h; PC2 = 0; CT1 režim 2, N = 2
#: (CLK1 = HSYNC); CT2 režim 0, N = 1 (OUT2 po dočítání zůstane 1);
#: PC2 = 1; čekací smyčka DJNZ (INT linka se mezitím aktivuje); EI;
#: INC C třikrát; JR $. 8253 a 8255 přes 0E003h-0E007h (po resetu je
#: MZ-800 v režimu MZ-700, porty 0D0h-0D7h tam nejsou připojené).
_PROG_INT = bytes.fromhex(
    "F3" "ED5E" "3E32" "ED47"
    "3E04" "3203E0"
    "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
    "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
    "3E05" "3203E0"
    "0600" "10FE"
    "FB" "0C" "0C" "0C" "18FE")
#: Adresa EI v _PROG_INT.
_PROG_INT_EI = _PROG + _PROG_INT.index(bytes.fromhex("FB0C0C0C"))
#: Tabulka vektorů IM 2 (I = 32h, libovolný vektor -> 3434h) a ISR.
_IM2_TABLE = 0x3200
_ISR = 0x3434
#: ISR: INC D; EI; RETI.
_ISR_CODE = bytes.fromhex("14" "FB" "ED4D")

#: Testovací hodnota WZ.
_WZ = 0x1234


def _hidden(emu):
    """Skrytý stav Z80 přes MCP (get_registers + get_cpu_flags)."""
    r = emu.call("get_registers")
    f = emu.call("get_cpu_flags")
    return {"WZ": r["WZ"], "q": f["q"], "int_pending": f["int_pending"],
            "nmi_pending": f["nmi_pending"], "ei_delay": f["ei_delay"],
            "total_cycles": f["total_cycles"]}


def _load_program(emu, prog):
    """Pauza, program na _PROG, PC = _PROG."""
    emu.call("pause")
    emu.call("mem_write", {"addr": _PROG, "data_hex": prog.hex()})
    emu.call("set_register", {"reg": "PC", "value": _PROG})


def _step_to(emu, addr, limit):
    """Krokuje, dokud PC != addr (nejvýš limit kroků)."""
    for _ in range(limit):
        if emu.call("get_registers")["PC"] == addr:
            return
        emu.call("step_into")
    raise TestFailure(f"PC did not reach {addr:04X}h in {limit} steps")


def _dirty(emu):
    """Stroj s jiným skrytým stavem než snapshot: reset a 3 snímky."""
    emu.call("reset")
    emu.call("pause")
    emu.call("run", {"frames": 3})
    emu.call("pause")


def _hidden_state_snapshot(emu, snap):
    """Připraví nenulový skrytý stav a uloží snapshot; vrátí uložený stav.

    Začíná resetem: stroj může mít z předchozího scénáře čekající INT a NMI
    (načtený snapshot), které by se při krokování programu přijaly.
    """
    emu.call("reset")
    _load_program(emu, _PROG_Q)
    _step_to(emu, _PROG_Q_END, 10)
    emu.call("set_register", {"reg": "WZ", "value": _WZ})
    emu.call("irq_inject", {"vector": 0x5A})
    emu.call("nmi_inject")
    saved = _hidden(emu)
    # Q je v buildech MZ na hranici instrukce vždy 0 (jádro po jedné
    # instrukci počítá Q až při dispatchi další), round-trip Q ověřuje
    # C test snapshot-z80-hidden.
    check(saved["WZ"] == _WZ and saved["int_pending"] and saved["nmi_pending"],
          f"non-default hidden state prepared {saved}")
    emu.call("snapshot_save", {"path": str(snap)})
    return saved


def case_roundtrip(emu, snap):
    """Scénář 1: round-trip skrytého stavu přes snapshot."""
    saved = _hidden_state_snapshot(emu, snap)
    _dirty(emu)
    emu.call("snapshot_load", {"path": str(snap)})
    loaded = _hidden(emu)
    check(loaded == saved, f"hidden Z80 state restored ({loaded} == {saved})")


def _strip_hidden(src, dst):
    """Kopie snapshotu ve starém formátu: bez hidden_state a bez checksum."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "cpu/z80.xml":
                text, n = re.subn(r"\s*<hidden_state>.*?</hidden_state>", "",
                                  data.decode("utf-8"), flags=re.S)
                data = text.encode("utf-8")
            elif item.filename == "manifest.xml":
                text = re.sub(r"\s*<checksum[^>]*>.*?</checksum>", "",
                              data.decode("utf-8"), flags=re.S)
                data = text.encode("utf-8")
            zout.writestr(item.filename, data)


def case_old_format(emu, snap, tmp):
    """Scénář 2: snapshot bez hidden_state nastaví výchozí skrytý stav."""
    old = tmp / "old.mzs"
    _strip_hidden(snap, old)
    with zipfile.ZipFile(old) as z:
        check(b"hidden_state" not in z.read("cpu/z80.xml"),
              "old-format copy has no hidden_state element")
    # Stroj s nenulovým skrytým stavem (stejná příprava jako snapshot).
    before = _hidden_state_snapshot(emu, tmp / "unused.mzs")
    emu.call("snapshot_load", {"path": str(old)})
    loaded = _hidden(emu)
    expect = {"WZ": 0, "q": 0, "int_pending": False, "nmi_pending": False,
              "ei_delay": False, "total_cycles": before["total_cycles"]}
    check(loaded == expect,
          f"old snapshot sets reset defaults, keeps total_cycles ({loaded} == {expect})")


def _trace(emu, n):
    """n kroků; vrátí (PC, C, D) po každém."""
    out = []
    for _ in range(n):
        emu.call("step_into")
        r = emu.call("get_registers")
        out.append((r["PC"], r["BC"] & 0xFF, r["DE"] >> 8))
    return out


def case_ei_pending_int(emu, snap):
    """Scénář 3: snapshot hned po EI s aktivní linkou INT."""
    emu.call("reset")
    _load_program(emu, _PROG_INT)
    emu.call("mem_write", {"addr": _IM2_TABLE, "data_hex": "34" * 257})
    emu.call("mem_write", {"addr": _ISR, "data_hex": _ISR_CODE.hex()})
    _step_to(emu, _PROG_INT_EI, 800)
    emu.call("step_into")  # vykoná EI
    check(emu.call("get_cpu_flags")["ei_delay"], "paused right after EI (EI delay set)")
    emu.call("snapshot_save", {"path": str(snap)})
    c0 = emu.call("get_registers")["BC"] & 0xFF
    a = _trace(emu, 12)
    check(any(pc == _ISR for pc, _, _ in a), "INT from CTC2 accepted in continuous run")
    check(a[0][1] == ((c0 + 1) & 0xFF), "instruction after EI executed before the INT")
    _dirty(emu)
    emu.call("snapshot_load", {"path": str(snap)})
    b = _trace(emu, 12)
    check(a == b, "run from snapshot equals continuous run after EI")


def run_platform(exe, tmp):
    """Všechny scénáře pro jednu binárku v jednom procesu."""
    emu = PipeEmu(exe, tmp)
    try:
        case_roundtrip(emu, tmp / "hidden.mzs")
        case_old_format(emu, tmp / "hidden.mzs", tmp)
        case_ei_pending_int(emu, tmp / "ei.mzs")
    finally:
        emu.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=280)
    found = [(n, _find_exe(n)) for n in _BINARIES]
    found = [(n, e) for n, e in found if e]
    if not found:
        print("SKIP: no emulator binary found")
        return 77
    failed = []
    for name, exe in found:
        print(f"== {name} ({exe})")
        tmp = Path(tempfile.mkdtemp(prefix="mz_snap_z80_"))
        try:
            run_platform(exe, tmp)
            shutil.rmtree(tmp, ignore_errors=True)
        except TestFailure as e:
            print(f"FAIL ({name}): {e}; artifacts kept in {tmp}")
            failed.append(name)
    if failed:
        print(f"FAIL: {', '.join(failed)}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
