#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

#include "engine.hh"
#include "ge_addrs.hh"

std::vector<uint8_t> inflate_1172(const uint8_t *p, size_t avail);

static uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

/* Bring the synthesizer and one compact-sequence player up exactly the way
 * GoldenEye's sndInit does (0x80006a2c): same voice counts, same custom reverb. */
uint32_t ge_audio_init(guest_t &g, const std::vector<uint8_t> &rom) {
  g.call(GE_alHeapInit, G_ALHEAP, G_AUDIO_HEAP, G_AUDIO_HEAP_LEN);

  memcpy(g.ptr(G_CTL), &rom[GE_ROM_INST_CTL], GE_ROM_INST_CTL_LEN);
  memcpy(g.ptr(G_TBL), &rom[GE_ROM_INST_TBL], GE_ROM_INST_TBL_LEN);
  g.call(GE_alBnkfNew, G_CTL, G_TBL);
  uint32_t bank = g.rd32(G_CTL + 4);

  /* ALSynConfig */
  g.wr32(G_SYNCONFIG + 0, 0);
  g.wr32(G_SYNCONFIG + 4, GE_MAX_PVOICES);
  g.wr32(G_SYNCONFIG + 8, GE_MAX_UPDATES);
  g.wr32(G_SYNCONFIG + 12, 0);
  g.wr32(G_SYNCONFIG + 16, G_STUBS);              /* dmaproc (dmaNew) */
  g.wr32(G_SYNCONFIG + 20, G_ALHEAP);
  g.wr32(G_SYNCONFIG + 24, GE_OUTPUT_RATE);
  g.wr8(G_SYNCONFIG + 28, GE_FX_TYPE);
  g.wr32(G_SYNCONFIG + 32, GE_custom_fx_params);
  g.call(GE_alInit, G_ALGLOBALS, G_SYNCONFIG);

  /* ALSeqpConfig: no oscillator callbacks, as in the game */
  g.wr32(G_SEQPCONFIG + 0, GE_SEQP_MAX_VOICES);
  g.wr32(G_SEQPCONFIG + 4, GE_SEQP_MAX_EVENTS);
  g.wr8(G_SEQPCONFIG + 8, GE_SEQP_MAX_CHANNELS);
  g.wr8(G_SEQPCONFIG + 9, 0);
  g.wr32(G_SEQPCONFIG + 12, G_ALHEAP);
  g.wr32(G_SEQPCONFIG + 16, 0);
  g.wr32(G_SEQPCONFIG + 20, 0);
  g.wr32(G_SEQPCONFIG + 24, 0);
  g.call(GE_alCSPNew, G_CSPLAYER, G_SEQPCONFIG);
  g.call(GE_alCSPSetBank, G_CSPLAYER, bank);
  return bank;
}

void ge_start_sequence(guest_t &g, const std::vector<uint8_t> &rom, int seq) {
  const uint8_t *t = &rom[GE_ROM_SEQ_TABLE];
  int n_seqs = be16(t);
  if(seq < 0 or seq >= n_seqs) {
    fprintf(stderr, "sequence %d out of range (0..%d)\n", seq, n_seqs-1);
    exit(-1);
  }
  const uint8_t *e = t + 4 + 8*seq;
  uint32_t offs = be32(e);
  uint16_t clen = be16(e + 6);
  std::vector<uint8_t> raw = inflate_1172(t + offs, clen);
  memcpy(g.ptr(G_SEQDATA), raw.data(), raw.size());
  g.call(GE_alCSeqNew, G_CSEQ, G_SEQDATA);
  g.call(GE_alCSPSetSeq, G_CSPLAYER, G_CSEQ);
  /* the game's musicSetVolume (0x8000703c): master volume scaled by a per-sequence
   * Q15 table, both read here from the ROM's own data segment */
  uint32_t vol = (static_cast<uint32_t>(g.rd16(GE_music_volume)) * g.rd16(GE_track_volumes + 2*seq)) >> 15;
  g.call(GE_alCSPSetVol, G_CSPLAYER, vol);
  g.call(GE_alCSPPlay, G_CSPLAYER);
}

