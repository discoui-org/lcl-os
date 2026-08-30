#include "ipc_probe.hpp"

#include <android/hardware_buffer.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <cstring>

namespace lcl::probe {

IpcProbeResult runAhbIpcProbe() {
    IpcProbeResult result{};

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
        std::cerr << "[Probe IPC] socketpair failed.\n";
        return result;
    }

    int pipeFd[2];
    if (pipe(pipeFd) != 0) {
        close(sv[0]);
        close(sv[1]);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "[Probe IPC] fork failed.\n";
        close(sv[0]);
        close(sv[1]);
        close(pipeFd[0]);
        close(pipeFd[1]);
        return result;
    }

    if (pid == 0) {
        // --- Child Process (Consumer / Compositor simulator) ---
        close(sv[0]);
        close(pipeFd[0]);

        AHardwareBuffer* receivedBuffer = nullptr;
        int recvErr = AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &receivedBuffer);

        uint32_t report[3] = {0, 0, 0}; // [0] = recvValid, [1] = pixelMatch, [2] = pixelHex

        if (recvErr == 0 && receivedBuffer != nullptr) {
            report[0] = 1;
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(receivedBuffer, &desc);

            void* ptr = nullptr;
            int lockErr = AHardwareBuffer_lock(receivedBuffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ptr);
            if (lockErr == 0 && ptr) {
                const auto* p = reinterpret_cast<const uint8_t*>(ptr);
                // Expect (0x12, 0x34, 0x56, 0xFF)
                report[2] = (p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2];
                if (p[0] == 0x12 && p[1] == 0x34 && p[2] == 0x56 && p[3] == 0xFF) {
                    report[1] = 1;
                }
                AHardwareBuffer_unlock(receivedBuffer, nullptr);
            }
            AHardwareBuffer_release(receivedBuffer);
        }

        write(pipeFd[1], report, sizeof(report));
        close(pipeFd[1]);
        close(sv[1]);
        _exit(0);
    } else {
        // --- Parent Process (Producer / Client simulator) ---
        close(sv[1]);
        close(pipeFd[1]);

        AHardwareBuffer_Desc desc{};
        desc.width = 320;
        desc.height = 240;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;

        AHardwareBuffer* producerBuffer = nullptr;
        int allocErr = AHardwareBuffer_allocate(&desc, &producerBuffer);
        if (allocErr == 0 && producerBuffer) {
            void* ptr = nullptr;
            if (AHardwareBuffer_lock(producerBuffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ptr) == 0 && ptr) {
                auto* p = reinterpret_cast<uint8_t*>(ptr);
                p[0] = 0x12; p[1] = 0x34; p[2] = 0x56; p[3] = 0xFF;
                AHardwareBuffer_unlock(producerBuffer, nullptr);
            }

            int sendErr = AHardwareBuffer_sendHandleToUnixSocket(producerBuffer, sv[0]);
            if (sendErr == 0) {
                result.socketTransferSuccess = true;
            }
            AHardwareBuffer_release(producerBuffer);
        }

        uint32_t report[3] = {0, 0, 0};
        read(pipeFd[0], report, sizeof(report));
        close(pipeFd[0]);
        close(sv[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        result.handleReceivedValid = (report[0] == 1);
        result.childImportSuccess = (report[0] == 1);
        result.childPixelMatch = (report[1] == 1);
        result.receivedPixelHex = report[2];
    }

    return result;
}

} // namespace lcl::probe
