/* A small replacement for interp_mips's sparse_mem backing store, for wasm32.
 *
 * The original mmaps a flat 4 GiB with MAP_NORESERVE, so only touched pages cost
 * anything.  wasm32's entire address space is 4 GiB, so that cannot work there.
 *
 * This project's physical addresses are all small: every kseg0/kseg1 address
 * is masked to 29 bits, the structures we place ourselves top out around 8.4 MB
 * (RAM_TBL plus the wavetable), and the game's own code and data sit below 1 MB.  So a
 * modest flat allocation covers it, with a guard so that an address outside the
 * region fails loudly instead of corrupting the heap.
 *
 * Only used in an Emscripten build; native builds link interp_mips's own version. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#include "sparse_mem.hh"

/* 64 MB.  The high-water mark here is about 8.4 MB (RAM_TBL plus the
 * wavetable), so this is roughly 8x headroom and still nothing in a browser.
 *
 * A genuinely sparse page table would be better -- it would cover the whole 4 GiB
 * address space while allocating only touched pages -- but sparse_mem's get/set are
 * inline templates that index mem[] directly, so that needs a change in interp_mips
 * rather than here.  r4300_t::ptr() range-checks against this in the wasm build. */
static const uint64_t WASM_MEM_SZ = 64UL << 20;

/* interp_mips's globals, unused here but referenced by its headers */
uint32_t *g_wr_pc = nullptr;
uint64_t *g_wr_icnt = nullptr;

static uint8_t *alloc_sim_ram() {
  void *p = mmap(nullptr, WASM_MEM_SZ, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if(p == MAP_FAILED) {
    fprintf(stderr, "sparse_mem: could not reserve %llu bytes of simulated RAM\n",
	    static_cast<unsigned long long>(WASM_MEM_SZ));
    abort();
  }
  memset(p, 0, WASM_MEM_SZ);
  return reinterpret_cast<uint8_t*>(p);
}

sparse_mem::sparse_mem() {
  mem = alloc_sim_ram();
}

sparse_mem::~sparse_mem() {
  munmap(mem, WASM_MEM_SZ);
}

void sparse_mem::clear() {
  memset(mem, 0, WASM_MEM_SZ);
}

uint64_t sparse_mem_wasm_size() {
  return WASM_MEM_SZ;
}

/* sparse_mem's device routing.  interp_mips defines these in the file we are
 * replacing; they are only ever reached when route_devices is true, which this
 * project never sets, so the bodies just fail loudly.  The explicit instantiations
 * match the access widths interpret.cc uses. */
template <typename T> T sparse_mem::route_load(uint64_t pa) {
  fprintf(stderr, "sparse_mem: device read at %#llx, but device routing is not built in\n",
	  static_cast<unsigned long long>(pa));
  abort();
}

template <typename T> void sparse_mem::route_store(uint64_t pa, T) {
  fprintf(stderr, "sparse_mem: device write at %#llx, but device routing is not built in\n",
	  static_cast<unsigned long long>(pa));
  abort();
}

#define INSTANTIATE(T) \
  template T sparse_mem::route_load<T>(uint64_t); \
  template void sparse_mem::route_store<T>(uint64_t, T)
INSTANTIATE(uint8_t);
INSTANTIATE(int8_t);
INSTANTIATE(uint16_t);
INSTANTIATE(int16_t);
INSTANTIATE(uint32_t);
INSTANTIATE(int32_t);
INSTANTIATE(uint64_t);
INSTANTIATE(int64_t);
#undef INSTANTIATE
