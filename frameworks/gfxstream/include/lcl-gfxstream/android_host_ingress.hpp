#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace lcl::gfxstream {

/**
 * Android-only bridge from an already capability-authorized LCL GPU socket to
 * gfxstream's upstream RenderChannel/Vulkan decoder.
 *
 * The socket enters this bridge only after the LCL control handshake has
 * completed.  Its payload is thereafter opaque gfxstream bytes; this class
 * defines no Vulkan command protocol and never exposes host pointers.
 */
class AndroidHostIngress final {
public:
    AndroidHostIngress();
    ~AndroidHostIngress();
    AndroidHostIngress(const AndroidHostIngress&) = delete;
    AndroidHostIngress& operator=(const AndroidHostIngress&) = delete;

    /** Initializes the headless upstream Vulkan decoder exactly once. */
    bool initialize(std::string& error);

    /**
     * Pumps one upgraded SOCK_SEQPACKET client until it disconnects.  The
     * caller retains and closes socketDescriptor.  Each inbound packet is
     * bounded and must not carry file descriptors.
     */
    void serve(int socketDescriptor) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace lcl::gfxstream
