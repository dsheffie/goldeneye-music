#!/usr/bin/env python3
# Extract GoldenEye 007 (NGEE) music: sequences -> .mid, instrument samples -> .wav, bank -> .json
import json, os, struct, sys, zlib
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ctl import parse_ctl

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'GoldenEye.z64')   # your own ROM
OUT = os.path.join(ROOT, 'music')
INST_CTL = 0x3b4450
INST_TBL = 0x3b87f0
SEQ_TABLE = 0x419790

# ---------------------------------------------------------------- sequences

def load_sequences(d):
    cnt = struct.unpack('>H', d[SEQ_TABLE:SEQ_TABLE+2])[0]
    seqs = []
    for i in range(cnt):
        off, ulen, clen = struct.unpack('>IHH', d[SEQ_TABLE+4+8*i:SEQ_TABLE+12+8*i])
        blob = d[SEQ_TABLE+off:SEQ_TABLE+off+clen]
        assert blob[:2] == b'\x11\x72'
        raw = zlib.decompressobj(-15).decompress(blob[2:])
        assert len(raw) == ulen
        seqs.append(raw)
    return seqs

class track_reader:
    """libultra cseq.c __getTrackByte: 0xFE hi lo len = replay 'len' raw bytes from (pos_of_FE - offset)."""
    def __init__(self, data, pos):
        self.d = bytearray(data)
        self.pos = pos
        self.bu_pos = 0
        self.bu_len = 0
    def byte(self):
        if self.bu_len:
            b = self.d[self.bu_pos]; self.bu_pos += 1; self.bu_len -= 1
            return b
        b = self.d[self.pos]; self.pos += 1
        if b == 0xFE:
            n = self.d[self.pos]; self.pos += 1
            if n != 0xFE:
                lo = self.d[self.pos]; ln = self.d[self.pos+1]; self.pos += 2
                self.bu_pos = self.pos - (((n << 8) | lo) + 4)
                self.bu_len = ln
                b = self.d[self.bu_pos]; self.bu_pos += 1; self.bu_len -= 1
        return b
    def varlen(self):
        v = 0
        while True:
            b = self.byte()
            v = (v << 7) | (b & 0x7f)
            if not (b & 0x80):
                return v

def decode_track(data, start):
    """returns list of (abs_tick, order, bytes) standard-MIDI events (no delta), plus tempo events separately."""
    r = track_reader(data, start)
    ev = []; tempo = []
    t = 0; last = 0; seq_no = 0
    while True:
        t += r.varlen()
        st = r.byte()
        seq_no += 1
        if st == 0xFF:
            ty = r.byte()
            if ty == 0x51:
                tempo.append((t, seq_no, b'\xff\x51\x03' + bytes([r.byte(), r.byte(), r.byte()])))
                last = 0
            elif ty == 0x2F:
                break
            elif ty == 0x2E:                      # loop start: two bytes, ignored by the player
                r.byte(); r.byte(); last = 0
                ev.append((t, seq_no, b'\xff\x06\x09loopStart'))
            elif ty == 0x2D:                      # loop end: count, cur_count, u32 offset (raw, not via backup)
                p = r.pos
                loop_ct, cur = r.d[p], r.d[p+1]
                last = 0
                if cur == 0 or cur == 0xFF:       # finished, or loop-forever: do not unroll
                    if cur == 0xFF:
                        ev.append((t, seq_no, b'\xff\x06\x07loopEnd'))
                    r.d[p+1] = loop_ct
                    r.pos = p + 6
                    if cur == 0xFF:
                        break                     # everything after an infinite loop is unreachable
                else:
                    r.d[p+1] = cur - 1
                    off = struct.unpack('>I', r.d[p+2:p+6])[0]
                    r.pos = p + 6 - off
            else:
                raise ValueError('unknown meta %#x' % ty)
            continue
        if st & 0x80:
            b1 = r.byte(); last = st
        else:
            b1 = st; st = last
        hi = st & 0xf0
        if hi in (0xC0, 0xD0):
            ev.append((t, seq_no, bytes([st, b1])))
            continue
        b2 = r.byte()
        if hi == 0x90:
            dur = r.varlen()
            ev.append((t, seq_no, bytes([st, b1, b2])))
            ev.append((t + dur, -1, bytes([0x80 | (st & 0x0f), b1, 0])))   # note-offs sort first at a tick
        else:
            ev.append((t, seq_no, bytes([st, b1, b2])))
    return ev, tempo, t

def vlq(n):
    out = [n & 0x7f]; n >>= 7
    while n:
        out.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(out))

def mtrk(events, end_tick):
    events = sorted(events, key=lambda e: (e[0], e[1]))
    body = bytearray(); t = 0
    for tick, _, data in events:
        body += vlq(tick - t) + data; t = tick
    body += vlq(max(end_tick, t) - t) + b'\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(body)) + bytes(body)

