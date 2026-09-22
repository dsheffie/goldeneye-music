#include <rsp.inc>
# Probe microcode: execute one instruction against a known state and report the result.
#
# Every source short in memory is distinct and every destination register starts at zero,
# so a data-movement instruction reveals its whole permutation in one run: whatever ends
# up non-zero came from exactly one place, and its value says which.  Nothing is inferred.

    .set noreorder
    .set noat

    .data
    .align 4
MARKERS:
    .half 1,2,3,4,5,6,7,8           # the eight source shorts, each identifiable
    .space 0x100 - 16
RESULT:
    .space 128                      # v8..v15 land here for the CPU to read

    .text
    .globl _start
_start:
    ori s0, zero, 0x000             # word 0: base for the instruction under test
    vxor $v08, $v08, $v08
    vxor $v09, $v09, $v09
    vxor $v10, $v10, $v10
    vxor $v11, $v11, $v11
    vxor $v12, $v12, $v12
    vxor $v13, $v13, $v13
    vxor $v14, $v14, $v14
    vxor $v15, $v15, $v15
    .globl probe_slot
probe_slot:
    nop                             # word 9: patched by the CPU

    ori s1, zero, 0x100             # where the dump goes
    sqv $v08, 0x00,s1
    sqv $v09, 0x10,s1
    sqv $v10, 0x20,s1
    sqv $v11, 0x30,s1
    sqv $v12, 0x40,s1
    sqv $v13, 0x50,s1
    sqv $v14, 0x60,s1
    sqv $v15, 0x70,s1
    break
