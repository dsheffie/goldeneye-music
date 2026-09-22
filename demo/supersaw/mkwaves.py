#!/usr/bin/env python3
# Generate the single-cycle wavetables the demo plays.  Everything here is synthesised
# from first principles -- additive sums of harmonics -- so the audio is original work
# with no third-party content anywhere in the ROM.
#
# One cycle is 64 samples, which keeps the playback rate sane: the mixer resamples, so a
# note of f Hz means asking for f*64 samples per second.  64 samples also band-limits the
# saw for free at 32 harmonics, which is why it does not tear itself apart at high notes.
import math, struct, os

N = 64

def write_wav(path, samples):
    b = b''.join(struct.pack('<h', max(-32767, min(32767, int(s)))) for s in samples)
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(b)) + b'WAVEfmt ' +
           struct.pack('<IHHIIHH', 16, 1, 1, 44100, 44100 * 2, 2, 16) +
           b'data' + struct.pack('<I', len(b)))
    open(path, 'wb').write(hdr + b)
    print('  %-22s %d samples' % (os.path.basename(path), len(samples)))

def saw(nharm):
    """Additive sawtooth: sum of 1/k harmonics, which is the bright edge a supersaw needs."""
    out = []
    for i in range(N):
        t = i / N
        v = sum(math.sin(2 * math.pi * k * t) / k for k in range(1, nharm + 1))
        out.append(v * 32767 * 0.55)
    return out

def soft(nharm):
    """The same shape with the upper harmonics rolled off, so crossfading between this
    and the bright saw reads as a filter opening without needing a real filter."""
    out = []
    for i in range(N):
        t = i / N
        v = sum(math.sin(2 * math.pi * k * t) / k * math.exp(-0.55 * (k - 1))
                for k in range(1, nharm + 1))
        out.append(v * 32767 * 0.8)
    return out

def kick(ms=260):
    """A one-shot kick: a sine whose pitch sweeps from a click down to a thud, with a
    fast amplitude decay.  Not a single-cycle table -- this one plays at its own rate and
    does not loop, which is what makes it percussion rather than a note."""
    n = int(44100 * ms / 1000)
    out, phase = [], 0.0
    for i in range(n):
        t = i / 44100.0
        f = 42.0 + 150.0 * math.exp(-t * 32.0)      # pitch envelope: click -> body
        phase += 2 * math.pi * f / 44100.0
        amp = math.exp(-t * 9.0)                    # body
        click = math.exp(-t * 420.0) * 0.5          # transient so it cuts through
        out.append((math.sin(phase) * amp + click) * 32767 * 0.82)
    return out

os.makedirs('assets', exist_ok=True)
write_wav('assets/saw.wav', saw(32))
write_wav('assets/soft.wav', soft(8))
write_wav('assets/kick.wav', kick())
