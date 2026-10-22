#!/usr/bin/env python3
"""E2E test: konec záznamu přes --exit-after-frames a závěrečný hash.

Program ve smyčce ``IN A,(0D2h)`` / ``JR`` synchronizuje GDG uvnitř skoro
každé instrukce, takže konec snímku zpracuje synchronizace uvnitř instrukce.
Pauza, kterou ``--exit-after-frames`` vyžádá v callbacku konce snímku,
dřív posunula poslední bod hranice snímku záznamu (B_f) proti přehrávání:
``--replay-verify`` pak skončil kódem 4 na posledním snímku (``cpu``,
``periph``, ``time``). Test nahraje z příkazové řádky několik délek
a každý záznam ověří v novém procesu: vždy kód 0.

Proces běží izolovaně (dočasný ``--cfg-dir``/``--work-dir``, vlastní INI,
``--no-save-ini``); zabíjí se jen vlastní PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import TestFailure, _find_exe, check  # noqa: E402
from test_replay import cli  # noqa: E402

#: Délky záznamu (snímky); každá končí na jiném místě smyčky.
FRAMES = [100, 101, 102, 503]


def make_mzf(path):
    """MZF se smyčkou IN A,(0D2h); JR -4 na 1200h."""
    body = bytes([0xDB, 0xD2, 0x18, 0xFC])
    hdr = bytearray(128)
    hdr[0] = 0x01
    name = b"IN LOOP\r"
    hdr[1:1 + len(name)] = name
    hdr[0x12:0x14] = len(body).to_bytes(2, "little")
    hdr[0x14:0x16] = (0x1200).to_bytes(2, "little")
    hdr[0x16:0x18] = (0x1200).to_bytes(2, "little")
    path.write_bytes(bytes(hdr) + body)


def main():
    emu_test_proc.install(deadline_s=170)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_replay_exit_"))
    ok = False
    try:
        mzf = tmp / "inloop.mzf"
        make_mzf(mzf)
        for n in FRAMES:
            rec = tmp / f"r{n}.mzr"
            code, res = cli(exe, tmp, f"rec{n}", ["--speed", "max", "--run-mzf", mzf, "--replay-record", rec,
                                                  "--exit-after-frames", n])
            check(code == 0 and rec.is_file(), f"{n} frames recorded ({code})")
            code, res = cli(exe, tmp, f"verify{n}", ["--replay", rec, "--replay-verify"])
            rp = (res or {}).get("replay") or {}
            check(code == 0, f"{n} frames: --replay-verify exit code 0 ({code}, {rp.get('outcome')}, frame {rp.get('outcome_frame')})")
        ok = True
    except TestFailure as e:
        print(f"FAIL: {e}")
    if ok:
        emu_test_proc.rmtree_retry(tmp)
        print("PASS")
        return 0
    print(f"Artifacts kept in {tmp}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
