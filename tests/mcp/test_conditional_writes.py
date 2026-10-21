#!/usr/bin/env python3
"""E2E test: podmíněné zápisy (expected_sha256, expected) přes MCP pipe.

Ověřuje:

  - ``mem_write`` s ``expected_sha256`` z ``mem_read`` zapíše a vrátí
    ``before_sha256`` (= očekávaný) a ``after_sha256`` (= hashlib nových
    dat); druhý zápis se stejným, už starým hashem skončí chybou
    ``Precondition failed:`` a paměť se nezmění; ``want_hashes`` bez
    podmínky; ``mem_write_force`` a ``region_write`` totéž,
  - ``set_register`` s ``expected`` (CAS) a ``previous``, ``set_cpu_flags``
    s objektem ``expected``,
  - za běhu: podmíněný zápis do slova, které program mění, se odmítne;
    do neměnného rozsahu projde (kontrola i zápis v témže drainu),
  - záznam vstupů: odmítnutý podmíněný zápis se nezaznamená, splněný ano;
    ``--replay-verify`` záznamu skončí kódem 0.

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
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
# Sdílená infrastruktura pipe testu (spuštění emulátoru, request/response).
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402
from test_replay import _ini, cli  # noqa: E402

#: Testovací program na 2000h: DI; LD HL,0; L: INC HL; LD (3000h),HL; JR L.
PROG = 0x2000
PROG_HEX = "F3 21 00 00 23 22 00 30 18 FA"
#: Slovo, které program přepisuje.
WORD = 0x3000
#: Rozsah, na který program nesahá.
STATIC = 0x3100
#: Prefix chyby nesplněné podmínky.
PRECOND = "Precondition failed:"


def _sha(data):
    """SHA-256 bajtů jako hex."""
    return hashlib.sha256(data).hexdigest()


def _expect_precondition(emu, cmd, data, needle):
    """Příkaz musí skončit chybou Precondition failed: ... needle."""
    try:
        emu.call(cmd, data)
    except TestFailure as e:
        msg = str(e)
        check(PRECOND in msg and needle in msg, f"{cmd} rejected: {msg[msg.find(PRECOND):][:150]}")
        return
    raise TestFailure(f"{cmd} was accepted although the precondition does not hold")


def case_memory(emu):
    """mem_write, mem_write_force, region_write s podmínkou v pauze."""
    print("Case: conditional memory writes in pause")
    emu.call("pause")
    emu.call("mem_write", {"addr": STATIC, "data_hex": "01020304"})
    h = emu.call("mem_read", {"addr": STATIC, "len": 4, "want_data": False})["sha256"]
    r = emu.call("mem_write", {"addr": STATIC, "data_hex": "A1A2A3A4", "expected_sha256": h})
    check(r.get("before_sha256") == h and r.get("after_sha256") == _sha(bytes.fromhex("A1A2A3A4")),
          "mem_write: before = expected, after = hash of the new bytes")
    _expect_precondition(emu, "mem_write", {"addr": STATIC, "data_hex": "FFFFFFFF",
                                            "expected_sha256": h}, f"expected_sha256={h}")
    now = base64.b64decode(emu.call("mem_read", {"addr": STATIC, "len": 4})["data_b64"])
    check(now == bytes.fromhex("A1A2A3A4"), "rejected write changed nothing")
    r = emu.call("mem_write", {"addr": STATIC, "data_hex": "A1A2A3A4", "want_hashes": True})
    check(r.get("before_sha256") == r.get("after_sha256"), "want_hashes without a condition")
    r = emu.call("mem_write", {"addr": STATIC, "data_hex": "A1"})
    check("before_sha256" not in r, "no hashes without condition or want_hashes")

    h = emu.call("mem_read", {"addr": STATIC, "len": 2, "want_data": False})["sha256"]
    r = emu.call("mem_write_force", {"addr": STATIC, "data_hex": "B1B2", "expected_sha256": h})
    check(r.get("before_sha256") == h, "mem_write_force with a condition")
    _expect_precondition(emu, "mem_write_force", {"addr": STATIC, "data_hex": "B1B2",
                                                  "expected_sha256": h}, "memory changed")

    regs = emu.call("regions_list")["regions"]
    ram = next(x for x in regs if x.get("kind") == "ram")
    rr = emu.call("region_read", {"region_id": ram["id"], "offset": STATIC, "length": 4,
                                  "want_data": False})
    obs = emu.call("state_observe", {"windows": [{"region": {"kind": "ram", "sub_id": 0},
                                                  "offset": STATIC, "length": 4}],
                                     "want_data": False})
    check(obs["windows"][0]["sha256"] == rr["sha256"], "observe region window hash = region_read")
    data = bytes.fromhex("C1C2C3C4")
    r = emu.call("region_write", {"region_id": ram["id"], "offset": STATIC,
                                  "data_b64": base64.b64encode(data).decode(),
                                  "expected_sha256": rr["sha256"]})
    check(r.get("written") and r.get("after_sha256") == _sha(data), "region_write with a condition")
    _expect_precondition(emu, "region_write", {"region_id": ram["id"], "offset": STATIC,
                                               "data_b64": base64.b64encode(data).decode(),
                                               "expected_sha256": rr["sha256"]}, "memory changed")


def case_registers(emu):
    """set_register s expected, set_cpu_flags s expected."""
    print("Case: conditional register and flag writes")
    emu.call("pause")
    emu.call("set_register", {"reg": "DE", "value": 0x1234})
    r = emu.call("set_register", {"reg": "DE", "value": 0x5678, "expected": 0x1234})
    check(r.get("previous") == 0x1234, f"set_register CAS accepted, previous {r.get('previous')}")
    _expect_precondition(emu, "set_register", {"reg": "DE", "value": 1, "expected": 0x1234},
                         "register DE changed (expected=0x1234, current=0x5678)")
    check(emu.call("get_registers")["DE"] == 0x5678, "rejected set_register changed nothing")

    emu.call("set_cpu_flags", {"i": 0x12})
    r = emu.call("set_cpu_flags", {"i": 0x34, "expected": {"i": 0x12}})
    check((r.get("previous") or {}).get("i") == 0x12, "set_cpu_flags CAS accepted, previous i")
    _expect_precondition(emu, "set_cpu_flags", {"i": 0x56, "expected": {"i": 0x12}},
                         "i: expected=0x12, current=0x34")
    check(emu.call("get_cpu_flags")["i"] == 0x34, "rejected set_cpu_flags changed nothing")


def case_running(emu):
    """Za běhu: měnící se slovo odmítne, neměnný rozsah zapíše."""
    print("Case: conditional writes while running")
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_HEX})
    emu.call("mem_write", {"addr": WORD, "data_hex": "0000"})
    emu.call("mem_write", {"addr": STATIC, "data_hex": "0102"})
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {})
    obs = emu.call("state_observe", {"windows": [{"addr": WORD, "len": 2},
                                                 {"addr": STATIC, "len": 2}],
                                     "want_data": False})
    hw, hs = obs["windows"][0]["sha256"], obs["windows"][1]["sha256"]
    check(obs["running"] is True, "observed while running")
    _expect_precondition(emu, "mem_write", {"addr": WORD, "data_hex": "FFFF",
                                            "expected_sha256": hw}, "memory changed")
    r = emu.call("mem_write", {"addr": STATIC, "data_hex": "0A0B", "expected_sha256": hs})
    check(r.get("before_sha256") == hs, "write to an unchanged range while running is accepted")
    emu.call("pause")


def case_replay(exe, tmp):
    """Záznam: odmítnutý zápis se nezaznamená, splněný ano; verify kód 0."""
    print("Case: replay records only accepted conditional writes")
    rec = tmp / "cond.mzr"
    cfg, ini = _ini(tmp, "rec")
    emu = PipeEmu(exe, cfg, ini)
    try:
        emu.call("pause")
        emu.call("set_speed", {"mode": "max"})
        emu.call("reset")
        emu.call("run", {"frames": 100})
        emu.call("replay_record_start", {"path": str(rec), "hash_interval": 10})
        h = emu.call("mem_read", {"addr": STATIC, "len": 2, "want_data": False})["sha256"]
        _expect_precondition(emu, "mem_write", {"addr": STATIC, "data_hex": "1111",
                                                "expected_sha256": "0" * 64}, "memory changed")
        emu.call("mem_write", {"addr": STATIC, "data_hex": "2222", "expected_sha256": h})
        _expect_precondition(emu, "set_register", {"reg": "BC", "value": 1, "expected": 0xEEEE},
                             "register BC changed")
        emu.call("run", {"frames": 30})
        st = emu.call("replay_record_stop")
        check(st.get("outcome") == "recorded", f"recording saved ({st.get('outcome_message')})")
    finally:
        emu.close()
    with zipfile.ZipFile(rec) as z:
        events = [json.loads(l) for l in z.read("inputs.jsonl").decode().splitlines() if l.strip()]
    dbg = [e for e in events if e.get("k") == "dbg"]
    check(len(dbg) == 1, f"exactly one debugger write recorded ({len(dbg)})")
    code, res = cli(exe, tmp, "verify", ["--replay", rec, "--replay-verify"])
    check(code == 0 and res and res.get("reason") == "replay_end",
          f"--replay-verify: exit code 0, replay_end ({code})")


def main():
    # Úklid spuštěných procesů i při selhání, přerušení nebo zabití
    # ctestem; vnitřní limit je kratší než TIMEOUT testu v ctestu.
    emu_test_proc.install(deadline_s=150)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_cond_writes_"))
    ok = False
    try:
        cfg, ini = _ini(tmp, "main")
        emu = PipeEmu(exe, cfg, ini)
        try:
            case_memory(emu)
            case_registers(emu)
            case_running(emu)
        finally:
            emu.close()
        case_replay(exe, tmp)
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
