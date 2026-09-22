#include "interpret.hh"
#include "sparse_mem.hh"
#include "rsp.hh"
#include "r4300.hh"
#include "rcp_bridge.hh"

void rsp_bridge::dma(uint32_t len_reg, bool to_rsp) { r->dma(len_reg, to_rsp); }
void rsp_bridge::run(uint32_t pc) { r->halted = false; r->run(pc); }
uint8_t *rsp_bridge::mem() { return r->mem; }
uint32_t &rsp_bridge::mem_addr() { return r->sp_mem_addr; }
uint32_t &rsp_bridge::dram_addr() { return r->sp_dram_addr; }
uint32_t &rsp_bridge::pc_reg() { return r->pc; }
uint32_t rsp_bridge::status() { return r->sp_status; }
void rsp_bridge::apply_status_write(uint32_t x) { r->apply_status_write(x); }

n64_rcp *ge_attach_rcp(r4300_t &g, rsp_t &rsp) {
  rsp.rdram = g.ptr(0x80000000u);
  n64_rcp *rcp = new n64_rcp(g.s, new rsp_bridge(&rsp), g.ptr(0x80000000u));
  g.sm->rcp = rcp;
  return rcp;
}
