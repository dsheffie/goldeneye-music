#ifndef __RSP_AVX512_HH__
#define __RSP_AVX512_HH__

#include <cstdint>
#include <cstring>
#include <immintrin.h>

#include "rsp.hh"

/* Vector-unit state for the statically recompiled audio microcode.
 *
 * An RSP vector register is 8 x 16-bit lanes = one xmm (lane i = element i).  The
 * awkward part of the RSP in SSE is the 48-bit accumulator per lane, normally kept
 * as three 16-bit vectors with hand-propagated carries.  With AVX-512 the eight
 * accumulators are simply 8 x int64 in one zmm: products come from vpmuldq, the
 * "clamp to signed 16" readout is vpmovsqw (saturating narrow), and the compare /
 * carry flags are mask registers.  Semantics mirror rsp.cc op for op. */
struct rsp_vec_t {
  __m128i v[32];
  __m512i acc;                           /* 8 x int64, sign-extended from 48 bits */
  __mmask8 vco_l = 0, vco_h = 0, vcc_l = 0, vcc_h = 0, vce = 0;
  rsp_vec_t() {
    for(int i = 0; i < 32; i++) {
      v[i] = _mm_setzero_si128();
    }
    acc = _mm512_setzero_si512();
  }
};

void aspmain_recomp(rsp_t &c, rsp_vec_t &V, uint32_t pc);
extern const uint32_t aspmain_recomp_hash;      /* FNV-1a of the text it was generated from */

