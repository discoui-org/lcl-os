#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <ctime>
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
#include "render/font_renderer.hpp"

namespace {

struct AsyncWallpaperTask {
    std::atomic<bool> ready{false};
    std::vector<uint32_t> pixels;
    uint32_t w{0};
    uint32_t h{0};
};

std::string getFormattedTime() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&tt, &tm_buf);

    int hour12 = tm_buf.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (tm_buf.tm_hour >= 12) ? "PM" : "AM";

    char datePart[32];
    std::strftime(datePart, sizeof(datePart), "%a %b %e", &tm_buf);

    std::string dateStr = datePart;
    size_t doubleSpace = dateStr.find("  ");
    if (doubleSpace != std::string::npos) {
        dateStr.replace(doubleSpace, 2, " ");
    }

    char timeBuf[64];
    std::snprintf(timeBuf, sizeof(timeBuf), "%s %d:%02d %s", dateStr.c_str(), hour12, tm_buf.tm_min, ampm);
    return std::string(timeBuf);
}

void renderWallpaper(uint32_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels || width == 0 || height == 0) return;

    std::vector<std::string> candidatePaths = {
        "/usr/share/wallpapers/wallpaper.jpg",
        "/usr/share/wallpaper.jpg",
        "/home/user/wallpaper.jpg",
        "assets/wallpaper.jpg",
        "wallpaper.jpg",
        "../wallpaper.jpg"
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

    std::cout << "[LCL Shell] wallpaper.jpg not found; rendering procedural gradient wallpaper fallback.\n";

    // Fallback: Elegant dark slate & midnight blue linear/radial gradient wallpaper
    for (uint32_t y = 0; y < height; ++y) {
        float fy = static_cast<float>(y) / static_cast<float>(height);
        for (uint32_t x = 0; x < width; ++x) {
            float fx = static_cast<float>(x) / static_cast<float>(width);

            float r = 15.0f * (1.0f - fy) + 30.0f * fy;
            float g = 23.0f * (1.0f - fy) + 27.0f * fy;
            float b = 42.0f * (1.0f - fy) + 75.0f * fy;

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

void renderMenuBar(uint32_t* pixels, uint32_t width, uint32_t height, lcl::render::FontRenderer& fontRenderer, const std::string& timeStr) {
    if (!pixels || width == 0 || height == 0) return;

    // Translucent dark slate background #0F172A with ~40% alpha (0x660F172A)
    // Bottom 1px accent border with ~60% alpha (0x991E293B)
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t bg = (y == height - 1) ? 0x991E293B : 0x660F172A;
        std::fill_n(pixels + y * width, width, bg);
    }

    // Render formatted date/time string aligned to the right side with 20px padding
    if (fontRenderer.isInitialized() && !timeStr.empty()) {
        int textW = fontRenderer.getTextWidth(timeStr);
        int textX = static_cast<int>(width) - textW - 20;
        if (textX < 0) textX = 10;
        int textY = (static_cast<int>(height) - fontRenderer.getCellHeight()) / 2;
        fontRenderer.renderString(pixels, width, height, textX, textY, timeStr, 0xFFF1F5F9);
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

    // Initialize FontRenderer for MenuBar time text
    lcl::render::FontRenderer fontRenderer;
    std::vector<std::string> fontPaths = {
        "/usr/share/fonts/inter/Inter-Regular.otf",
        "assets/fonts/inter/Inter-Regular.otf"
    };
    for (const auto& fpath : fontPaths) {
        if (fontRenderer.loadFont(fpath, 14.0f)) {
            std::cout << "[LCL Shell] Loaded MenuBar font: " << fpath << "\n";
            break;
        }
    }

    // 2. Register Role as DesktopWallpaper / Shell
    lcl::protocol::LCLHeader regHeader{};
    regHeader.opcode = lcl::protocol::LCLOpcode::RegisterRole;
    regHeader.payloadSize = sizeof(lcl::protocol::LCLMsgRegisterRole);

    lcl::protocol::LCLMsgRegisterRole regMsg{};
    regMsg.role = lcl::protocol::LCLRole::DesktopWallpaper;
    std::strncpy(regMsg.clientName, "lcl-desktop-shell", sizeof(regMsg.clientName) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, regHeader, &regMsg);

    // 3. Request Surface 1 (Wallpaper) Creation (0,0 = Fullscreen)
    uint32_t width = 1280;
    uint32_t height = 800;
    const uint32_t menuBarHeight = 32;

    lcl::protocol::LCLHeader wpSurfHeader{};
    wpSurfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    wpSurfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate wpSurfMsg{};
    wpSurfMsg.surfaceId = 1;
    wpSurfMsg.x = 0;
    wpSurfMsg.y = 0;
    wpSurfMsg.width = 0;  // 0 = request full screen width from Compositor
    wpSurfMsg.height = 0; // 0 = request full screen height from Compositor
    std::strncpy(wpSurfMsg.title, "LCL Wallpaper", sizeof(wpSurfMsg.title) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, wpSurfHeader, &wpSurfMsg);

    // 4. Set Decoration Mode to None (Frameless) for Surface 1
    lcl::protocol::LCLHeader wpDecHeader{};
    wpDecHeader.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    wpDecHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode wpDecMsg{};
    wpDecMsg.surfaceId = 1;
    wpDecMsg.mode = lcl::protocol::LCLDecorationMode::None;

    lcl::protocol::sendMsgWithFd(socketFd, wpDecHeader, &wpDecMsg);

    // 5. Set Window Layer to BOTTOM and unfocusable = 1 for Surface 1
    lcl::protocol::LCLHeader wpLayerHeader{};
    wpLayerHeader.opcode = lcl::protocol::LCLOpcode::SetWindowLayer;
    wpLayerHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowLayer);

    lcl::protocol::LCLMsgSetWindowLayer wpLayerMsg{};
    wpLayerMsg.surfaceId = 1;
    wpLayerMsg.layer = lcl::protocol::LCLWindowLayer::Bottom;
    wpLayerMsg.unfocusable = 1;

    lcl::protocol::sendMsgWithFd(socketFd, wpLayerHeader, &wpLayerMsg);

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

    // 6. Request Surface 2 (Menu Bar) Creation
    lcl::protocol::LCLHeader mbSurfHeader{};
    mbSurfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    mbSurfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate mbSurfMsg{};
    mbSurfMsg.surfaceId = 2;
    mbSurfMsg.x = 0;
    mbSurfMsg.y = 0;
    mbSurfMsg.width = width;
    mbSurfMsg.height = menuBarHeight;
    std::strncpy(mbSurfMsg.title, "LCL MenuBar", sizeof(mbSurfMsg.title) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, mbSurfHeader, &mbSurfMsg);

    // Set Decoration Mode None (Frameless) for Surface 2
    lcl::protocol::LCLHeader mbDecHeader{};
    mbDecHeader.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    mbDecHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode mbDecMsg{};
    mbDecMsg.surfaceId = 2;
    mbDecMsg.mode = lcl::protocol::LCLDecorationMode::None;

    lcl::protocol::sendMsgWithFd(socketFd, mbDecHeader, &mbDecMsg);

    // Set Window Layer TopMost and unfocusable = 1 for Surface 2
    lcl::protocol::LCLHeader mbLayerHeader{};
    mbLayerHeader.opcode = lcl::protocol::LCLOpcode::SetWindowLayer;
    mbLayerHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowLayer);

    lcl::protocol::LCLMsgSetWindowLayer mbLayerMsg{};
    mbLayerMsg.surfaceId = 2;
    mbLayerMsg.layer = lcl::protocol::LCLWindowLayer::TopMost;
    mbLayerMsg.unfocusable = 1;

    lcl::protocol::sendMsgWithFd(socketFd, mbLayerHeader, &mbLayerMsg);

    // Set Reserved Zone (struts) top = 32
    lcl::protocol::LCLHeader resHeader{};
    resHeader.opcode = lcl::protocol::LCLOpcode::SetReservedZone;
    resHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetReservedZone);

    lcl::protocol::LCLMsgSetReservedZone resMsg{};
    resMsg.surfaceId = 2;
    resMsg.top = menuBarHeight;
    resMsg.bottom = 0;
    resMsg.left = 0;
    resMsg.right = 0;

    lcl::protocol::sendMsgWithFd(socketFd, resHeader, &resMsg);

    // Set Effect Graph for Surface 2 (Menu Bar)
    // Region: full menu bar surface, Source: Backdrop
    // Ordered Pipeline: Blur -> Saturation -> Brightness
    std::vector<lcl::protocol::FilterOp> mbFilters = {
        { lcl::protocol::FilterType::Blur, 15.0f },
        { lcl::protocol::FilterType::Saturation, 1.4f },
        { lcl::protocol::FilterType::Brightness, 1.1f }
    };

    lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
    graphMsg.surfaceId = 2;
    graphMsg.regionCount = 1;
    graphMsg.filterCount = static_cast<uint32_t>(mbFilters.size());

    lcl::protocol::EffectRegion graphRegion{};
    graphRegion.x = 0;
    graphRegion.y = 0;
    graphRegion.width = static_cast<uint32_t>(width);
    graphRegion.height = static_cast<uint32_t>(menuBarHeight);
    graphRegion.source = lcl::protocol::EffectSourceType::Backdrop;
    graphRegion.blendMode = lcl::protocol::EffectBlendMode::Normal;
    graphRegion.filterCount = static_cast<uint16_t>(mbFilters.size());
    graphRegion.filterOffset = 0;
    graphRegion.opacity = 1.0f;

    size_t graphPayloadSize = sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader) +
                              sizeof(lcl::protocol::EffectRegion) +
                              mbFilters.size() * sizeof(lcl::protocol::FilterOp);
    std::vector<uint8_t> graphPayload(graphPayloadSize);
    uint8_t* graphDst = graphPayload.data();
    std::memcpy(graphDst, &graphMsg, sizeof(graphMsg));
    graphDst += sizeof(graphMsg);
    std::memcpy(graphDst, &graphRegion, sizeof(graphRegion));
    graphDst += sizeof(graphRegion);
    std::memcpy(graphDst, mbFilters.data(), mbFilters.size() * sizeof(lcl::protocol::FilterOp));

    lcl::protocol::LCLHeader graphHeader{};
    graphHeader.opcode = lcl::protocol::LCLOpcode::SetEffectGraph;
    graphHeader.payloadSize = static_cast<uint32_t>(graphPayload.size());

    lcl::protocol::sendMsgWithFd(socketFd, graphHeader, graphPayload.data());

    // 7. Create SHM Buffer and Initialize Solid Black Wallpaper (Surface 1)
    size_t shmSizeWallpaper = static_cast<size_t>(width) * height * 4;
    int shmFdWallpaper = memfd_create("lcl_wallpaper_shm", MFD_CLOEXEC);
    if (shmFdWallpaper < 0) {
        std::cerr << "[LCL Shell ERROR] memfd_create failed: " << strerror(errno) << "\n";
        close(socketFd);
        return 1;
    }

    if (ftruncate(shmFdWallpaper, shmSizeWallpaper) < 0) {
        std::cerr << "[LCL Shell ERROR] ftruncate failed: " << strerror(errno) << "\n";
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    uint32_t* shmPixelsWallpaper = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeWallpaper, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdWallpaper, 0));
    if (shmPixelsWallpaper == MAP_FAILED) {
        std::cerr << "[LCL Shell ERROR] mmap failed: " << strerror(errno) << "\n";
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    // Fill initial wallpaper buffer with solid black (0xFF000000)
    std::fill_n(shmPixelsWallpaper, width * height, 0xFF000000);

    lcl::protocol::LCLHeader wpAttachHeader{};
    wpAttachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
    wpAttachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

    lcl::protocol::LCLMsgAttachBuffer wpAttachMsg{};
    wpAttachMsg.surfaceId = 1;
    wpAttachMsg.width = width;
    wpAttachMsg.height = height;
    wpAttachMsg.stride = width * 4;
    wpAttachMsg.format = 1;

    lcl::protocol::sendMsgWithFd(socketFd, wpAttachHeader, &wpAttachMsg, shmFdWallpaper);
    std::cout << "[LCL Shell] Initial black wallpaper surface attached (" << width << "x" << height << ") at LAYER_BOTTOM.\n";

    // Launch Async Background Thread for loading and decoding wallpaper.jpg
    auto wpTask = std::make_shared<AsyncWallpaperTask>();
    wpTask->w = width;
    wpTask->h = height;

    std::thread bgWpThread([wpTask]() {
        wpTask->pixels.resize(wpTask->w * wpTask->h, 0xFF000000);
        renderWallpaper(wpTask->pixels.data(), wpTask->w, wpTask->h);
        wpTask->ready.store(true);
    });
    bgWpThread.detach();

    // 8. Create SHM Buffer and Render MenuBar (Surface 2)
    size_t shmSizeMenuBar = static_cast<size_t>(width) * menuBarHeight * 4;
    int shmFdMenuBar = memfd_create("lcl_menubar_shm", MFD_CLOEXEC);
    if (shmFdMenuBar < 0) {
        std::cerr << "[LCL Shell ERROR] memfd_create for menubar failed: " << strerror(errno) << "\n";
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    if (ftruncate(shmFdMenuBar, shmSizeMenuBar) < 0) {
        std::cerr << "[LCL Shell ERROR] ftruncate for menubar failed: " << strerror(errno) << "\n";
        close(shmFdMenuBar);
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    uint32_t* shmPixelsMenuBar = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeMenuBar, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdMenuBar, 0));
    if (shmPixelsMenuBar == MAP_FAILED) {
        std::cerr << "[LCL Shell ERROR] mmap for menubar failed: " << strerror(errno) << "\n";
        close(shmFdMenuBar);
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    std::string currentTimeStr = getFormattedTime();
    std::string lastTimeStr = currentTimeStr;
    renderMenuBar(shmPixelsMenuBar, width, menuBarHeight, fontRenderer, currentTimeStr);

    lcl::protocol::LCLHeader mbAttachHeader{};
    mbAttachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
    mbAttachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

    lcl::protocol::LCLMsgAttachBuffer mbAttachMsg{};
    mbAttachMsg.surfaceId = 2;
    mbAttachMsg.width = width;
    mbAttachMsg.height = menuBarHeight;
    mbAttachMsg.stride = width * 4;
    mbAttachMsg.format = 1;

    lcl::protocol::sendMsgWithFd(socketFd, mbAttachHeader, &mbAttachMsg, shmFdMenuBar);
    std::cout << "[LCL Shell] MenuBar surface attached (" << width << "x" << menuBarHeight << ") at LAYER_TOPMOST.\n";

    // 9. Main Shell Loop
    bool running = true;
    while (running) {
        // Check if async wallpaper image decoding finished
        if (wpTask && wpTask->ready.load()) {
            if (wpTask->w == width && wpTask->h == height && !wpTask->pixels.empty()) {
                std::memcpy(shmPixelsWallpaper, wpTask->pixels.data(), wpTask->pixels.size() * sizeof(uint32_t));
                lcl::protocol::sendMsgWithFd(socketFd, wpAttachHeader, &wpAttachMsg, -1);
                std::cout << "[LCL Shell] Async wallpaper image loaded (" << width << "x" << height << ") and attached.\n";
            }
            wpTask.reset();
        }

        // Per-second time update check
        currentTimeStr = getFormattedTime();
        if (currentTimeStr != lastTimeStr) {
            lastTimeStr = currentTimeStr;
            renderMenuBar(shmPixelsMenuBar, width, menuBarHeight, fontRenderer, currentTimeStr);
            lcl::protocol::sendMsgWithFd(socketFd, mbAttachHeader, &mbAttachMsg, -1);
        }

        lcl::protocol::LCLHeader header{};
        std::vector<uint8_t> payload;
        int receivedFd = -1;

        while (lcl::protocol::recvMsgWithFd(socketFd, header, payload, receivedFd)) {
            if (header.opcode == lcl::protocol::LCLOpcode::ConfigureBounds &&
                payload.size() >= sizeof(lcl::protocol::LCLMsgConfigureBounds)) {
                auto* cfg = reinterpret_cast<const lcl::protocol::LCLMsgConfigureBounds*>(payload.data());
                if (cfg->surfaceId == 1) { // Wallpaper surface
                    if (cfg->width > 0 && cfg->height > 0 && (cfg->width != width || cfg->height != height)) {
                        width = cfg->width;
                        height = cfg->height;

                        munmap(shmPixelsWallpaper, shmSizeWallpaper);
                        shmSizeWallpaper = static_cast<size_t>(width) * height * 4;
                        ftruncate(shmFdWallpaper, shmSizeWallpaper);
                        shmPixelsWallpaper = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeWallpaper, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdWallpaper, 0));

                        if (shmPixelsWallpaper != MAP_FAILED) {
                            std::fill_n(shmPixelsWallpaper, width * height, 0xFF000000);
                            wpAttachMsg.width = width;
                            wpAttachMsg.height = height;
                            wpAttachMsg.stride = width * 4;
                            lcl::protocol::sendMsgWithFd(socketFd, wpAttachHeader, &wpAttachMsg, shmFdWallpaper);

                            // Trigger new async wallpaper load for resized dimensions
                            wpTask = std::make_shared<AsyncWallpaperTask>();
                            wpTask->w = width;
                            wpTask->h = height;
                            std::thread bgResizeThread([wpTask]() {
                                wpTask->pixels.resize(wpTask->w * wpTask->h, 0xFF000000);
                                renderWallpaper(wpTask->pixels.data(), wpTask->w, wpTask->h);
                                wpTask->ready.store(true);
                            });
                            bgResizeThread.detach();
                        }

                        // Reallocate MenuBar SHM as well on width resize
                        munmap(shmPixelsMenuBar, shmSizeMenuBar);
                        shmSizeMenuBar = static_cast<size_t>(width) * menuBarHeight * 4;
                        ftruncate(shmFdMenuBar, shmSizeMenuBar);
                        shmPixelsMenuBar = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeMenuBar, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdMenuBar, 0));

                        if (shmPixelsMenuBar != MAP_FAILED) {
                            renderMenuBar(shmPixelsMenuBar, width, menuBarHeight, fontRenderer, currentTimeStr);
                            mbAttachMsg.width = width;
                            mbAttachMsg.height = menuBarHeight;
                            mbAttachMsg.stride = width * 4;
                            lcl::protocol::sendMsgWithFd(socketFd, mbAttachHeader, &mbAttachMsg, shmFdMenuBar);
                        }
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::SurfaceDestroy) {
                running = false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (shmPixelsWallpaper != MAP_FAILED) munmap(shmPixelsWallpaper, shmSizeWallpaper);
    if (shmFdWallpaper >= 0) close(shmFdWallpaper);
    if (shmPixelsMenuBar != MAP_FAILED) munmap(shmPixelsMenuBar, shmSizeMenuBar);
    if (shmFdMenuBar >= 0) close(shmFdMenuBar);
    if (socketFd >= 0) close(socketFd);

    return 0;
}
