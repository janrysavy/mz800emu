#!/usr/bin/env python3
"""E2E test: state_observe - registry a okna paměti z jednoho okamžiku (MCP pipe).

Ověřuje:

  - za běhu vrací ``state_observe`` registry a okno paměti ze stejného
    okamžiku: smyčka ``INC HL; LD (3000h),HL`` drží invariant mezi PC, HL
    a slovem na 3000h ve všech vzorcích; kontrolní běh se dvěma
    samostatnými voláními (``get_cpu_panel_batch`` + ``mem_read``)
    invariant poruší, takže test neshodu umí zachytit,
  - v pauze ``stable: true``, ``drain: "paused"``, ``run_seq`` se po kroku
    zvýší o 1; ``sha256`` okna se rovná ``sha256`` z ``mem_read`` téhož
    rozsahu i ``hashlib`` ze ``data_b64``,
  - okno regionu podle druhu a ``sub_id`` (``ram``) vrátí stejná data
    jako okno přes CPU adresu; neexistující region = chyba okna
    ``region_unavailable`` a ``complete: false``, volání uspěje,
  - limity: 17 oken, součet nad 65536 B, okno přes konec adresního
    prostoru nebo regionu, neznámý druh a neznámá sekce = chyba
    ``Invalid parameters``,
  - ``hashes: ["cpu", "ram"]`` vrátí hashe částí stavu, ``include`` vrátí
    ``raster`` a ``memory_map``.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import hashlib
import json
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

#: Testovací program na 2000h: DI; LD HL,0; L: INC HL; LD (3000h),HL; JR L.
PROG = 0x2000
PROG_HEX = "F3 21 00 00 23 22 00 30 18 FA"
#: Adresa instrukce LD (3000h),HL: na ní je slovo na 3000h o 1 menší než HL.
PC_BEFORE_STORE = 0x2005
#: Slovo, které program přepisuje.
WORD = 0x3000

#: Počet vzorků konzistence za běhu (každý vzorek čeká na konec snímku).
SAMPLES = 60


class Emu(PipeEmu):
    """PipeEmu s voláním, které vrátí i chybovou odpověď."""

    def call_raw(self, cmd, data=None, timeout=30.0):
        """Pošle request a vrátí celou response (úspěch i chybu)."""
        self.rid += 1
        rid = self.rid
        req = {"type": "request", "req_id": rid, "cmd": cmd, "data": data or {}}
        self.proc.stdin.write(json.dumps(req) + "\n")
        self.proc.stdin.flush()
        resp = self._read(timeout, lambda m: m.get("type") == "response"
                          and m.get("req_id") == rid)
        if resp is None:
            raise TestFailure(f"no response to {cmd} (emulator exit code: {self.proc.poll()})")
        return resp


def _word(data_b64):
    """Slovo little endian z base64 dat okna."""
    b = base64.b64decode(data_b64)
    return b[0] | (b[1] << 8)


def _consistent(pc, hl, word):
    """Invariant smyčky: před LD (3000h),HL je slovo HL - 1, jinde HL."""
    expected = (hl - 1) & 0xFFFF if pc == PC_BEFORE_STORE else hl
    return word == expected


def _start_loop(emu):
    """Nahraje smyčku, nastaví PC a rozběhne emulaci."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("mem_write", {"addr": WORD, "data_hex": "0000"})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {})


