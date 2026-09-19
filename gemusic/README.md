# gemusic

Renders GoldenEye 007 (N64, NTSC-U `NGEE`) music to WAV on an ordinary Unix box by
running the console's own code rather than reimplementing it:

- **CPU side** - the ROM's libaudio (sequence player, voice allocation, envelopes, the
  synthesis driver, the custom reverb setup) executes unmodified on the `interp_mips`
  R4x00 simulator.  No boot, no OS, no interrupts: the image is staged in memory and
  `alInit` / `alCSPNew` / `alCSPSetSeq` / `alCSPPlay` / `alAudioFrame` are called directly.
- **RSP side** - each frame's audio command list is executed by the ROM's real audio
  microcode (`aspMain`, started by the real `rspboot`) on `rsp.cc` (in `~/code/rsp-bt`),
  a small model of the RSP: MIPS integer subset, SP DMA registers, and the COP2 vector unit.

Nothing of Nintendo's or Rare's is in this directory; everything is read from your ROM.

    make                      # INTERP=~/code/interp_mips by default
    ./gemusic -r GoldenEye.z64 -s 3 -o seq03.wav
    ./gemusic -r GoldenEye.z64 -s 3 -t 300 --fade 10 -o long.wav

Output is 22047 Hz stereo, the true hardware rate (`osAiSetFrequency(22050)` rounds to a
DAC divider of 2208).  Tunes that loop forever are cut at `--seconds` with a fade;
jingles stop on their own after two seconds of digital silence.  Per-sequence volume
comes from the game's own table.

## LLVM backend (optional)

    make clean && make LLVM=1
    ./gemusic -r GoldenEye.z64 -s 3 --rsp llvm -o seq03.wav      # add --dump-ir to see the IR

The RSP interpreter and an LLVM binary translator for it live in `~/code/rsp-bt`
(`RSPBT=` to point elsewhere); see its README.  It translates the microcode at first
sight, memoized by a hash of instruction memory, and is handed the audio ABI's command
table as jump-target hints.  Byte-identical to the interpreter on all 63 sequences and
about 23x faster on the RSP portion, portable to any LLVM target.

## AVX-512 backend (optional, x86)

    make clean && make AVX512=1 ROM=GoldenEye.z64
    ./gemusic -r GoldenEye.z64 -s 3 --rsp avx512 -o seq03.wav

`rsp2avx512.py` statically recompiles the ROM's `aspMain` into one C++ function (a label
per instruction, delay slots folded into their branches, `jr` back through a switch on
the 12-bit PC) and `rsp_avx512.hh` supplies the vector unit: a register is one xmm, the
eight 48-bit accumulators are 8 x int64 in one zmm (products via `vpmuldq`, the clamp on
readout via the saturating narrow `vpmovsqw`), element broadcast is a constant `vpermw`,
and the compare/carry flags are mask registers.  The generated source is derived from
the ROM, so it lives in `obj/` and is never checked in; at startup the backend checks a
hash of the ROM's microcode against the one it was generated from.  Output is
byte-identical to the interpreter on all 63 sequences, with the RSP portion ~15x faster.
Switching between the two build configurations needs a `make clean`.

## Verification

`tests/adpcm_test` drives the microcode's ADPCM decoder with a hand-built command list.
All 105 ADPCM wavetables in the music bank decode bit-exact against an independent
reference decoder, which exercises rspboot, SP DMA, the vector loads/stores and the
multiply-accumulate ops end to end.

## Notes

- `ge_addrs.hh` holds every ROM-specific address and how it was found.  Another
  libaudio game needs only a new copy of that file (and possibly more RSP opcodes:
  `rsp.cc` implements what rspboot + aspMain execute and dies loudly on anything else).
- GoldenEye's code is FR=0 (it builds double constants with `mtc1` to odd/even register
  pairs), so the guest runs with Status.FR clear.
- `interp_mips` mirrors the r9999 RTL, where `div`/`sqrt` trap to an OS soft-float
  emulator.  There is no OS here, so `main()` sets its `FP_NODIVTRAP` knob.
- Dependencies: zlib, boost::program_options, plus whatever interp_mips links
  (boost_serialization, capstone, its SoftFloat submodule).  On a non-x86 host point
  `SOFTFLOAT=` at the matching SoftFloat build.
