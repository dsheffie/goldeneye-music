#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

#include "engine.hh"
#ifdef GEMUSIC_R4300BT
#include <chrono>
#include "interpret.hh"
#include "r4300cfg.hh"
#include "r4300bt.hh"
#endif
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
uint32_t ge_audio_init(r4300_t &g, const std::vector<uint8_t> &rom) {
  g.call(GE_alHeapInit, RAM_ALHEAP, RAM_AUDIO_HEAP, RAM_AUDIO_HEAP_LEN);

  memcpy(g.ptr(RAM_CTL), &rom[GE_ROM_INST_CTL], GE_ROM_INST_CTL_LEN);
  memcpy(g.ptr(RAM_TBL), &rom[GE_ROM_INST_TBL], GE_ROM_INST_TBL_LEN);
  g.call(GE_alBnkfNew, RAM_CTL, RAM_TBL);
  uint32_t bank = g.rd32(RAM_CTL + 4);

  /* ALSynConfig */
  g.wr32(RAM_SYNCONFIG + 0, 0);
  g.wr32(RAM_SYNCONFIG + 4, GE_MAX_PVOICES);
  g.wr32(RAM_SYNCONFIG + 8, GE_MAX_UPDATES);
  g.wr32(RAM_SYNCONFIG + 12, 0);
  g.wr32(RAM_SYNCONFIG + 16, RAM_STUBS);              /* dmaproc (dmaNew) */
  g.wr32(RAM_SYNCONFIG + 20, RAM_ALHEAP);
  g.wr32(RAM_SYNCONFIG + 24, GE_OUTPUT_RATE);
  g.wr8(RAM_SYNCONFIG + 28, GE_FX_TYPE);
  g.wr32(RAM_SYNCONFIG + 32, GE_custom_fx_params);
  g.call(GE_alInit, RAM_ALGLOBALS, RAM_SYNCONFIG);

  /* ALSeqpConfig: no oscillator callbacks, as in the game */
  g.wr32(RAM_SEQPCONFIG + 0, GE_SEQP_MAX_VOICES);
  g.wr32(RAM_SEQPCONFIG + 4, GE_SEQP_MAX_EVENTS);
  g.wr8(RAM_SEQPCONFIG + 8, GE_SEQP_MAX_CHANNELS);
  g.wr8(RAM_SEQPCONFIG + 9, 0);
  g.wr32(RAM_SEQPCONFIG + 12, RAM_ALHEAP);
  g.wr32(RAM_SEQPCONFIG + 16, 0);
  g.wr32(RAM_SEQPCONFIG + 20, 0);
  g.wr32(RAM_SEQPCONFIG + 24, 0);
  g.call(GE_alCSPNew, RAM_CSPLAYER, RAM_SEQPCONFIG);
  g.call(GE_alCSPSetBank, RAM_CSPLAYER, bank);
  return bank;
}

void ge_start_sequence(r4300_t &g, const std::vector<uint8_t> &rom, int seq) {
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
  memcpy(g.ptr(RAM_SEQDATA), raw.data(), raw.size());
  g.call(GE_alCSeqNew, RAM_CSEQ, RAM_SEQDATA);
  g.call(GE_alCSPSetSeq, RAM_CSPLAYER, RAM_CSEQ);
  /* the game's musicSetVolume (0x8000703c): master volume scaled by a per-sequence
   * Q15 table, both read here from the ROM's own data segment */
  uint32_t vol = (static_cast<uint32_t>(g.rd16(GE_music_volume)) * g.rd16(GE_track_volumes + 2*seq)) >> 15;
  g.call(GE_alCSPSetVol, RAM_CSPLAYER, vol);
  g.call(GE_alCSPPlay, RAM_CSPLAYER);
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

}