def cseq_to_smf(raw):
    offs = struct.unpack('>16I', raw[:64]); division = struct.unpack('>I', raw[64:68])[0]
    tracks = []; tempos = []; end = 0
    for o in offs:
        if o:
            ev, tp, t_end = decode_track(raw, o)
            tracks.append(ev); tempos += tp
            end = max([end, t_end] + [e[0] for e in ev])
    chunks = [mtrk(tempos, end)] + [mtrk(ev, end) for ev in tracks]
    return b'MThd' + struct.pack('>IHHH', 6, 1, len(chunks), division) + b''.join(chunks), len(tracks), end, division

# ---------------------------------------------------------------- samples

def expand_book(book):
    order, npred, c = book['order'], book['npredictors'], book['coefs']
    tabs = []
    for p in range(npred):
        t = np.zeros((8, order + 8), dtype=np.int64)
        k = p * order * 8
        for j in range(order):
            for i in range(8):
                t[i][j] = c[k]; k += 1
        for i in range(1, 8):
            t[i][order] = t[i-1][order-1]
        t[0][order] = 1 << 11
        for kk in range(1, 8):
            for m in range(kk, 8):
                t[m][kk+order] = t[m-kk][order]
        tabs.append(t)
    return tabs

def vadpcm_decode(data, book):
    order = book['order']; tabs = expand_book(book)
    nframes = len(data) // 9
    out = np.zeros(nframes * 16, dtype=np.int64)
    state = np.zeros(order, dtype=np.int64)
    for f in range(nframes):
        fr = data[9*f:9*f+9]
        scale = fr[0] >> 4; tab = tabs[fr[0] & 0xf]
        nib = np.empty(16, dtype=np.int64)
        for i in range(8):
            nib[2*i] = fr[1+i] >> 4; nib[2*i+1] = fr[1+i] & 0xf
        nib = np.where(nib >= 8, nib - 16, nib) << scale
        for h in range(2):
            vec = np.concatenate((state, nib[8*h:8*h+8]))
            # floor division, then saturate BEFORE feedback: the RSP's vector ops clamp every
            # sample, so this is what the console plays (verified bit-exact against the real
            # audio microcode for all 105 wavetables).  Nintendo's offline vadpcm_dec feeds
            # back unclamped values and differs on the few samples that overshoot.
            o8 = np.clip((tab @ vec) >> 11, -32768, 32767)
            out[16*f+8*h:16*f+8*h+8] = o8
            state = o8[8-order:]
    return out

def write_wav(path, pcm, rate, key_base, loop):
    pcm16 = np.clip(pcm, -32768, 32767).astype('<i2').tobytes()
    fmt = struct.pack('<HHIIHH', 1, 1, rate, rate*2, 2, 16)
    chunks = b'fmt ' + struct.pack('<I', len(fmt)) + fmt
    if loop or key_base is not None:
        nloops = 1 if loop else 0
        smpl = struct.pack('<9I', 0, 0, 1000000000 // rate, key_base or 60, 0, 0, 0, nloops, 0)
        if loop:
            smpl += struct.pack('<6I', 0, 0, loop['start'], loop['end'] - 1, 0, 0 if loop['count'] == 0xFFFFFFFF else loop['count'])
        chunks += b'smpl' + struct.pack('<I', len(smpl)) + smpl
    chunks += b'data' + struct.pack('<I', len(pcm16)) + pcm16
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 4 + len(chunks)) + b'WAVE' + chunks)

# ---------------------------------------------------------------- main

def main():
    d = open(ROM, 'rb').read()
    os.makedirs(OUT + '/sequences', exist_ok=True)
    os.makedirs(OUT + '/samples', exist_ok=True)

    seqs = load_sequences(d)
    for i, raw in enumerate(seqs):
        open('%s/sequences/seq_%02d.cseq' % (OUT, i), 'wb').write(raw)
        if struct.unpack('>16I', raw[:64]) == (0,)*16:
            print('seq %02d: empty' % i); continue
        smf, ntrk, end, div = cseq_to_smf(raw)
        open('%s/sequences/seq_%02d.mid' % (OUT, i), 'wb').write(smf)
        print('seq %02d: %2d tracks, %6d ticks, division %d, %d bytes' % (i, ntrk, end, div, len(smf)))

    banks, _, _ = parse_ctl(d, INST_CTL)
    bank = banks[0]; rate = bank['sample_rate']
    done = {}
    for ii, inst in enumerate(bank['instruments']):
        for si, snd in enumerate(inst['sounds']):
            wt = snd['wavetable']
            if wt['base'] not in done:
                blob = d[INST_TBL+wt['base']:INST_TBL+wt['base']+wt['len']]
                if wt['type'] == 0:
                    pcm = vadpcm_decode(blob, wt['book'])
                else:
                    pcm = np.frombuffer(blob[:len(blob)&~1], dtype='>i2').astype(np.int64)
                name = 'sample_%03d_tbl%06x.wav' % (len(done), wt['base'])
                write_wav('%s/samples/%s' % (OUT, name), pcm, rate, snd['keymap']['key_base'], wt.get('loop'))
                done[wt['base']] = (name, pcm)
            snd['wav'] = done[wt['base']][0]
    json.dump(bank, open(OUT + '/bank.json', 'w'), indent=1)
    print('%d instruments, %d unique samples, rate %d' % (len(bank['instruments']), len(done), rate))
    return done, bank

if __name__ == '__main__':
    main()
