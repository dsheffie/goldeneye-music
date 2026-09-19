#ifndef __GUEST_HH__
#define __GUEST_HH__

#include <cstdint>
#include <cstddef>
#include <vector>

class state_t;
class sparse_mem;

/* Runs GoldenEye's own libaudio on the interp_mips R4x00 core.  No OS, no boot, no
 * interrupts: the image is staged in memory and guest functions are called directly
 * (args in a0..a3, return when pc reaches a magic ra). */
struct guest_t {
  sparse_mem *sm = nullptr;
  state_t *s = nullptr;
  uint64_t n_insns = 0;

  guest_t(const std::vector<uint8_t> &rom);
  ~guest_t();

  uint32_t call(uint32_t fn, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0);
  uint8_t *ptr(uint32_t vaddr);                      /* host pointer for a kseg0/kseg1 address */
  uint32_t rd32(uint32_t vaddr);
  uint16_t rd16(uint32_t vaddr);
  void wr32(uint32_t vaddr, uint32_t v);
  void wr8(uint32_t vaddr, uint8_t v);
};

/* guest memory map for our own structures (all kseg0, all below 16 MB physical
 * because the audio microcode masks DRAM addresses to 24 bits) */
#define G_AUDIO_HEAP      0x80400000u
#define G_AUDIO_HEAP_LEN  0x00300000u
#define G_ALHEAP          0x80700000u
#define G_ALGLOBALS       0x80700100u
#define G_SYNCONFIG       0x80700600u
#define G_SEQPCONFIG      0x80700700u
#define G_CMDLEN          0x80700800u
#define G_STUBS           0x80700900u
#define G_RETURN          0x80700a00u
#define G_CSPLAYER        0x80701000u
#define G_CSEQ            0x80701800u
#define G_CMDLIST         0x80710000u
#define G_OUTBUF          0x80720000u
#define G_SEQDATA         0x80730000u
#define G_CTL             0x80740000u
#define G_TBL             0x80800000u
#define G_STACK_TOP       0x807f0000u

#endif
