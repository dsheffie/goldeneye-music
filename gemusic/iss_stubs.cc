/* Stubs for the parts of interp_mips this project never uses.
 *
 * interp_mips is an SGI Indy (IP22) system simulator; we want only its R4x00 CPU
 * core.  sparse_mem's device routing and cache/working-set instrumentation still
 * reference the Indy device models at link time, so the whole machine would come
 * along -- including sgi_seeq.cc, which includes <linux/if_tun.h> and so does not
 * build outside Linux.
 *
 * Every one of these is unreachable here.  The device calls sit behind
 * sparse_mem::route_devices, which defaults to false and which we never set (we
 * construct a bare sparse_mem in guest.cc); the instrumentation calls sit behind
 * null checks on the two globals defined below.  So the bodies are never entered,
 * and defining them lets us link the CPU core alone.  If a stub ever fires it means
 * something turned a feature on, hence the abort rather than a silent return. */
#include <cstdio>
#include <cstdlib>

#include "cache_model.hh"
#include "ws_profile.hh"
#include "sgi_mc.hh"
#include "sgi_hpc.hh"
#include "sgi_scc.hh"

static void unreachable(const char *what) {
  fprintf(stderr, "gemusic: %s is stubbed out but was called\n", what);
  abort();
}

/* instrumentation: null means "off", and nothing ever turns it on */
cache_model *g_cmodel = nullptr;
ws_profile *g_wsprof = nullptr;

void cm_load(cache_model *, uint32_t, void *, int) { unreachable("cm_load"); }
void cm_store(cache_model *, uint32_t, const void *, int) { unreachable("cm_store"); }
void cache_model::dram_wr(uint32_t, uint8_t) { unreachable("cache_model::dram_wr"); }

/* Indy devices: reachable only through sparse_mem::route_devices */
uint32_t sgi_mc::read(uint32_t, size_t) { unreachable("sgi_mc::read"); return 0; }
void sgi_mc::write(uint32_t, uint32_t, size_t) { unreachable("sgi_mc::write"); }

uint32_t sgi_hpc::read(uint32_t, size_t) { unreachable("sgi_hpc::read"); return 0; }
void sgi_hpc::write(uint32_t, uint32_t, size_t) { unreachable("sgi_hpc::write"); }
uint8_t sgi_hpc::ioc2_local0_live() { unreachable("sgi_hpc::ioc2_local0_live"); return 0; }
void sgi_hpc::enet_poll() { unreachable("sgi_hpc::enet_poll"); }

uint8_t sgi_scc::read(uint32_t) { unreachable("sgi_scc::read"); return 0; }
void sgi_scc::write(uint32_t, uint8_t) { unreachable("sgi_scc::write"); }
void sgi_scc::tick(uint64_t) { unreachable("sgi_scc::tick"); }
