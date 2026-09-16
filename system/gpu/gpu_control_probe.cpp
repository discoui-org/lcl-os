#include "system/ipc/gpu_protocol.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

int connectToGpud(const std::string& path) {
    if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
    const int socketFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (socketFd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (connect(socketFd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(socketFd);
        return -1;
    }
    return socketFd;
}

bool waitForReply(int socketFd, lcl::gpu_protocol::Header& header,
                  std::vector<uint8_t>& payload) {
    pollfd ready{socketFd, POLLIN, 0};
    int result = -1;
    do {
        result = poll(&ready, 1, 3'000);
    } while (result < 0 && errno == EINTR);
    return result > 0 && (ready.revents & POLLIN) != 0 &&
        lcl::gpu_protocol::receivePacket(socketFd, header, payload) ==
            lcl::gpu_protocol::ReceiveStatus::Received;
}

} // namespace

int main(int argc, char** argv) {
    std::string socketPath = "/Runtime/lcl-gpu.sock";
    if (argc == 3 && std::string(argv[1]) == "--socket") {
        socketPath = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: lcl-gpu-control-probe [--socket PATH]\n";
        return 2;
    }

    const int socketFd = connectToGpud(socketPath);
    if (socketFd < 0) {
        std::cerr << "could not connect to " << socketPath << ": "
                  << std::strerror(errno) << '\n';
        return 1;
    }
    lcl::gpu_protocol::Hello hello{};
    hello.nonce = 0x4c434c4750550001ull;
    if (!lcl::gpu_protocol::sendPacket(socketFd, lcl::gpu_protocol::Opcode::Hello,
                                       &hello, sizeof(hello))) {
        std::cerr << "could not send lcl-gpu hello\n";
        close(socketFd);
        return 1;
    }

    lcl::gpu_protocol::Header header{};
    std::vector<uint8_t> payload;
    if (!waitForReply(socketFd, header, payload)) {
        std::cerr << "lcl-gpud did not return a capability reply\n";
        close(socketFd);
        return 1;
    }
    const auto* device = lcl::gpu_protocol::payloadAs<lcl::gpu_protocol::DeviceInfo>(
        header, payload, lcl::gpu_protocol::Opcode::DeviceInfo);
    if (!device || device->maxImageDimension2D == 0) {
        std::cerr << "lcl-gpud returned an invalid capability reply\n";
        close(socketFd);
        return 1;
    }
    std::cout << "api=" << (device->vulkanApiVersion >> 22) << '.'
              << ((device->vulkanApiVersion >> 12) & 0x3ff)
              << " vendor=" << device->vendorId
              << " device=" << device->deviceId
              << " maxImage2D=" << device->maxImageDimension2D
              << " ahb=" << ((device->flags & lcl::gpu_protocol::
                                  kDeviceSupportsAndroidHardwareBuffer) != 0)
              << " externalFenceFd=" << ((device->flags & lcl::gpu_protocol::
                                             kDeviceSupportsExternalFenceFd) != 0)
              << " name=" << device->deviceName << '\n';
    close(socketFd);
    return 0;
}
