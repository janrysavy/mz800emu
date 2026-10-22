#!/usr/bin/env python3
"""E2E test: čtení paměti zachycení zvuku přes MCP (trace_read audio_*,
audio_capture_summary, audio_capture_pcm) na skutečném emulátoru (MCP pipe).

Ověřuje:

  1  zachycení se soubory i pamětí: řádky ``audio_psg_writes``
     a ``audio_levels`` přes trace_read jsou totéž co psg_writes.csv
     a events.csv; ``audio_frames`` má jeden souhrn na snímek;
     ``emu_audio_capture_pcm`` (44100 Hz stereo, po stránkách) je bajt po
     bajtu totéž co data reference.wav a sdl.wav;
  2  souhrn: program zapíše do PSG0 kanálu 0 dělič 254 a hlasitost 0 ->
     nota psg0_0 s děličem 254; posun SDL cesty proti referenci (headless
     vyrábí SDL blok každý snímek) vyjde jeden snímek (podle nástupu tónu,
     ne podle minim rozdílu po periodách tónu);
  3  zachycení jen do paměti (bez souborů): nezaloží složku, souhrn
     funguje; nový start nahradí paměť a starý kurzor vrátí
     ``segment_replaced:``.

Program (MZ-800, RAM 3000h): DI; zápisy 8Eh, 0Fh (tón 254), 90h
(hlasitost 0) na port 0F2h; JR $. Proces běží izolovaně (dočasný
``--cfg-dir`` / ``--work-dir``, ``--no-save-ini``); zabíjí se jen vlastní
PID. Výstup anglicky.

Exit code: 0 = PASS, 1 = FAIL.
"""

import base64
import csv
import io
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_test_proc  # noqa: E402 - úklid spuštěných procesů
from test_cmt_snapshot import PipeEmu, TestFailure, _find_exe, check  # noqa: E402

PROG = 0x3000
#: DI; LD A,8Eh; OUT (0F2h),A; LD A,0Fh; OUT (0F2h),A; LD A,90h;
#: OUT (0F2h),A; JR $ - PSG0 kanál 0: dělič (0Fh << 4) | 0Eh = 254, útlum 0.
PROG_PSG = bytes.fromhex("F3" "3E8E" "D3F2" "3E0F" "D3F2" "3E90" "D3F2" "18FE")


def read_rows(emu, channel, columns=None):
    """Všechny řádky kanálu stránkami (kurzor); vrátí (sloupce, řádky, poslední odpověď)."""
    rows = []
    cursor = None
    last = None
    for _ in range(10000):
        req = {"channel": channel, "limit": 5000}
        if columns:
            req["columns"] = columns
        if cursor:
            req["cursor"] = cursor
        last = emu.call("trace_read", req)
        rows.extend(last["rows"])
        cursor = last["next_cursor"]
        if last["end_of_data"]:
            return last["columns"], rows, last
    raise TestFailure(f"trace_read {channel} did not reach end_of_data")


def read_pcm(emu, source, total):
    """PCM 44100 Hz stereo celého zdroje stránkami po 1 MiB."""
    out = b""
    pos = 0
    while pos < total:
        r = emu.call("audio_capture_pcm", {"source": source, "sample_from": pos,
                                           "samples": total - pos, "rate": 44100,
                                           "stereo": True, "encoding": "pcm_b64",
                                           "max_bytes": 1 << 20})
        data = base64.b64decode(r["data_b64"])
        if not data:
            break
        out += data
        pos = r["next_sample"]
    return out


def csv_rows(path):
    """Řádky CSV bez hlavičky jako slovníky."""
    return list(csv.DictReader(io.StringIO(path.read_text(encoding="utf-8"))))


