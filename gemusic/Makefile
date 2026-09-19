# gemusic: render GoldenEye 007 music by running the ROM's own libaudio on the
# interp_mips R4x00 simulator and its audio microcode on a small RSP model.
INTERP ?= $(HOME)/code/interp_mips
# the RSP interpreter (rsp.cc) and the optional LLVM translator live in rsp-bt
RSPBT ?= $(HOME)/code/rsp-bt
SOFTFLOAT ?= $(INTERP)/softfloat/build/Linux-x86_64-GCC/softfloat.a

CXX ?= g++
CXXFLAGS = -std=c++17 -O2 -g -I$(INTERP) -I$(INTERP)/softfloat/source/include -I$(RSPBT)
LIBS = -lz -lboost_program_options -lboost_serialization -lcapstone -lpthread

# everything interp_mips links except its main()
ISS = disassemble helper interpret sparse_mem cache_model sgi_mc sgi_hpc sgi_scc sgi_scsi sgi_seeq saveState gdbstub shmem cosim
ISS_OBJ = $(addprefix obj/iss_,$(addsuffix .o,$(ISS)))
OBJ = obj/gemusic.o obj/guest.o obj/rsp.o

# optional x86 backend: the ROM's audio microcode statically recompiled to AVX-512.
# The translation is derived from the ROM, so it is generated here, never checked in.
ROM ?= ../GoldenEye.z64
# optional: LLVM binary translation of the microcode (make LLVM=1)
ifeq ($(LLVM),1)
LLVM_CONFIG ?= llvm-config
CXXFLAGS += -DGEMUSIC_LLVM
RSPBT_OBJ = $(RSPBT)/obj/rspbt.o $(RSPBT)/obj/rspFunc.o $(RSPBT)/obj/rspInstruction.o
LIBS += $(shell $(LLVM_CONFIG) --ldflags --libs)
endif

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

obj/iss_%.o: $(INTERP)/%.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

obj/aspmain_recomp.cc: rsp2avx512.py $(ROM)
	@mkdir -p obj
	python3 rsp2avx512.py $(ROM) $@

obj/aspmain_recomp.o: obj/aspmain_recomp.cc rsp_avx512.hh $(RSPBT)/rsp.hh
	$(CXX) $(CXXFLAGS) $(AVX512_FLAGS) -I. -c $< -o $@

obj/%.o: %.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) $(AVX512_FLAGS) -MMD -c $< -o $@

tests/adpcm_test: obj/adpcm_test.o obj/guest.o obj/rsp.o $(ISS_OBJ)
	$(CXX) -o $@ $^ $(SOFTFLOAT) $(LIBS)

obj/adpcm_test.o: tests/adpcm_test.cc
	@mkdir -p obj
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

test: tests/adpcm_test
	python3 tests/check_adpcm.py $(ROM)

-include obj/*.d

clean:
	rm -rf obj gemusic tests/adpcm_test
