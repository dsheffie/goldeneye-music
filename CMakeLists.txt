# goldeneye-music
#
# Everything optional is detected, reported at configure time, and simply left out
# when missing.  The only hard requirements are a C++17 compiler, zlib, Boost and
# the two sibling checkouts (interp_mips for the R4300 core, rsp-bt for the RSP).
cmake_minimum_required(VERSION 3.20)
project(goldeneye_music C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- sibling trees

set(INTERP_MIPS "$ENV{HOME}/code/interp_mips" CACHE PATH "interp_mips checkout (the R4300 core)")
set(RSPBT "$ENV{HOME}/code/rsp-bt" CACHE PATH "rsp-bt checkout (the RSP interpreter and translator)")

foreach(d IN ITEMS "${INTERP_MIPS}" "${RSPBT}")
  if(NOT EXISTS "${d}")
    message(FATAL_ERROR "not found: ${d}\n  set -DINTERP_MIPS=... and -DRSPBT=... to your checkouts")
  endif()
endforeach()

# SoftFloat builds into a target-named directory; take whichever one exists.
file(GLOB SOFTFLOAT_LIB "${INTERP_MIPS}/softfloat/build/*/softfloat.a")
list(GET SOFTFLOAT_LIB 0 SOFTFLOAT_LIB)
if(NOT SOFTFLOAT_LIB)
  message(FATAL_ERROR "SoftFloat is not built: run make in ${INTERP_MIPS}/softfloat/build/<target>")
endif()

# ---------------------------------------------------------------- dependencies

find_package(ZLIB REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options serialization)

find_library(CAPSTONE_LIB capstone REQUIRED)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 QUIET sdl2)
endif()
if(NOT SDL2_FOUND)
  find_package(SDL2 QUIET)
endif()

# The translator uses llvm::CodeGenOptLevel and the LLJIT/PassBuilder shape, which is
# LLVM 18 and later.  Older LLVMs are ignored rather than failing the build.
#
# LLVM's config package declares itself compatible only with an exact version, so
# find_package(LLVM 18) never matches LLVM 19 or 20 -- the version has to be checked
# afterwards.  Debian and Homebrew also install versions side by side without always
# leaving one in cmake's search path, so walk the candidates newest-first.
set(LLVM_MIN 18)

# a macro, not a function: find_package sets a pile of LLVM_* variables and a function
# would keep every one of them except what we forward in its own scope
macro(try_llvm dir)
  if(NOT LLVM_FOUND)
    if(NOT "${dir}" STREQUAL "")
      set(LLVM_DIR "${dir}" CACHE PATH "" FORCE)
    endif()
    find_package(LLVM CONFIG QUIET)
    if(LLVM_FOUND AND LLVM_PACKAGE_VERSION VERSION_LESS ${LLVM_MIN})
      set(LLVM_TOO_OLD "${LLVM_PACKAGE_VERSION}")
      set(LLVM_FOUND FALSE)
    endif()
  endif()
endmacro()

