#ifndef __GEMUSIC_WASM_BOOST_HASH_SHIM__
#define __GEMUSIC_WASM_BOOST_HASH_SHIM__

/* A stand-in for boost/functional/hash.hpp, for the Emscripten build only.
 *
 * interp_mips's sim_bitvec.hh includes that header for exactly one thing,
 * boost::hash_combine, and boost is not part of emscripten's ports.  Putting the
 * real boost include directory on the command line would drag all of /usr/include
 * in ahead of the wasm sysroot and shadow libc++'s own headers, so instead this
 * provides the one function, with boost's mixing constant and shifts.
 *
 * Native builds use the real header; nothing in this project reads the resulting
 * hash (it exists for checkpoint comparison in interp_mips). */
#include <cstddef>
#include <cstdint>
#include <functional>

namespace boost {
  template <typename T>
  inline void hash_combine(uint64_t &seed, const T &v) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
  }
  template <typename T>
  inline void hash_combine(size_t &seed, const T &v) {
    seed ^= std::hash<T>{}(v) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
  }
}

#endif
