# Theory of operation

How this project plays GoldenEye 007's music without booting the game, without an
operating system, and without emulating a Nintendo 64.

## The idea

A Nintendo 64 game's music is produced by two processors working together.  The main
CPU (an R4300, MIPS III) runs the audio part of libultra, Nintendo's library for the
machine -- "libaudio" below, the two names mean the same code.  It walks the sequence
data, allocates voices, computes envelopes and pitches, and emits a list of commands.

The RSP executes that list.  It is a small MIPS integer core with a vector unit,
sitting inside the RCP (the console's second chip, which also holds the rasteriser and
the interfaces to memory and the outside world).  The RSP has no cache: it runs out of
4 KB of instruction memory, IMEM, against 4 KB of data memory, DMEM, both of which are
filled by DMA before it starts.  The program it runs is called microcode, but it is
ordinary MIPS machine code for that core, and it is what actually decodes, resamples,
mixes and reverberates the samples.  The audio interface then sends the finished buffer
to the digital-to-analogue converter.

The load-bearing observation is that **libultra's audio library is a synchronous,
self-contained subroutine**.  `alAudioFrame` takes a command buffer, an output buffer and
a sample count; it walks its internal event queue, updates voice state, writes a command
list, and returns.  It never blocks, never yields, and asks the operating system for
exactly one service.  The game's audio thread is a thin loop around it.

So we do not need the game.  We need the library, and something to run it on.

### What is simulated, and what is not

Both of the console's processors are simulated: the R4300 by `interp_mips`, the RSP by
`rsp-bt`.  Everything else -- the heap we hand libaudio, the buffers, the file we write
or the sound the player makes -- is ordinary C++ compiled for whatever machine you are
running on, and runs at full speed.

The two live in different address spaces, so an R4300 address means nothing to the
native code until it is translated; that is what `r4300_t::ptr()` does.

## What the music actually is

Before any of the machinery, it helps to know what is being played.  There is no
recorded audio in the ROM.  The music is **wavetable synthesis driven by a sequencer**,
much like a tracker or a hardware sampler: short recordings of instruments, pitch-shifted
and shaped in real time, arranged by a note list.  The numbers below are from this ROM.

### The notes: compact MIDI

63 sequences, each a stream of MIDI-like events on up to 16 tracks, with 384 ticks to a
quarter note.  They are not standard MIDI files.  libultra's "compact" format changes
three things:

- a note-on carries its own **duration**, so there are no separate note-offs;
- repeated passages are coded as a **back-reference** -- an escape byte followed by a
  distance and a length, replaying bytes from earlier in the same track, which is
  ordinary dictionary compression applied to note data;
- loops are **in-band markers** rather than a convention, including infinite ones, which
  is how a level theme repeats forever.

Each sequence is then deflate-compressed by Rare's own wrapper.  All 63 come to 123 KB
compressed, 201 KB expanded.

### The instruments: a bank

One bank of 75 instruments, sharing 138 "sounds" over 106 samples.  An instrument is not
a single recording; it is a small structure describing how to play one:

- a **keymap** per sound giving the key and velocity range it covers, its root key and a
  detune in cents.  An instrument can hold up to 12 sounds split across the keyboard, so
  a piano-like instrument uses different recordings low and high rather than stretching
  one a long way.  63 of the sounds cover a single key each (drums and one-shots), 53
  cover the whole range, and 46 are detuned.
- an **envelope**: attack, decay and release times in microseconds with two volume
  levels.  Attacks here run from instant to 0.79 s, releases from 1 ms to 0.25 s.
- **volume, pan, priority** (which voice gets stolen when they run out) and a pitch-bend
  range; four instruments also carry tremolo or vibrato settings.

### The samples: VADPCM

106 wavetables, all but one compressed with VADPCM, Nintendo's four-bit ADPCM.  It codes
audio in 9-byte frames of 16 samples: one header byte holding a shift and a codebook
index, then 16 nibbles.  Each nibble is sign-extended, shifted, and added to a prediction
made from the previous two samples using a per-instrument codebook -- order 2 everywhere
here, with either 1 or 4 predictor sets to choose between per frame.  That is 3.56x
smaller than 16-bit audio: 386 KB in the ROM, 1375 KB decoded.

