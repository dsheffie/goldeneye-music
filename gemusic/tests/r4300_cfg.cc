/* Static function discovery and CFG dump for the R4300 side of the audio path.
 *
 * Two halves that check each other:
 *
 *   1. run the real thing -- libaudio init, then some frames of a tune -- recording
 *      every indirect transfer it takes and every pc it executes;
 *   2. walk the code statically from alAudioFrame, using the recorded indirect targets
 *      as hints, and confirm the walk covered every pc the run actually executed.
 *
 * Half two is the claim the translator rests on (DESIGN_R4300.md, "Discovery"): the
 * audio path is a closed world reachable by a static walk.  Half one is the only honest
 * way to test it.  The .dot files are the debugging aid -- a discovery bug shows up as a
 * misshapen graph long before it shows up as wrong audio. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>
#include <sys/stat.h>

#include "interpret.hh"
#include "disassemble.hh"
#include "engine.hh"
#include "ge_addrs.hh"
#include "r4300cfg.hh"

namespace {
  std::map<uint32_t, std::set<uint32_t>> g_indirect;   /* jalr/jr pc -> observed targets */
  std::set<uint32_t> g_pcs;                            /* every pc executed */
  r4300_t *g_cpu = nullptr;
}

static void record(r4300_t &c, uint32_t pc, uint32_t insn) {
  g_pcs.insert(pc);
  if((insn >> 26) != 0) {
    return;
  }
  const uint32_t fn = insn & 0x3f;
  if(fn != 8 and fn != 9) {                            /* jr, jalr */
    return;
  }
  const uint32_t rs = (insn >> 21) & 31;
  if(fn == 8 and rs == 31) {                           /* jr $ra needs no hint */
    return;
  }
  g_indirect[pc].insert(static_cast<uint32_t>(c.s->gpr[rs]));
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s <rom.z64> [seq] [frames] [outdir]\n", argv[0]);
    return 2;
  }
  const int frames = (argc > 3) ? atoi(argv[3]) : 30;
  const std::string dir = (argc > 4) ? argv[4] : "cfg";

  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if(rom.empty()) {
    fprintf(stderr, "could not read %s\n", argv[1]);
    return 1;
  }

  std::vector<int> seqs;                        /* "3" or "0,3,7" or "all" */
  {
    const std::string a = (argc > 2) ? argv[2] : "3";
    if(a == "all") {
      engine_t probe(rom);
      for(int i = 0; i < probe.n_sequences(); i++) {
	seqs.push_back(i);
      }
    }
    else {
      size_t p = 0;
      while(p < a.size()) {
	size_t c = a.find(',', p);
	seqs.push_back(atoi(a.substr(p, c - p).c_str()));
	p = (c == std::string::npos) ? a.size() : c + 1;
      }
    }
  }

  std::vector<int16_t> buf(2 * GE_FRAME_SAMPLES);
  engine_t *keep = nullptr;
  for(size_t i = 0; i < seqs.size(); i++) {
    engine_t *eng = new engine_t(rom);
    eng->start(seqs[i]);
    g_cpu = eng->r4300;
    eng->r4300->on_step = record;                      /* the frame loop only */
    for(int f = 0; f < frames; f++) {
      eng->render(buf.data());
    }
    eng->r4300->on_step = nullptr;
    if(i + 1 == seqs.size()) {
      keep = eng;                                      /* its RAM is what we read from */
    }
    else {
      delete eng;
    }
  }
  g_cpu = keep->r4300;

  r4300_cfg cfg;
  /* Reads go through ptr_any, not ptr: the game runs from a TLB alias at 0x70000000 and
   * masking that with 0x1fffffff lands 256 MB away in unwritten memory. */
  cfg.read = [](uint32_t va, uint32_t &out) {
    uint8_t *p = g_cpu->ptr_any(va);
    if(p == nullptr) {
      return false;
    }
    memcpy(&out, p, 4);
    out = __builtin_bswap32(out);                      /* the ROM is big endian */
    return true;
  };
  initCapstone();
  cfg.disasm = [](uint32_t insn, uint32_t pc) { return getAsmString(insn, pc); };
  cfg.hints = g_indirect;

  if(const char *d = getenv("DIS")) {            /* DIS=addr[,count] -- disassemble a range */
    uint32_t a = strtoul(d, nullptr, 16), n = 16;
    if(const char *c = strchr(d, ',')) {
      n = strtoul(c + 1, nullptr, 0);
    }
    for(uint32_t i = 0; i < n; i++) {
      uint32_t insn = 0;
      if(cfg.read(a + i * 4, insn)) {
	printf("%08x  %08x  %s\n", a + i * 4, insn, cfg.disasm(insn, a + i * 4).c_str());
      }
    }
  }

  cfg.discover({GE_alAudioFrame});

  mkdir(dir.c_str(), 0755);
  cfg.write_dot(dir);
  cfg.report(stdout);

  /* The closed-world check: nothing the run executed may lie outside the static walk. */
  std::set<uint32_t> covered;
  for(const auto &kv : cfg.funcs) {
    for(const auto &b : kv.second.blocks) {
      for(uint32_t pc = b.second.start; pc < b.second.end; pc += 4) {
	covered.insert(pc);
      }
    }
  }
  size_t missing = 0, in_stubs = 0;
  for(uint32_t pc : g_pcs) {
    if(covered.count(pc)) {
      continue;
    }
    if(pc >= 0x80700000u and pc < 0x80710000u) {       /* our own DMA stub, not ROM code */
      in_stubs++;
      continue;
    }
    if(missing++ < 20) {
      printf("NOT COVERED %08x\n", pc);
    }
  }
  printf("\nexecuted %zu distinct pcs; static walk covers %zu; %zu executed but not walked"
	 " (%zu more in our own stubs)\n", g_pcs.size(), covered.size(), missing, in_stubs);
  printf("wrote %zu graphs to %s/\n", cfg.funcs.size(), dir.c_str());
  return missing == 0 ? 0 : 1;
}
