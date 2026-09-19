#ifndef __ENGINE_HH__
#define __ENGINE_HH__

#include <cstdint>
#include <vector>

#include "guest.hh"
#include "rsp.hh"

std::vector<uint8_t> inflate_1172(const uint8_t *p, size_t avail);
uint32_t ge_audio_init(guest_t &g, const std::vector<uint8_t> &rom);
void ge_start_sequence(guest_t &g, const std::vector<uint8_t> &rom, int seq);

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
  guest_t *g = nullptr;
  rsp_t *rsp = nullptr;

  engine_t(const std::vector<uint8_t> &rom);
  ~engine_t();
  int n_sequences() const;
  void start(int seq);
  void render(int16_t *out);           /* out[2*GE_FRAME_SAMPLES], interleaved L/R */
};

#endif
