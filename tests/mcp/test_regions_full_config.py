#!/usr/bin/env python3
"""E2E test: výčet a čtení regionů v plné sestavě MZ-800 (MCP pipe).

S MemExtem Luftner (128 RAM + 128 FLASH bank), RAMdiskem STD 16 MB
(256 bank) a PEZIKem na portu 68h (8 bank) má MZ-800 532 regionů
(PEZIK na E8h se s RAMdiskem STD vylučuje, ``ramdisk.c``).
``regions_list`` dřív výčet usekl na 128 položek a ``region_read`` /
``region_write`` odmítly každé ID nad 128, takže banky RAMdisku byly přes
MCP nedostupné. Ověřuje:

  - ``regions_list`` vrátí všechny regiony (``count`` = délka pole, ID
    0..N-1 bez mezer), včetně všech 256 bank RAMdisku STD a bank PEZIK,
  - ``region_write`` + ``region_read`` fungují na regionu s ID nad 512,
  - ``region_read`` s ID = N (první neplatné) skončí chybou.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``, soubory RAMdisku a FLASH v dočasném adresáři); zabíjí
se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Počet regionů sestavy (12 základních + 256 MemExt + 256 RAMdisk STD
#: + 8 PEZIK 68h; PROHIBITED jen po OUT 0E5h).
EXPECTED_REGIONS = 12 + 256 + 256 + 8
#: Testovací vzorek zapsaný do banky PEZIK.
PATTERN = bytes(range(0x40, 0x50))


def _ini(tmp):
    """INI plné sestavy; všechny soubory zařízení v dočasném adresáři."""
    return (
        "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
        "[MEMEXT]\nconnected = 1\ntype = LUFTNER\n"
        f"flash_filepath = {(tmp / 'flash.dat').as_posix()}\n"
        "[RAMDISK]\nmr1r18_pluged = 1\nmr1r18_type = STANDARD\n"
        "mr1r18_size = 16M\n"
        f"mr1r18_filepath = {(tmp / 'rd-std.dat').as_posix()}\n"
        "pezik_e8_pluged = 0\n"
        "pezik_68_pluged = 1\npezik_68_backuped = 0\n"
        f"pezik_68_filepath = {(tmp / 'pezik-68.dat').as_posix()}\n"
    )


def _region_read_error(emu, region_id):
    """Pošle region_read a vrátí text chyby, nebo None při úspěchu."""
    emu.rid += 1
    rid = emu.rid
    req = {"type": "request", "req_id": rid, "cmd": "region_read",
           "data": {"region_id": region_id, "offset": 0, "length": 1}}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(10.0, lambda m: m.get("type") == "response"
                     and m.get("req_id") == rid)
    if resp is None:
        raise TestFailure("no response to region_read")
    return None if resp.get("success") else (resp.get("error") or "error")


def run(emu):
    """Ověří výčet všech regionů a čtení/zápis vysokého ID."""
    res = emu.call("regions_list")
    regions = res.get("regions") or []
    n = res.get("count")
    print(f"  regions_list: count={n}, len={len(regions)}")
    check(n == len(regions), f"count matches the array length ({n})")
    check(n == EXPECTED_REGIONS, f"all regions listed: {n} == {EXPECTED_REGIONS}")
    check([r.get("id") for r in regions] == list(range(n)),
          "region IDs are 0..N-1 without gaps")

    std = [r for r in regions if r.get("kind") == "ramdisk_std"]
    check(len(std) == 256, f"256 RAMdisk STD banks listed (got {len(std)})")
    pezik = [r for r in regions if r.get("kind") == "ramdisk_pezik"]
    check(len(pezik) == 8, f"8 PEZIK banks listed (got {len(pezik)})")
    last = pezik[-1]
    check(last["id"] > 512, f"last PEZIK bank has an ID above 512 ({last['id']})")

    emu.call("region_write", {"region_id": last["id"], "offset": 0x100,
                              "data_b64": base64.b64encode(PATTERN).decode()})
    rd = emu.call("region_read", {"region_id": last["id"], "offset": 0x100,
                                  "length": len(PATTERN)})
    got = base64.b64decode(rd.get("data_b64", ""))
    check(got == PATTERN, f"region {last['id']} reads back what was written")

    err = _region_read_error(emu, n)
    check(err is not None, f"region_read with ID {n} (= count) fails: {err}")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_regions_full_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text(_ini(tmp), encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            emu.call("pause")
            run(emu)
            ok = True
        finally:
            emu.close()
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        # RAMdisk a FLASH soubory může emulátor dopisovat ještě při ukončení.
        time.sleep(0.2)
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
