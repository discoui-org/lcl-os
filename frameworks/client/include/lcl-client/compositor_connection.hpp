#pragma once

#include "system/ipc/lcl_protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcl::client {

/**
 * Low-level non-blocking compositor transport used by legacy retained
 * producers. New toolkit integrations should normally use SurfaceClient.
 */
class CompositorConnection {
public:
    enum class Ownership { Borrowed, Owned };

    CompositorConnection() = default;
    ~CompositorConnection();
    CompositorConnection(const CompositorConnection&) = delete;
    CompositorConnection& operator=(const CompositorConnection&) = delete;

    bool connect(const std::string& socketPath, uint32_t attempts = 1,
                 uint32_t retryDelayMs = 0);
    bool adopt(int socketFd, Ownership ownership = Ownership::Borrowed);
    void disconnect() noexcept;
    int fd() const noexcept { return m_fd; }
    bool connected() const noexcept { return m_fd >= 0; }

    bool send(protocol::LCLOpcode opcode, uint32_t requestId,
              const void* payload, uint32_t payloadSize,
              int passedFd = -1);
    protocol::ReceiveStatus receive(protocol::LCLHeader& header,
                                    std::vector<uint8_t>& payload,
                                    int& receivedFd);

private:
    int m_fd{-1};
    bool m_owned{false};
};

} // namespace lcl::client

