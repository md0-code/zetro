"""The CPU conformance material: the SingleStepTests Z80 suite, fetched into
work/cpu/ and converted for the C runner (tests/cpu_suite.c).

    python tests/cpu_suite.py fetch          download at the pinned commit and
                                             convert each file to work/cpu/z80/*.bin
    python tests/cpu_suite.py run [args]     run the runner over the suite (args go
                                             to it: --only NAME, --show N, --no-cycles)
    python tests/cpu_suite.py zex [zexdoc|zexall]
                                             run the instruction exercisers ZEXDOC and
                                             ZEXALL to their end (minutes each)
    python tests/cpu_suite.py status         show what is fetched

The suite is third-party test data (MIT, github.com/SingleStepTests/z80):
1604 files of 1000 tests, one file for each opcode, each test the registers
and memory before and after one instruction and the bus pins at every
T-state. It is never committed. It lives in the main checkout's work/cpu/
(a worktree under work/wt/ finds it there).

Converted format (little-endian), one file per JSON file:
    'ZRS1', u32 count, then per test:
    u8 name length, name bytes,
    the initial state, the final state, each:
        9 x u16: pc sp wz ix iy af_ bc_ de_ hl_
        16 x u8: a b c d e f h l i r ei im p q iff1 iff2
        u16 n, n x (u16 address, u8 byte)
    u16 m, m x (u16 address, u8 data, u8 pins): pins bit 0 RD, 1 WR, 2 MREQ,
        3 IORQ, bit 7 set when the data pins are not driven
    u8 k, k x (u16 port, u8 value, u8 write)
"""
from __future__ import annotations

import hashlib
import json
import os
import urllib.request
import struct
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main_checkout() -> Path:
    """The main checkout's root (a worktree lives under <main>/work/wt/<name>)."""
    if ROOT.parent.name == 'wt' and ROOT.parent.parent.name == 'work':
        return ROOT.parent.parent.parent
    return ROOT


WORK = Path(os.environ['ZETRO_CPU_DIR']) if os.environ.get('ZETRO_CPU_DIR') else main_checkout() / 'work' / 'cpu'
RUNNER = ROOT / 'build' / 'x64' / ('cpu_suite.exe' if os.name == 'nt' else 'cpu_suite')
REPO = 'https://github.com/SingleStepTests/z80.git'
COMMIT = 'ebe1875d48f374bcfd4b505d8eb8ee751568b5f7'
SOURCE = WORK / 'z80-src'
OUT = WORK / 'z80'
MAGIC = b'ZRS1'
# Frank Cringle's Z80 instruction exercisers (GPL, CP/M programs; test data, never committed).
ZEX_URL = 'https://raw.githubusercontent.com/anotherlin/z80emu/master/testfiles/'
ZEX = {
    'zexdoc': '34923a7ed82285d3038b2d54bd64899e12173eebb61f9d07b4fc72e78af2ae8f',
    'zexall': '6e2da55147a04f28d303d5da6a1e6b771557ac244653590a0f24a2d39c8537e8',
}
WORDS = ['pc', 'sp', 'wz', 'ix', 'iy', 'af_', 'bc_', 'de_', 'hl_']
BYTES = ['a', 'b', 'c', 'd', 'e', 'f', 'h', 'l', 'i', 'r', 'ei', 'im', 'p', 'q', 'iff1', 'iff2']


def pack_state(s: dict) -> bytes:
    out = struct.pack('<9H', *[s[k] & 0xFFFF for k in WORDS]) + bytes(s[k] & 0xFF for k in BYTES)
    return out + struct.pack('<H', len(s['ram'])) + b''.join(struct.pack('<HB', a, v) for a, v in s['ram'])


def pack_cycle(c: list) -> bytes:
    address, data, pins = c
    bits = (pins[0] == 'r') | (pins[1] == 'w') << 1 | (pins[2] == 'm') << 2 | (pins[3] == 'i') << 3 | (data is None) << 7
    return struct.pack('<HBB', (address or 0) & 0xFFFF, (data or 0) & 0xFF, bits)


def convert(job: tuple) -> str:
    src, dst = job
    tests = json.loads(Path(src).read_bytes())
    parts = [MAGIC, struct.pack('<I', len(tests))]
    for t in tests:
        name = t['name'].encode('utf-8')[:255]
        ports = t.get('ports', [])
        parts += [struct.pack('<B', len(name)), name, pack_state(t['initial']), pack_state(t['final']),
                  struct.pack('<H', len(t['cycles']))]
        parts += [pack_cycle(c) for c in t['cycles']]
        parts += [struct.pack('<B', len(ports))] + [struct.pack('<HBB', p[0] & 0xFFFF, p[1] & 0xFF, p[2] == 'w') for p in ports]
    tmp = Path(dst).with_suffix('.tmp')
    tmp.write_bytes(b''.join(parts))
    tmp.replace(dst)
    return Path(dst).name


def git(*args: str) -> None:
    subprocess.run(['git', '-C', str(SOURCE), *args], check=True)


def fetch() -> int:
    if not (SOURCE / 'v1' / '00.json').exists():
        SOURCE.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '-q', str(SOURCE)], check=True)
        git('fetch', '-q', '--depth', '1', REPO, COMMIT)
        git('checkout', '-q', 'FETCH_HEAD')
    files = sorted((SOURCE / 'v1').glob('*.json'))
    OUT.mkdir(parents=True, exist_ok=True)
    jobs = [(str(f), str(OUT / (f.stem + '.bin'))) for f in files if not current(OUT / (f.stem + '.bin'))]
    with ProcessPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as pool:
        for i, _ in enumerate(pool.map(convert, jobs), 1):
            if i % 200 == 0 or i == len(jobs):
                print(f'converted {i}/{len(jobs)}', flush=True)
    print(f'{len(files)} files at {COMMIT[:12]} in {OUT}')
    return 0


def zex(names: list[str]) -> int:
    """Runs the instruction exercisers to their end (minutes each); fetched on first use, checked by hash."""
    folder = WORK / 'zex'
    folder.mkdir(parents=True, exist_ok=True)
    code = 0
    for name in names:
        if name not in ZEX:
            print(f'zex takes {" or ".join(ZEX)}', file=sys.stderr)
            return 2
        path = folder / f'{name}.com'
        if not path.exists():
            with urllib.request.urlopen(urllib.request.Request(ZEX_URL + f'{name}.com', headers={'User-Agent': 'curl/8'}), timeout=120) as r:
                path.write_bytes(r.read())
        if hashlib.sha256(path.read_bytes()).hexdigest() != ZEX[name]:
            print(f'{path}: not the expected file', file=sys.stderr)
            return 1
        code |= subprocess.run([str(RUNNER), '--com', str(path)]).returncode
    return code


def current(path: Path) -> bool:
    if not path.exists():
        return False
    with path.open('rb') as f:
        return f.read(4) == MAGIC


def main(argv) -> int:
    if argv[:1] == ['fetch']:
        return fetch()
    if argv[:1] == ['run']:
        if not any(OUT.glob('*.bin')):
            print('fetch first: python tests/cpu_suite.py fetch', file=sys.stderr)
            return 2
        return subprocess.run([str(RUNNER), str(OUT)] + argv[1:]).returncode
    if argv[:1] == ['zex']:
        return zex(argv[1:] or list(ZEX))
    if argv[:1] == ['status']:
        print(f'{sum(1 for _ in OUT.glob("*.bin"))} files converted in {OUT}')
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
