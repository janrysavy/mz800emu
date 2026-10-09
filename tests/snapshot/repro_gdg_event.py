"""Standalone snapshot/GDG regression. Original test bytes; no game or ROM payload.

SPDX-License-Identifier: MIT
Run with an installed mz800emu executable; optional dependency directories are
added to PATH. Outputs remain local. This script never downloads files.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import threading
import time
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--dependency-dir', type=Path, action='append', default=[])
    parser.add_argument('--dmd', type=int, choices=(0, 2), default=0)
    parser.add_argument('--seed-line', type=int, choices=range(312), default=0)
    parser.add_argument('--legacy-snapshot', action='store_true')
    parser.add_argument('--cdb', type=Path, help='Optional installed CDB for a host-only crash stack')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ)
    env['PATH'] = os.pathsep.join([*(str(p.resolve()) for p in args.dependency_dir), env['PATH']])
    command = [str(args.exe.resolve()), '--mcp-pipe', '--no-save-ini',
               '--no-first-run-windows', f'--work-dir={out}', f'--cfg-dir={out}']
    report = dict(completed=False, command=command, game_payload=False, requests=[],
        script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        executable_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest())
    save = lambda: (out/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    save()
    with (out/'stderr.log').open('wb') as log:
        process = subprocess.Popen(command, cwd=out, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=log, env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        debugger = None
        received = queue.Queue()
        def pump():
            for raw in process.stdout:
                try: received.put(json.loads(raw))
                except ValueError: received.put({'non_json':raw.decode(errors='replace')})
            received.put(None)
        threading.Thread(target=pump, daemon=True).start()
        serial = 0
        def call(cmd, **data):
            nonlocal serial
            serial += 1
            request = dict(req_id=serial, cmd=cmd, data=data)
            report['requests'].append(dict(request=request))
            save()
            process.stdin.write(json.dumps(request).encode()+b'\n')
            process.stdin.flush()
            while True:
                response = received.get(timeout=30)
                if response is None: raise RuntimeError('Emulator exited before response')
                if response.get('req_id') == serial:
                    report['requests'][-1]['response'] = response
                    save()
                    if not response.get('success'): raise RuntimeError(response)
                    return response.get('data', {})
        try:
            hello = received.get(timeout=20)
            assert hello and hello.get('type') == 'hello', hello
            report['hello'] = hello
            call('pause')
            clocks = call('get_platform_info').get('clocks')
            report['clocks'] = clocks
            # The fixture supplies only our JP/NOP bytes, never ROM content.
            # Emulator startup/installed firmware is outside this payload.
            for port, value in ((0xe0,0),(0xe1,0),(0xce,args.dmd),(0xcc,0x89)):
                call('io_write', port=port, value=value)
            call('io_read', port=0xe1)
            call('set_cpu_flags', iff1=False, iff2=False)
            call('set_speed', mode='max')
            call('mem_write', addr=0x3000, data_hex='c30030') # JP $3000
            call('mem_write', addr=0x3100, data_hex='00'*8192) # NOPs
            call('set_register', reg='PC', value=0x3000)
            if args.seed_line:
                call('run_until_raster', line=args.seed_line, col=200, max_cycles=100000)
            seed = out/'seed.mzs'
            report['saved_raster'] = call('get_raster_pos')
            report.update(dmd=args.dmd, legacy_fixture=args.legacy_snapshot)
            call('snapshot_save', path=str(seed), description='Original MIT-licensed JP/NOP fixture')
            if args.legacy_snapshot:
                # Explicit compatibility fixture, not a gameplay snapshot:
                # remove only the new optional GDG event element.
                legacy = out/'legacy-seed.mzs'
                with zipfile.ZipFile(seed) as src:
                    entries = {name:src.read(name) for name in src.namelist()}
                entries['hw/gdg.xml'], removed = re.subn(rb'\s*<event>.*?</event>', b'', entries['hw/gdg.xml'], flags=re.S)
                assert removed == 1
                checksum = hashlib.sha256()
                for name in sorted(entries):
                    if name != 'manifest.xml': checksum.update(entries[name])
                entries['manifest.xml'], replaced = re.subn(
                    rb'(<checksum[^>]*>\s*<value[^>]*>)[0-9a-f]+(</value>)',
                    lambda m: m[1]+checksum.hexdigest().encode()+m[2], entries['manifest.xml'])
                assert replaced == 1
                with zipfile.ZipFile(legacy, 'w', zipfile.ZIP_DEFLATED) as dst:
                    for name, data in entries.items(): dst.writestr(name, data)
                seed = legacy
            call('run', frames=1) # reach the next frame before selecting line 100
            call('run_until_raster', line=100, col=750, max_cycles=100000)
            call('set_register', reg='PC', value=0x3100)
            # At col 790..793 the next GDG event is AFTER_LAST_SCREEN_PIXEL.
            # 20-tick NOPs and the 1135-tick line length reach this phase
            # within four rows. No device or snapshot bytes are edited.
            for step in range(400):
                raster = call('get_raster_pos')
                if 46 <= raster['scanline'] <= 245 and 790 <= raster['column_pixel'] < 794:
                    break
                call('step_into')
            else: raise AssertionError('Could not reach selected raster phase')
            report['pre_restore_raster'] = raster
            call('snapshot_load', path=str(seed))
            report['restored_raster'] = call('get_raster_pos')
            # Force a visible change in otherwise blank VRAM using the API.
            # OUT to DMD also invalidates the screen before the stale event.
            call('io_write', port=0xce, value=args.dmd)
            if args.cdb:
                cdb_commands = out/'cdb-commands.txt'
                cdb_log = out/'cdb.log'
                cdb_commands.write_text('sxe -c ".lastevent; .exr -1; r; kv; q" av\n'
                    'sxe -c ".lastevent; q" epr\n.echo REPRO_DEBUGGER_READY\ng\n')
                debugger = subprocess.Popen([str(args.cdb.resolve()), '-p', str(process.pid),
                    '-logo', str(cdb_log), '-cf', str(cdb_commands)], stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                deadline = time.monotonic()+20
                while not cdb_log.exists() or 'REPRO_DEBUGGER_READY' not in cdb_log.read_text(errors='replace'):
                    if time.monotonic() > deadline or debugger.poll() is not None:
                        raise RuntimeError('CDB attach did not become ready')
                    time.sleep(.05)
            resumed = call('run', frames=1)
            assert resumed['actual_frames'] == 1 and resumed['complete'], resumed
            report['after_run_raster'] = call('get_raster_pos')
            assert report['after_run_raster']['frame_number'] == report['saved_raster']['frame_number']+1
            assert call('get_platform_info').get('clocks') == clocks
            report['completed'] = True
        except Exception as exc:
            report['failure'] = repr(exc)
        finally:
            # Capture spontaneous termination before the owned cleanup kill.
            time.sleep(.05)
            report['exit_before_cleanup'] = process.poll()
            if process.poll() is None:
                try: call('shutdown')
                except Exception: pass
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
            report['final_exit_code'] = process.returncode
            if debugger:
                try: debugger.wait(timeout=5)
                except subprocess.TimeoutExpired: debugger.terminate(); debugger.wait(timeout=5)
            save()
    print(json.dumps({k:v for k,v in report.items() if k != 'requests'}, indent=2))
    if not report['completed']: raise SystemExit(1)


if __name__ == '__main__':
    main()

