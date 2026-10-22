#!/usr/bin/env python3
"""E2E test: postupné hledání proměnných (memsearch) a Freeze Bytes přes MCP pipe.

Ověřuje na skutečném emulátoru (bez hry, data se zapisují přes MCP):

  1  User RAM, emulace v pauze: ``region_write`` 05h na dvě buňky,
     ``memsearch_start`` eq 5, zápis 04h do jedné, ``memsearch_filter``
     dec_by 1 nechá právě tu buňku; ``memsearch_list`` dá ``z80_addr``
     a ``bp_hint`` (MEM_W, zóna RAM), undo/redo vrátí počet kandidátů;
  2  běžící program snižuje bajt na 5000h: filtry ``dec`` za běhu (vzorek
     na hranici snímku, ``sample_frame`` roste) najdou 5000h, ``unchanged``
     bez nového vzorku a ruční vyřazení;
  3  Freeze Bytes: ``freeze_add`` 77h, program na buňku nesahá, po 2 snímcích
     ``region_read`` čte 77h i po přepsání nulou; ``freeze_list`` a
     ``freeze_remove``;
  4  MemExt PEHU (výchozí sestava): výčet regionů má 128 kusů po 4 KB
     (oprava výčtu, dřív 64), hledání v předvolbě ``memext`` najde zápis
     do kusu 41h a ``bp_hint`` je MEM_W v zóně MMEXT_BANK (banka 20h,
     ``bank_offset`` v horní půlce banky);
  5  VRAM plane (MZ-800): kandidát nemá adresu Z80 ani ``bp_hint``;
  6  BCD u16 LE: 0099h -> 0100h projde ``inc_by 1``;
  7  MZ-700 a MZ-1500 (pokud binárky existují): základní průběh nad User RAM.

Procesy izolované (dočasný ``--cfg-dir`` / ``--work-dir``, ``--no-save-ini``,
``cwd`` = kořen repa); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL, 77 = SKIP (chybí binárka).
"""

import base64
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    PipeEmu, TestFailure, _find_exe, check)

PROG = 0x3000
#: DI; LD A,0C8h; LD (5000h),A; smyčka: LD BC,0400h; zpoždění DEC BC / LD A,B /
#: OR C / JR NZ; LD HL,5000h; DEC (HL); JR smyčka.
PROG_DEC = bytes.fromhex("F3" "3EC8" "320050"
                         "010004" "0B" "78" "B1" "20FB" "210050" "35" "18F2")
#: DI; JR $ (program na sledované buňky nesahá).
PROG_IDLE = bytes.fromhex("F3" "18FE")


def regions(emu):
    """Výčet regionů jako slovník (kind, sub_id) -> deskriptor."""
    return {(r["kind"], r["sub_id"]): r for r in emu.call("regions_list")["regions"]}


def rwrite(emu, regs, kind, offset, data, sub_id=0):
    """Zapíše bajty do regionu přes region_write (bez vedlejšího efektu)."""
    rid = regs[(kind, sub_id)]["id"]
    emu.call("region_write", {"region_id": rid, "offset": offset,
                              "data_b64": base64.b64encode(bytes(data)).decode()})


def rread(emu, regs, kind, offset, n, sub_id=0):
    """Přečte bajty z regionu přes region_read."""
    rid = regs[(kind, sub_id)]["id"]
    r = emu.call("region_read", {"region_id": rid, "offset": offset, "length": n})
    return base64.b64decode(r["data_b64"])


