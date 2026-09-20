/* Does the engine actually run under wasm?  No SDL, no audio: load a ROM, render a
 * few seconds, and report whether any sound came out.  Runs under node. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fstream>
#include <iterator>
#include <cmath>

#include "engine.hh"
#include "ge_addrs.hh"

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s <rom.z64> [seq] [frames]\n", argv[0]);
    return 2;
  }
  int seq = (argc > 2) ? atoi(argv[2]) : 3;
  int n_frames = (argc > 3) ? atoi(argv[3]) : 60;

  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  printf("rom: %zu bytes\n", rom.size());
  if(rom.size() < 0x43865a or memcmp(&rom[0x3b], "NGEE", 4) != 0) {
    fprintf(stderr, "not an NGEE image\n");
    return 1;
  }

  engine_t eng(rom);
  printf("sequences: %d\n", eng.n_sequences());
  seq_info_t info = ge_sequence_info(rom, seq);
  printf("seq %d: %.1f s, loops=%d, empty=%d\n", seq, info.seconds, info.loops, info.empty);

  eng.start(seq);
  printf("started\n");

  std::vector<int16_t> frame(2 * GE_FRAME_SAMPLES);
  double energy = 0.0;
  int peak = 0, nonzero = 0;
  for(int f = 0; f < n_frames; f++) {
    eng.render(frame.data());
    for(int i = 0; i < 2*GE_FRAME_SAMPLES; i++) {
      int v = frame[i];
      energy += static_cast<double>(v) * v;
      peak = (abs(v) > peak) ? abs(v) : peak;
      nonzero += (v != 0);
    }
  }
  size_t n = static_cast<size_t>(n_frames) * 2 * GE_FRAME_SAMPLES;
  printf("rendered %d frames (%.2f s): rms %.1f, peak %d, %d/%zu nonzero\n",
	 n_frames, static_cast<double>(n_frames) * GE_FRAME_SAMPLES / GE_OUTPUT_RATE,
	 std::sqrt(energy / n), peak, nonzero, n);
  return (peak > 0) ? 0 : 1;
}
