#include "lcl-gfxstream/guest_socket_iostream.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>

// This is the sole symbol referenced by the tiny LCL patch applied to an
// out-of-tree gfxstream build overlay.  The upstream code still owns every
// Vulkan encoder and wire opcode; LCL supplies only its authorized IOStream.
extern "C" ::gfxstream::guest::IOStream* lcl_gfxstream_open_socket_iostream() {
    const char* configured = std::getenv("LCL_GPU_SOCKET");
    const std::string endpoint = configured && configured[0]
                                     ? configured
                                     : "/Runtime/lcl-gpu.sock";
    static std::atomic_uint64_t sequence{0};
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint64_t nonce =
        (static_cast<std::uint64_t>(static_cast<unsigned int>(getpid())) << 32U) ^
        ticks ^ sequence.fetch_add(1, std::memory_order_relaxed);
    auto stream = lcl::gfxstream::GuestSocketIOStream::openAuthorized(endpoint, nonce);
    return stream ? stream.release() : nullptr;
}