def case_files_and_memory(emu, cap_dir):
    """Scénáře 1 a 2: paměť vs soubory, PCM bajt po bajtu, souhrn s notou."""
    emu.call("pause")
    emu.call("mem_write", {"addr": PROG, "data_hex": PROG_PSG.hex()})
    st = emu.call("audio_capture_start", {"dir": cap_dir.as_posix(),
                                          "pcm": ["reference", "sdl"]})
    check(st["pending"], "capture with memory planned")
    emu.call("run", {"frames": 10})          # začátek zachycení + rozjezd SDL
    emu.call("pause")
    emu.call("set_register", {"reg": "PC", "value": PROG})
    emu.call("run", {"frames": 30})          # zápisy do PSG uvnitř zachycení
    emu.call("pause")
    st = emu.call("audio_capture_status")
    check(st["active"] and st["memory"] is not None, "capture runs with memory")
    origin = st["origin_ticks"]
    check(isinstance(origin, int) and origin > 0, f"origin_ticks known ({origin})")

    # Čtení za běhu zachycení (live) i po stopu.
    _, live_rows, last = read_rows(emu, "audio_frames", ["seq"])
    check(last["live"] and len(live_rows) == st["frames"],
          f"audio_frames while capturing: one row per frame ({len(live_rows)})")
    st = emu.call("audio_capture_stop")
    seg = st["segment"]
    mem = st["memory"]["streams"]

    cols, rows, last = read_rows(emu, "audio_psg_writes")
    check(not last["live"] and last["segment"] == seg, "readable after stop")
    csvw = csv_rows(cap_dir / "psg_writes.csv")
    c = {n: i for i, n in enumerate(cols)}
    mine = [(r[c["t"]] - origin, r[c["frame"]], r[c["psg"]], r[c["byte"]], r[c["ch"]], r[c["reg"]], r[c["value"]])
            for r in rows]
    theirs = [(int(r["ticks"]), int(r["frame"]), int(r["psg"]), r["byte"], int(r["channel"]), r["reg"], int(r["value"]))
              for r in csvw]
    check(mine == theirs and len(mine) == mem["psg_writes"]["next_seq"],
          f"audio_psg_writes rows equal psg_writes.csv ({len(mine)} rows)")
    tone = [r for r in rows if r[c["reg"]] == "tone" and r[c["value"]] == 254]
    check(tone and tone[0][c["hz"]] and tone[0][c["note"]], f"tone row has hz and note ({tone[:1]})")

    cols, rows, _ = read_rows(emu, "audio_levels")
    c = {n: i for i, n in enumerate(cols)}
    csve = csv_rows(cap_dir / "events.csv")
    mine = [(r[c["t"]] - origin, r[c["frame"]], r[c["channel"]], r[c["level"]]) for r in rows]
    theirs = [(int(r["ticks"]), int(r["frame"]), r["channel"], int(r["level"])) for r in csve]
    check(mine == theirs, f"audio_levels rows equal events.csv ({len(mine)} rows)")

    cols, rows, _ = read_rows(emu, "audio_frames")
    c = {n: i for i, n in enumerate(cols)}
    check(len(rows) == st["frames"] and [r[0] for r in rows] == list(range(st["frames"])),
          f"audio_frames: one row per captured frame ({len(rows)})")
    psg00 = st["channels"].index("psg0_0")
    check(any(r[c["edges"]][psg00] > 0 for r in rows[-5:]), "psg0_0 has level changes at the end")

    for src, name in (("reference", "reference.wav"), ("sdl", "sdl.wav")):
        wav = (cap_dir / name).read_bytes()[44:]
        total = mem["pcm_" + src]["next_seq"]
        pcm = read_pcm(emu, src, total)
        check(pcm == wav and len(pcm) == total * 4,
              f"audio_capture_pcm 44100 Hz stereo equals {name} data ({len(pcm)} B)")

    prev = emu.call("audio_capture_pcm", {})
    data = base64.b64decode(prev["data_b64"])
    check(data[:4] == b"RIFF" and prev["rate"] == 11025 and prev["channels"] == 1
          and len(prev["data_b64"]) < 50000, "default PCM window: small mono WAV")

    s = emu.call("audio_capture_summary")
    notes = [n for n in s["psg_notes"] if n["ch"] == "psg0_0" and n.get("divider") == 254]
    check(notes and notes[-1]["max_level"] == 15, f"summary: psg0_0 note divider 254 ({notes[-1:]})")
    rv = s["ref_vs_sdl"]
    spf = 44100 // s["fps"]
    # Bez zvukového zařízení je SDL cesta o jeden snímek za referencí
    # (změřeno sondou na nástupu tónu: minimum rozdílu přesně při 882 vzorcích).
    check(rv is not None and abs(rv["lag_samples"] - spf) <= 8 and not rv["lag_ambiguous"],
          f"summary: output path lag = one frame ({rv})")
    print(f"  ref_vs_sdl: lag_samples={rv.get('lag_samples')} lag_frames={rv.get('lag_frames')} "
          f"diff_rms={rv.get('diff_rms')} reference_rms={rv.get('reference_rms')} "
          f"sdl_missing_blocks={rv.get('sdl_missing_blocks')}")
    print(f"  ctc0: {s['ctc0']['likely']} edges={s['ctc0']['edges']}; "
          f"sound_spans={s['sound_spans'][:3]} silent_spans={s['silent_spans'][:3]}")
    _, _, last = read_rows(emu, "audio_levels", ["seq"])
    return last["next_cursor"], seg


def case_memory_only(emu, tmp, old_cursor, old_seg):
    """Scénář 3: jen paměť, žádná složka; nový start = segment_replaced."""
    st = emu.call("audio_capture_start", {"events": False, "psg_writes": False,
                                          "reference_wav": False, "sdl_wav": False,
                                          "frames": 5})
    check(st["dir"] == "" and st["segment"] == old_seg + 1, "memory-only capture: no folder, new segment")
    emu.call("run", {"frames": 10})
    emu.call("pause")
    st = emu.call("audio_capture_status")
    check(not st["active"] and st["stop_reason"] == "Frame count reached" and st["frames"] == 5,
          f"memory-only capture stopped after 5 frames ({st['stop_reason']})")
    s = emu.call("audio_capture_summary")
    check(s["frames"] == [0, 4], f"summary of memory-only capture ({s['frames']})")
    try:
        emu.call("trace_read", {"channel": "audio_levels", "cursor": old_cursor})
        raise TestFailure("old cursor accepted after a new capture")
    except TestFailure as e:
        check("segment_replaced:" in str(e), f"old cursor -> segment_replaced ({e})")
    new_dirs = [p for p in tmp.rglob("*_audio_*") if p.is_dir()]
    check(not new_dirs, "no default capture folder created")


def main():
    emu_test_proc.install(deadline_s=280)
    exe = _find_exe()
    print(f"Using binary: {exe}")
    tmp = Path(tempfile.mkdtemp(prefix="mz_audio_read_"))
    ok = False
    try:
        ini = tmp / "test.ini"
        ini.write_text("[BREAKPOINTS]\nauto_load = 0\nauto_save = 0\n", encoding="utf-8")
        cfg = tmp / "cfg"
        cfg.mkdir()
        emu = PipeEmu(exe, cfg, ini)
        try:
            cursor, seg = case_files_and_memory(emu, tmp / "cap")
            case_memory_only(emu, tmp, cursor, seg)
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
