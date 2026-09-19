#!/usr/bin/env python3
# Statically recompile GoldenEye's audio microcode (aspMain) into C++ with AVX-512
# intrinsics.  usage: rsp2avx512.py <rom.z64> <out.cc>
#
# The output is derived from the ROM, so it is generated at build time and never
# checked in.  One label per instruction; a branch is emitted together with its delay
# slot; indirect jumps (jr: the command dispatch and subroutine returns) go back
# through a switch on the 12-bit PC.
import struct, sys, zlib

ROM_DATA_1172 = 0x21990
DATA_VADDR = 0x80020d90
ASPMAIN_TEXT = 0x80022280
LOAD_ADDR = 0x080            # rspboot DMAs 0xf80 bytes of text to IMEM 0x080
TEXT_LEN = 0xf80

rom = open(sys.argv[1], 'rb').read()
assert rom[ROM_DATA_1172:ROM_DATA_1172+2] == b'\x11\x72'
seg = zlib.decompressobj(-15).decompress(rom[ROM_DATA_1172+2:])
text = seg[ASPMAIN_TEXT-DATA_VADDR:ASPMAIN_TEXT-DATA_VADDR+TEXT_LEN]
words = {LOAD_ADDR + 4*i: struct.unpack('>I', text[4*i:4*i+4])[0] for i in range(TEXT_LEN // 4)}

def fnv1a(b):
    h = 0x811c9dc5
    for c in b:
        h = ((h ^ c) * 0x01000193) & 0xffffffff
    return h

class Unsupported(Exception):
    pass

def R(n):
    return 'c.r[%d]' % n

def setr(n, expr):
    return '%s = %s;' % (R(n), expr) if n else '(void)(%s);' % expr

VEC = {0x00: 'vmulf', 0x04: 'vmudl', 0x05: 'vmudm', 0x06: 'vmudn', 0x07: 'vmudh', 0x08: 'vmacf',
       0x0d: 'vmadm', 0x0e: 'vmadn', 0x0f: 'vmadh', 0x10: 'vadd', 0x11: 'vsub', 0x14: 'vaddc',
       0x23: 'vge', 0x24: 'vcl', 0x28: 'vand', 0x2c: 'vxor'}

def vt_select(vt, e):
    """the element field reshapes vt: one vpermw with a constant index vector"""
    if e < 2:
        return 'V.v[%d]' % vt
    if e < 4:
        lanes = [(i & ~1) | (e & 1) for i in range(8)]
    elif e < 8:
        lanes = [(i & ~3) | (e & 3) for i in range(8)]
    else:
        lanes = [e & 7] * 8
    return '_mm_permutexvar_epi16(_mm_setr_epi16(%s), V.v[%d])' % (','.join(map(str, lanes)), vt)

def is_branch(w):
    op = w >> 26
    return op in (1, 2, 3, 4, 5, 6, 7) or (op == 0 and (w & 63) in (8, 9))

def simple(w, pc):
    """C++ for one non-branch instruction"""
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sa = (w >> 6) & 31
    imm = w & 0xffff; simm = imm - 0x10000 if imm & 0x8000 else imm
    ea = '(%s + %du)' % (R(rs), simm & 0xffffffff)
    if w == 0:
        return ';'
    if op == 0:
        f = w & 63
        t = {0x00: '%s << %d' % (R(rt), sa), 0x02: '%s >> %d' % (R(rt), sa),
             0x03: 'static_cast<uint32_t>(static_cast<int32_t>(%s) >> %d)' % (R(rt), sa),
             0x04: '%s << (%s & 31)' % (R(rt), R(rs)), 0x06: '%s >> (%s & 31)' % (R(rt), R(rs)),
             0x07: 'static_cast<uint32_t>(static_cast<int32_t>(%s) >> (%s & 31))' % (R(rt), R(rs)),
             0x20: '%s + %s' % (R(rs), R(rt)), 0x21: '%s + %s' % (R(rs), R(rt)),
             0x22: '%s - %s' % (R(rs), R(rt)), 0x23: '%s - %s' % (R(rs), R(rt)),
             0x24: '%s & %s' % (R(rs), R(rt)), 0x25: '%s | %s' % (R(rs), R(rt)),
             0x26: '%s ^ %s' % (R(rs), R(rt)), 0x27: '~(%s | %s)' % (R(rs), R(rt)),
             0x2a: 'static_cast<int32_t>(%s) < static_cast<int32_t>(%s)' % (R(rs), R(rt)),
             0x2b: '%s < %s' % (R(rs), R(rt))}.get(f)
        if f == 0x0d:
            return 'c.halted = true; return;'
        if t is None:
            raise Unsupported('special %#x' % f)
        return setr(rd, t)
    if op in (8, 9):
        return setr(rt, '%s + %du' % (R(rs), simm & 0xffffffff))
    if op == 0x0a:
        return setr(rt, 'static_cast<int32_t>(%s) < %d' % (R(rs), simm))
    if op == 0x0b:
        return setr(rt, '%s < %du' % (R(rs), simm & 0xffffffff))
    if op == 0x0c:
        return setr(rt, '%s & %du' % (R(rs), imm))
    if op == 0x0d:
        return setr(rt, '%s | %du' % (R(rs), imm))
    if op == 0x0e:
        return setr(rt, '%s ^ %du' % (R(rs), imm))
    if op == 0x0f:
        return setr(rt, '%du' % (imm << 16))
    if op == 0x10:
        if rs == 0:
            return setr(rt, 'c.mfc0(%d)' % rd)
        if rs == 4:
            return 'c.mtc0(%d, %s);' % (rd, R(rt))
        raise Unsupported('cop0 rs=%d' % rs)
    if op == 0x20:
        return setr(rt, 'static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(c.mem[%s & 0xfff])))' % ea)
    if op == 0x21:
        return setr(rt, 'static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rspv::rd16(c.mem, %s))))' % ea)
    if op == 0x23:
        return setr(rt, 'rspv::rd32(c.mem, %s)' % ea)
    if op == 0x24:
        return setr(rt, 'c.mem[%s & 0xfff]' % ea)
    if op == 0x25:
        return setr(rt, 'rspv::rd16(c.mem, %s)' % ea)
    if op == 0x28:
        return 'c.mem[%s & 0xfff] = static_cast<uint8_t>(%s);' % (ea, R(rt))
    if op == 0x29:
        return 'rspv::wr16(c.mem, %s, %s);' % (ea, R(rt))
    if op == 0x2b:
        return 'rspv::wr32(c.mem, %s, %s);' % (ea, R(rt))
    if op == 0x12:
        if not (w >> 25) & 1:
            e = (w >> 7) & 15
            if rs == 0:
                return setr(rt, 'rspv::mfc2(V.v[%d], %d)' % (rd, e))
            if rs == 4:
                return 'V.v[%d] = rspv::mtc2(V.v[%d], %d, %s);' % (rd, rd, e, R(rt))
            raise Unsupported('cop2 move rs=%d' % rs)
        e = (w >> 21) & 15; vt = rt; vs = rd; vd = sa; f = w & 63
        if f == 0x1d:
            return 'V.v[%d] = rspv::vsar(V, %d);' % (vd, e)
        if f not in VEC:
            raise Unsupported('vector op %#x' % f)
        return 'V.v[%d] = rspv::%s(V, V.v[%d], %s);' % (vd, VEC[f], vs, vt_select(vt, e))
    if op in (0x32, 0x3a):
        kind = rd; e = (w >> 7) & 15; o = w & 0x7f; o = o - 128 if o & 0x40 else o
        if op == 0x32:
            if kind in (1, 2):
                return 'V.v[%d] = rspv::load_n(V.v[%d], c.mem, %s + %du, %d, %d);' % (rt, rt, R(rs), (o << kind) & 0xffffffff, e, 1 << kind)
            name = {3: 'ldv', 4: 'lqv', 5: 'lrv'}.get(kind)
            if name is None:
                raise Unsupported('vector load %d' % kind)
            return 'V.v[%d] = rspv::%s(V.v[%d], c.mem, %s + %du, %d);' % (rt, name, rt, R(rs), (o << min(kind, 4)) & 0xffffffff, e)
        if kind in (1, 2, 3):
            return 'rspv::store_n(V.v[%d], c.mem, %s + %du, %d, %d);' % (rt, R(rs), (o << kind) & 0xffffffff, e, 1 << kind)
        if kind == 4:
            return 'rspv::sqv(V.v[%d], c.mem, %s + %du, %d);' % (rt, R(rs), (o << 4) & 0xffffffff, e)
        raise Unsupported('vector store %d' % kind)
    raise Unsupported('opcode %#x' % op)

def label(a):
    return 'L_%03x' % a

def target_ok(t):
    return LOAD_ADDR <= t <= LOAD_ADDR + TEXT_LEN - 4

out = []
n_bad = 0
for pc in sorted(words):
    w = words[pc]
    head = '    case 0x%03x: %s: ' % (pc, label(pc))
    try:
        if not is_branch(w):
            out.append(head + '{ ' + simple(w, pc) + ' }')
            continue
        dpc = pc + 4
        if dpc not in words or is_branch(words[dpc]):
            raise Unsupported('branch without a plain delay slot')
        delay = simple(words[dpc], dpc)
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
        simm = (w & 0xffff) - 0x10000 if w & 0x8000 else w & 0xffff
        nxt = pc + 8
        fall = 'goto %s;' % label(nxt) if target_ok(nxt) else 'rsp_recomp_die(0x%03x);' % nxt
        def go(t):
            return 'goto %s;' % label(t) if target_ok(t) else 'rsp_recomp_die(0x%03x);' % t
        if op == 0:
            link = setr(rd, '0x%03xu' % (nxt & 0xffc)) if (w & 63) == 9 else ''
            body = 'uint32_t tgt = %s & 0xffc; %s %s pc = tgt; continue;' % (R(rs), link, delay)
        elif op in (2, 3):
            t = (w << 2) & 0xffc
            link = setr(31, '0x%03xu' % (nxt & 0xffc)) if op == 3 else ''
            body = '%s %s %s' % (link, delay, go(t))
        else:
            t = (dpc + (simm << 2)) & 0xffc
            if op == 1:
                if rt not in (0, 1):
                    raise Unsupported('regimm %d' % rt)
                cond = 'static_cast<int32_t>(%s) %s 0' % (R(rs), '<' if rt == 0 else '>=')
            else:
                cond = {4: '%s == %s' % (R(rs), R(rt)), 5: '%s != %s' % (R(rs), R(rt)),
                        6: 'static_cast<int32_t>(%s) <= 0' % R(rs), 7: 'static_cast<int32_t>(%s) > 0' % R(rs)}[op]
            body = 'bool taken = (%s); %s if(taken) { %s } %s' % (cond, delay, go(t), fall)
        out.append(head + '{ ' + body + ' }')
    except Unsupported as e:
        n_bad += 1                                  # data words that live in the text section
        out.append(head + '{ rsp_recomp_die(0x%03x); } /* %08x: %s */' % (pc, w, e))

with open(sys.argv[2], 'w') as f:
    f.write('/* GENERATED by rsp2avx512.py from the ROM -- do not edit, do not check in. */\n')
    f.write('#include <cstdio>\n#include <cstdlib>\n#include "rsp_avx512.hh"\n\n')
    f.write('#pragma GCC diagnostic ignored "-Wunused-label"\n\n')
    f.write('const uint32_t aspmain_recomp_hash = 0x%08xu;\n\n' % fnv1a(text))
    f.write('[[noreturn]] static void rsp_recomp_die(uint32_t pc) {\n  fprintf(stderr, "recompiled microcode reached untranslated pc %03x\\n", pc);\n  exit(-1);\n}\n\n')
    f.write('void aspmain_recomp(rsp_t &c, rsp_vec_t &V, uint32_t pc) {\n  for(;;) {\n    switch(pc)\n    {\n')
    f.write('\n'.join(out))
    f.write('\n    default: rsp_recomp_die(pc);\n    }\n    rsp_recomp_die(0xfff);   /* fell off the end of IMEM */\n  }\n}\n')
print('%d instructions translated, %d untranslatable words (data)' % (len(words) - n_bad, n_bad))
