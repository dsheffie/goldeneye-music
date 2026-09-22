# gemusic: render GoldenEye 007 music by running the ROM's own libaudio on the
# interp_mips R4x00 simulator and its audio microcode on a small RSP model.
INTERP ?= $(HOME)/code/vr4300
# the RSP interpreter (rsp.cc) and the optional LLVM translator live in rsp-bt
RSPBT ?= $(HOME)/code/rsp-bt
# whichever target directory SoftFloat's own Makefile produced
SOFTFLOAT ?= $(firstword $(wildcard $(INTERP)/softfloat/build/*/softfloat.a))

UNAME_S = $(shell uname -s)
UNAME_M = $(shell uname -m)

CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -g -I$(INTERP) -I$(INTERP)/softfloat/source/include -I$(RSPBT)
LIBS = -lz -lboost_program_options -lboost_serialization -lcapstone -lpthread

ifeq ($(UNAME_S),Darwin)
# Homebrew (/opt/homebrew on Apple silicon, /usr/local on Intel) or MacPorts
BREW ?= $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW),)
CXXFLAGS += -I$(BREW)/include
LIBS += -L$(BREW)/lib
endif
# macOS has no MAP_NORESERVE; it is only a hint, so zero is the right stand-in
CXXFLAGS += -DMAP_NORESERVE=0
endif

# interp_mips is an SGI Indy simulator; we want only its R4x00 CPU core.  iss_stubs.cc
# supplies the handful of device / instrumentation symbols the core still references
# so the Indy machine (including the Linux-only sgi_seeq.cc) stays out of the link.
ISS = helper interpret sparse_mem n64_rcp
ISS_CORE = $(addprefix obj/iss_,$(addsuffix .o,$(ISS)))
ISS_OBJ = $(ISS_CORE) obj/iss_stubs.o
OBJ = obj/gemusic.o obj/engine.o obj/r4300.o obj/rsp.o obj/rcp_bridge.o

# optional x86 backend: the ROM's audio microcode statically recompiled to AVX-512.
# The translation is derived from the ROM, so it is generated here, never checked in.
ROM ?= ../GoldenEye.z64
# The feature switches below change CXXFLAGS but not any source file, so make on its own
# would happily link stale objects built with different flags -- which silently produces a
# binary that ignores the feature you just asked for.  Wipe obj/ when the set changes.
CONFIG_SIG := LLVM=$(LLVM)|AVX512=$(AVX512)|R4300BT=$(R4300BT)
ifneq ($(CONFIG_SIG),$(shell cat obj/.config 2>/dev/null))
$(shell rm -rf obj && mkdir -p obj && echo '$(CONFIG_SIG)' > obj/.config)
endif

# optional: LLVM binary translation of the microcode (make LLVM=1)
ifeq ($(LLVM),1)
LLVM_CONFIG ?= llvm-config
CXXFLAGS += -DGEMUSIC_LLVM
RSPBT_OBJ = $(RSPBT)/obj/rspbt.o $(RSPBT)/obj/rspFunc.o $(RSPBT)/obj/rspInstruction.o
LIBS += $(shell $(LLVM_CONFIG) --ldflags --libs)
endif

# optional: LLVM binary translation of the R4300 audio path (make R4300BT=1).
# Pulls in the LLVM libraries whether or not the RSP translator is built.
ifeq ($(R4300BT),1)
LLVM_CONFIG ?= llvm-config
CXXFLAGS += -DGEMUSIC_R4300BT
LLVM_CXXFLAGS = $(filter-out -std=%,$(shell $(LLVM_CONFIG) --cxxflags))
OBJ += obj/r4300bt.o obj/r4300cfg.o
ifneq ($(LLVM),1)
LIBS += $(shell $(LLVM_CONFIG) --ldflags --libs)
endif
endif

# x86-64 only; on Apple silicon use the default interpreter or LLVM backend
ifeq ($(AVX512),1)
CXXFLAGS += -DGEMUSIC_AVX512
OBJ += obj/aspmain_recomp.o
AVX512_FLAGS = -mavx512f -mavx512bw -mavx512dq -mavx512vl
endif

all: gemusic

gemusic: $(OBJ) $(ISS_OBJ) $(RSPBT_OBJ)
	$(CXX) -o $@ $(OBJ) $(ISS_OBJ) $(RSPBT_OBJ) $(SOFTFLOAT) $(LIBS)

$(RSPBT)/obj/%.o: $(RSPBT)/%.cc
	$(MAKE) -C $(RSPBT)

obj/rsp.o: $(RSPBT)/rsp.cc $(RSPBT)/rsp.hh
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(AVX512_FLAGS) -c $< -o $@

obj/iss_stubs.o: iss_stubs.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

obj/iss_%.o: $(INTERP)/%.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

# gemms: the XMMS-style live player (needs SDL2).  Interpreter backend only, so it has
# no LLVM or AVX-512 requirement.  make gemms
GEMMS_OBJ = obj/gemms.o obj/engine_plain.o obj/r4300.o obj/rsp.o

# sdl2-config if present, otherwise pkg-config
SDL_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2)
SDL_LIBS ?= $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2)

gemms: $(GEMMS_OBJ) $(ISS_OBJ)
	$(CXX) -o $@ $(GEMMS_OBJ) $(ISS_OBJ) $(SOFTFLOAT) $(LIBS) $(SDL_LIBS)

obj/gemms.o: gemms.cc engine.hh pixfont.hh
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(SDL_CFLAGS) -c $< -o $@

obj/engine_plain.o: engine.cc engine.hh
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

obj/aspmain_recomp.cc: rsp2avx512.py $(ROM)
	@mkdir -p obj
	python3 rsp2avx512.py $(ROM) $@

obj/aspmain_recomp.o: obj/aspmain_recomp.cc rsp_avx512.hh $(RSPBT)/rsp.hh
	$(CXX) $(CXXFLAGS) $(AVX512_FLAGS) -I. -c $< -o $@

obj/%.o: %.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(AVX512_FLAGS) -MMD -c $< -o $@

tests/r4300_indirect: obj/r4300_indirect.o obj/engine.o obj/r4300.o obj/rsp.o $(ISS_OBJ)
	$(CXX) -o $@ $^ $(SOFTFLOAT) $(LIBS)

obj/r4300_indirect.o: tests/r4300_indirect.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -I. -MMD -c $< -o $@

# static function discovery + CFG dump.  A dev tool, not part of the renderer: it links
# the real disassembler so the graphs carry readable code, which pulls in capstone.
tests/r4300_cfg: obj/r4300_cfg.o obj/r4300cfg.o obj/engine.o obj/r4300.o obj/rsp.o \
		 $(ISS_CORE) obj/iss_stubs_rd.o obj/iss_disassemble.o
	$(CXX) -o $@ $^ $(SOFTFLOAT) $(LIBS) -lcapstone

obj/r4300_cfg.o: tests/r4300_cfg.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -I. -MMD -c $< -o $@

obj/r4300cfg.o: $(RSPBT)/r4300cfg.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

obj/r4300bt.o: $(RSPBT)/r4300bt.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) -I. -MMD -c $< -o $@

obj/iss_stubs_rd.o: iss_stubs.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -DGEMUSIC_REAL_DISASM -MMD -c $< -o $@

# boots an ordinary N64 ROM, to exercise the RCP register block
tests/n64_boot: obj/n64_boot.o obj/rcp_bridge.o obj/r4300.o obj/rsp.o $(ISS_OBJ)
	$(CXX) -o $@ $^ $(SOFTFLOAT) $(LIBS)

obj/n64_boot.o: tests/n64_boot.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -I. -MMD -c $< -o $@

tests/adpcm_test: obj/adpcm_test.o obj/r4300.o obj/rsp.o $(ISS_OBJ)
	$(CXX) -o $@ $^ $(SOFTFLOAT) $(LIBS)

obj/adpcm_test.o: tests/adpcm_test.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

test: tests/adpcm_test
	python3 tests/check_adpcm.py $(ROM)

-include obj/*.d

clean:
	rm -rf obj gemusic gemms tests/adpcm_test
