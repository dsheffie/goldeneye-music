#ifndef __ENGINE_HH__
#define __ENGINE_HH__

#include <cstdint>
#include <vector>

#include "r4300.hh"
#include "rsp.hh"
#ifdef GEMUSIC_LLVM
#include "rspbt.hh"
#endif

std::vector<uint8_t> inflate_1172(const uint8_t *p, size_t avail);
uint32_t ge_audio_init(r4300_t &g, const std::vector<uint8_t> &rom);
#ifdef GEMUSIC_R4300BT
void ge_enable_r4300bt(r4300_t &g, const std::vector<uint8_t> &rom, int seq);
extern double ge_r4300bt_setup_seconds;
#endif
void ge_start_sequence(r4300_t &g, const std::vector<uint8_t> &rom, int seq);

/* How long one pass through a sequence is, read from the compact-MIDI data itself. */
struct seq_info_t {
  double seconds = 0.0;      /* one pass: to the loop end, or to the end of the last track */
  bool loops = false;        /* has an infinite loop */
  bool empty = true;         /* no note-ons at all */
};
seq_info_t ge_sequence_info(const std::vector<uint8_t> &rom, int seq);

/* The whole renderer behind two calls: GoldenEye's libaudio on interp_mips, its audio
 * microcode on the RSP interpreter.  render() produces GE_FRAME_SAMPLES stereo frames. */
struct engine_t {
  const std::vector<uint8_t> &rom;
  r4300_t *r4300 = nullptr;
  /* installed on the machine before init runs, so a tool can watch initialisation */
  void (*on_step)(r4300_t &, uint32_t pc, uint32_t insn) = nullptr;
  rsp_t *rsp = nullptr;
#ifdef GEMUSIC_LLVM
  rspbt *bt = nullptr;                 /* null unless the JIT was requested and built in */
  std::vector<uint32_t> jr_hints;
#endif

  /* use_jit is honoured only in a build with the LLVM translator; everything else
   * falls back to the RSP interpreter, which is fast enough for real-time playback. */
  engine_t(const std::vector<uint8_t> &rom, bool use_jit = false);
  ~engine_t();
  int n_sequences() const;
  void start(int seq);
  void render(int16_t *out);           /* out[2*GE_FRAME_SAMPLES], interleaved L/R */
  bool jit_active() const;             /* what the engine is actually using */
};

#endif
