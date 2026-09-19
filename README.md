# goldeneye-music

Tools for getting the music out of a GoldenEye 007 (N64, NTSC-U `NGEE`) ROM by running
the console's own code.  **No ROM, audio, or anything else derived from the game is in
this repository** - you supply your own big-endian `.z64` image.

- `gemusic/` - renders any of the game's 63 sequences to WAV.  The ROM's own libaudio
  runs on the [interp_mips](https://github.com/dsheffie/interp_mips) R4x00 simulator and
  the ROM's real audio microcode runs on the RSP model from
  [rsp-bt](https://github.com/dsheffie/rsp-bt): an interpreter, an LLVM binary
  translator, or a build-time AVX-512 recompilation, all byte-identical.  See
  `gemusic/README.md`.  The same directory builds `gemms`, an XMMS-style player that
  runs the engine live.
- `tools/` - Python: `extract_music.py` (sequences to standard MIDI, instrument samples
  to WAV with loop points, bank to JSON), `ctl.py` (libultra bank parser),
  `render_all.py` (batch render through gemusic).

See [THEORY_OF_OPERATION.md](THEORY_OF_OPERATION.md) for how it works.

## Building

    git clone https://github.com/dsheffie/interp_mips ~/code/interp_mips   # plus its SoftFloat submodule
    git clone https://github.com/dsheffie/rsp-bt      ~/code/rsp-bt
    cmake -S . -B build && cmake --build build -j

Everything optional is detected and reported at configure time:

| | needs | effect if missing |
|---|---|---|
| RSP JIT | LLVM 18 or later | RSP interpreter only, still ~19x real time |
| `gemms` player | SDL2 | not built |
| tests | `-DROM=/path/to/GoldenEye.z64` | not registered |

`-DINTERP_MIPS=` and `-DRSPBT=` override the sibling checkout paths, and `-DENABLE_JIT=OFF`
forces the interpreter.  `ctest` runs the ADPCM bit-exactness check.  A plain Makefile is
also kept in `gemusic/` (it additionally has an x86-only AVX-512 backend, which the JIT
supersedes: the JIT is faster and needs no ROM at build time).

Generated files (the recompiled microcode, dumped LLVM IR, rendered audio) are derived
from the ROM and are git-ignored on purpose.