def case_running_consistency(emu):
    """Za běhu: observe drží invariant, dvě volání ho poruší."""
    print("Case: consistency while running")
    _start_loop(emu)
    bad = 0
    frames = set()
    for _ in range(SAMPLES):
        r = emu.call("state_observe", {"windows": [{"addr": WORD, "len": 2}]})
        pc, hl = r["regs"]["PC"], r["regs"]["HL"]
        if not _consistent(pc, hl, _word(r["windows"][0]["data_b64"])):
            bad += 1
        frames.add(r["time"]["frame"])
        if not (r["running"] and not r["stable"] and r["drain"] == "frame"):
            raise TestFailure(f"running observe flags: {r['running']} {r['stable']} {r['drain']}")
    check(bad == 0, f"state_observe consistent in {SAMPLES - bad}/{SAMPLES} samples")
    check(len(frames) > 1, f"samples come from {len(frames)} different frames")

    bad_two = 0
    for _ in range(SAMPLES // 2):
        p = emu.call("get_cpu_panel_batch")
        m = emu.call("mem_read", {"addr": WORD, "len": 2})
        if not _consistent(p["regs"]["PC"], p["regs"]["HL"], _word(m["data_b64"])):
            bad_two += 1
    check(bad_two > 0, f"two separate calls are inconsistent in {bad_two}/{SAMPLES // 2} "
                       "samples (the test can detect a mismatch)")
    emu.call("pause")


def case_paused(emu):
    """V pauze: stable, run_seq po kroku, hash okna = mem_read = hashlib."""
    print("Case: paused observe, run_seq and window hash")
    emu.call("pause")
    r = emu.call("state_observe", {"windows": [{"name": "loop", "addr": PROG, "len": 10}]})
    check(r["running"] is False and r["stable"] is True and r["drain"] == "paused",
          f"paused: running={r['running']} stable={r['stable']} drain={r['drain']}")
    w = r["windows"][0]
    data = base64.b64decode(w["data_b64"])
    check(w.get("name") == "loop" and w.get("addr") == PROG and w.get("len") == 10,
          f"window echo ({sorted(w)})")
    check(w["sha256"] == hashlib.sha256(data).hexdigest(), "window sha256 matches hashlib")
    m = emu.call("mem_read", {"addr": PROG, "len": 10, "want_data": False})
    check(w["sha256"] == m["sha256"], "window sha256 equals mem_read sha256")
    check(r["complete"] is True, "complete: true")
    seq = r["run_seq"]
    emu.call("step_into")
    r2 = emu.call("state_observe", {"windows": [{"addr": PROG, "len": 10, "want_data": False}]})
    check(r2["run_seq"] == seq + 1, f"run_seq after a step: {seq} -> {r2['run_seq']}")
    check("data_b64" not in r2["windows"][0] and r2["windows"][0]["sha256"] == w["sha256"],
          "want_data false per window: hash only")
    g = emu.call("state_observe", {"windows": [{"addr": PROG, "len": 4}], "want_data": False})
    check("data_b64" not in g["windows"][0], "global want_data false")


def case_region_windows(emu):
    """Okno regionu podle druhu a sub_id, nedostupný region."""
    print("Case: region windows")
    r = emu.call("state_observe", {"windows": [
        {"addr": PROG, "len": 10},
        {"region": {"kind": "ram", "sub_id": 0}, "offset": PROG, "length": 10},
        {"name": "gone", "region": {"kind": "ramdisk_pezik", "sub_id": 200},
         "offset": 0, "length": 16},
    ]})
    cpu_w, reg_w, gone = r["windows"]
    check(reg_w["sha256"] == cpu_w["sha256"], "region ram window equals CPU window")
    check(reg_w.get("region") == {"kind": "ram", "sub_id": 0} and reg_w.get("length") == 10,
          f"region window echo ({reg_w.get('region')}, {reg_w.get('length')})")
    check(gone.get("error") == "region_unavailable" and "sha256" not in gone
          and gone.get("name") == "gone", f"unavailable region window ({gone})")
    check(r["complete"] is False, "complete: false with an unavailable window")


def case_limits(emu):
    """Neplatné parametry = chyba celého volání, nic se nečte."""
    print("Case: parameter limits")
    bad = {
        "17 windows": {"windows": [{"addr": 0, "len": 1}] * 17},
        "sum over 64 KiB": {"windows": [{"addr": 0, "len": 0x8000},
                                        {"addr": 0x8000, "len": 0x8000},
                                        {"addr": 0, "len": 1}]},
        "past 0FFFFh": {"windows": [{"addr": 0xFFFF, "len": 2}]},
        "len 0": {"windows": [{"addr": 0, "len": 0}]},
        "unknown kind": {"windows": [{"region": {"kind": "nope", "sub_id": 0},
                                      "offset": 0, "length": 1}]},
        "past region end": {"windows": [{"region": {"kind": "ram", "sub_id": 0},
                                         "offset": 0xFFFF, "length": 2}]},
        "unknown section": {"windows": [], "include": ["bogus"]},
        "unknown hash part": {"windows": [], "hashes": ["screen"]},
        "windows not a list": {"windows": 5},
    }
    for what, data in bad.items():
        resp = emu.call_raw("state_observe", data)
        check(not resp.get("success") and "Invalid parameters" in (resp.get("error") or ""),
              f"{what}: {resp.get('error')}")
    full = emu.call("state_observe", {"windows": [{"addr": 0, "len": 0x8000},
                                                  {"addr": 0x8000, "len": 0x8000}],
                                      "want_data": False})
    check(len(full["windows"]) == 2, "exactly 64 KiB in two windows is accepted")


def case_sections(emu):
    """Volitelné sekce a hashe částí stavu."""
    print("Case: include sections and state hashes")
    r = emu.call("state_observe", {"windows": [], "include": ["raster", "memory_map"],
                                   "hashes": ["cpu", "ram"]})
    check("raster" in r and "scanline" in r["raster"], "raster section present")
    check("memory_map" in r and len(r["memory_map"].get("slots", [])) == 16,
          "memory_map section with 16 slots")
    h = r.get("hashes") or {}
    check(len(h.get("cpu") or "") == 64 and len(h.get("ram") or "") == 64,
          f"cpu and ram hashes ({sorted(h)})")
    check("im2" not in r and "last_instr" not in r, "sections not asked for are absent")
    check(r["windows"] == [] and r["complete"] is True, "no windows: empty list, complete")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=100)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_state_observe_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n",
                       encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = Emu(exe, cfg, ini)
        try:
            case_running_consistency(emu)
            case_paused(emu)
            case_region_windows(emu)
            case_limits(emu)
            case_sections(emu)
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
