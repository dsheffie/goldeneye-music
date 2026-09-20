/* Decode one wavetable through the real audio microcode on rsp_t and dump the PCM,
 * so it can be diffed against an independent VADPCM decoder.
 * usage: adpcm_test <rom> <wavetable offset in ctl, hex> <out.raw> */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "../r4300.hh"
#include "rsp.hh"
#include "../ge_addrs.hh"

static uint32_t be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

int main(int argc, char *argv[]) {
  if(argc != 4) {
    fprintf(stderr, "usage: %s rom wt_offs_hex out.raw\n", argv[0]);
    return -1;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  uint32_t wto = static_cast<uint32_t>(strtoul(argv[2], nullptr, 16));
  const uint8_t *ctl = &rom[GE_ROM_INST_CTL];
  uint32_t base = be32(ctl + wto), len = be32(ctl + wto + 4), book = be32(ctl + wto + 16);
  uint32_t order = be32(ctl + book), npred = be32(ctl + book + 4);

  r4300_t g(rom);
  memcpy(g.ptr(RAM_CTL), ctl, GE_ROM_INST_CTL_LEN);
  memcpy(g.ptr(RAM_TBL), &rom[GE_ROM_INST_TBL], GE_ROM_INST_TBL_LEN);
  rsp_t rsp;
  rsp.rdram = g.ptr(0x80000000u);

  const uint32_t state = 0x00700000u, dst = 0x00720000u;
  FILE *fp = fopen(argv[3], "wb");
  uint32_t n_frames = len / 9;
  for(uint32_t f = 0; f < n_frames; f += 10) {
    uint32_t nf = (n_frames - f < 10) ? (n_frames - f) : 10;
    uint32_t src = ((RAM_TBL & 0x1fffffffu) + base + 9*f);
    uint32_t skew = src & 7, nbytes = (9*nf + skew + 7) & ~7u;
    std::vector<uint32_t> cl = {
      0x07000000u, 0,                                        /* aSegment(0,0)            */
      0x0b000000u | (16*order*npred), (RAM_CTL & 0x1fffffffu) + book + 8,  /* aLoadADPCM  */
      0x08000000u, nbytes,                                   /* aSetBuffer(0,0,0,nbytes) */
      0x04000000u, src & ~7u,                                /* aLoadBuffer              */
      0x08000000u | skew, (0x140u << 16) | (nf*32),          /* aSetBuffer(0,skew,0x140,n)*/
      0x01000000u | ((f == 0 ? 1u : 0u) << 16), state,       /* aADPCMdec(A_INIT?,state) */
      0x08000000u, (0x140u << 16) | (nf*32 + 32),            /* aSetBuffer(0,0,0x140,n+32)*/
      0x06000000u, dst                                       /* aSaveBuffer              */
    };
    for(size_t i = 0; i < cl.size(); i++) {
      g.wr32(RAM_CMDLIST + 4*i, cl[i]);
    }
    const uint32_t task[16] = {2, 0, GE_rspbootText & 0x1fffffffu, GE_rspbootText_LEN,
			       GE_aspMainText & 0x1fffffffu, 0x1000, GE_aspMainData & 0x1fffffffu, 0x800,
			       0, 0, 0, 0, RAM_CMDLIST & 0x1fffffffu, static_cast<uint32_t>(cl.size()*4), 0, 0};
    for(int i = 0; i < 16; i++) {
      uint8_t *p = rsp.mem + 0xfc0 + 4*i;
      p[0] = task[i] >> 24; p[1] = task[i] >> 16; p[2] = task[i] >> 8; p[3] = task[i];
    }
    memcpy(rsp.mem + 0x1000, g.ptr(GE_rspbootText), GE_rspbootText_LEN);
    rsp.run(0);
    fwrite(g.ptr(0x80000000u + dst), 1, nf*32 + 32, fp);     /* 32-byte state prefix + samples */
  }
  fclose(fp);
  fprintf(stderr, "wavetable base %#x len %u order %u npred %u: %u frames, %lu RSP insns\n", base, len, order, npred, n_frames, rsp.n_insns);
  return 0;
}
