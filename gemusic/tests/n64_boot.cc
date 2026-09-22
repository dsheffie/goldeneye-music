/* Boot an ordinary N64 ROM far enough to exercise the RCP register block.
 *
 * gemusic's own harness never boots anything: it stages libaudio in memory and calls it.
 * A ROM built against an open SDK instead expects a machine -- it kicks the RSP through
 * the SP registers, pulls its filesystem out of the cartridge with PI, and hands finished
 * audio to AI.  Running one is the only way to find out which of those registers are
 * wrong, since nothing else in this project touches them.
 *
 * Boots the way the hardware does: PIF copies IPL3 out of the ROM header into DMEM and
 * enters it, and IPL3 loads the rest itself. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "interpret.hh"
#include "sparse_mem.hh"
#include "rsp.hh"
#include "n64_rcp.hh"
#include "rcp_bridge.hh"

namespace {
  uint64_t g_samples = 0, g_bufs = 0;
  std::vector<int16_t> g_audio;
  sparse_mem *g_sm = nullptr;

  /* the DAC's job: take the buffer and keep the numbers */
  void ai_take(void *, uint32_t dram, uint32_t len) {
    g_bufs++;
    const int16_t *p = reinterpret_cast<const int16_t*>(g_sm->mem + dram);
    for(uint32_t i = 0; i < len / 2; i++) {
      uint16_t be = static_cast<uint16_t>(p[i]);
      g_audio.push_back(static_cast<int16_t>(__builtin_bswap16(be)));
    }
    g_samples += len / 4;
  }
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "usage: %s <rom.z64> [max_insns] [out.wav] [max_buffers]\n", argv[0]);
    return 2;
  }
  const uint64_t budget = (argc > 2) ? strtoull(argv[2], nullptr, 0) : 200000000ULL;
  /* how much audio to capture; one buffer is a little under 40 ms */
  const uint64_t want_bufs = (argc > 4) ? strtoull(argv[4], nullptr, 0) : 200ULL;

  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if(rom.size() < 0x1000) {
    fprintf(stderr, "%s is not a ROM\n", argv[1]);
    return 1;
  }
  printf("%s: %zu bytes\n", argv[1], rom.size());

  sparse_mem *sm = new sparse_mem();
  g_sm = sm;
  state_t *s = new state_t(*sm);
  initState(s);

  /* the cartridge, at PI domain 1 */
  memcpy(sm->mem + 0x10000000u, rom.data(), rom.size());

  rsp_t *rsp = new rsp_t();
  rsp->rdram = sm->mem;
  n64_rcp *rcp = new n64_rcp(s, new rsp_bridge(rsp), sm->mem);
  rcp->set_ai_sink(ai_take, nullptr);
  sm->rcp = rcp;

  /* PIF copies the ROM header's boot block into DMEM and enters it there */
  memcpy(rsp->mem + 0x40, rom.data() + 0x40, 0x1000 - 0x40);

  /* the register state IPL3 is entered with on real hardware */
  s->cpr0[CPR0_SR] = 0x34000000u;                 /* kernel, CU1, FR=1 */
  s->gpr[11] = static_cast<int64_t>(static_cast<int32_t>(0xa4000040u));   /* t3 */
  s->gpr[20] = 1;                                 /* s4: TV type / rom type */
  s->gpr[22] = 0x3f;                              /* s6: CIC seed, 6102 */
  s->gpr[23] = 0;                                 /* s7 */
  s->gpr[29] = static_cast<int64_t>(static_cast<int32_t>(0xa4001ff0u));   /* sp */
  s->gpr[31] = static_cast<int64_t>(static_cast<int32_t>(0xa4001550u));   /* ra */
  s->pc = static_cast<int64_t>(static_cast<int32_t>(0xa4000040u));

  const uint64_t trace = getenv("TRACE") ? strtoull(getenv("TRACE"), nullptr, 0) : 0;
  /* DMEM as it was when the CPU left IPL3.  It cannot be read at the end: the RSP runs
   * out of the same 4 KB and DMAs straight over the boot flags. */
  uint8_t dmem_at_handoff[16] = {0};
  bool handed_off = false;
  uint64_t n = 0, n_exc = 0;
  bool exl_prev = false;
  uint32_t last_pc = 0;
  while(n < budget) {
    last_pc = static_cast<uint32_t>(s->pc);
    if(n < trace) {
      /* through sparse_mem, so the SP window reaches DMEM rather than plain RDRAM */
      uint32_t pa = (last_pc >= 0x80000000u) ? (last_pc & 0x1fffffffu) : last_pc;
      printf("%6llu %08x  %08x\n", static_cast<unsigned long long>(n), last_pc,
	     __builtin_bswap32(sm->get<uint32_t>(pa)));
    }
    execMips(s);
    maybe_take_interrupt(s);          /* interrupts are live here, unlike the audio path */
    n++;
    rcp->tick(n);
    if(not handed_off and static_cast<uint32_t>(s->pc) < 0xa4000000u) {
      memcpy(dmem_at_handoff, rsp->mem, sizeof(dmem_at_handoff));
      handed_off = true;
    }
    {   /* an exception or interrupt was entered when EXL goes 0 -> 1 */
      const bool exl = (s->cpr0[CPR0_SR] & SR_EXL) != 0;
      if(exl and not exl_prev) { n_exc++; }
      exl_prev = exl;
    }
    if((n & 0xffffff) == 0) {
      printf("  %llu M insns, pc=%08x, %llu audio buffers\n",
	     static_cast<unsigned long long>(n >> 20), static_cast<uint32_t>(s->pc),
	     static_cast<unsigned long long>(g_bufs));
      fflush(stdout);
    }
    if(g_bufs >= want_bufs) {
      printf("  got %llu audio buffers, stopping\n", (unsigned long long)g_bufs);
      break;
    }
  }
  printf("stopped after %llu instructions at pc=%08x (previous %08x)\n",
	 static_cast<unsigned long long>(n), static_cast<uint32_t>(s->pc), last_pc);
  {   /* the flags IPL3 leaves in DMEM for the C runtime to pick up */
    const uint8_t *d = dmem_at_handoff;
    const uint32_t memsize = (static_cast<uint32_t>(d[0]) << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
    printf("boot flags at IPL3 handoff: memsize=%08x (%u MB) tvtype=%u resettype=%u consoletype=%u\n",
	   memsize, memsize >> 20, d[9], d[10], d[11]);
  }
  printf("exceptions/interrupts entered: %llu\n", static_cast<unsigned long long>(n_exc));
  printf("CP0: Status=%08x Cause=%08x  (IE=%u EXL=%u ERL=%u IM=%02x IP=%02x)\n",
	 s->cpr0[CPR0_SR], s->cpr0[CPR0_CAUSE], s->cpr0[CPR0_SR] & 1,
	 (s->cpr0[CPR0_SR] >> 1) & 1, (s->cpr0[CPR0_SR] >> 2) & 1,
	 (s->cpr0[CPR0_SR] >> 8) & 0xff, (s->cpr0[CPR0_CAUSE] >> 8) & 0xff);
  printf("MI: intr=%02x mask=%02x\n", rcp->intr(), rcp->mask());
  {   /* the loop it stopped in, so a spin-wait can be identified */
    const uint32_t base = (static_cast<uint32_t>(s->pc) & ~0x1fu) - 0x20;
    printf("around the final pc:\n");
    for(uint32_t a = base; a < base + 0x60; a += 4) {
      uint32_t pa = (a >= 0x80000000u) ? (a & 0x1fffffffu) : a;
      printf("  %08x  %08x%s\n", a, __builtin_bswap32(sm->get<uint32_t>(pa)),
	     (a == static_cast<uint32_t>(s->pc)) ? "   <-- pc" : "");
    }
  }
  printf("ISViewer: %llu magic accesses, %llu bytes logged\n",
	 (unsigned long long)rcp->n_isv_probe, (unsigned long long)rcp->n_isv_bytes);
  printf("RSP kicks: %llu, audio buffers: %llu, %llu stereo samples, %llu vblanks\n",
	 static_cast<unsigned long long>(rcp->n_kicks),
	 static_cast<unsigned long long>(rcp->n_ai_bufs),
	 static_cast<unsigned long long>(g_samples), static_cast<unsigned long long>(rcp->n_vi));

  if(argc > 3 and not g_audio.empty()) {
    FILE *f = fopen(argv[3], "wb");
    const uint32_t nb = static_cast<uint32_t>(g_audio.size() * 2), rate = 44100;
    const uint32_t hdr[] = {0x46464952, 36 + nb, 0x45564157, 0x20746d66, 16, 0x00020001,
			    rate, rate * 4, 0x00100004, 0x61746164, nb};
    fwrite(hdr, 1, 4, f); fwrite(&hdr[1], 1, 4, f);
    fwrite(&hdr[2], 1, 4, f); fwrite(&hdr[3], 1, 4, f); fwrite(&hdr[4], 1, 4, f);
    fwrite(&hdr[5], 1, 4, f); fwrite(&hdr[6], 1, 4, f); fwrite(&hdr[7], 1, 4, f);
    fwrite(&hdr[8], 1, 4, f); fwrite(&hdr[9], 1, 4, f); fwrite(&hdr[10], 1, 4, f);
    fwrite(g_audio.data(), 2, g_audio.size(), f);
    fclose(f);
    printf("wrote %s\n", argv[3]);
  }
  return 0;
}