namespace rspv {

static inline __m512i sext48(__m512i a) {
  return _mm512_srai_epi64(_mm512_slli_epi64(a, 16), 16);
}
static inline __m512i wide_s(__m128i x) { return _mm512_cvtepi16_epi64(x); }
static inline __m512i wide_u(__m128i x) { return _mm512_cvtepu16_epi64(x); }
/* vpmuldq multiplies the sign-extended low dword of each qword lane: exact for any
 * mix of signed/unsigned 16-bit operands once they are widened */
static inline __m512i mul(__m512i a, __m512i b) { return _mm512_mul_epi32(a, b); }

static inline __m128i sat_mid(__m512i acc) {            /* clamp_s16(acc >> 16) */
  return _mm512_cvtsepi64_epi16(_mm512_srai_epi64(acc, 16));
}
static inline __m128i clamp_low(__m512i acc) {          /* rsp.cc clamp_acc_low */
  __mmask8 lt = _mm512_cmplt_epi64_mask(acc, _mm512_set1_epi64(-0x80000000LL));
  __mmask8 gt = _mm512_cmpgt_epi64_mask(acc, _mm512_set1_epi64(0x7fffffffLL));
  __m128i lo = _mm512_cvtepi64_epi16(acc);
  lo = _mm_mask_mov_epi16(lo, gt, _mm_set1_epi16(-1));
  return _mm_mask_mov_epi16(lo, lt, _mm_setzero_si128());
}
static inline void set_acc_low(rsp_vec_t &V, __m128i res) {
  V.acc = _mm512_or_si512(_mm512_andnot_si512(_mm512_set1_epi64(0xffff), V.acc), wide_u(res));
}

static inline __m128i vmulf(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_add_epi64(_mm512_slli_epi64(mul(wide_s(s), wide_s(t)), 1), _mm512_set1_epi64(0x8000)));
  return sat_mid(V.acc);
}
static inline __m128i vmudl(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = _mm512_srli_epi64(mul(wide_u(s), wide_u(t)), 16);
  return clamp_low(V.acc);
}
static inline __m128i vmudm(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(mul(wide_s(s), wide_u(t)));
  return sat_mid(V.acc);
}
static inline __m128i vmudn(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(mul(wide_u(s), wide_s(t)));
  return clamp_low(V.acc);
}
static inline __m128i vmudh(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_slli_epi64(mul(wide_s(s), wide_s(t)), 16));
  return sat_mid(V.acc);
}
static inline __m128i vmacf(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_add_epi64(V.acc, _mm512_slli_epi64(mul(wide_s(s), wide_s(t)), 1)));
  return sat_mid(V.acc);
}
static inline __m128i vmadm(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_add_epi64(V.acc, mul(wide_s(s), wide_u(t))));
  return sat_mid(V.acc);
}
static inline __m128i vmadn(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_add_epi64(V.acc, mul(wide_u(s), wide_s(t))));
  return clamp_low(V.acc);
}
static inline __m128i vmadh(rsp_vec_t &V, __m128i s, __m128i t) {
  V.acc = sext48(_mm512_add_epi64(V.acc, _mm512_slli_epi64(mul(wide_s(s), wide_s(t)), 16)));
  return sat_mid(V.acc);
}
static inline __m128i vadd(rsp_vec_t &V, __m128i s, __m128i t) {
  __m512i x = _mm512_add_epi64(_mm512_add_epi64(wide_s(s), wide_s(t)), _mm512_maskz_set1_epi64(V.vco_l, 1));
  V.acc = _mm512_or_si512(_mm512_andnot_si512(_mm512_set1_epi64(0xffff), V.acc), _mm512_and_si512(x, _mm512_set1_epi64(0xffff)));
  V.vco_l = V.vco_h = 0;
  return _mm512_cvtsepi64_epi16(x);
}
static inline __m128i vsub(rsp_vec_t &V, __m128i s, __m128i t) {
  __m512i x = _mm512_sub_epi64(_mm512_sub_epi64(wide_s(s), wide_s(t)), _mm512_maskz_set1_epi64(V.vco_l, 1));
  V.acc = _mm512_or_si512(_mm512_andnot_si512(_mm512_set1_epi64(0xffff), V.acc), _mm512_and_si512(x, _mm512_set1_epi64(0xffff)));
  V.vco_l = V.vco_h = 0;
  return _mm512_cvtsepi64_epi16(x);
}
static inline __m128i vaddc(rsp_vec_t &V, __m128i s, __m128i t) {
  __m512i x = _mm512_add_epi64(wide_u(s), wide_u(t));
  __m128i res = _mm512_cvtepi64_epi16(x);
  set_acc_low(V, res);
  V.vco_l = _mm512_test_epi64_mask(x, _mm512_set1_epi64(0x10000));
  V.vco_h = 0;
  return res;
}
static inline __m128i vsar(rsp_vec_t &V, uint32_t e) {
  if(e == 8) { return _mm512_cvtepi64_epi16(_mm512_srai_epi64(V.acc, 32)); }
  if(e == 9) { return _mm512_cvtepi64_epi16(_mm512_srai_epi64(V.acc, 16)); }
  if(e == 10) { return _mm512_cvtepi64_epi16(V.acc); }
  return _mm_setzero_si128();
}
static inline __m128i vge(rsp_vec_t &V, __m128i s, __m128i t) {
  __mmask8 gt = _mm_cmpgt_epi16_mask(s, t), eq = _mm_cmpeq_epi16_mask(s, t);
  __mmask8 ge = static_cast<__mmask8>(gt | (eq & ~(V.vco_h & V.vco_l)));
  __m128i res = _mm_mask_blend_epi16(ge, t, s);
  set_acc_low(V, res);
  V.vcc_l = ge;
  V.vcc_h = 0;
  V.vco_l = V.vco_h = 0;
  return res;
}
static inline __m128i vcl(rsp_vec_t &V, __m128i s, __m128i t) {
  __m128i sum16 = _mm_add_epi16(s, t);                         /* low 16 of su+tu   */
  __mmask8 carry = _mm_cmplt_epu16_mask(sum16, s);             /* su+tu overflowed  */
  __mmask8 zero = _mm_cmpeq_epi16_mask(sum16, _mm_setzero_si128());
  /* sum == 0 (17-bit)  <=> zero & !carry ;  sum <= 0x10000 <=> !carry | zero */
  __mmask8 le = static_cast<__mmask8>((V.vce & (~carry | zero)) | (~V.vce & (zero & ~carry)));
  __mmask8 upd_l = static_cast<__mmask8>(V.vco_l & ~V.vco_h);
  V.vcc_l = static_cast<__mmask8>((V.vcc_l & ~upd_l) | (le & upd_l));
  __mmask8 geu = _mm_cmpge_epu16_mask(s, t);                   /* (su - tu) >= 0    */
  __mmask8 upd_h = static_cast<__mmask8>(~V.vco_l & ~V.vco_h);
  V.vcc_h = static_cast<__mmask8>((V.vcc_h & ~upd_h) | (geu & upd_h));
  __m128i neg_t = _mm_sub_epi16(_mm_setzero_si128(), t);
  __m128i r_carry = _mm_mask_blend_epi16(V.vcc_l, s, neg_t);   /* lanes with vco_l  */
  __m128i r_plain = _mm_mask_blend_epi16(V.vcc_h, s, t);       /* lanes without     */
  __m128i res = _mm_mask_blend_epi16(V.vco_l, r_plain, r_carry);
  set_acc_low(V, res);
  V.vco_l = V.vco_h = 0;
  V.vce = 0;
  return res;
}
static inline __m128i vand(rsp_vec_t &V, __m128i s, __m128i t) {
  __m128i res = _mm_and_si128(s, t);
  set_acc_low(V, res);
  return res;
}
static inline __m128i vxor(rsp_vec_t &V, __m128i s, __m128i t) {
  __m128i res = _mm_xor_si128(s, t);
  set_acc_low(V, res);
  return res;
}

