#!/usr/bin/env python3
# Render every GoldenEye sequence to music/wav/ with gemusic.  Looping tunes get one
# full pass of their loop (length from the extracted MIDI) plus a fade into the second
# pass; jingles end by themselves; sequences with no notes are skipped.
import glob, os, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FADE = 10.0

def midi_info(path):
    b = open(path, 'rb').read()
    ntrk, div = struct.unpack('>HH', b[10:14]); o = 14; notes = 0; end = 0; tempos = []
    for _ in range(ntrk):
        n = struct.unpack('>I', b[o+4:o+8])[0]; q = o + 8; e = q + n; t = 0
        while q < e:
            v = 0
            while True:
                c = b[q]; q += 1; v = (v << 7) | (c & 0x7f)
                if not c & 0x80:
                    break
            t += v; st = b[q]
            if st == 0xff:
                if b[q+1] == 0x51:
                    tempos.append((t, int.from_bytes(b[q+3:q+6], 'big')))
                q += 3 + b[q+2]
            else:
                hi = st & 0xf0
                notes += hi == 0x90
                q += 2 if hi in (0xc0, 0xd0) else 3
        end = max(end, t); o = e
    tempos.sort(); sec = 0.0; lt = 0; us = 500000
    for tt, u in tempos:
        sec += (tt - lt) * us / div / 1e6; lt = tt; us = u
    return notes, sec + (end - lt) * us / div / 1e6

def render(job):
    i, seconds = job
    out = '%s/music/wav/seq_%02d.wav' % (ROOT, i)
    r = subprocess.run([ROOT + '/gemusic/gemusic', '-r', ROOT + '/GoldenEye.z64', '-s', str(i),
                        '-t', '%.1f' % seconds, '--fade', str(FADE), '-o', out], capture_output=True, text=True)
    return i, r.returncode, r.stderr.strip().split('\n')[-1]

os.makedirs(ROOT + '/music/wav', exist_ok=True)
jobs = []
for p in sorted(glob.glob(ROOT + '/music/sequences/seq_*.mid')):
    i = int(p[-6:-4]); notes, sec = midi_info(p)
    if notes == 0:
        print('seq %02d: no notes, skipped' % i)
    else:
        jobs.append((i, sec + FADE))
with ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    for i, rc, msg in ex.map(render, jobs):
        print(msg if rc == 0 else 'seq %02d FAILED: %s' % (i, msg))
