#ifndef __R4300_HH__
#define __R4300_HH__

#include <cstdint>
#include <cstddef>
#include <vector>

class state_t;
class sparse_mem;
class r4300bt;

/* Runs GoldenEye's own libaudio on the interp_mips R4x00 core.  No OS, no boot, no
 * interrupts: the image is staged in memory and ROM functions are called directly
 * (args in a0..a3, return when pc reaches a magic ra). */
struct r4300_t {
  sparse_mem *sm = nullptr;
  state_t *s = nullptr;
  uint64_t n_insns = 0;
  /* optional per-instruction hook, for analysis tools; null in normal use */
  void (*on_step)(r4300_t &, uint32_t pc, uint32_t insn) = nullptr;
  /* optional binary translator; call() uses it for any function it has translated */
  r4300bt *bt = nullptr;

  r4300_t(const std::vector<uint8_t> &rom);
  ~r4300_t();

  uint32_t call(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);
  uint8_t *ptr(uint32_t vaddr);                      /* host pointer for a kseg0/kseg1 address */
  /* kseg0/kseg1 mask, plus the game's 4 MB alias at 0x70000000 -> physical 0 */
  uint8_t *ptr_any(uint32_t vaddr);
  uint32_t rd32(uint32_t vaddr);
  uint16_t rd16(uint32_t vaddr);
  void wr32(uint32_t vaddr, uint32_t v);
  void wr8(uint32_t vaddr, uint8_t v);
};

/* where we place our own structures in the simulated RAM (all kseg0, all below 16 MB
 * because the audio microcode masks DRAM addresses to 24 bits) */
#define RAM_AUDIO_HEAP      0x80400000u
#define RAM_AUDIO_HEAP_LEN  0x00300000u
#define RAM_ALHEAP          0x80700000u
#define RAM_ALGLOBALS       0x80700100u
#define RAM_SYNCONFIG       0x80700600u
#define RAM_SEQPCONFIG      0x80700700u
#define RAM_CMDLEN          0x80700800u
#define RAM_STUBS           0x80700900u
#define RAM_RETURN          0x80700a00u
/* scratch word a squashed store writes instead of its real address, for the branch-likely
 * if-conversion in r4300bt (DESIGN_R4300.md).  Nothing else may read it. */
#define RAM_BITBUCKET       0x80700b00u
#define RAM_CSPLAYER        0x80701000u
#define RAM_CSEQ            0x80701800u
#define RAM_CMDLIST         0x80710000u
#define RAM_OUTBUF          0x80720000u
#define RAM_SEQDATA         0x80730000u
#define RAM_CTL             0x80740000u
#define RAM_TBL             0x80800000u
#define RAM_STACK_TOP       0x807f0000u

#endif
