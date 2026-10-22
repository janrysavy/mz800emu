#!/usr/bin/env python3
"""E2E test: cputrack zapisuje bajty instrukce tak, jak je CPU načetlo (MCP pipe).

Cputrack dřív četl bajty instrukce až po jejím vykonání z aktuální mapy
paměti. Instrukce, která přepíše vlastní bajty (nebo přepne banking pod
sebou), se tak zapsala s bajty nového stavu: ``LD (3001h),A`` na 3001h
s A = 0 se zapsala jako ``NOP`` (1 bajt, 00h). Nově cputrack bere bajty
z fetch instrukce. Ověřuje:

  - záznam na 3001h má délku 3 a bajty ``32 01 30`` (paměť už má 00h),
  - záznamy okolních instrukcí (``XOR A`` na 3000h, ``JR $`` na 3004h)
    jsou beze změny.

Program (MZ-800, RAM 3000h): ``XOR A; LD (3001h),A; JR $``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI
s adresářem cputrack v dočasném adresáři a rozsahem PC 3000h-300Fh,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import shutil
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Program: XOR A; LD (3001h),A (přepíše vlastní opkód na 00h); JR $.
PROG = 0x3000
PROG_HEX = "AF32013018FE"
#: Velikost záznamu cputrack [B] (docs CPU-track_format.md).
RECORD = 12


def _first_records(trace_dir):
    """Vrátí {pc: (délka, bajty)} prvního záznamu každé adresy."""
    data = b"".join(p.read_bytes() for p in sorted(trace_dir.glob("ct.*.bin")))
    out = {}
    for i in range(0, len(data) - RECORD + 1, RECORD):
        pc, ln = struct.unpack_from("<HB", data, i)
        if pc not in out:
            out[pc] = (ln, data[i + 3:i + 3 + ln])
    return out


def run(emu, trace_dir):
    """Zaznamená program a zkontroluje bajty samomodifikující instrukce."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("trace_start", {"channel": "cputrack"})
    emu.call("run", {"frames": 1})
    emu.call("trace_stop", {"channel": "cputrack"})

    rec = _first_records(trace_dir)
    print(f"  records: { {f'{k:04X}': (v[0], v[1].hex()) for k, v in rec.items()} }")
    check(rec.get(0x3000) == (1, bytes([0xAF])), "XOR A at 3000h recorded as AF")
    check(rec.get(0x3001) == (3, bytes([0x32, 0x01, 0x30])),
          f"LD (3001h),A recorded with its fetched bytes 32 01 30 ({rec.get(0x3001)})")
    check(rec.get(0x3004) == (2, bytes([0x18, 0xFE])), "JR $ at 3004h recorded as 18 FE")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cputrack_bytes_"))
    ok = False
    try:
        trace_dir = tmp / "trace"
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
                       "[TRACE_CPUTRACK]\n"
                       f"dir = {trace_dir.as_posix()}\nname = ct\n"
                       "save_on_exit = 0\n"
                       f"pc_range_lo = {PROG}\npc_range_hi = {PROG + 0x0F}\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            run(emu, trace_dir)
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
