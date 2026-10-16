#!/usr/bin/env python3
"""Blocking RUN must wake on a breakpoint without waiting for another frame.

Use an assembled run_breakpoint_latency.asm binary, never host opcode patches:
  python test_run_breakpoint_latency.py --exe /path/mz800emu --program loop.bin
The regression checks every exact14-T instruction interval and reports host
latency separately. At most one interval can cross a physical frame boundary;
the initial beam phase is retained. Only the
owned emulator process is launched/terminated; no register/memory repairs.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import statistics
import struct
import subprocess
import tempfile
import time

import emu_test_proc


class Client:
    def __init__(self, sock):
        self.stream = sock.makefile('rwb')
        self.serial = 0
        hello = json.loads(self.stream.readline())
        assert hello['type'] == 'hello', hello

    def call(self, cmd, **data):
        self.serial += 1
        self.stream.write(json.dumps(dict(req_id=self.serial,cmd=cmd,data=data)).encode()+b'\n')
        self.stream.flush()
        while True:
            response = json.loads(self.stream.readline())
            if response.get('req_id') == self.serial:
                assert response.get('success'), response
                return response.get('data',{})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--samples',type=int,default=256)
    parser.add_argument('--max-host-ms',type=float,default=25)
    parser.add_argument('--report',type=Path)
    args = parser.parse_args()
    assert 16 <= args.samples <= 512
    executable, program = args.exe.resolve(), args.program.read_bytes()
    assert len(program) == 4, 'Assemble the maintained NOP/JP fixture first'
    emu_test_proc.install(deadline_s=90)
    report = dict(executable=str(executable),samples=[],passed=False,
                  scope='Host wakeup latency; guest CPU14 T-states per breakpoint')
    with tempfile.TemporaryDirectory(prefix='run-wake-',dir=Path.cwd()) as directory:
        work = Path(directory)
        header = bytearray(128)
        header[0] = 1
        header[1:18] = b'RUN WAKE TEST'.ljust(17,b'\r')
        struct.pack_into('<HHH',header,18,len(program),0x1200,0x1200)
        image = work/'loop.mzf'
        image.write_bytes(header+program)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1',0))
            port = reservation.getsockname()[1]
        env = dict(os.environ,SDL_VIDEO_DRIVER='dummy')
        with (work/'emulator.log').open('wb') as log:
            process = subprocess.Popen([str(executable),'--headless','--no-save-ini',
                '--no-first-run-windows',f'--mcp-tcp-port={port}',
                f'--cfg-dir={work}',f'--work-dir={work}'],
                cwd=executable.parent,env=env,stdout=log,stderr=log,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            sock = None
            try:
                deadline = time.monotonic()+15
                while sock is None:
                    try: sock = socket.create_connection(('127.0.0.1',port),timeout=1)
                    except OSError:
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(0.02)
                sock.settimeout(30)
                sock.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
                client = Client(sock)
                client.call('pause')
                client.call('set_speed',mode='max')
                client.call('media_run_mzf',path=str(image))
                client.call('bp_add',addr=0x1200)
                report['clocks'] = client.call('get_platform_info')['clocks']
                begin = client.call('get_cpu_panel_batch')
                assert begin['regs']['PC'] == 0x1200
                stable = {k:v for k,v in begin['regs'].items() if k not in ('IR','WZ')}
                for index in range(args.samples):
                    started = time.perf_counter()
                    result = client.call('run',frames=1000)
                    host_ms = (time.perf_counter()-started)*1000
                    end = client.call('get_cpu_panel_batch')
                    cycles = (end['flags']['total_cycles']-begin['flags']['total_cycles'])&0xFFFFFFFF
                    row = dict(index=index,host_ms=host_ms,cycles=cycles,
                               pc=end['regs']['PC'],stopped_by=result['stopped_by'],
                               actual_frames=result['actual_frames'])
                    report['samples'].append(row)
                    assert row['stopped_by'] == 'breakpoint' and row['actual_frames'] in (0,1), row
                    assert row['pc'] == 0x1200 and cycles == 14, row
                    assert {k:v for k,v in end['regs'].items() if k not in ('IR','WZ')} == stable
                    assert (end['regs']['IR']&0xFF00) == (begin['regs']['IR']&0xFF00)
                    begin = end
                values = [v['host_ms'] for v in report['samples']]
                report['median_host_ms'] = statistics.median(values)
                report['worst_host_ms'] = max(values)
                report['no_frame_stops'] = sum(v['actual_frames'] == 0 for v in report['samples'])
                assert report['no_frame_stops'] >= args.samples-1, report['samples']
                report['host_limit_ms'] = args.max_host_ms
                assert report['worst_host_ms'] < args.max_host_ms, report['worst_host_ms']
                report['passed'] = True
            finally:
                if sock is not None:
                    try: client.call('shutdown')
                    except Exception: pass
                    sock.close()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                if args.report:
                    args.report.parent.mkdir(parents=True,exist_ok=True)
                    args.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(passed=report['passed'],samples=len(report['samples']),
        median_host_ms=report['median_host_ms'],worst_host_ms=report['worst_host_ms'])))


if __name__ == '__main__':
    main()
