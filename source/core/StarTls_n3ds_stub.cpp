#include <array>
#include <cstddef>

namespace {

// PLACEHOLDER: single-threaded TLS storage for phase-1 N3DS bring-up.
// libctru's archive driver uses __thread path buffers, so returning nullptr
// here makes normal sdmc:/ filesystem calls write near address zero.
alignas(8) std::array<unsigned char, 0x2000> sN3dsMainThreadTls{};

}

extern "C" unsigned int __ctru_heap_size = 40 * 1024 * 1024;
extern "C" unsigned int __ctru_linear_heap_size = 16 * 1024 * 1024;

extern "C" void* __aeabi_read_tp() {
  return sN3dsMainThreadTls.data();
}
