# Theory of operation

How this project plays GoldenEye 007's music without booting the game, without an
operating system, and without emulating a Nintendo 64.

## The idea

A Nintendo 64 game's music is produced by two processors working together.  The main
CPU (an R4300, MIPS III) runs libultra's audio library: it walks the sequence data,
allocates voices, computes envelopes and pitches, and emits a list of commands.  The RSP
-- a small MIPS integer core with a vector unit, inside the RCP -- executes those
commands with the game's audio microcode, and that is where samples are actually
decoded, resampled, mixed and reverberated.  The audio interface then DMAs the finished
buffer to the DAC.

The load-bearing observation is that **libultra's audio library is a synchronous,
self-contained subroutine**.  `alAudioFrame` takes a command buffer, an output buffer and
a sample count; it walks its internal event queue, updates voice state, writes a command
list, and returns.  It never blocks, never yields, and asks the operating system for
exactly one service.  The game's audio thread is a thin loop around it.

So we do not need the game.  We need the library, and something to run it on.

## What the console does, and what we do instead

On the console, each audio frame is four layers:

1. The video interrupt wakes the audio manager thread.
2. That thread calls `alAudioFrame` to build the RSP command list.
3. `osSpTaskLoad` / `osSpTaskStartGo` hand the list to the RSP; an SP interrupt reports
   completion.
4. The audio interface DMAs the result to the DAC.

We keep layer 2 and delete the rest:

| Console | Here |
|---|---|
| Audio thread, scheduler, message queues | direct calls on one host thread |
| VI interrupt as the frame clock | a `for` loop |
| Interrupts generally | never enabled (`Status.IE` is clear); nothing can fire |
| `osSpTaskLoad` / SP interrupt handshake | the task descriptor is written into DMEM and the RSP model is called synchronously; it is finished when it returns |
| Audio interface DMA to the DAC | a WAV file, or the player's ring buffer |
| PI DMA fetching samples from cartridge ROM | the wavetable is pre-loaded; the callback is the identity function |

Two processors, two models:

- **`interp_mips`** (David Sheffield's MIPS III/IV simulator) runs the R4300 side: the
  ROM's own libaudio code, unmodified.
- **`rsp-bt`** supplies the RSP: an interpreter, an LLVM binary translator, and in this
  repository a build-time AVX-512 recompilation.  All three are byte-identical.

For 30 seconds of music the split is about 33 million R4300 instructions against 127
million RSP instructions.

## The fake machine

`guest_t` (`gemusic/guest.cc`) is a Nintendo 64 with almost everything removed:

- **Memory.**  `sparse_mem`'s flat 4 GiB backing store.  Into it go the ROM's boot code
  (raw at ROM offset 0x1000) and its data segment, which the game stores as a compressed
  block and inflates in place at 0x80020d90 -- the audio microcode and all initialised
  data live inside it.  Our own structures (heap, bank, command list, output buffer) are
  placed above them at addresses chosen in `gemusic/guest.hh`.
- **`Status` = 0x30000000.**  Kernel mode, coprocessor 1 enabled, `FR` clear, interrupts
  off.  `FR` matters: the compiler built double constants by writing halves to an
  odd/even register pair, which only works in `FR=0`.  The PIF boot ROM leaves `FR` set,
  but libultra threads take their `Status` from the thread context, which never sets it.
- **One TLB entry.**  GoldenEye's first act is to map a 4 MB page, virtual 0x70000000
  onto physical 0, and to run from there; the function pointers in its data point into
  that alias.  We install the same entry directly.  The guest never writes the TLB
  afterwards -- across 30 seconds of audio it executes zero `tlbwr`, `tlbwi`, `tlbr` and
  `tlbp` -- but 98.7% of instruction fetches still translate through that one entry.
- **Eight instructions of synthetic MIPS** implementing the one OS service, below.

## The host/guest bridge

`guest_t::call(fn, a0..a3)` is the whole interface:

1. put the arguments in `a0`-`a3`;
2. set `sp` to a scratch area and `ra` to a magic address in unmapped space;
3. set `pc` to the target;
4. step the simulator until `pc` reaches the magic address;
5. return `v0`.

The o32 ABI passes the first four arguments in registers, which covers every function we
call.  A guest that wanders outside the loaded image -- the signature of an unexpected
exception -- is reported and aborts rather than running on.

## The one OS service

libaudio obtains sample data through a DMA callback installed at initialisation.  On the
console it fetches from cartridge ROM on demand.  Here the entire wavetable is already
resident, so `dmaNew` returns a `dmaCall` that masks its argument to a physical address
and returns it, which is what the microcode then uses directly.  That is the two-line
stub in `guest.cc`.

## Sequencing

Initialisation, mirroring what the game's own `sndInit` does:

    alHeapInit    -> give libaudio a heap
    alBnkfNew     -> relocate the instrument bank in place (pointers become absolute)
    alInit        -> build the synthesizer: 24 voices, the game's custom reverb
    alCSPNew      -> create a compact-sequence player
    alCSPSetBank  -> point it at the bank

Per tune:

    inflate the sequence from the ROM's sequence table
    alCSeqNew / alCSPSetSeq / alCSPSetVol (the game's own per-track volume) / alCSPPlay

Per frame:

    alAudioFrame(cmdlist, &len, outbuf, 736)   <- R4300, builds the command list
    write the OSTask descriptor into DMEM
    run the RSP on the ROM's rspboot + aspMain microcode
    take 736 stereo samples out of the output buffer

`outBuf` is passed as a *physical* address: it goes verbatim into the microcode's
save-buffer command, whose top byte is interpreted as a segment id.

## The time model

The sequence player's clock advances by the sample count we ask for, not by any wall
clock.  Each `alAudioFrame` call moves virtual time forward by 736 samples at 22047 Hz,
about 33 ms.  Rendering faster than real time is therefore just calling it in a tighter
loop: the command-line renderer runs flat out (about 19x real time on the interpreter
alone), while the player throttles by keeping a ring buffer full.  Both use the same
engine and produce the same samples.

## What had to be recovered from the ROM

Everything ROM-specific is in `gemusic/ge_addrs.hh`, with the reasoning for each entry:
ten libaudio function addresses, the instrument bank and wavetable offsets, the sequence
table, the microcode and boot-code addresses, the synthesizer configuration, and the
compressed data segment.  The function addresses were found by matching GoldenEye's audio
initialisation against the Perfect Dark decompilation, and by the event-type constant
each small player call posts.

## Why this is faithful

Nothing here reimplements the music.  The sequence player, the voice allocator, the
envelope generator, the synthesis driver and the sample pipeline are all the original
code, executed as machine instructions.  The parts we supply are the ones that carry no
musical meaning: a processor to run on, a heap, a DMA callback, and somewhere to put the
samples.

The sample pipeline is checked directly: all 105 ADPCM wavetables in the music bank
decode bit-exact through the real microcode against an independent reference decoder
(`make test`).