49 of the samples loop, and each loop point stores the decoder's saved state -- the two
samples it must resume predicting from -- so a loop can restart mid-stream without a
click.

### Playing them: what the RSP does per voice

Nominal rate is 22050 Hz, 24 voices at once.  For each sounding voice, every frame:

1. **decode** the ADPCM, continuing from the previous frame's predictor state;
2. **resample** to change pitch, interpolating with a 4-tap filter at a fractional
   read position -- this is what turns one recording into every note of a scale, the
   ratio coming from the key's distance above the sample's root, in cents;
3. **ramp volume and pan** towards their targets, which is how envelopes are applied:
   the CPU computes a target and a rate, and the mixer walks towards it;
4. **mix** into a dry and a wet bus, the wet one feeding a reverb built from delay lines
   with feedback.  GoldenEye uses a custom reverb setting with its own parameter table
   rather than one of the library's presets.

The split of labour is worth noting: the CPU decides *what* should sound and how loud,
and emits a list of commands; the RSP does every sample-touching step above.  That is why
the two halves of this program exist.

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
| Audio thread, scheduler, message queues | direct calls on one ordinary thread |
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
million RSP instructions -- but those two numbers are not comparable, and it is worth
saying why.  **55% of the RSP instructions executed here are vector ops**: 32% vector
ALU, each doing eight 16-bit lanes with a 48-bit accumulator, and 23% vector loads and
stores moving 16 bytes apiece.  Counting one `vmulf` as "one instruction" beside one
scalar `addu` understates it by a factor of eight.  Measured in element operations rather
than instructions, the RSP is doing roughly **13x** the work of the R4300, not the 4x the
instruction counts suggest -- which is why it dominates the run time.

Instructions per second is a poor headline for this program generally, since the two
processors have different ISAs and the RSP's are so much wider.  The honest measure is
how much audio comes out per second of wall clock: see the timings below.

## The fake machine

`r4300_t` (`gemusic/r4300.cc`) is a Nintendo 64 with almost everything removed:

- **Memory.**  `sparse_mem`'s flat 4 GiB backing store.  Into it go the ROM's boot code
  (raw at ROM offset 0x1000) and its data segment, which the game stores as a compressed
  block and inflates in place at 0x80020d90 -- the audio microcode and all initialised
  data live inside it.  Our own structures (heap, bank, command list, output buffer) are
  placed above them at addresses chosen in `gemusic/r4300.hh`.
- **`Status` = 0x30000000.**  Kernel mode, coprocessor 1 enabled, `FR` clear, interrupts
  off.  `FR` matters: the compiler built double constants by writing halves to an
  odd/even register pair, which only works in `FR=0`.  The PIF boot ROM leaves `FR` set,
  but libultra threads take their `Status` from the thread context, which never sets it.
- **One TLB entry.**  GoldenEye's first act is to map a 4 MB page, virtual 0x70000000
  onto physical 0, and to run from there; the function pointers in its data point into
  that alias.  We install the same entry directly.  The R4300 code never writes the TLB
  afterwards -- across 30 seconds of audio it executes zero `tlbwr`, `tlbwi`, `tlbr` and
  `tlbp` -- but 98.7% of instruction fetches still translate through that one entry.
- **Eight instructions of synthetic MIPS** implementing the one OS service, below.

## Calling into the ROM's code

Calling an R4300 function means handing the simulator a starting address and some
arguments, then stepping it until that function returns.  `r4300_t::call(fn, a0..a3)`
is the whole interface:

1. put the arguments in `a0`-`a3`;
2. set `sp` to a scratch area and `ra` to a magic address in unmapped space;
3. set `pc` to the target;
4. step the simulator until `pc` reaches the magic address;
5. return `v0`.

The o32 ABI (the calling convention this code was compiled for) passes the first four
arguments in registers, which covers every function we call.  If the R4300 wanders
outside the loaded image -- the signature of an unexpected exception -- that is
reported and aborts rather than running on.

## The one OS service

libaudio obtains sample data through a DMA callback installed at initialisation.  On the
console it fetches from cartridge ROM on demand.  Here the entire wavetable is already
resident, so `dmaNew` returns a `dmaCall` that masks its argument to a physical address
and returns it, which is what the microcode then uses directly.  That is the two-line
stub in `r4300.cc`.

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
