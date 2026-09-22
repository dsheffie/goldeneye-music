/* Phase 1 of the R4300 static-translation prototype.
 *
 * Runs the real thing -- libaudio init, then a few frames of a tune -- and records
 * every indirect control transfer it takes: the pc, what kind, and where it went.
 * That gives the target sets a static walker would need as hints for jr and jalr.
 * Also writes the loaded RAM image so the walker can read jump tables out of it. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <string>

#include "interpret.hh"
#include "engine.hh"
#include "ge_addrs.hh"

namespace {
  /* pc -> observed targets, split by the kind of transfer */
  std::map<uint32_t, std::set<uint32_t>> g_jr, g_jalr, g_jr_ra;

  /* Where each jalr's pointer came from, and whether anything ever writes there.
   * A handler slot written after init would break a translation that treats the
   * target as constant, so this is the check that matters. */
  uint32_t g_last_load_addr[32] = {0};        /* per destination register */
  /* slots we already know hold handler pointers, so init-time writes are visible */
  const uint32_t g_known_slots[] = {0x00400004, 0x00400008, 0x00701008};
  std::map<uint32_t, std::set<uint32_t>> g_slot;        /* jalr pc -> slot addresses */
  std::set<uint32_t> g_slots_all;
  std::map<uint32_t, std::set<uint32_t>> g_slot_written; /* slot -> pcs that wrote it */
  uint64_t g_stores = 0;
  std::map<std::string, uint64_t> g_histo;     /* executed instruction mix */
  std::set<uint32_t> g_pcs;                    /* every pc executed */
  uint64_t g_exits = 0;                        /* times control reached our harness */
}

static const char *mnemonic(uint32_t insn) {
  uint32_t op = insn >> 26, fn = insn & 0x3f, rs = (insn >> 21) & 31;
  if(insn == 0) { return "nop"; }
  switch(op)
    {
    case 0x00:
      switch(fn)
	{
	case 0x00: return "sll";      case 0x02: return "srl";     case 0x03: return "sra";
	case 0x04: return "sllv";     case 0x06: return "srlv";    case 0x07: return "srav";
	case 0x08: return "jr";       case 0x09: return "jalr";
	case 0x0c: return "syscall";  case 0x0d: return "break";   case 0x0f: return "sync";
	case 0x10: return "mfhi";     case 0x11: return "mthi";    case 0x12: return "mflo";
	case 0x13: return "mtlo";     case 0x14: return "dsllv";   case 0x16: return "dsrlv";
	case 0x17: return "dsrav";    case 0x18: return "mult";    case 0x19: return "multu";
	case 0x1a: return "div";      case 0x1b: return "divu";    case 0x1c: return "dmult";
	case 0x1d: return "dmultu";   case 0x1e: return "ddiv";    case 0x1f: return "ddivu";
	case 0x20: return "add";      case 0x21: return "addu";    case 0x22: return "sub";
	case 0x23: return "subu";     case 0x24: return "and";     case 0x25: return "or";
	case 0x26: return "xor";      case 0x27: return "nor";     case 0x2a: return "slt";
	case 0x2b: return "sltu";     case 0x2c: return "dadd";    case 0x2d: return "daddu";
	case 0x2e: return "dsub";     case 0x2f: return "dsubu";   case 0x38: return "dsll";
	case 0x3a: return "dsrl";     case 0x3b: return "dsra";    case 0x3c: return "dsll32";
	case 0x3e: return "dsrl32";   case 0x3f: return "dsra32";
	default: return "special?";
	}
    case 0x01: return "regimm";
    case 0x02: return "j";        case 0x03: return "jal";     case 0x04: return "beq";
    case 0x05: return "bne";      case 0x06: return "blez";    case 0x07: return "bgtz";
    case 0x08: return "addi";     case 0x09: return "addiu";   case 0x0a: return "slti";
    case 0x0b: return "sltiu";    case 0x0c: return "andi";    case 0x0d: return "ori";
    case 0x0e: return "xori";     case 0x0f: return "lui";
    case 0x10: return "cop0";
    case 0x11:
      switch(rs)
	{
	case 0x00: return "mfc1";  case 0x01: return "dmfc1"; case 0x02: return "cfc1";
	case 0x04: return "mtc1";  case 0x05: return "dmtc1"; case 0x06: return "ctc1";
	case 0x08: return "bc1";
	default: return (rs == 0x11) ? "fp.d" : ((rs == 0x10) ? "fp.s" : "fp.other");
	}
    case 0x14: return "beql";     case 0x15: return "bnel";    case 0x16: return "blezl";
    case 0x17: return "bgtzl";    case 0x18: return "daddi";   case 0x19: return "daddiu";
    case 0x1a: return "ldl";      case 0x1b: return "ldr";
    case 0x20: return "lb";       case 0x21: return "lh";      case 0x22: return "lwl";
    case 0x23: return "lw";       case 0x24: return "lbu";     case 0x25: return "lhu";
    case 0x26: return "lwr";      case 0x27: return "lwu";     case 0x28: return "sb";
    case 0x29: return "sh";       case 0x2a: return "swl";     case 0x2b: return "sw";
    case 0x2c: return "sdl";      case 0x2d: return "sdr";     case 0x2e: return "swr";
    case 0x31: return "lwc1";     case 0x35: return "ldc1";    case 0x37: return "ld";
    case 0x39: return "swc1";     case 0x3d: return "sdc1";    case 0x3f: return "sd";
    default: return "op?";
    }
}