engine_t::~engine_t() {
#ifdef GEMUSIC_LLVM
  delete bt;
#endif
  delete rsp;
  delete r4300;
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

#ifdef GEMUSIC_R4300BT
namespace {
  /* on_step is a plain function pointer, so the warm-up's observations live here */
  std::map<uint32_t, std::set<uint32_t>> g_hints;
  void watch_indirect(r4300_t &c, uint32_t pc, uint32_t insn) {
    if((insn >> 26) != 0) {
      return;
    }
    const uint32_t fn = insn & 0x3f;
    const uint32_t rs = (insn >> 21) & 31;
    if((fn != 8 and fn != 9) or (fn == 8 and rs == 31)) {   /* jr $ra needs no hint */
      return;
    }
    g_hints[pc].insert(static_cast<uint32_t>(c.s->gpr[rs]));
  }
}

/* Translate the R4300 audio path.  Call after libaudio has initialised, so the static
 * walk reads the image the frame loop will actually run.
 *
 * The walk needs to be told where the indirect calls go: libaudio is reached almost
 * entirely through handler pointers, and a walk with no observed targets finds 4
 * functions out of 86.  So run a few frames first and watch them -- on a throwaway
 * machine, because doing it on the real one would advance the sequence.  The code image
 * is identical either way, which is what makes the observations transferable.
 *
 * The translation is kept across tunes: a new machine per tune does not change the code,
 * and both state and RAM are arguments. */
double ge_r4300bt_setup_seconds = 0.0;

void ge_enable_r4300bt(r4300_t &g, const std::vector<uint8_t> &rom, int seq) {
  static r4300bt *bt = nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  /* The translation hard-codes the FR=0 FP register model (a single in the (r&1) half of
   * slot (r&~1)), which is what this ROM's code is compiled for.  Under FR=1 the layout
   * is different and the translation would be wrong, so leave it to the interpreter. */
  if(((g.s->cpr0[CPR0_SR] >> 26) & 1u) != 0) {
    fprintf(stderr, "r4300bt: SR.FR is set; not translating\n");
    return;
  }
  if(bt == nullptr) {
    {
      r4300_t warm(rom);
      ge_audio_init(warm, rom);
      ge_start_sequence(warm, rom, seq);
      warm.on_step = watch_indirect;
      for(int i = 0; i < 20; i++) {          /* 20 frames reaches every handler */
	warm.call(GE_alAudioFrame, RAM_CMDLIST, RAM_CMDLEN, RAM_OUTBUF & 0x1fffffffu, GE_FRAME_SAMPLES);
      }
    }
    const double warm_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    r4300_cfg cfg;
    cfg.read = [&g](uint32_t va, uint32_t &out) {
      uint8_t *p = g.ptr_any(va);
      if(p == nullptr) {
	return false;
      }
      memcpy(&out, p, 4);
      out = __builtin_bswap32(out);
      return true;
    };
    cfg.hints = g_hints;
    cfg.discover({GE_alAudioFrame});
    std::vector<uint32_t> all;
    for(const auto &kv : cfg.funcs) {
      all.push_back(kv.first);
    }
    bt = new r4300bt();
    bt->dump_ir = (getenv("R4300BT_IR") != nullptr);
    bt->translate(cfg, all);
    ge_r4300bt_setup_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "r4300bt: warm-up %.2f s, discovery+codegen+LLVM %.2f s\n",
	    warm_s, bt->compile_seconds);
  }
  g.bt = bt;
}
#endif

void engine_t::start(int seq) {
  delete rsp;
  delete r4300;
  r4300 = new r4300_t(rom);                  /* a fresh machine per tune: no state leaks */
  rsp = new rsp_t();
  rsp->rdram = r4300->ptr(0x80000000u);
  r4300->on_step = on_step;
  ge_audio_init(*r4300, rom);
  ge_start_sequence(*r4300, rom, seq);
#ifdef GEMUSIC_R4300BT
  ge_enable_r4300bt(*r4300, rom, seq);
#endif
#ifdef GEMUSIC_LLVM
  /* the audio ABI's command dispatch table sits at +0x10 in the microcode's data:
   * 16 big-endian handler addresses.  Only a hint for the translator's jr targets. */
  jr_hints.clear();
  for(int i = 0; i < 16; i++) {
    jr_hints.push_back(r4300->rd16(GE_aspMainData + 0x10 + 2*i));
  }
#endif
}

void engine_t::render(int16_t *out) {
  r4300->call(GE_alAudioFrame, RAM_CMDLIST, RAM_CMDLEN, RAM_OUTBUF & 0x1fffffffu, GE_FRAME_SAMPLES);
  uint32_t n_cmds = r4300->rd32(RAM_CMDLEN);
  const uint32_t task[16] = {
    2, 0,
    GE_rspbootText & 0x1fffffffu, GE_rspbootText_LEN,
    GE_aspMainText & 0x1fffffffu, 0x1000,
    GE_aspMainData & 0x1fffffffu, 0x800,
    0, 0, 0, 0,
    RAM_CMDLIST & 0x1fffffffu, n_cmds * 8,
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
    memcpy(rsp->mem, r4300->ptr(GE_aspMainData), 0x800);
    memcpy(rsp->mem + 0x1080, r4300->ptr(GE_aspMainText), 0xf80);
    rsp->r[1] = 0xfc0;
    bt->run(*rsp, 0x080, jr_hints);
  }
  else
#endif
  {
    memcpy(rsp->mem + 0x1000, r4300->ptr(GE_rspbootText), GE_rspbootText_LEN);
    rsp->run(0);
  }
  const uint8_t *o = r4300->ptr(RAM_OUTBUF);
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
