#!/usr/bin/env python3
"""E2E test: záznam zápisů do paměti (ramlog) přes MCP pipe na skutečném emulátoru.

Ověřuje:

  1  MZ-800, program ``LD (nn),A``, ``LD (HL),n``, ``PUSH``, ``LDIR``:
     ``insn_pc`` = začátek zapisující instrukce, old/new, pořadí bajtů PUSH
     (vyšší bajt dřív, Zilog UM0080)
     podle jádra (nižší bajt dřív), ``ramlog_who_wrote`` a ``ramlog_writers``;
     MCP ``mem_write`` = zdroj ``debugger``;
  2  malý ring: ``fill`` počítá ``dropped``, ``ring`` ``overwritten``;
     poslední zapisovatel z mapy i pro přepsaný záznam (``in_ring: false``);
  3  přijetí přerušení (IM 2 z CTC2): push návratové adresy se zdrojem
     ``int_ack``, ``insn_pc`` = vložená návratová adresa;
  4  banking: stejná CPU adresa 8000h jednou do RAM, po ``IN A,(E0h)`` do
     grafické VRAM; WF REPLACE (``OUT (CCh),81h``) zapíše plán I hodnotou
     a plán II nulou;
  5  reset a načtení snapshotu záznam vyprázdní (nový segment, starý kurzor
     -> ``segment_replaced:``);
  6  bitová shoda: stejný program 60 snímků se zapnutým (celá paměť)
     a vypnutým ramlogem dá stejné registry a paměť;
  7  MZ-700 a MZ-1500 (pokud binárky existují): RAM a VRAM D000h;
  8  spuštění z konfigurace: INI ``[RAMLOG] mode = ALWAYS`` s rozsahem
     a režimem ``fill``, CLI ``--ramlog-*`` přebije INI.

Porty a režimy podle zdroje emulátoru (mz800_memory.c memory_map_pread,
mz800_vramctrl.c vramctrl_mz800_set_wf_rf_reg). Procesy izolované
(dočasný ``--cfg-dir`` / ``--work-dir``, ``--no-save-ini``); zabíjí se jen
vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import shutil
import subprocess
import sys
import threading
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_snapshot_gdg_event import (  # noqa: E402
    _REPO_ROOT, PipeEmu, TestFailure, _find_exe, check)
from test_cli_speed import TcpEmu  # noqa: E402 - headless TCP s vlastními volbami

PROG = 0x3000
#: Program z rešerše: DI; LD A,55h; LD (4000h),A; LD HL,4001h; LD (HL),66h;
#: LD SP,4010h; LD BC,1234h; PUSH BC; LD HL,4000h; LD DE,4020h; LD BC,2;
#: LDIR; JR $.
PROG_BASIC = bytes.fromhex("F3" "3E55" "320040" "210140" "3666" "311040" "013412" "C5"
                           "210040" "112040" "010200" "EDB0" "18FE")
#: LD HL,0; LD DE,5000h; LD BC,100; LDIR (na 300Ah); JR $.
PROG_LDIR100 = bytes.fromhex("F3" "210000" "110050" "016400" "EDB0" "18FE")
#: DI; LD SP,4100h; IM 2; I = 32h; CTC1/CTC2 jako test_trace_read (trvalé INT
#: z CTC2); EI; LD HL,5000h; smyčka LD (HL),C; INC HL; H = 50h-5Fh; INC C.
PROG_INT = bytes.fromhex(
    "F3" "310041" "ED5E" "3E32" "ED47"
    "3E04" "3203E0"
    "3E74" "3207E0" "3E02" "3205E0" "AF" "3205E0"
    "3EB0" "3207E0" "3E01" "3206E0" "AF" "3206E0"
    "3E05" "3203E0"
    "0600" "10FE"
    "FB" "210050"
    "71" "23" "7C" "E60F" "F650" "67" "0C" "18F5")
#: ISR na 3434h: INC D; EI; RETI; tabulka IM 2 3200h-3300h -> 3434h.
ISR = bytes.fromhex("14" "FB" "ED4D")
#: DI; IN A,(E1h) (VRAM pryč); LD A,11h; LD (8000h),A (RAM); IN A,(E0h)
#: (VRAM 8000h); XOR A; OUT (CEh),A (DMD 320x200); LD A,81h; OUT (CCh),A
#: (WF REPLACE, plán I); LD A,0AAh; LD (8000h),A (na 3013h); JR $.
PROG_BANK = bytes.fromhex("F3" "DBE1" "3E11" "320080" "DBE0" "AF" "D3CE" "3E81" "D3CC"
                          "3EAA" "320080" "18FE")
#: MZ-700 / MZ-1500: LD A,55h; LD (4000h),A; LD A,41h; LD (0D000h),A; JR $.
PROG_700 = bytes.fromhex("F3" "3E55" "320040" "3E41" "3200D0" "18FE")


def rows(emu, columns, flt=None, start="oldest"):
    """Všechny řádky kanálu ramlog (stránkami)."""
    out, cursor = [], None
    for _ in range(1000):
        req = {"channel": "ramlog", "columns": columns, "limit": 5000}
        if flt:
            req["filter"] = flt
        if cursor:
            req["cursor"] = cursor
        else:
            req["from"] = start
        r = emu.call("trace_read", req)
        out.extend(r["rows"])
        cursor = r["next_cursor"]
        if r["end_of_data"]:
            return out, r
    raise TestFailure("trace_read ramlog did not reach end_of_data")


def load(emu, prog, extra=None):
    """Pauza, program na 3000h (+ další bloky {adresa: bajty}), PC = 3000h."""
    emu.call("pause")
    for addr, data in (extra or {}).items():
        emu.call("mem_write", {"addr": addr, "data_hex": data.hex()})
    emu.call("mem_write", {"addr": PROG, "data_hex": prog.hex()})
    emu.call("set_register", {"reg": "PC", "value": PROG})


def case_basic(emu):
    """Scénář 1: insn_pc, old/new, PUSH, LDIR, dotazy, zdroj debugger."""
    load(emu, PROG_BASIC, {0x4000: b"\xAA" * 0x30})
    st = emu.call("ramlog_start", {"ranges": [{"from": 0x4000, "to": 0x402F}]})
    check(st["active"] and st["next_seq"] == 0, "ramlog started on 4000h-402Fh")
    emu.call("trace_start", {"channel": "cputrack"})
    emu.call("run", {"frames": 1})
    emu.call("pause")
    emu.call("trace_stop", {"channel": "cputrack"})
    # Čas t = začátek zapisující instrukce (stejný jako t cputrack),
    # tin = T-stav jádra uvnitř instrukce v okamžiku zápisu.
    ct = emu.call("trace_read", {"channel": "cputrack", "columns": ["pc", "t"], "limit": 20})
    t_insn = {}
    for pc, t in ct["rows"]:
        t_insn.setdefault(pc, t)
    tt, _ = rows(emu, ["t", "pc", "tin"])
    check(all(t == t_insn.get(pc) for t, pc, _ in tt[:5]),
          f"t = start of the writing instruction as in cputrack ({tt[:5]}, {t_insn})")
    check(tt[0][2] == 13 and tt[2][2] < tt[3][2] <= 11,
          f"tin within the instruction: LD (nn),A 13, PUSH bytes in write order ({tt})")
    got, last = rows(emu, ["pc", "addr", "old", "new", "src"])
    want = [["3003", "4000", "AA", "55", "insn"], ["3009", "4001", "AA", "66", "insn"],
            ["3011", "400F", "AA", "12", "insn"], ["3011", "400E", "AA", "34", "insn"],
            ["301B", "4020", "AA", "55", "insn"], ["301B", "4021", "AA", "66", "insn"]]
    check(got == want, f"rows: start of writing instruction, old/new, PUSH high byte first ({got})")
    w = emu.call("ramlog_who_wrote", {"addr": 0x4001})
    cell = w["cells"][0]
    check(cell["last"]["pc"] == "3009" and cell["last"]["in_ring"] and len(cell["history"]) == 1,
          f"who_wrote(4001h) = 3009h ({cell['last']})")
    wr = emu.call("ramlog_writers", {"from": 0x4000, "to": 0x402F})
    check(wr["total"] == 4 and wr["writers"][0]["count"] == 2,
          f"writers: 4 instructions, most writes first ({wr['writers'][:2]})")
    emu.call("mem_write", {"addr": 0x4005, "data_hex": "77"})
    newest, _ = rows(emu, ["pc", "addr", "new", "src"], start="newest")
    check(newest[-1] == ["0000", "4005", "77", "debugger"], f"mem_write recorded as debugger ({newest[-1:]})")
    return last["next_cursor"], last["segment"]


def case_overflow(emu):
    """Scénář 2: malý ring fill a ring, mapa i pro přepsané."""
    for mode in ("fill", "ring"):
        load(emu, PROG_LDIR100)
        emu.call("ramlog_start", {"ranges": [{"from": 0x5000, "to": 0x50FF}], "mode": mode, "capacity": 16})
        emu.call("run", {"frames": 1})
        emu.call("pause")
        st = emu.call("ramlog_status")
        if mode == "fill":
            check(st["total"] == 100 and st["next_seq"] == 16 and st["dropped"] == 84,
                  f"fill: 16 kept, 84 dropped ({st['next_seq']}, {st['dropped']})")
        else:
            check(st["next_seq"] == 100 and st["first_seq"] == 84 and st["overwritten"] == 84,
                  f"ring: newest 16 kept, 84 overwritten ({st['first_seq']})")
        cell = emu.call("ramlog_who_wrote", {"addr": 0x5000})["cells"][0]
        check(cell["last"]["pc"] == "300A" and cell["last"]["in_ring"] == (mode == "fill"),
              f"{mode}: last writer of 5000h from the map ({cell['last']})")


def case_int_ack(emu):
    """Scénář 3: push návratové adresy při přijetí přerušení."""
    load(emu, PROG_INT, {0x3200: b"\x34" * 257, 0x3434: ISR})
    emu.call("ramlog_start", {"ranges": [{"from": 0x40F0, "to": 0x40FF}]})
    emu.call("run", {"frames": 10})
    emu.call("pause")
    # Návratová adresa se opakuje: od druhého přijetí jsou zápisy beze změny
    # (ve výchozím výpisu skryté), proto all.
    got, _ = rows(emu, ["seq", "pc", "addr", "new", "src"], flt={"src": "int_ack", "all": True})
    check(len(got) >= 4 and len(got) % 2 == 0, f"interrupt acknowledge pushes recorded ({len(got)} rows)")
    # Vyšší bajt návratové adresy se zapisuje dřív (Zilog UM0080).
    hi, lo = got[0], got[1]
    ret = int(hi[3], 16) << 8 | int(lo[3], 16)
    check(hi[2] == "40FF" and lo[2] == "40FE" and int(lo[1], 16) == ret,
          f"int_ack insn_pc = pushed return address {ret:04X}h ({lo}, {hi})")


def case_banking(emu):
    """Scénář 4: 8000h do RAM, pak do grafické VRAM (WF REPLACE: plán I a II)."""
    load(emu, PROG_BANK)
    emu.call("ramlog_start", {"ranges": [{"from": 0x8000, "to": 0x8000}]})
    emu.call("run", {"frames": 1})
    emu.call("pause")
    got, _ = rows(emu, ["pc", "space", "offset", "plane", "new"])
    check(len(got) == 3, f"3 records ({got})")
    check(got[0][0] == "3005" and got[0][1] in ("ram", "memext") and got[0][4] == "11",
          f"first write: RAM cell ({got[0]})")
    check(got[1] == ["3013", "vram", 0, 0, "AA"] and got[2] == ["3013", "vram", 0x2000, 1, "00"],
          f"VRAM write: plane I value, plane II zero (REPLACE) ({got[1:]})")
    cells = emu.call("ramlog_who_wrote", {"addr": 0x8000})["cells"]
    check(len(cells) == 4 and cells[0]["last"]["pc"] == "3013" and cells[2]["last"] is None,
          f"who_wrote(8000h) with VRAM mapped: 4 planes ({[c['last'] for c in cells]})")


def case_reset_snapshot(emu, tmp):
    """Scénář 5: reset a snapshot load = nový segment, starý kurzor neplatí."""
    load(emu, PROG_BASIC)
    emu.call("ramlog_start", {"ranges": [{"from": 0x4000, "to": 0x402F}]})
    emu.call("run", {"frames": 1})
    emu.call("pause")
    _, last = rows(emu, ["seq"])
    seg = last["segment"]
    cursor = last["next_cursor"]
    emu.call("reset")
    st = emu.call("ramlog_status")
    check(st["active"] and st["segment"] == seg + 1 and st["next_seq"] == 0,
          f"reset clears the log, recording continues ({st['segment']}, {st['next_seq']})")
    try:
        emu.call("trace_read", {"channel": "ramlog", "cursor": cursor})
        raise TestFailure("old cursor accepted after reset")
    except TestFailure as e:
        check("segment_replaced:" in str(e), "old cursor after reset -> segment_replaced")
    snap = tmp / "rl.mzs"
    emu.call("pause")
    emu.call("snapshot_save", {"path": str(snap)})
    load(emu, PROG_BASIC)
    emu.call("run", {"frames": 1})
    emu.call("pause")
    before = emu.call("ramlog_status")
    emu.call("snapshot_load", {"path": str(snap)})
    st = emu.call("ramlog_status")
    check(before["next_seq"] > 0 and st["next_seq"] == 0 and st["segment"] == before["segment"] + 1,
          f"snapshot load clears the log ({before['next_seq']} -> {st['next_seq']})")
    emu.call("ramlog_stop")


def state(emu):
    """Registry a celá logická paměť."""
    regs = emu.call("get_registers")
    mem = b""
    for a in (0x0000, 0x8000):
        mem += base64.b64decode(emu.call("mem_read", {"addr": a, "len": 0x8000})["data_b64"])
    return regs, mem


def case_bit_identity(exe, tmp):
    """Scénář 6: se zapnutým ramlogem (celá paměť) stejný stav jako bez něj."""
    res = []
    for on in (False, True):
        d = tmp / ("bit_" + str(on))
        d.mkdir()
        emu = PipeEmu(exe, d)
        try:
            load(emu, PROG_INT, {0x3200: b"\x34" * 257, 0x3434: ISR})
            if on:
                emu.call("ramlog_start", {"capacity": 1 << 20})
            emu.call("run", {"frames": 60})
            emu.call("pause")
            res.append(state(emu))
            if on:
                st = emu.call("ramlog_status")
                check(st["total"] > 1000, f"ramlog on: {st['total']} writes recorded")
        finally:
            emu.close()
    check(res[0][0] == res[1][0], "registers equal with and without ramlog")
    check(res[0][1] == res[1][1], "memory equal with and without ramlog")


def case_platform(name, tmp):
    """Scénář 7: MZ-700 / MZ-1500 - RAM a VRAM D000h."""
    exe = _find_exe(name)
    if not exe:
        print(f"  SKIP: {name} binary not found")
        return
    d = tmp / name
    d.mkdir()
    emu = PipeEmu(exe, d)
    try:
        load(emu, PROG_700)
        emu.call("ramlog_start", {"ranges": [{"from": 0x4000, "to": 0x4000}, {"from": 0xD000, "to": 0xD000}]})
        emu.call("run", {"frames": 1})
        emu.call("pause")
        got, _ = rows(emu, ["pc", "space", "offset", "new"])
        check(len(got) == 2 and got[0][0] == "3003" and got[0][1] in ("ram", "memext") and got[0][3] == "55"
              and got[1] == ["3008", "vram", 0, "41"], f"{name}: RAM and VRAM D000h ({got})")
    finally:
        emu.close()


class PipeEmuCfg(PipeEmu):
    """PipeEmu s vlastním obsahem INI a dalšími volbami příkazové řádky."""

    @emu_test_proc.kill_on_init_failure
    def __init__(self, exe, tmp, ini_text, extra):
        ini = tmp / "emu.ini"
        ini.write_text(ini_text, encoding="utf-8")
        args = [str(exe), "--mcp-pipe", "--headless", "--no-save-ini",
                "--no-first-run-windows", f"--cfg-dir={tmp}",
                f"--work-dir={tmp}", f"--config={ini}"] + list(extra)
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL, text=True, bufsize=1,
                                     encoding="utf-8", cwd=str(_REPO_ROOT))
        import queue
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()
        self.rid = 0
        self.dead = False
        if not self._read(20.0, lambda m: m.get("type") == "hello"):
            raise TestFailure("no hello from emulator")


def case_config(exe, tmp):
    """Scénář 8: [RAMLOG] v INI spustí záznam od startu, CLI přebije INI."""
    # Čísla v INI jsou vždy hex (cfgmodule.c, prefix 0x volitelný).
    ini = ("[RAMLOG]\nmode = ALWAYS\nring = FILL\ncapacity = 0x40\n"
           "range_lo = 0x4000\nrange_hi = 0x403F\n")
    # Pipe režim přijímá jen minimum voleb (main_pipe.c), volby --ramlog-*
    # proto přes headless TCP (volný port od systému, ne 23800).
    cli = ["--ramlog-capacity=128", "--ramlog-ring=ring", "--ramlog-lo=0x5001", "--ramlog-hi=0x5000"]
    for name, extra, want in (
            ("ini", None, (True, 64, "fill", 0x4000, 0x403F)),
            ("cli", cli, (True, 128, "ring", 0x5000, 0x5001)),
            ("off", ["--ramlog-mode=off"], (False, 64, "fill", 0x4000, 0x403F))):
        d = tmp / ("cfg_" + name)
        d.mkdir()
        if extra is None:
            emu = PipeEmuCfg(exe, d, ini, [])
        else:
            (d / "emu.ini").write_text(ini, encoding="utf-8")
            emu = TcpEmu(exe, d, d / "emu.ini", None, extra)
        try:
            st = emu.call("ramlog_status")
            r = st["ranges"][0] if st["ranges"] else {}
            got = (st["active"], st["capacity"], st["mode"], r.get("from"), r.get("to"))
            if name == "off":
                check(not st["active"] and not st["allocated"], f"--ramlog-mode=off overrides INI {got}")
            else:
                check(got == want, f"{name}: started from configuration {got}")
        finally:
            emu.close()


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe("mz800emu")
    if not exe:
        print("SKIP: mz800emu binary not found")
        return 77
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_ramlog_"))
    ok = False
    try:
        d = tmp / "main"
        d.mkdir()
        emu = PipeEmu(exe, d)
        try:
            case_basic(emu)
            case_overflow(emu)
            case_int_ack(emu)
            case_banking(emu)
            case_reset_snapshot(emu, tmp)
        finally:
            emu.close()
        case_bit_identity(exe, tmp)
        case_config(exe, tmp)
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