def load(emu, prog):
    """Pauza, program na 3000h, PC = 3000h."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": prog.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})


def items(emu, sid, limit=1000):
    """Všechny kandidáty sezení (jedna stránka do 1000)."""
    return emu.call("memsearch_list", {"session_id": sid, "limit": limit})["items"]


def case_paused_ram(emu, label="mz800emu"):
    """Scénář 1: pauza, eq 5 -> dec_by 1, list, undo/redo, close."""
    load(emu, PROG_IDLE)
    regs = regions(emu)
    rwrite(emu, regs, "ram", 0x4080, [5, 5])
    st = emu.call("memsearch_start", {"space": "ram", "type": "u8", "op": "eq", "value": 5})
    sid = st["session_id"]
    n0 = st["candidates"]
    check(n0 >= 2 and st["space_size"] == 0x10000 and st["spans"][0]["kind"] == "ram",
          f"{label}: start eq 5 over User RAM ({n0} candidates)")
    rwrite(emu, regs, "ram", 0x4080, [4])
    f = emu.call("memsearch_filter", {"session_id": sid, "op": "dec_by", "value": 1})
    check(f["candidates"] == 1 and f["removed"] == n0 - 1, f"{label}: dec_by 1 leaves one candidate ({f})")
    it = items(emu, sid)[0]
    check(it["region_offset"] == 0x4080 and it["z80_addr"] == 0x4080 and it["cur"] == 4 and it["prev"] == 5,
          f"{label}: candidate 4080h, 5 -> 4 ({it})")
    hint = it["bp_hint"]
    check(hint and hint["addr"] == 0x4080 and hint["values"]["type"] == "MEM_W"
          and hint["values"]["zone"] == "RAM", f"{label}: write breakpoint hint in zone RAM ({hint})")
    u = emu.call("memsearch_undo", {"session_id": sid})
    check(u["candidates"] == n0, f"{label}: undo restores {n0} candidates")
    r = emu.call("memsearch_redo", {"session_id": sid})
    check(r["candidates"] == 1, f"{label}: redo")
    emu.call("memsearch_close", {"session_id": sid})


def case_running(emu):
    """Scénář 2: běžící program snižuje 5000h; filtry za běhu."""
    load(emu, PROG_DEC)
    emu.call("run", {"frames": 1})
    st = emu.call("memsearch_start", {"space": "ram"})
    sid = st["session_id"]
    frame0 = st["sample_frame"]
    emu.call("run")                                   # běží bez omezení
    try:
        last = frame0
        for _ in range(3):
            f = emu.call("memsearch_filter", {"session_id": sid, "op": "dec"})
            check(f["sample_frame"] > last, f"sample while running at a later frame ({last} -> {f['sample_frame']})")
            last = f["sample_frame"]
    finally:
        emu.call("pause")
    offs = [i["region_offset"] for i in items(emu, sid)]
    check(0x5000 in offs and len(offs) <= 8, f"running decrement found at 5000h ({[hex(o) for o in offs]})")
    # Bez nového vzorku: poslední vzorek proti předchozímu, 5000h se snížila -> unchanged ji vyřadí.
    f = emu.call("memsearch_filter", {"session_id": sid, "op": "unchanged", "sample": False})
    check(0x5000 not in [i["region_offset"] for i in items(emu, sid)],
          f"unchanged without a new sample removes 5000h ({f['candidates']} left)")
    emu.call("memsearch_undo", {"session_id": sid})
    so = [i["space_offset"] for i in items(emu, sid) if i["region_offset"] == 0x5000]
    e = emu.call("memsearch_eliminate", {"session_id": sid, "space_offsets": so})
    check(e["removed"] == 1, "eliminate removes 5000h by hand")
    emu.call("memsearch_close", {"session_id": sid})


def case_freeze(emu):
    """Scénář 3: Freeze Bytes přes MCP."""
    load(emu, PROG_IDLE)
    regs = regions(emu)
    rwrite(emu, regs, "ram", 0x6000, [0, 0])
    a = emu.call("freeze_add", {"kind": "ram", "offset": 0x6000, "bytes": [0x77, 0x78]})
    check(a["added"] == 2 and a["frozen"] >= 2, f"freeze_add 2 bytes ({a})")
    emu.call("run", {"frames": 2})
    emu.call("pause")
    check(rread(emu, regs, "ram", 0x6000, 2) == b"\x77\x78", "frozen bytes written back by the frame hook")
    rwrite(emu, regs, "ram", 0x6000, [0])
    emu.call("run", {"frames": 2})
    emu.call("pause")
    check(rread(emu, regs, "ram", 0x6000, 1) == b"\x77", "frozen byte restored after overwrite")
    lst = emu.call("freeze_list")
    keys = [(x["kind"], x["offset"], x["value"]) for x in lst["entries"]]
    check(("ram", 0x6000, 0x77) in keys and lst["max"] == 256, f"freeze_list ({keys})")
    r = emu.call("freeze_remove", {"all": True})
    check(r["removed"] == lst["count"] and emu.call("freeze_list")["count"] == 0, "freeze_remove all")


def case_pehu(emu):
    """Scénář 4: MemExt PEHU - 128 kusů, hledání v předvolbě memext, bp_hint MMEXT_BANK."""
    load(emu, PROG_IDLE)
    regs = regions(emu)
    pehu = sorted(sid for (k, sid) in regs if k == "memext_ram")
    if not pehu:
        print("  SKIP: MemExt not connected in the default configuration")
        return
    check(len(pehu) == 128 and pehu[-1] == 0x7F, f"PEHU regions: 128 raw 4 KB parts ({len(pehu)})")
    rwrite(emu, regs, "memext_ram", 0x123, [0x5A], sub_id=0x41)
    st = emu.call("memsearch_start", {"space": "memext", "op": "eq", "value": 0x5A})
    sid = st["session_id"]
    rwrite(emu, regs, "memext_ram", 0x123, [0x5B], sub_id=0x41)
    emu.call("memsearch_filter", {"session_id": sid, "op": "inc_by", "value": 1})
    its = [i for i in items(emu, sid) if i["sub_id"] == 0x41 and i["region_offset"] == 0x123]
    check(len(its) == 1, "candidate in PEHU part 41h (bank 20h, upper half)")
    hint = its[0]["bp_hint"]
    check(hint and hint["addr"] == 0x1123 and hint["values"]["zone"] == "MMEXT_BANK"
          and hint["values"]["bank_id"] == 0x20 and hint["values"]["bp_addr_space"] == "bank_offset",
          f"PEHU write breakpoint hint ({hint})")
    emu.call("memsearch_close", {"session_id": sid})


def case_vram_plane(emu):
    """Scénář 5: kandidát ve VRAM plane bez adresy Z80 a bez bp_hint."""
    load(emu, PROG_IDLE)
    regs = regions(emu)
    if ("vram_phys_plane", 0) not in regs:
        print("  SKIP: no VRAM plane region")
        return
    rwrite(emu, regs, "vram_phys_plane", 0x10, [0x3C])
    st = emu.call("memsearch_start", {"space": [{"kind": "vram_phys_plane", "sub_id": 0}],
                                      "op": "eq", "value": 0x3C})
    sid = st["session_id"]
    rwrite(emu, regs, "vram_phys_plane", 0x10, [0x3D])
    emu.call("memsearch_filter", {"session_id": sid, "op": "changed"})
    its = items(emu, sid)
    check(len(its) == 1 and its[0]["region_offset"] == 0x10 and its[0]["z80_addr"] is None
          and its[0]["bp_hint"] is None, f"VRAM plane candidate without CPU address ({its})")
    emu.call("memsearch_close", {"session_id": sid})


def case_bcd(emu):
    """Scénář 6: BCD u16 LE 0099h -> 0100h."""
    load(emu, PROG_IDLE)
    regs = regions(emu)
    rwrite(emu, regs, "ram", 0x7000, [0x99, 0x00])
    st = emu.call("memsearch_start", {"space": [{"kind": "ram", "offset": 0x7000, "length": 0x100}],
                                      "type": "bcd", "width": 2, "op": "eq", "value": 99})
    sid = st["session_id"]
    rwrite(emu, regs, "ram", 0x7000, [0x00, 0x01])
    f = emu.call("memsearch_filter", {"session_id": sid, "op": "inc_by", "value": 1})
    its = items(emu, sid)
    check(f["candidates"] == 1 and its[0]["region_offset"] == 0x7000 and its[0]["cur"] == 100,
          f"BCD 99 -> 100 ({its})")
    emu.call("memsearch_close", {"session_id": sid})


def case_platform(name, tmp):
    """Scénář 7: základní průběh na MZ-700 / MZ-1500."""
    exe = _find_exe(name)
    if not exe:
        print(f"  SKIP: {name} binary not found")
        return
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        case_paused_ram(emu, name)
    finally:
        emu.close()


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_memsearch_"))
    ok = False
    try:
        d = tmp / "main"
        d.mkdir()
        emu = PipeEmu(exe, d)
        try:
            case_paused_ram(emu)
            case_running(emu)
            case_freeze(emu)
            case_pehu(emu)
            case_vram_plane(emu)
            case_bcd(emu)
        finally:
            emu.close()
        for name in ("mz700emu-pal", "mz1500emu"):
            case_platform(name, tmp)
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
