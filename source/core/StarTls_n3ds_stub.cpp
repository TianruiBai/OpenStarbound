#include <array>
#include <cstddef>

namespace {

// PLACEHOLDER: single-threaded TLS storage for phase-1 N3DS bring-up.
// libctru's archive driver uses __thread path buffers, so returning nullptr
// here makes normal sdmc:/ filesystem calls write near address zero.
alignas(8) std::array<unsigned char, 0x2000> sN3dsMainThreadTls{};

}

// New3DS has 256 MB total RAM. With ~20 MB for ELF sections and
// ~10 MB linear heap, allocate 100 MB for the app heap.
// This leaves headroom for GPU framebuffers, RomFS cache, and OS.
// Note: Citra may emulate original 3DS FCRAM layout (64-128MB);
// if the game fails to start, reduce these values.
extern "C" unsigned int __ctru_heap_size = 100 * 1024 * 1024;
extern "C" unsigned int __ctru_linear_heap_size = 10 * 1024 * 1024;

extern "C" void* __aeabi_read_tp() {
  return sN3dsMainThreadTls.data();
}