static void record(r4300_t &c, uint32_t pc, uint32_t insn) {
  if(insn == 0 and getenv("ZERODBG") != nullptr) {
    static int n = 0;
    if(n++ < 12) { fprintf(stderr, "[zero] pc=%08x\n", pc); }
  }
  g_histo[mnemonic(insn)]++;
  g_pcs.insert(pc);
  if(pc >= 0x80700000u and pc < 0x80710000u) {   /* our stubs / return trampoline */
    g_exits++;
  }
  uint32_t op = insn >> 26;
  uint32_t rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
  int32_t simm = static_cast<int16_t>(insn & 0xffff);
  uint32_t ea = static_cast<uint32_t>(c.s->gpr[rs] + simm) & 0x1fffffff;

  if(op == 0x23) {                              /* lw: remember where rt came from */
    g_last_load_addr[rt] = ea;
  }
  else if(op == 0x2b or op == 0x3f or op == 0x29 or op == 0x28) {   /* sw sd sh sb */
    g_stores++;
    uint32_t n = (op == 0x3f) ? 8 : ((op == 0x2b) ? 4 : ((op == 0x29) ? 2 : 1));
    for(uint32_t slot : g_slots_all) {          /* any overlap with a handler slot? */
      if(ea < slot + 4 and slot < ea + n) {
	g_slot_written[slot].insert(pc);
      }
    }
  }

  if(op != 0) {
    return;
  }
  uint32_t fn = insn & 0x3f;
  if(fn != 8 and fn != 9) {                 /* jr, jalr */
    return;
  }
  uint32_t tgt = static_cast<uint32_t>(c.s->gpr[rs]);
  if(fn == 9) {
    g_jalr[pc].insert(tgt);
    uint32_t slot = g_last_load_addr[rs];       /* rs held the pointer; where from? */
    if(slot != 0) {
      g_slot[pc].insert(slot);
      g_slots_all.insert(slot);
    }
  }
  else if(rs == 31) {
    g_jr_ra[pc].insert(tgt);
  }
  else {
    g_jr[pc].insert(tgt);
  }
}

static void dump(FILE *f, const char *kind, const std::map<uint32_t, std::set<uint32_t>> &m) {
  for(const auto &kv : m) {
    for(uint32_t t : kv.second) {
      fprintf(f, "%s %08x %08x\n", kind, kv.first, t);
    }
  }
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s <rom.z64> [seq] [frames]\n", argv[0]);
    return 2;
  }
  int seq = (argc > 2) ? atoi(argv[2]) : 3;
  int frames = (argc > 3) ? atoi(argv[3]) : 30;

  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  for(uint32_t s : g_known_slots) {
    g_slots_all.insert(s);
  }
  engine_t eng(rom);
  bool trace_init = (getenv("TRACE_INIT") != nullptr);
  if(trace_init) {
    eng.on_step = record;                   /* watch initialisation as well */
  }
  eng.start(seq);
  eng.r4300->on_step = record;

  std::vector<int16_t> buf(2 * GE_FRAME_SAMPLES);
  for(int f = 0; f < frames; f++) {
    eng.render(buf.data());
  }

  auto sites = [](const std::map<uint32_t, std::set<uint32_t>> &m) {
    size_t poly = 0;
    for(const auto &kv : m) {
      poly += (kv.second.size() > 1);
    }
    return std::make_pair(m.size(), poly);
  };
  auto ja = sites(g_jalr), jr = sites(g_jr), ra = sites(g_jr_ra);
  printf("over %d frames, %llu instructions:\n", frames, static_cast<unsigned long long>(eng.r4300->n_insns));
  printf("  jalr      : %zu sites, %zu with more than one target\n", ja.first, ja.second);
  printf("  jr (table): %zu sites, %zu with more than one target\n", jr.first, jr.second);
  printf("  jr $ra    : %zu sites, %zu with more than one target\n", ra.first, ra.second);

  for(uint32_t slot : g_known_slots) {
    auto it = g_slot_written.find(slot);
    printf("  slot %08x written by: %s\n", slot,
	   (it == g_slot_written.end()) ? "nothing in this trace"
					: [&]{ static char b[128]; int n = 0;
					       for(uint32_t pc : it->second) { n += snprintf(b+n, sizeof(b)-n, "%08x ", pc); }
					       return b; }());
  }
  if(getenv("HISTO") != nullptr) {
    std::vector<std::pair<uint64_t, std::string>> v;
    for(const auto &kv : g_histo) { v.push_back({kv.second, kv.first}); }
    std::sort(v.rbegin(), v.rend());
    printf("  %zu distinct instructions executed:\n", v.size());
    for(const auto &p : v) { printf("     %-10s %llu\n", p.second.c_str(), (unsigned long long)p.first); }
  }
  printf("  stores executed: %llu\n", static_cast<unsigned long long>(g_stores));
  for(const auto &kv : g_slot) {
    for(uint32_t slot : kv.second) {
      auto it = g_slot_written.find(slot);
      printf("  jalr %08x loads its pointer from %08x : %s\n", kv.first, slot,
	     (it == g_slot_written.end()) ? "never written during the frame loop"
					  : "WRITTEN -- not a constant");
    }
  }

  {
    FILE *pf = fopen("r4300_pcs.txt", "w");
    for(uint32_t pc : g_pcs) { fprintf(pf, "%08x\n", pc); }
    fclose(pf);
    printf("  distinct pcs executed: %zu ; instructions inside our own stubs: %llu\n",
	   g_pcs.size(), static_cast<unsigned long long>(g_exits));
  }
  FILE *f = fopen("r4300_indirect.txt", "w");
  dump(f, "jalr", g_jalr);
  dump(f, "jr", g_jr);
  dump(f, "ra", g_jr_ra);
  fclose(f);
  FILE *m = fopen("r4300_ram.bin", "wb");
  fwrite(eng.r4300->ptr(0x80000000u), 1, 0x900000, m);
  fclose(m);
  printf("wrote r4300_indirect.txt and r4300_ram.bin\n");
  return 0;
}
