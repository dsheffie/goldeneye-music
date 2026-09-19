#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <boost/program_options.hpp>

#include "guest.hh"
#include "engine.hh"
#include "rsp.hh"
#ifdef GEMUSIC_AVX512
#include "rsp_avx512.hh"
#endif
#ifdef GEMUSIC_LLVM
#include "rspbt.hh"
#endif
#include <chrono>
#include "ge_addrs.hh"

namespace po = boost::program_options;

int main(int argc, char *argv[]) {
  std::string rom_name = "GoldenEye.z64", out_name = "out.wav";
  int seq = 2;
  double seconds = 180.0, fade = 8.0;
  bool verbose = false, dump_ir = false;
  std::string rsp_backend = "interp";
  try {
    po::options_description desc("Options");
    desc.add_options()
      ("help,h", "print help")
      ("rom,r", po::value<std::string>(&rom_name), "GoldenEye 007 (NGEE) big-endian ROM")
      ("seq,s", po::value<int>(&seq), "sequence number")
      ("out,o", po::value<std::string>(&out_name), "output wav")
      ("seconds,t", po::value<double>(&seconds), "maximum seconds to render (looping tunes never end)")
      ("fade", po::value<double>(&fade), "fade-out seconds when cut off by --seconds")
      ("verbose,v", po::bool_switch(&verbose), "per-second voice/command counts on stderr")
      ("dump-ir", po::bool_switch(&dump_ir), "llvm backend: write the translation's IR (before/after optimization) to ./rsp_*.ll")
      ("rsp", po::value<std::string>(&rsp_backend), "RSP backend: interp, avx512 (build-time recompiled microcode), llvm (rsp-bt translator)")
      ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    if(vm.count("help")) {
      std::cout << desc << "\n";
      return 0;
    }
  }
  catch(po::error &e) {
    std::cerr << "command-line error : " << e.what() << "\n";
    return -1;
  }

  std::ifstream in(rom_name, std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if(rom.size() < 0x43865a or memcmp(&rom[0x3b], "NGEE", 4) != 0) {
    fprintf(stderr, "%s is not a big-endian GoldenEye 007 (NGEE) image\n", rom_name.c_str());
    return -1;
  }

  /* interp_mips mirrors the r9999 RTL, where div/sqrt trap to an OS soft-float
   * emulator.  There is no OS here; this knob makes the ISS execute them itself. */
  setenv("FP_NODIVTRAP", "1", 1);

  guest_t g(rom);
  ge_audio_init(g, rom);
  ge_start_sequence(g, rom, seq);

  rsp_t rsp;
  rsp.rdram = g.ptr(0x80000000u);
  bool use_recomp = (rsp_backend == "avx512"), use_llvm = (rsp_backend == "llvm");
  if(not(use_recomp) and not(use_llvm) and rsp_backend != "interp") {
    fprintf(stderr, "unknown --rsp backend %s\n", rsp_backend.c_str());
    return -1;
  }
#ifdef GEMUSIC_AVX512
  rsp_vec_t rsp_vec;
  if(use_recomp) {
    /* the translation is only valid for the exact microcode it was generated from */
    uint32_t h = 0x811c9dc5u;
    const uint8_t *t = g.ptr(GE_aspMainText);
    for(int i = 0; i < 0xf80; i++) {
      h = (h ^ t[i]) * 0x01000193u;
    }
    if(h != aspmain_recomp_hash) {
      fprintf(stderr, "this ROM's audio microcode is not the one the avx512 backend was built from\n");
      return -1;
    }
  }
#else
  if(use_recomp) {
    fprintf(stderr, "built without the avx512 backend (make AVX512=1 ROM=...)\n");
    return -1;
  }
#endif
#ifdef GEMUSIC_LLVM
  rspbt bt;
  bt.dump_ir = dump_ir;
  /* the audio ABI's command dispatch table sits at +0x10 in the microcode's data: 16
   * big-endian handler addresses.  Only a hint for the translator's jr targets. */
  std::vector<uint32_t> jr_hints;
  for(int i = 0; i < 16; i++) {
    jr_hints.push_back(g.rd16(GE_aspMainData + 0x10 + 2*i));
  }
#else
  if(use_llvm) {
    fprintf(stderr, "built without the llvm backend (make LLVM=1)\n");
    return -1;
  }
#endif
  double rsp_seconds = 0.0;
  std::vector<int16_t> pcm;

  int n_frames = static_cast<int>(seconds * GE_OUTPUT_RATE / GE_FRAME_SAMPLES);
  const int silence_limit = 2 * GE_OUTPUT_RATE / GE_FRAME_SAMPLES;   /* 2 s */
  int n_silent = 0;
  bool sounded = false, ended = false;
  for(int f = 0; f < n_frames and not(ended); f++) {
    /* CPU side: GoldenEye's sequencer + synthesis driver build this frame's command list */
    /* outBuf is a PHYSICAL address (the game passes osVirtualToPhysical(buf)): it goes
     * verbatim into A_SAVEBUFF, whose top byte the microcode treats as a segment id */
    g.call(GE_alAudioFrame, G_CMDLIST, G_CMDLEN, G_OUTBUF & 0x1fffffffu, GE_FRAME_SAMPLES);
    uint32_t n_cmds = g.rd32(G_CMDLEN);
    if(verbose and (f % 30) == 0) {
      int n_adpcm = 0;
      for(uint32_t i = 0; i < n_cmds; i++) {
	n_adpcm += ((g.rd32(G_CMDLIST + 8*i) >> 24) == 1);      /* A_ADPCM: one per sounding voice chunk */
      }
      fprintf(stderr, "frame %5d: %4u cmds, %3d adpcm decodes\n", f, n_cmds, n_adpcm);
    }

    /* RSP side: what osSpTaskLoad/osSpTaskStartGo do -- OSTask at DMEM 0xfc0, rspboot
     * in IMEM, run.  rspboot DMAs aspMain's text and data in by itself. */
    const uint32_t task[16] = {
      2, 0,                                                 /* M_AUDTASK, flags */
      GE_rspbootText & 0x1fffffffu, GE_rspbootText_LEN,
      GE_aspMainText & 0x1fffffffu, 0x1000,
      GE_aspMainData & 0x1fffffffu, 0x800,
      0, 0, 0, 0,                                           /* dram stack, output buffer */
      G_CMDLIST & 0x1fffffffu, n_cmds * 8,
      0, 0                                                  /* yield buffer */
    };
    for(int i = 0; i < 16; i++) {
      uint32_t x = task[i];
      uint8_t *p = rsp.mem + 0xfc0 + 4*i;
      p[0] = static_cast<uint8_t>(x >> 24);
      p[1] = static_cast<uint8_t>(x >> 16);
      p[2] = static_cast<uint8_t>(x >> 8);
      p[3] = static_cast<uint8_t>(x);
    }
    auto t0 = std::chrono::steady_clock::now();
    if(not(use_recomp) and not(use_llvm)) {
      memcpy(rsp.mem + 0x1000, g.ptr(GE_rspbootText), GE_rspbootText_LEN);
      rsp.run(0);
    }
#ifdef GEMUSIC_LLVM
    else if(use_llvm) {
      memcpy(rsp.mem, g.ptr(GE_aspMainData), 0x800);           /* rspboot's job, natively */
      memcpy(rsp.mem + 0x1080, g.ptr(GE_aspMainText), 0xf80);
      rsp.r[1] = 0xfc0;
      bt.run(rsp, 0x080, jr_hints);
    }
#endif
#ifdef GEMUSIC_AVX512
    else if(use_recomp) {
      /* rspboot's whole job, done natively: microcode data to DMEM 0, text to IMEM
       * 0x080 (only so IMEM looks right), task pointer in $1, enter at 0x080 */
      memcpy(rsp.mem, g.ptr(GE_aspMainData), 0x800);
      memcpy(rsp.mem + 0x1080, g.ptr(GE_aspMainText), 0xf80);
      rsp.r[1] = 0xfc0;
      rsp.halted = false;
      aspmain_recomp(rsp, rsp_vec, 0x080);
    }
#endif
    rsp_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const uint8_t *o = g.ptr(G_OUTBUF);
    bool silent = true;
    for(int i = 0; i < 2*GE_FRAME_SAMPLES; i++) {
      int16_t x = static_cast<int16_t>((o[2*i] << 8) | o[2*i+1]);
      silent = silent and (x == 0);
      pcm.push_back(x);
    }
    sounded = sounded or not(silent);
    n_silent = silent ? (n_silent + 1) : 0;
    ended = sounded and (n_silent >= silence_limit);           /* a jingle that finished */
  }
  if(not(ended)) {
    size_t n_fade = static_cast<size_t>(fade * GE_OUTPUT_RATE), n = pcm.size() / 2;
    n_fade = (n_fade > n) ? n : n_fade;
    for(size_t i = 0; i < n_fade; i++) {
      double gain = static_cast<double>(n_fade - i) / static_cast<double>(n_fade);
      size_t k = n - n_fade + i;
      pcm[2*k] = static_cast<int16_t>(pcm[2*k] * gain);
      pcm[2*k+1] = static_cast<int16_t>(pcm[2*k+1] * gain);
    }
  }
#ifdef GEMUSIC_LLVM
  if(use_llvm) {
    fprintf(stderr, "rspbt: %lu compiles (%.3f s), %lu interpreter fallbacks\n", bt.n_compiles, bt.compile_seconds, bt.n_fallbacks);
  }
#endif
  fprintf(stderr, "seq %d: %.1f s%s, %lu R4300 insns, %lu RSP insns, RSP backend %s took %.3f s\n", seq,
	  static_cast<double>(pcm.size() / 2) / GE_OUTPUT_RATE, ended ? " (ended)" : " (faded)", g.n_insns, rsp.n_insns,
	  rsp_backend.c_str(), rsp_seconds);

  FILE *fp = fopen(out_name.c_str(), "wb");
  if(fp == nullptr) {
    fprintf(stderr, "could not open %s\n", out_name.c_str());
    return -1;
  }
  uint32_t n_bytes = static_cast<uint32_t>(pcm.size() * 2);
  const uint32_t rate = GE_OUTPUT_RATE;
  const uint32_t hdr[] = {0x46464952u, 36 + n_bytes, 0x45564157u, 0x20746d66u, 16,
			  (2u << 16) | 1u, rate, rate * 4, (16u << 16) | 4u, 0x61746164u, n_bytes};
  fwrite(hdr, sizeof(hdr), 1, fp);                          /* little-endian hosts */
  fwrite(pcm.data(), 2, pcm.size(), fp);
  fclose(fp);
  return 0;
}