/* ---- engine_t: one frame of audio at a time, portable interpreter backend ---- */

engine_t::engine_t(const std::vector<uint8_t> &rom, bool use_jit) : rom(rom) {
#ifdef GEMUSIC_LLVM
  if(use_jit) {
    bt = new rspbt();
  }
#else
  (void)use_jit;
#endif
  /* interp_mips mirrors the r9999 RTL, where div/sqrt trap to an OS soft-float
   * emulator.  There is no OS here; this knob makes the ISS execute them itself. */
  setenv("FP_NODIVTRAP", "1", 1);
}

engine_t::~engine_t() {
#ifdef GEMUSIC_LLVM
  delete bt;
#endif
  delete rsp;
  delete g;
}

bool engine_t::jit_active() const {
#ifdef GEMUSIC_LLVM
  return bt != nullptr;
#else
  return false;
#endif
}

int engine_t::n_sequences() const {
  return be16(&rom[GE_ROM_SEQ_TABLE]);
}

void engine_t::start(int seq) {
  delete rsp;
  delete g;
  g = new guest_t(rom);                  /* a fresh machine per tune: no state leaks */
  rsp = new rsp_t();
  rsp->rdram = g->ptr(0x80000000u);
  ge_audio_init(*g, rom);
  ge_start_sequence(*g, rom, seq);
#ifdef GEMUSIC_LLVM
  /* the audio ABI's command dispatch table sits at +0x10 in the microcode's data:
   * 16 big-endian handler addresses.  Only a hint for the translator's jr targets. */
  jr_hints.clear();
  for(int i = 0; i < 16; i++) {
    jr_hints.push_back(g->rd16(GE_aspMainData + 0x10 + 2*i));
  }
#endif
}

void engine_t::render(int16_t *out) {
  g->call(GE_alAudioFrame, G_CMDLIST, G_CMDLEN, G_OUTBUF & 0x1fffffffu, GE_FRAME_SAMPLES);
  uint32_t n_cmds = g->rd32(G_CMDLEN);
  const uint32_t task[16] = {
    2, 0,
    GE_rspbootText & 0x1fffffffu, GE_rspbootText_LEN,
    GE_aspMainText & 0x1fffffffu, 0x1000,
    GE_aspMainData & 0x1fffffffu, 0x800,
    0, 0, 0, 0,
    G_CMDLIST & 0x1fffffffu, n_cmds * 8,
    0, 0
  };
  for(int i = 0; i < 16; i++) {
    uint8_t *p = rsp->mem + 0xfc0 + 4*i;
    p[0] = static_cast<uint8_t>(task[i] >> 24);
    p[1] = static_cast<uint8_t>(task[i] >> 16);
    p[2] = static_cast<uint8_t>(task[i] >> 8);
    p[3] = static_cast<uint8_t>(task[i]);
  }
#ifdef GEMUSIC_LLVM
  if(bt != nullptr) {
    /* rspboot's job, done natively: microcode data to DMEM, text to IMEM 0x080 */
    memcpy(rsp->mem, g->ptr(GE_aspMainData), 0x800);
    memcpy(rsp->mem + 0x1080, g->ptr(GE_aspMainText), 0xf80);
    rsp->r[1] = 0xfc0;
    bt->run(*rsp, 0x080, jr_hints);
  }
  else
#endif
  {
    memcpy(rsp->mem + 0x1000, g->ptr(GE_rspbootText), GE_rspbootText_LEN);
    rsp->run(0);
  }
  const uint8_t *o = g->ptr(G_OUTBUF);
  for(int i = 0; i < 2*GE_FRAME_SAMPLES; i++) {
    out[i] = static_cast<int16_t>((o[2*i] << 8) | o[2*i+1]);
  }
}

/* ---- sequence length, from the compact-MIDI data (libultra cseq.c semantics) ---- */

