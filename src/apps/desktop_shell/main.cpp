#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#define STB_IMAGE_IMPLEMENTATION
#include "render/stb_image.h"

#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "core/display/display_scale.hpp"

namespace {

void renderWallpaper(uint32_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return;

    std::vector<std::string> candidatePaths = {
        "/usr/share/wallpapers/wallpaper.png",
        "/usr/share/wallpaper.png",
        "/home/user/wallpaper.png",
        "assets/wallpaper.png",
        "wallpaper.png",
        "../wallpaper.png"
    };

    int imgW = 0, imgH = 0, channels = 0;
    unsigned char* imgData = nullptr;
    std::string loadedPath;

    for (const auto& path : candidatePaths) {
        imgData = stbi_load(path.c_str(), &imgW, &imgH, &channels, 4);
        if (imgData) {
            loadedPath = path;
            break;
        }
    }

    if (imgData && imgW > 0 && imgH > 0) {
        std::cout << "[LCL Shell] Loaded wallpaper image from '" << loadedPath
                  << "' (" << imgW << "x" << imgH << " -> " << width << "x" << height << ").\n";

        // Bilinear interpolation scaling to target surface
        for (uint32_t y = 0; y < height; ++y) {
            float v = (static_cast<float>(y) + 0.5f) * (static_cast<float>(imgH) / static_cast<float>(height)) - 0.5f;
            int y0 = std::clamp(static_cast<int>(std::floor(v)), 0, imgH - 1);
            int y1 = std::clamp(y0 + 1, 0, imgH - 1);
            float fy = v - std::floor(v);

            for (uint32_t x = 0; x < width; ++x) {
                float u = (static_cast<float>(x) + 0.5f) * (static_cast<float>(imgW) / static_cast<float>(width)) - 0.5f;
                int x0 = std::clamp(static_cast<int>(std::floor(u)), 0, imgW - 1);
                int x1 = std::clamp(x0 + 1, 0, imgW - 1);
                float fx = u - std::floor(u);

                const unsigned char* p00 = imgData + (y0 * imgW + x0) * 4;
                const unsigned char* p01 = imgData + (y0 * imgW + x1) * 4;
                const unsigned char* p10 = imgData + (y1 * imgW + x0) * 4;
                const unsigned char* p11 = imgData + (y1 * imgW + x1) * 4;

                auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

                float r = lerp(lerp(p00[0], p01[0], fx), lerp(p10[0], p11[0], fx), fy);
                float g = lerp(lerp(p00[1], p01[1], fx), lerp(p10[1], p11[1], fx), fy);
                float b = lerp(lerp(p00[2], p01[2], fx), lerp(p10[2], p11[2], fx), fy);

                uint8_t ru = static_cast<uint8_t>(std::clamp(r, 0.0f, 255.0f));
                uint8_t gu = static_cast<uint8_t>(std::clamp(g, 0.0f, 255.0f));
                uint8_t bu = static_cast<uint8_t>(std::clamp(b, 0.0f, 255.0f));

                pixels[y * width + x] = (0xFF000000) | (ru << 16) | (gu << 8) | bu;
            }
        }

        stbi_image_free(imgData);
        return;
    }

    std::cout << "[LCL Shell] wallpaper.png not found; rendering procedural gradient wallpaper fallback.\n";

    // Fallback: Elegant dark slate & midnight blue linear/radial gradient wallpaper
    // Top-left:  Deep Slate Blue  #0F172A
    // Bottom:    Midnight Indigo  #1E1B4B
    for (uint32_t y = 0; y < height; ++y) {
        float fy = static_cast<float>(y) / static_cast<float>(height);
        for (uint32_t x = 0; x < width; ++x) {
            float fx = static_cast<float>(x) / static_cast<float>(width);

            // Base vertical gradient
            float r = 15.0f * (1.0f - fy) + 30.0f * fy;
            float g = 23.0f * (1.0f - fy) + 27.0f * fy;
            float b = 42.0f * (1.0f - fy) + 75.0f * fy;

            // Soft radial ambient light source at (50%, 35%)
            float cx = fx - 0.5f;
            float cy = fy - 0.35f;
            float dist = std::sqrt(cx * cx + cy * cy);
            float glow = std::max(0.0f, 1.0f - dist * 1.6f);

            r = std::min(255.0f, r + glow * 20.0f);
            g = std::min(255.0f, g + glow * 35.0f);
            b = std::min(255.0f, b + glow * 60.0f);

            uint8_t ru = static_cast<uint8_t>(r);
            uint8_t gu = static_cast<uint8_t>(g);
            uint8_t bu = static_cast<uint8_t>(b);

            pixels[y * width + x] = (0xFF000000) | (ru << 16) | (gu << 8) | bu;
        }
    }
}

} // namespace