/* ---- DMEM <-> vector.  A register as 16 big-endian bytes is just a bswap of each lane. ---- */

static inline __m128i bswap_lanes(__m128i x) {
  return _mm_shuffle_epi8(x, _mm_setr_epi8(1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14));
}
static inline void to_bytes(__m128i x, uint8_t *b) { _mm_storeu_si128(reinterpret_cast<__m128i*>(b), bswap_lanes(x)); }
static inline __m128i from_bytes(const uint8_t *b) { return bswap_lanes(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b))); }

static inline __m128i load_n(__m128i cur, const uint8_t *mem, uint32_t ea, uint32_t e, uint32_t n) {
  uint8_t b[16];
  to_bytes(cur, b);
  for(uint32_t i = 0; i < n and (e + i) < 16; i++) {
    b[e + i] = mem[(ea + i) & 0xfff];
  }
  return from_bytes(b);
}
static inline __m128i ldv(__m128i cur, const uint8_t *mem, uint32_t ea, uint32_t e) {
  ea &= 0xfff;
  if((e == 0 or e == 8) and ea <= 0xff8) {
    uint64_t q;
    memcpy(&q, mem + ea, 8);
    __m128i x = bswap_lanes(_mm_cvtsi64_si128(static_cast<int64_t>(q)));
    return (e == 0) ? _mm_blend_epi16(cur, x, 0x0f) : _mm_blend_epi16(cur, _mm_slli_si128(x, 8), 0xf0);
  }
  return load_n(cur, mem, ea, e, 8);
}
static inline __m128i lqv(__m128i cur, const uint8_t *mem, uint32_t ea, uint32_t e) {
  if(e == 0 and (ea & 15) == 0) {
    return from_bytes(mem + (ea & 0xff0));
  }
  return load_n(cur, mem, ea, e, 16 - (ea & 15));
}
static inline __m128i lrv(__m128i cur, const uint8_t *mem, uint32_t ea, uint32_t e) {
  uint32_t start = 16 - ((ea & 15) - e);
  if(start >= 16) {
    return cur;                                   /* aligned: nothing to the left of ea */
  }
  uint8_t b[16];
  to_bytes(cur, b);
  ea &= ~15u;
  for(uint32_t i = start; i < 16; i++) {
    b[i] = mem[ea & 0xfff];
    ea++;
  }
  return from_bytes(b);
}
static inline void store_n(__m128i x, uint8_t *mem, uint32_t ea, uint32_t e, uint32_t n) {
  uint8_t b[16];
  to_bytes(x, b);
  for(uint32_t i = 0; i < n; i++) {
    mem[(ea + i) & 0xfff] = b[(e + i) & 15];
  }
}
static inline void sqv(__m128i x, uint8_t *mem, uint32_t ea, uint32_t e) {
  if(e == 0 and (ea & 15) == 0) {
    to_bytes(x, mem + (ea & 0xff0));
    return;
  }
  store_n(x, mem, ea, e, 16 - (ea & 15));
}
static inline uint32_t mfc2(__m128i x, uint32_t e) {
  uint8_t b[16];
  to_bytes(x, b);
  return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>((b[e & 15] << 8) | b[(e + 1) & 15])));
}
static inline __m128i mtc2(__m128i cur, uint32_t e, uint32_t val) {
  uint8_t b[16];
  to_bytes(cur, b);
  b[e] = static_cast<uint8_t>(val >> 8);
  if(e != 15) {
    b[e + 1] = static_cast<uint8_t>(val);
  }
  return from_bytes(b);
}

/* ---- integer-side DMEM access (wraps, unaligned legal) ---- */
static inline uint32_t rd16(const uint8_t *m, uint32_t a) { return static_cast<uint32_t>((m[a & 0xfff] << 8) | m[(a + 1) & 0xfff]); }
static inline uint32_t rd32(const uint8_t *m, uint32_t a) {
  return (static_cast<uint32_t>(m[a & 0xfff]) << 24) | (m[(a + 1) & 0xfff] << 16) | (m[(a + 2) & 0xfff] << 8) | m[(a + 3) & 0xfff];
}
static inline void wr16(uint8_t *m, uint32_t a, uint32_t x) { m[a & 0xfff] = static_cast<uint8_t>(x >> 8); m[(a + 1) & 0xfff] = static_cast<uint8_t>(x); }
static inline void wr32(uint8_t *m, uint32_t a, uint32_t x) {
  m[a & 0xfff] = static_cast<uint8_t>(x >> 24); m[(a + 1) & 0xfff] = static_cast<uint8_t>(x >> 16);
  m[(a + 2) & 0xfff] = static_cast<uint8_t>(x >> 8); m[(a + 3) & 0xfff] = static_cast<uint8_t>(x);
}

}

#endif
