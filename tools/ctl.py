#!/usr/bin/env python3
# Parse a libultra ALBankFile (.ctl) embedded in the ROM.
import struct

def u8(d,o):  return d[o]
def s8(d,o):  return struct.unpack('>b',d[o:o+1])[0]
def u16(d,o): return struct.unpack('>H',d[o:o+2])[0]
def s16(d,o): return struct.unpack('>h',d[o:o+2])[0]
def u32(d,o): return struct.unpack('>I',d[o:o+4])[0]
def s32(d,o): return struct.unpack('>i',d[o:o+4])[0]

def parse_ctl(d, base):
    """returns (bank dict, ctl_extent, tbl_extent)"""
    ext = [0]; tblext = [0]
    def touch(o, n):
        ext[0] = max(ext[0], o + n)
    assert u16(d, base) == 0x4231
    nbank = u16(d, base+2)
    banks = []
    for b in range(nbank):
        bo = u32(d, base+4+4*b)
        p = base + bo
        ninst = u16(d, p)
        bank = {'flags': u8(d,p+2), 'sample_rate': s32(d,p+4), 'percussion': u32(d,p+8), 'instruments': []}
        touch(bo, 12 + 4*ninst)
        ioffs = [u32(d, p+12+4*i) for i in range(ninst)]
        if bank['percussion']:
            ioffs_p = [bank['percussion']]
        else:
            ioffs_p = []
        def parse_inst(io):
            q = base + io
            nsnd = s16(d, q+14)
            inst = {'offset': io, 'volume': u8(d,q), 'pan': u8(d,q+1), 'priority': u8(d,q+2), 'flags': u8(d,q+3),
                    'trem': [u8(d,q+4+k) for k in range(4)], 'vib': [u8(d,q+8+k) for k in range(4)],
                    'bend_range': s16(d,q+12), 'sounds': []}
            touch(io, 16 + 4*nsnd)
            for s in range(nsnd):
                so = u32(d, q+16+4*s)
                r = base + so
                touch(so, 16)
                envo, kmo, wto = u32(d,r), u32(d,r+4), u32(d,r+8)
                snd = {'offset': so, 'pan': u8(d,r+12), 'volume': u8(d,r+13), 'flags': u8(d,r+14)}
                e = base + envo; touch(envo, 16)
                snd['envelope'] = {'attack_time': s32(d,e), 'decay_time': s32(d,e+4), 'release_time': s32(d,e+8),
                                   'attack_volume': u8(d,e+12), 'decay_volume': u8(d,e+13)}
                k = base + kmo; touch(kmo, 8)
                snd['keymap'] = {'vel_min': u8(d,k), 'vel_max': u8(d,k+1), 'key_min': u8(d,k+2), 'key_max': u8(d,k+3),
                                 'key_base': u8(d,k+4), 'detune': s8(d,k+5)}
                w = base + wto; touch(wto, 20)
                wt = {'offset': wto, 'base': u32(d,w), 'len': s32(d,w+4), 'type': u8(d,w+8), 'flags': u8(d,w+9)}
                tblext[0] = max(tblext[0], wt['base'] + wt['len'])
                loopo = u32(d, w+12)
                if wt['type'] == 0:
                    booko = u32(d, w+16)
                    bk = base + booko
                    order, npred = s32(d,bk), s32(d,bk+4)
                    n = order*npred*8
                    touch(booko, 8 + 2*n)
                    wt['book'] = {'order': order, 'npredictors': npred,
                                  'coefs': list(struct.unpack('>%dh'%n, d[bk+8:bk+8+2*n]))}
                    if loopo:
                        l = base + loopo; touch(loopo, 12+32)
                        wt['loop'] = {'start': u32(d,l), 'end': u32(d,l+4), 'count': u32(d,l+8),
                                      'state': list(struct.unpack('>16h', d[l+12:l+44]))}
                else:
                    if loopo:
                        l = base + loopo; touch(loopo, 12)
                        wt['loop'] = {'start': u32(d,l), 'end': u32(d,l+4), 'count': u32(d,l+8)}
                snd['wavetable'] = wt
                inst['sounds'].append(snd)
            return inst
        bank['instruments'] = [parse_inst(io) if io else None for io in ioffs]
        bank['percussion_inst'] = parse_inst(ioffs_p[0]) if ioffs_p else None
        banks.append(bank)
    return banks, ext[0], tblext[0]

if __name__ == '__main__':
    import sys
    d = open(sys.argv[1], 'rb').read()      # usage: ctl.py <rom.z64>
    for base in (0x2ebde0, 0x3b4450):
        banks, ce, te = parse_ctl(d, base)
        b = banks[0]
        insts = [i for i in b['instruments'] if i]
        nsnd = sum(len(i['sounds']) for i in insts) + (len(b['percussion_inst']['sounds']) if b['percussion_inst'] else 0)
        wts = {}
        for i in insts + ([b['percussion_inst']] if b['percussion_inst'] else []):
            for s in i['sounds']:
                wts[s['wavetable']['base']] = s['wavetable']
        types = [w['type'] for w in wts.values()]
        print("ctl @%#x: insts=%d (nonnull %d) perc=%s sounds=%d unique_wavetables=%d adpcm=%d raw=%d rate=%d ctl_size=%#x tbl_extent=%#x"%(
            base, len(b['instruments']), len(insts), bool(b['percussion_inst']), nsnd, len(wts), types.count(0), types.count(1), b['sample_rate'], ce, te))
        print("   ctl ends @%#x ; candidate tbl start (16-aligned) @%#x ; tbl would end @%#x"%(base+ce, (base+ce+15)&~15, ((base+ce+15)&~15)+te))
