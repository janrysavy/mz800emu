#!/usr/bin/env python3
"""E2E test: staré region_id po změně HW skončí chybou, ne pádem (MCP pipe).

ID regionu je index do výčtu ``regions_list``. Načtení snapshotu s menším
RAMdiskem STD výčet změní. Dřív cache regionů zůstala ze starého výčtu
a ``region_read`` se starým ID banky 255 četl za koncem zmenšeného bufferu
RAMdisku (access violation, proces skončil kódem 0xC0000005). Ověřuje:

  - ``region_read`` i ``region_write`` se starým ID po změně HW skončí
    chybou ``stale region id``, proces běží dál,
  - chybu dostane i staré ID regionu, který po změně dál existuje
    (banka 0), protože ID se mohla posunout,
  - po novém ``regions_list`` čtení s novými ID funguje.

Proces A (RAMdisk 64K) uloží snapshot, proces B (RAMdisk 16M) ho načte.
Procesy běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402


#: Binárka emulátoru (nastaví main()).
_EXE = None


def _ini(tmp, size):
    """INI s RAMdiskem STD dané velikosti; soubory v dočasném adresáři."""
    return (
        "[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n"
        "[RAMDISK]\nmr1r18_pluged = 1\nmr1r18_type = STANDARD\n"
        f"mr1r18_size = {size}\n"
        f"mr1r18_filepath = {(tmp / 'rd-std.dat').as_posix()}\n"
        "pezik_e8_pluged = 0\npezik_68_pluged = 0\n"
    )


def _start(tmp, name, size):
    """Spustí izolovaný proces s RAMdiskem STD velikosti @p size."""
    d = tmp / name
    (d / "cfg").mkdir(parents=True)
    ini = d / "test.ini"
    ini.write_text(_ini(d, size), encoding="utf-8")
    return PipeEmu(_EXE, d / "cfg", ini)


def _std_banks(emu):
    """Regiony RAMdisku STD z čerstvého regions_list."""
    regions = emu.call("regions_list").get("regions") or []
    return [r for r in regions if r.get("kind") == "ramdisk_std"]


def _request(emu, cmd, data):
    """Pošle request a vrátí (success, error) bez výjimky při chybě."""
    emu.rid += 1
    rid = emu.rid
    req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data}
    emu.proc.stdin.write(json.dumps(req) + "\n")
    emu.proc.stdin.flush()
    resp = emu._read(20.0, lambda m: m.get("type") == "response"
                     and m.get("req_id") == rid)
    if resp is None:
        raise TestFailure(f"no response to {cmd} (emulator exit code: "
                          f"{emu.proc.poll()})")
    return bool(resp.get("success")), resp.get("error") or ""


def run(tmp):
    """Snapshot s RAMdiskem 64K načtený do procesu s 16M, pak stará ID."""
    snap = tmp / "rd64k.mzs"
    a = _start(tmp, "a", "64K")
    try:
        a.call("pause")
        check(len(_std_banks(a)) == 1, "process A has 1 RAMdisk STD bank")
        a.call("snapshot_save", {"path": str(snap)})
    finally:
        a.close()

    b = _start(tmp, "b", "16M")
    try:
        b.call("pause")
        banks = _std_banks(b)
        check(len(banks) == 256, f"process B has 256 RAMdisk STD banks "
                                 f"(got {len(banks)})")
        first, last = banks[0], banks[-1]
        b.call("region_read", {"region_id": last["id"], "offset": 0,
                               "length": 16})

        b.call("snapshot_load", {"path": str(snap)})

        ok, err = _request(b, "region_read", {"region_id": last["id"],
                                              "offset": 0, "length": 65536})
        print(f"  region_read old ID {last['id']}: success={ok} error={err!r}")
        check(not ok and "stale region id" in err,
              "region_read with a stale ID fails with 'stale region id'")
        check(b.proc.poll() is None, "emulator still running after stale read")

        ok, err = _request(b, "region_write", {
            "region_id": last["id"], "offset": 0,
            "data_b64": base64.b64encode(b"\x55" * 16).decode()})
        print(f"  region_write old ID {last['id']}: success={ok} error={err!r}")
        check(not ok and "stale region id" in err,
              "region_write with a stale ID fails with 'stale region id'")

        ok, err = _request(b, "region_read", {"region_id": first["id"],
                                              "offset": 0, "length": 16})
        print(f"  region_read old ID {first['id']} (bank 0): success={ok} "
              f"error={err!r}")
        check(not ok and "stale region id" in err,
              "an old ID of a region that still exists is stale too")

        banks = _std_banks(b)
        check(len(banks) == 1, f"fresh regions_list has 1 RAMdisk STD bank "
                               f"(got {len(banks)})")
        rd = b.call("region_read", {"region_id": banks[0]["id"], "offset": 0,
                                    "length": 16})
        check(rd.get("length") == 16, "region_read with a fresh ID works")
        check(b.proc.poll() is None, "emulator still running at the end")
    finally:
        b.close()


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=105)
    global _EXE
    _EXE = _find_exe()
    print(f"Using binary: {_EXE}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_regions_stale_"))
    ok = False
    try:
        run(tmp)
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        # RAMdisk soubory může emulátor dopisovat ještě při ukončení.
        time.sleep(0.2)
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
