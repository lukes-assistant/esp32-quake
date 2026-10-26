#!/usr/bin/env python3
"""Turn the PROF lines printed by a CONFIG_QUAKE_PROFILE build into a per-function profile.

Usage: profile_report.py <serial log> [build/quake.elf] [--top N] [--lines]

The log may contain other output; the BENCH summary lines and the last
PROF_BEGIN..PROF_END block are used. Symbols are read with the ESP-IDF
toolchain's nm (riscv32-esp-elf-nm must be on PATH, e.g. after export.sh).
With --lines, the hottest individual addresses are also resolved to
source lines via addr2line.
"""
import argparse
import bisect
import collections
import re
import subprocess
import sys


def read_log(path, tag='PROF'):
    bench, samples, hz = [], None, None
    with open(path, 'rb') as f:
        for raw in f:
            line = re.sub(r'\x1b\[[0-9;]*m', '', raw.decode('utf-8', 'replace')).strip()
            if line.startswith('BENCH '):
                bench.append(line)
            elif line.startswith(tag + '_BEGIN'):
                hz = int(line.split()[2])
                samples = collections.Counter()
            elif line.startswith(tag + ' ') and samples is not None:
                _, pc, count = line.split()
                samples[int(pc, 16)] += int(count)
    return bench, samples, hz


def read_symbols(elf):
    out = subprocess.run(['riscv32-esp-elf-nm', '-n', '-S', '--defined-only', '-C', elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(maxsplit=3)
        if len(parts) == 4 and parts[2] in 'tTwW':
            syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
    syms.sort()
    return syms


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('elf', nargs='?', default='build/quake.elf')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--lines', action='store_true', help='also show the hottest source lines')
    ap.add_argument('--core', type=int, default=0, choices=(0, 1),
                    help='which core\'s samples to report (core 1 needs CONFIG_QUAKE_PROFILE_CORE1)')
    args = ap.parse_args()

    bench, samples, hz = read_log(args.log, 'PROF' if args.core == 0 else 'PROF1')
    for b in bench:
        print(b)
    if not samples:
        sys.exit('no PROF_BEGIN..PROF_END block found (is CONFIG_QUAKE_PROFILE enabled?)')

    syms = read_symbols(args.elf)
    starts = [s[0] for s in syms]
    total = sum(samples.values())
    funcs = collections.Counter()
    for pc, n in samples.items():
        i = bisect.bisect_right(starts, pc) - 1
        if i >= 0 and pc < syms[i][0] + max(syms[i][1], 1):
            funcs[syms[i][2]] += n
        else:
            funcs['?? %08x' % pc] += n

    print(f'\n{total} samples at {hz} Hz ({total / hz:.1f} s) on core {args.core}\n')
    print(f'{"%":>6} {"cum%":>6} {"samples":>8}  function')
    cum = 0
    for name, n in funcs.most_common(args.top):
        cum += n
        print(f'{100 * n / total:6.2f} {100 * cum / total:6.2f} {n:8}  {name}')

    if args.lines:
        hot = samples.most_common(args.top)
        out = subprocess.run(['riscv32-esp-elf-addr2line', '-f', '-C', '-e', args.elf]
                             + ['%x' % pc for pc, _ in hot],
                             capture_output=True, text=True, check=True).stdout.splitlines()
        print(f'\n{"%":>6} {"samples":>8}  address   location')
        for (pc, n), func, loc in zip(hot, out[0::2], out[1::2]):
            print(f'{100 * n / total:6.2f} {n:8}  {pc:08x}  {func} {loc.split("/")[-1]}')


if __name__ == '__main__':
    main()
