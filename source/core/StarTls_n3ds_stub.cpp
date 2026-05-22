#include <array>
#include <cstddef>

namespace {

// PLACEHOLDER: single-threaded TLS storage for phase-1 N3DS bring-up.
// libctru's archive driver uses __thread path buffers, so returning nullptr
// here makes normal sdmc:/ filesystem calls write near address zero.
alignas(8) std::array<unsigned char, 0x2000> sN3dsMainThreadTls{};

}

// New3DS has 256 MB total RAM, but Citra currently emulates the original
// 3DS 64 MB FCRAM layout.  The ELF .text + .rodata + .data + .bss uses
// ~20 MB.  Keep the linear heap small (8 MB) since the texture upload
// budget is only 6 MB, leaving ~36 MB for the application heap which is
// needed for the 900 MB packed.pak index + 50K descriptor database.
extern "C" unsigned int __ctru_heap_size = 64 * 1024 * 1024;
extern "C" unsigned int __ctru_linear_heap_size = 8 * 1024 * 1024;

extern "C" void* __aeabi_read_tp() {
  return sN3dsMainThreadTls.data();
}