int main() {
    std::cout << "====================================================\n"
              << "  lcl-desktop-shell v0.1.0 - Desktop Shell Daemon  \n"
              << "====================================================\n";

    // 1. Connect to Compositor Unix Domain Socket
    int socketFd = -1;
    for (int i = 0; i < 50; ++i) {
        socketFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socketFd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, lcl::core::kCompositorSocket, sizeof(addr.sun_path) - 1);
            if (connect(socketFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            close(socketFd);
            socketFd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (socketFd < 0) {
        std::cerr << "[LCL Shell ERROR] Could not connect to compositor socket: " << lcl::core::kCompositorSocket << "\n";
        return 1;
    }

    // Set non-blocking socket
    int flags = fcntl(socketFd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(socketFd, F_SETFL, flags | O_NONBLOCK);
    }

    std::cout << "[LCL Shell] Connected to Compositor IPC socket successfully.\n";

    // 2. Register Role as DesktopWallpaper
    lcl::protocol::LCLHeader regHeader{};
    regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
    regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

    lcl::protocol::LCLMsgRegisterRole regMsg{};
    regMsg.role = lcl::protocol::LCLRole::DesktopWallpaper;
    std::strncpy(regMsg.clientName, "lcl-desktop-shell", sizeof(regMsg.clientName) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, regHeader, &regMsg);

    // 3. Request Full-Screen Surface Creation (0,0 = Fullscreen)
    uint32_t width = 1280;
    uint32_t height = 800;

    lcl::protocol::LCLHeader surfHeader{};
    surfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    surfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate surfMsg{};
    surfMsg.surfaceId = 1;
    surfMsg.x = 0;
    surfMsg.y = 0;
    surfMsg.width = 0;  // 0 = request full screen width from Compositor
    surfMsg.height = 0; // 0 = request full screen height from Compositor
    std::strncpy(surfMsg.title, "LCL Wallpaper", sizeof(surfMsg.title) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, surfHeader, &surfMsg);

    // 4. Set Decoration Mode to None (Frameless)
    lcl::protocol::LCLHeader decHeader{};
    decHeader.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    decHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode decMsg{};
    decMsg.surfaceId = 1;
    decMsg.mode = lcl::protocol::LCLDecorationMode::None;

    lcl::protocol::sendMsgWithFd(socketFd, decHeader, &decMsg);

    // 5. Set Window Layer to BOTTOM and unfocusable = 1
    lcl::protocol::LCLHeader layerHeader{};
    layerHeader.opcode = lcl::protocol::LCLOpcode::SetWindowLayer;
    layerHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowLayer);

    lcl::protocol::LCLMsgSetWindowLayer layerMsg{};
    layerMsg.surfaceId = 1;
    layerMsg.layer = lcl::protocol::LCLWindowLayer::Bottom;
    layerMsg.unfocusable = 1;

    lcl::protocol::sendMsgWithFd(socketFd, layerHeader, &layerMsg);

    // Read initial ConfigureBounds from Compositor (up to 20ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;
        while (lcl::protocol::recvMsgWithFd(socketFd, header, payload, receivedFd)) {
            if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->width > 0 && cfg->height > 0) {
                    width = cfg->width;
                    height = cfg->height;
                }
            }
        }
    }

    // 6. Create SHM Buffer and Render Wallpaper
    size_t shmSize = static_cast<size_t>(width) * height * 4;
    int shmFd = memfd_create("lcl_wallpaper_shm", MFD_CLOEXEC);
    if (shmFd < 0) {
        std::cerr << "[LCL Shell ERROR] memfd_create failed: " << strerror(errno) << "\n";
        close(socketFd);
        return 1;
    }

    if (ftruncate(shmFd, shmSize) < 0) {
        std::cerr << "[LCL Shell ERROR] ftruncate failed: " << strerror(errno) << "\n";
        close(shmFd);
        close(socketFd);
        return 1;
    }

    uint32_t* shmPixels = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));
    if (shmPixels == MAP_FAILED) {
        std::cerr << "[LCL Shell ERROR] mmap failed: " << strerror(errno) << "\n";
        close(shmFd);
        close(socketFd);
        return 1;
    }

    renderWallpaper(shmPixels, width, height);

    // 7. Attach Buffer to Compositor
    lcl::protocol::LCLHeader attachHeader{};
    attachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
    attachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

    lcl::protocol::LCLMsgAttachBuffer attachMsg{};
    attachMsg.surfaceId = 1;
    attachMsg.width = width;
    attachMsg.height = height;
    attachMsg.stride = width * 4;
    attachMsg.format = 1;

    lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, shmFd);
    std::cout << "[LCL Shell] Wallpaper surface attached (" << width << "x" << height << ") at LAYER_BOTTOM.\n";

    // 8. Event Loop
    bool running = true;
    while (running) {
        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;

        while (lcl::protocol::recvMsgWithFd(socketFd, header, payload, receivedFd)) {
            if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->width > 0 && cfg->height > 0 && (cfg->width != width || cfg->height != height)) {
                    width = cfg->width;
                    height = cfg->height;

                    munmap(shmPixels, shmSize);
                    shmSize = static_cast<size_t>(width) * height * 4;
                    ftruncate(shmFd, shmSize);
                    shmPixels = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0));

                    if (shmPixels != MAP_FAILED) {
                        renderWallpaper(shmPixels, width, height);
                        attachMsg.width = width;
                        attachMsg.height = height;
                        attachMsg.stride = width * 4;
                        lcl::protocol::sendMsgWithFd(socketFd, attachHeader, &attachMsg, shmFd);
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                running = false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (shmPixels != MAP_FAILED) munmap(shmPixels, shmSize);
    if (shmFd >= 0) close(shmFd);
    if (socketFd >= 0) close(socketFd);

    return 0;
}
