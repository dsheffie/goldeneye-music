#!/usr/bin/env python3
# Decode every ADPCM wavetable of the music bank through the real audio microcode on
# rsp.cc (tests/adpcm_test) and compare bit-for-bit with the reference VADPCM decoder
# in ../tools/extract_music.py.
import os, struct, subprocess, sys
import numpy as np
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', '..', 'tools'))
import extract_music as E
from ctl import parse_ctl

# the helper sits next to this script in a Makefile build and in the build tree for
# an out-of-tree cmake build, so let the caller say where it is
adpcm_test = os.environ.get('ADPCM_TEST', os.path.join(here, 'adpcm_test'))
rom = sys.argv[1] if len(sys.argv) > 1 else E.ROM
d = open(rom, 'rb').read()
bank = parse_ctl(d, E.INST_CTL)[0][0]
seen = {}
for inst in bank['instruments']:
    for s in inst['sounds']:
        w = s['wavetable']
        if w['type'] == 0:
            seen[w['base']] = w
ok = 0
for base, w in sorted(seen.items()):
    raw_name = '/tmp/gemusic_adpcm_%d.raw' % os.getpid()
    subprocess.run([adpcm_test, rom, '%x' % w['offset'], raw_name], check=True, stderr=subprocess.DEVNULL)
    raw = np.frombuffer(open(raw_name, 'rb').read(), dtype='>i2').astype(int)
    os.unlink(raw_name)
    n_frames = w['len'] // 9
    got = []; p = 0
    for f in range(0, n_frames, 10):
        nf = min(10, n_frames - f)
        got.append(raw[p+16:p+16+nf*16]); p += 16 + nf*16      # skip the 16-sample state prefix
    got = np.concatenate(got)
    ref = E.vadpcm_decode(d[E.INST_TBL+base:E.INST_TBL+base+w['len']], w['book'])
    n = min(len(got), len(ref))
    if abs(got[:n] - ref[:n]).max() == 0:
        ok += 1
    else:
        print('MISMATCH wavetable base %#x' % base)
print('%d/%d wavetables bit-exact' % (ok, len(seen)))
sys.exit(0 if ok == len(seen) else 1)
