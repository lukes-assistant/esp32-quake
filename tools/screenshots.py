#!/usr/bin/env python3
"""Turn the SHOT lines of a console log (CONFIG_QUAKE_SCREENSHOTS) into PNG files.

usage: screenshots.py <console log> <output prefix>   -> <prefix>-0.png, <prefix>-1.png, ...
"""
import sys
from PIL import Image

def main():
    log, prefix = sys.argv[1], sys.argv[2]
    shot = None
    for raw in open(log, 'rb'):
        line = raw.decode('ascii', 'replace').strip()
        if line.startswith('SHOT_BEGIN'):
            _, n, w, h = line.split()
            shot = {'n': int(n), 'w': int(w), 'h': int(h), 'rows': {}, 'pal': None}
        elif shot is None:
            continue
        elif line.startswith('SHOT_PAL '):
            shot['pal'] = bytes.fromhex(line[9:])
        elif line.startswith('SHOT '):
            parts = line.split()
            if len(parts) == 3 and len(parts[2]) == shot['w'] * 2:
                shot['rows'][int(parts[1])] = bytes.fromhex(parts[2])
        elif line == 'SHOT_END':
            w, h = shot['w'], shot['h']
            pixels = b''.join(shot['rows'].get(y, bytes(w)) for y in range(h))
            img = Image.frombytes('P', (w, h), pixels)
            img.putpalette(shot['pal'])
            name = '%s-%d.png' % (prefix, shot['n'])
            img.convert('RGB').save(name)
            print('%s (%d/%d rows)' % (name, len(shot['rows']), h))
            shot = None

if __name__ == '__main__':
    main()
