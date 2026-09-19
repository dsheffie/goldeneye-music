# goldeneye-music

Tools for getting the music out of a GoldenEye 007 (N64, NTSC-U `NGEE`) ROM by running
the console's own code.  **No ROM, audio, or anything else derived from the game is in
this repository** - you supply your own big-endian `.z64` image.

- `gemusic/` - renders any of the game's 63 sequences to WAV.  The ROM's own libaudio
  runs on the [interp_mips](https://github.com/dsheffie/interp_mips) R4x00 simulator and
  the ROM's real audio microcode runs on the RSP model from
  [rsp-bt](https://github.com/dsheffie/rsp-bt): an interpreter, an LLVM binary
  translator, or a build-time AVX-512 recompilation, all byte-identical.  See
  `gemusic/README.md`.
- `tools/` - Python: `extract_music.py` (sequences to standard MIDI, instrument samples
  to WAV with loop points, bank to JSON), `ctl.py` (libultra bank parser),
  `render_all.py` (batch render through gemusic).

## Building

    git clone https://github.com/dsheffie/interp_mips ~/code/interp_mips   # plus its SoftFloat submodule
    git clone https://github.com/dsheffie/rsp-bt      ~/code/rsp-bt
    cd gemusic && make            # INTERP= and RSPBT= override the sibling paths

Generated files (the recompiled microcode, dumped LLVM IR, rendered audio) are derived
from the ROM and are git-ignored on purpose.
