/* Learn an instruction's behaviour by running it, rather than by reading about it.
 *
 * The RSP Programmer's Guide describes the transpose loads in terms of a "Slice" it never
 * binds to a memory offset, so which of the eight memory shorts reaches which register
 * and lane is not derivable from the document.  It is, however, directly observable: fill
 * memory with eight distinct shorts, zero the destination registers, run the instruction,
 * and read off where each marker landed.  Nothing is inferred -- the mapping is measured.
 *
 * Output goes through ISViewer so the run is scriptable against any emulator. */
#include <libdragon.h>
#include <rsp.h>

DEFINE_RSP_UCODE(rsp_probe);

extern uint32_t probe_slot[];

#define DMEM ((volatile uint32_t *)0xA4000000)

/* lwc2 = 0x32.  base=s0(16), vt, opcode(form), element, 7-bit offset */
static uint32_t vload(uint32_t form, uint32_t vt, uint32_t e, uint32_t off) {
	return (0x32u << 26) | (16u << 21) | (vt << 16) | (form << 11) | (e << 7) | (off & 0x7f);
}

static void run_probe(const char *name, uint32_t insn) {
	/* Patch the slot, which the microcode places at a known word index. */
	const uint32_t slot = 9;
	uint32_t *text = (uint32_t *)rsp_probe.code;
	text[slot] = insn;

	rsp_wait();
	rsp_load(&rsp_probe);

	/* The markers ship in the ucode's data section, so rsp_load has already placed
	 * them; only the result area needs clearing. */
	for(int i = 0x40; i < 0x60; i++) {
		DMEM[i] = 0;
	}
	rsp_run();

	debugf("%s insn=%08lx\n", name, (unsigned long)insn);
	for(int r = 0; r < 8; r++) {
		debugf("  v%02d:", r + 8);
		for(int l = 0; l < 8; l++) {
			const uint32_t w = DMEM[0x40 + r * 4 + l / 2];
			const uint32_t h = (l & 1) ? (w & 0xffff) : (w >> 16);
			debugf(" %2lu", (unsigned long)h);
		}
		debugf("\n");
	}
}

int main(void) {
	debug_init_isviewer();
	debugf("=== RSP probe ===\n");

	/* s0 must point at the markers; the microcode uses it as the base register.  It is
	 * set by leaving the value in DMEM is not possible, so the instruction carries a
	 * zero offset and the microcode's own `ori s0` supplies the address. */
	for(uint32_t e = 0; e < 16; e += 2) {
		char nm[32];
		sprintf(nm, "ltv e=%lu", (unsigned long)e);
		run_probe(nm, vload(11, 8, e, 0));
	}
	debugf("=== done ===\n");
	while(1) { }
}
