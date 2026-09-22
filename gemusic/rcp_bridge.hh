#ifndef __RCP_BRIDGE_HH__
#define __RCP_BRIDGE_HH__

#include <cstdint>
#include "n64_rcp.hh"

struct rsp_t;
struct r4300_t;

/* Hands the vr4300 simulator's SP block our RSP model.  The two were written apart --
 * the RSP lives in rsp-bt, the register file in the CPU simulator -- so this is the
 * only place that knows about both. */
struct rsp_bridge : public rsp_iface {
  rsp_t *r;
  explicit rsp_bridge(rsp_t *r) : r(r) {}
  void dma(uint32_t len_reg, bool to_rsp) override;
  void run(uint32_t pc) override;
  uint8_t *mem() override;
  uint32_t &mem_addr() override;
  uint32_t &dram_addr() override;
  uint32_t &pc_reg() override;
  uint32_t status() override;
  void apply_status_write(uint32_t x) override;
};

/* Give a machine an RCP, so a ROM can drive the RSP itself instead of us handing it a
 * task.  Not used by the GoldenEye path, which calls libaudio directly. */
n64_rcp *ge_attach_rcp(r4300_t &g, rsp_t &rsp);

#endif