if(NOT LLVM_DIR)
  file(GLOB LLVM_CANDIDATES
       /usr/lib/llvm-*/lib/cmake/llvm /usr/lib/llvm-*/cmake
       /usr/local/opt/llvm*/lib/cmake/llvm /opt/homebrew/opt/llvm*/lib/cmake/llvm)
  list(SORT LLVM_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
  foreach(d IN LISTS LLVM_CANDIDATES)
    try_llvm("${d}")
  endforeach()
  if(NOT LLVM_FOUND)
    unset(LLVM_DIR CACHE)
  endif()
endif()
try_llvm("")
if(NOT LLVM_FOUND AND LLVM_TOO_OLD)
  message(STATUS "LLVM ${LLVM_TOO_OLD} is too old (need ${LLVM_MIN}+); JIT disabled")
endif()

option(ENABLE_JIT "RSP binary translation via LLVM" ${LLVM_FOUND})
if(ENABLE_JIT AND NOT LLVM_FOUND)
  message(FATAL_ERROR "ENABLE_JIT was requested but no LLVM ${LLVM_MIN}+ was found")
endif()

set(ROM "" CACHE FILEPATH "a GoldenEye 007 (NGEE) ROM; only needed to run the tests")

# ---------------------------------------------------------------- the shared core

# interp_mips is an SGI Indy simulator; iss_stubs.cc lets us link only its CPU core.
add_library(iss_core STATIC
  ${INTERP_MIPS}/interpret.cc ${INTERP_MIPS}/sparse_mem.cc ${INTERP_MIPS}/helper.cc
  ${INTERP_MIPS}/disassemble.cc ${INTERP_MIPS}/saveState.cc
  gemusic/iss_stubs.cc)
target_include_directories(iss_core PUBLIC ${INTERP_MIPS} ${INTERP_MIPS}/softfloat/source/include)
target_link_libraries(iss_core PUBLIC ${SOFTFLOAT_LIB} ${CAPSTONE_LIB}
  Boost::program_options Boost::serialization ZLIB::ZLIB)
if(APPLE)
  target_compile_definitions(iss_core PUBLIC MAP_NORESERVE=0)   # macOS has no such flag; it is only a hint
endif()

add_library(gengine STATIC gemusic/engine.cc gemusic/guest.cc ${RSPBT}/rsp.cc)
target_include_directories(gengine PUBLIC gemusic ${RSPBT})
target_link_libraries(gengine PUBLIC iss_core)

if(ENABLE_JIT)
  target_sources(gengine PRIVATE ${RSPBT}/rspbt.cc ${RSPBT}/rspFunc.cc ${RSPBT}/rspInstruction.cc)
  target_compile_definitions(gengine PUBLIC GEMUSIC_LLVM)
  target_include_directories(gengine SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_definitions(gengine PRIVATE ${LLVM_DEFINITIONS_LIST})
  if(LLVM_LINK_LLVM_DYLIB)
    target_link_libraries(gengine PUBLIC LLVM)
  else()
    llvm_map_components_to_libnames(LLVM_LIBS core orcjit native passes)
    target_link_libraries(gengine PUBLIC ${LLVM_LIBS})
  endif()
endif()

# ---------------------------------------------------------------- executables

add_executable(gemusic gemusic/gemusic.cc)
target_link_libraries(gemusic PRIVATE gengine)

if(SDL2_FOUND)
  add_executable(gemms gemusic/gemms.cc)
  target_link_libraries(gemms PRIVATE gengine)
  if(SDL2_LIBRARIES AND SDL2_INCLUDE_DIRS)          # pkg-config
    target_include_directories(gemms SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gemms PRIVATE ${SDL2_LIBRARIES})
    target_link_directories(gemms PRIVATE ${SDL2_LIBRARY_DIRS})
  else()                                            # SDL2's own config package
    target_link_libraries(gemms PRIVATE SDL2::SDL2)
  endif()
endif()

# ---------------------------------------------------------------- tests

if(ROM)
  enable_testing()
  add_executable(adpcm_test gemusic/tests/adpcm_test.cc)
  target_link_libraries(adpcm_test PRIVATE gengine)
  add_test(NAME adpcm_bit_exact
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/gemusic/tests/check_adpcm.py ${ROM})
  set_tests_properties(adpcm_bit_exact PROPERTIES
    ENVIRONMENT "ADPCM_TEST=$<TARGET_FILE:adpcm_test>")
endif()

# ---------------------------------------------------------------- summary

message(STATUS "")
message(STATUS "goldeneye-music")
message(STATUS "  interp_mips ....... ${INTERP_MIPS}")
message(STATUS "  rsp-bt ............ ${RSPBT}")
if(ENABLE_JIT)
  message(STATUS "  RSP JIT ........... LLVM ${LLVM_PACKAGE_VERSION}")
else()
  message(STATUS "  RSP JIT ........... no (needs LLVM ${LLVM_MIN}+); interpreter only")
endif()
if(SDL2_FOUND)
  message(STATUS "  gemms player ...... yes")
else()
  message(STATUS "  gemms player ...... no (SDL2 not found)")
endif()
message(STATUS "")