namespace {
  struct cseq_reader_t {
    std::vector<uint8_t> d;
    size_t pos = 0, bu_pos = 0, bu_len = 0;
    /* 0xFE hi lo len replays 'len' raw bytes from (position of the 0xFE - offset) */
    uint8_t byte() {
      if(bu_len != 0) {
	bu_len--;
	return d.at(bu_pos++);
      }
      uint8_t b = d.at(pos++);
      if(b == 0xfe) {
	uint8_t n = d.at(pos++);
	if(n != 0xfe) {
	  uint8_t lo = d.at(pos), len = d.at(pos + 1);
	  pos += 2;
	  bu_pos = pos - (((static_cast<size_t>(n) << 8) | lo) + 4);
	  bu_len = len;
	  bu_len--;
	  b = d.at(bu_pos++);
	}
      }
      return b;
    }
    uint32_t varlen() {
      uint32_t v = 0;
      for(;;) {
	uint8_t b = byte();
	v = (v << 7) | (b & 0x7f);
	if((b & 0x80) == 0) {
	  return v;
	}
      }
    }
  };
}

seq_info_t ge_sequence_info(const std::vector<uint8_t> &rom, int seq) {
  seq_info_t info;
  const uint8_t *t = &rom[GE_ROM_SEQ_TABLE];
  const uint8_t *e = t + 4 + 8*seq;
  std::vector<uint8_t> raw = inflate_1172(t + be32(e), be16(e + 6));
  if(raw.size() < 68) {
    return info;
  }
  uint32_t division = be32(&raw[64]);
  std::vector<std::pair<uint64_t, uint32_t>> tempos;       /* (tick, usec per quarter) */
  uint64_t end_tick = 0;
  try {
    for(int trk = 0; trk < 16; trk++) {
      uint32_t offs = be32(&raw[4*trk]);
      if(offs == 0) {
	continue;
      }
      cseq_reader_t r;
      r.d = raw;
      r.pos = offs;
      uint64_t tick = 0;
      uint8_t last = 0;
      for(int n_events = 0; n_events < 2000000; n_events++) {
	tick += r.varlen();
	uint8_t st = r.byte();
	if(st == 0xff) {
	  uint8_t ty = r.byte();
	  last = 0;
	  if(ty == 0x51) {
	    uint32_t us = (static_cast<uint32_t>(r.byte()) << 16);
	    us |= (static_cast<uint32_t>(r.byte()) << 8);
	    us |= r.byte();
	    tempos.push_back({tick, us});
	  }
	  else if(ty == 0x2e) {
	    r.byte();
	    r.byte();
	  }
	  else if(ty == 0x2d) {                            /* loop end: count, cur, u32 offset (raw bytes) */
	    size_t p = r.pos;
	    uint8_t cur = r.d.at(p + 1);
	    if(cur == 0xff) {
	      info.loops = true;
	      break;
	    }
	    if(cur == 0) {
	      r.d.at(p + 1) = r.d.at(p);
	      r.pos = p + 6;
	    }
	    else {
	      r.d.at(p + 1) = static_cast<uint8_t>(cur - 1);
	      r.pos = p + 6 - be32(&r.d.at(p + 2));
	    }
	  }
	  else {
	    break;                                         /* 0x2f end of track, or unknown */
	  }
	  continue;
	}
	uint8_t b1;
	if(st & 0x80) {
	  b1 = r.byte();
	  last = st;
	}
	else {
	  b1 = st;
	  st = last;
	}
	(void)b1;
	uint8_t hi = st & 0xf0;
	if(hi == 0xc0 or hi == 0xd0) {
	  continue;
	}
	r.byte();
	if(hi == 0x90) {
	  r.varlen();                                      /* note-ons carry their duration */
	  info.empty = false;
	}
      }
      end_tick = (tick > end_tick) ? tick : end_tick;
    }
  }
  catch(std::out_of_range &) {
    /* malformed data: report what we have */
  }
  std::sort(tempos.begin(), tempos.end());
  double us = 0.0, us_per_q = 488.0 * division;           /* alCSPNew's default uspt is 488 */
  uint64_t at = 0;
  for(const auto &tp : tempos) {
    if(tp.first >= end_tick) {
      break;
    }
    us += static_cast<double>(tp.first - at) * us_per_q / division;
    at = tp.first;
    us_per_q = tp.second;
  }
  us += static_cast<double>(end_tick - at) * us_per_q / division;
  info.seconds = us * 1e-6;
  return info;
}
