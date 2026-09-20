#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <zlib.h>

#include "interpret.hh"
#include "sparse_mem.hh"
#include "globals.hh"
#include "r4300.hh"
#include "ge_addrs.hh"

/* interp_mips expects these from its main.cc */
uint32_t g_watch_pa = 0, g_watch_ldpc = 0;
uint64_t g_watch_lo = 0, g_watch_hi = ~0ULL;
uint64_t g_cur_pc = 0, g_cur_icnt = 0;
uint64_t g_htrace = 0;
namespace globals {
  bool enClockFuncts = false;
  uint64_t icountMIPS = 0;
  uint64_t cycle = 0;
  bool trace_retirement = false;
  bool trace_fp = false;
  bool report_syscalls = false;
  FILE *pctrace = nullptr;
  uint32_t pctrace_start = 0;
  bool pctrace_on = false;
  bool asidpc_armed = false;
  retire_trace *retire_log = nullptr;
}

std::vector<uint8_t> inflate_1172(const uint8_t *p, size_t avail) {
  if(p[0] != 0x11 or p[1] != 0x72) {
    fprintf(stderr, "not a 1172 block\n");
    exit(-1);
  }
  std::vector<uint8_t> out(1 << 20);
  z_stream z;
  memset(&z, 0, sizeof(z));
  inflateInit2(&z, -15);
  z.next_in = const_cast<Bytef*>(p + 2);
  z.avail_in = static_cast<uInt>(avail - 2);
  z.next_out = out.data();
  z.avail_out = static_cast<uInt>(out.size());
  int rc = inflate(&z, Z_FINISH);
  if(rc != Z_STREAM_END) {
    fprintf(stderr, "inflate failed (%d)\n", rc);
    exit(-1);
  }
  out.resize(z.total_out);
  inflateEnd(&z);
  return out;
}

r4300_t::r4300_t(const std::vector<uint8_t> &rom) {
  sm = new sparse_mem();
  s = new state_t(*sm);
  initState(s);
  /* kernel mode, FPU on, FR=0.  The PIF hands IPL3 0x34000000 (FR=1), but libultra
   * threads get their Status from the thread context, which never sets FR, and the
   * code depends on it: IDO builds double constants with mtc1 to the odd/even pair
   * (e.g. 0x8001942c: mtc1 at,$f13 ; mtc1 zero,$f12 ; div.d $f2,$f6,$f12). */
  s->cpr0[CPR0_SR] = 0x30000000u;
  /* GoldenEye runs through a 4 MB TLB alias, virtual 0x70000000 -> physical 0 (its
   * first tlbwi, at 0x800004a8), and the function pointers in its data point there. */
  s->tlb[0].entry_hi = 0x70000000u;
  s->tlb[0].page_mask = 0x007fe000u;
  s->tlb[0].entry_lo0 = 0x1fu;
  s->tlb[0].entry_lo1 = 0x01u;

  /* boot code, raw; then the data segment GoldenEye inflates in place behind it */
  memcpy(ptr(GE_BOOT_VADDR), &rom[GE_ROM_BOOT_OFFS], GE_ROM_DATA_1172 - GE_ROM_BOOT_OFFS);
  std::vector<uint8_t> data = inflate_1172(&rom[GE_ROM_DATA_1172], 0x100000);
  memcpy(ptr(GE_DATA_VADDR), data.data(), data.size());

  /* dmaNew: returns &dmaCall.  dmaCall(addr,len,state): samples are already resident
   * (the bank is relocated against RAM_TBL), so the "DMA" just returns the physical
   * address, which is what libaudio puts straight into A_LOADBUFF. */
  const uint32_t stubs[] = {
    0x3c020000u | ((RAM_STUBS + 0x10) >> 16),             /* lui  v0, hi            */
    0x34420000u | ((RAM_STUBS + 0x10) & 0xffff),          /* ori  v0, v0, lo        */
    0x03e00008u, 0x00000000u,                           /* jr ra ; nop            */
    0x3c011fffu, 0x3421ffffu,                           /* lui at,0x1fff ; ori at,at,0xffff */
    0x03e00008u, 0x00811024u                            /* jr ra ; and v0, a0, at */
  };
  for(size_t i = 0; i < sizeof(stubs)/sizeof(stubs[0]); i++) {
    wr32(RAM_STUBS + 4*i, stubs[i]);
  }
}

r4300_t::~r4300_t() {
  delete s;
  delete sm;
}

uint8_t *r4300_t::ptr(uint32_t vaddr) {
  return sm->mem + (vaddr & 0x1fffffffu);
}

uint32_t r4300_t::rd32(uint32_t vaddr) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(ptr(vaddr)));
}

uint16_t r4300_t::rd16(uint32_t vaddr) {
  return __builtin_bswap16(*reinterpret_cast<uint16_t*>(ptr(vaddr)));
}

void r4300_t::wr32(uint32_t vaddr, uint32_t v) {
  *reinterpret_cast<uint32_t*>(ptr(vaddr)) = __builtin_bswap32(v);
}

void r4300_t::wr8(uint32_t vaddr, uint8_t v) {
  *ptr(vaddr) = v;
}

uint32_t r4300_t::call(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  const uint32_t args[4] = {a0, a1, a2, a3};
  for(int i = 0; i < 4; i++) {
    s->gpr[4+i] = static_cast<int64_t>(static_cast<int32_t>(args[i]));
  }
  s->gpr[29] = static_cast<int64_t>(static_cast<int32_t>(RAM_STACK_TOP - 64));
  s->gpr[31] = static_cast<int64_t>(static_cast<int32_t>(RAM_RETURN));
  s->pc = static_cast<int64_t>(static_cast<int32_t>(fn));
  const int64_t ret_pc = static_cast<int64_t>(static_cast<int32_t>(RAM_RETURN));
  /* 1ULL: on a 32-bit target (wasm32) unsigned long is 32 bits, the shift is
   * masked to 0, and the budget would come out as 1 -- every call "times out". */
  uint64_t budget = 1ULL << 32;
  while(s->pc != ret_pc) {
    uint32_t pc32 = static_cast<uint32_t>(s->pc);
    /* outside the boot image or our stubs means the R4300 took an exception */
    uint32_t pa = (pc32 >= 0x80000000u) ? (pc32 & 0x1fffffffu) : (pc32 - 0x70000000u);
    if(not((pa >= (GE_BOOT_VADDR & 0x1fffffffu) and pa < (GE_DATA_VADDR & 0x1fffffffu)) or (pc32 >= RAM_STUBS and pc32 < RAM_RETURN))) {
      fprintf(stderr, "R4300 left the loaded image: pc=%08x (call %08x) cause=%08x epc=%08x badva=%08x sr=%08x\n",
	      pc32, fn, s->cpr0[CPR0_CAUSE], s->cpr0[CPR0_EPC], s->cpr0[CPR0_BADVADDR], s->cpr0[CPR0_SR]);
      fprintf(stderr, "  f6=%016lx f12=%016lx fcsr=%08x %08x %08x %08x %08x\n", s->cpr1[6], s->cpr1[12],
	      s->fcr1[0], s->fcr1[1], s->fcr1[2], s->fcr1[3], s->fcr1[4]);
      exit(-1);
    }
    execMips(s);
    n_insns++;
    if(--budget == 0) {
      fprintf(stderr, "call to %08x never returned\n", fn);
      exit(-1);
    }
  }
  return static_cast<uint32_t>(s->gpr[2]);
}
