#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <optional>
#include <unordered_map>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "core/ipc/ipc_manager.hpp"
#include "core/ipc/lcl_protocol.hpp"
#include "core/app/app_bundle_parser.hpp"
#include "core/display/display_scale.hpp"
#include "lcl-ui/core/image_loader.hpp"
#include "render/font_renderer.hpp"

namespace {

constexpr float kDockPanelRadiusPx = 26.0f;
constexpr int kDockIconSizePx = 60;
constexpr int kDockIconGapPx = 12;
constexpr int kDockIconPadXPx = 14;
constexpr int kDockIconPadYPx = 14;
constexpr uint32_t kDockPanelMaxHeightPx = 88;
constexpr uint32_t kDockBottomInsetPx = 4;

struct AsyncWallpaperTask {
    std::atomic<bool> ready{false};
    std::vector<uint32_t> pixels;
    uint32_t w{0};
    uint32_t h{0};
};

struct DockWindowState {
    uint32_t windowId{0};
    std::string title;
    std::string appId;
    bool isFocused{false};
};

struct DockLayout {
    uint32_t panelX{0};
    uint32_t panelY{0};
    uint32_t panelW{0};
    uint32_t panelH{0};
    int shownIcons{0};
    int iconStartIndex{0};
};

std::string normalizeName(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> buildAppIconCatalog() {
    std::unordered_map<std::string, std::string> out;
    static const std::vector<std::string> appDirs = {
        "/home/user/Applications",
        "/Applications",
    };

    for (const auto& dir : appDirs) {
        for (const auto& meta : lcl::core::AppBundleParser::scanDirectory(dir)) {
            if (!meta.valid || meta.icon.empty()) continue;

            std::filesystem::path iconPath(meta.icon);
            if (iconPath.is_relative()) {
                iconPath = std::filesystem::path(meta.bundlePath) / iconPath;
            }
            if (!std::filesystem::exists(iconPath)) continue;

            const std::string iconFull = iconPath.string();
            const std::string nameKey = normalizeName(meta.name);
            const std::string bundleKey = normalizeName(std::filesystem::path(meta.bundlePath).stem().string());
            if (!nameKey.empty()) out[nameKey] = iconFull;
            if (!bundleKey.empty()) out[bundleKey] = iconFull;
        }
    }

    return out;
}

std::string resolveIconPathForWindow(const DockWindowState& w,
                                     const std::unordered_map<std::string, std::string>& catalog) {
    if (!w.appId.empty()) {
        const std::string appKey = normalizeName(w.appId);
        auto it = catalog.find(appKey);
        if (it != catalog.end()) return it->second;
    }

    const std::string titleKey = normalizeName(w.title);
    auto exact = catalog.find(titleKey);
    if (exact != catalog.end()) return exact->second;

    for (const auto& [k, v] : catalog) {
        if (k.empty()) continue;
        if (titleKey.find(k) != std::string::npos || k.find(titleKey) != std::string::npos) {
            return v;
        }
    }
    return "";
}

DockLayout computeDockLayout(uint32_t width, uint32_t height, size_t windowCount) {
    DockLayout out{};
    const int iconSize = kDockIconSizePx;
    const int iconGap = kDockIconGapPx;
    const int padX = kDockIconPadXPx;
    const int padY = kDockIconPadYPx;

    const uint32_t targetPanelH = static_cast<uint32_t>(iconSize + padY * 2);
    const uint32_t availableH = (height > kDockBottomInsetPx) ? (height - kDockBottomInsetPx) : height;
    out.panelH = std::min<uint32_t>(availableH, std::min<uint32_t>(targetPanelH, kDockPanelMaxHeightPx));
    out.panelY = (height > out.panelH + kDockBottomInsetPx)
        ? (height - out.panelH - kDockBottomInsetPx)
        : 0;

    if (windowCount == 0) {
        return out;
    }

    const int maxIconsByScreen = std::max(1, (static_cast<int>(width) - padX * 2 + iconGap) / (iconSize + iconGap));
    out.shownIcons = std::min<int>(static_cast<int>(windowCount), maxIconsByScreen);
    out.iconStartIndex = std::max(0, static_cast<int>(windowCount) - out.shownIcons);

    const int rowW = out.shownIcons * iconSize + (out.shownIcons - 1) * iconGap;
    const int panelW = rowW + padX * 2;
    out.panelW = static_cast<uint32_t>(std::max(1, panelW));
    out.panelX = (width > out.panelW) ? ((width - out.panelW) / 2) : 0;

    return out;
}

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

    std::optional<lcl::ui::ImageData> loaded;
    std::string loadedPath;

    for (const auto& path : candidatePaths) {
        loaded = lcl::ui::ImageLoader::loadArgb32(path);
        if (loaded && loaded->isValid()) {
            loadedPath = path;
            break;
        }
    }

    if (loaded && loaded->isValid()) {
        const uint32_t imgW = loaded->width;
        const uint32_t imgH = loaded->height;
        std::cout << "[LCL Shell] Loaded wallpaper image from '" << loadedPath
                  << "' (" << imgW << "x" << imgH << " -> " << width << "x" << height << ").\n";

        lcl::ui::ImageData scaled = lcl::ui::ImageLoader::resizeBilinear(*loaded, width, height);
        if (scaled.isValid()) {
            std::copy(scaled.pixels.begin(), scaled.pixels.end(), pixels);
        }
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

uint32_t blendSrcOver(uint32_t dst, uint32_t src) {
    const float sa = static_cast<float>((src >> 24) & 0xFF) / 255.0f;
    const float sr = static_cast<float>((src >> 16) & 0xFF);
    const float sg = static_cast<float>((src >> 8) & 0xFF);
    const float sb = static_cast<float>(src & 0xFF);

    const float da = static_cast<float>((dst >> 24) & 0xFF) / 255.0f;
    const float dr = static_cast<float>((dst >> 16) & 0xFF);
    const float dg = static_cast<float>((dst >> 8) & 0xFF);
    const float db = static_cast<float>(dst & 0xFF);

    const float outA = sa + da * (1.0f - sa);
    if (outA <= 0.0001f) return 0;

    const float outR = (sr * sa + dr * da * (1.0f - sa)) / outA;
    const float outG = (sg * sa + dg * da * (1.0f - sa)) / outA;
    const float outB = (sb * sa + db * da * (1.0f - sa)) / outA;

    const uint32_t a = static_cast<uint32_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
    const uint32_t r = static_cast<uint32_t>(std::clamp(outR, 0.0f, 255.0f));
    const uint32_t g = static_cast<uint32_t>(std::clamp(outG, 0.0f, 255.0f));
    const uint32_t b = static_cast<uint32_t>(std::clamp(outB, 0.0f, 255.0f));
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void drawRoundedRect(uint32_t* pixels,
                     uint32_t width,
                     uint32_t height,
                     int x,
                     int y,
                     int w,
                     int h,
                     float radius,
                     uint32_t color) {
    if (!pixels || width == 0 || height == 0 || w <= 0 || h <= 0) return;

    const float r = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(width), x + w);
    const int y1 = std::min(static_cast<int>(height), y + h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            bool inside = true;
            if (r > 0.001f) {
                const float lx = (static_cast<float>(px) + 0.5f) - static_cast<float>(x);
                const float ly = (static_cast<float>(py) + 0.5f) - static_cast<float>(y);
                const float cx = std::clamp(lx, r, static_cast<float>(w) - r);
                const float cy = std::clamp(ly, r, static_cast<float>(h) - r);
                const float dx = lx - cx;
                const float dy = ly - cy;
                inside = (dx * dx + dy * dy) <= (r * r);
            }

            if (inside) {
                const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px);
                pixels[idx] = blendSrcOver(pixels[idx], color);
            }
        }
    }
}

void drawRoundedRectStroke(uint32_t* pixels,
                           uint32_t width,
                           uint32_t height,
                           int x,
                           int y,
                           int w,
                           int h,
                           float radius,
                           int strokeWidth,
                           uint32_t color) {
    if (!pixels || width == 0 || height == 0 || w <= 0 || h <= 0 || strokeWidth <= 0) return;

    const float outerR = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
    const int innerX = x + strokeWidth;
    const int innerY = y + strokeWidth;
    const int innerW = w - strokeWidth * 2;
    const int innerH = h - strokeWidth * 2;
    const float innerR = std::max(0.0f, outerR - static_cast<float>(strokeWidth));

    const auto isInsideRounded = [](float px, float py, int rx, int ry, int rw, int rh, float rr) {
        if (rw <= 0 || rh <= 0) return false;

        const float lx = px - static_cast<float>(rx);
        const float ly = py - static_cast<float>(ry);
        if (lx < 0.0f || ly < 0.0f || lx > static_cast<float>(rw) || ly > static_cast<float>(rh)) return false;
        if (rr <= 0.001f) return true;

        const float cx = std::clamp(lx, rr, static_cast<float>(rw) - rr);
        const float cy = std::clamp(ly, rr, static_cast<float>(rh) - rr);
        const float dx = lx - cx;
        const float dy = ly - cy;
        return (dx * dx + dy * dy) <= (rr * rr);
    };

    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(width), x + w);
    const int y1 = std::min(static_cast<int>(height), y + h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float sx = static_cast<float>(px) + 0.5f;
            const float sy = static_cast<float>(py) + 0.5f;

            if (!isInsideRounded(sx, sy, x, y, w, h, outerR)) continue;
            if (isInsideRounded(sx, sy, innerX, innerY, innerW, innerH, innerR)) continue;

            const size_t idx = static_cast<size_t>(py) * static_cast<size_t>(width) + static_cast<size_t>(px);
            pixels[idx] = blendSrcOver(pixels[idx], color);
        }
    }
}

void drawRoundedImage(uint32_t* dst,
                      uint32_t dstW,
                      uint32_t dstH,
                      int x,
                      int y,
                      int w,
                      int h,
                      const std::vector<uint32_t>& src,
                      float radius) {
    if (!dst || dstW == 0 || dstH == 0 || w <= 0 || h <= 0) return;
    if (src.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) return;

    const float r = std::max(0.0f, std::min(radius, std::min(w, h) * 0.5f));
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(static_cast<int>(dstW), x + w);
    const int y1 = std::min(static_cast<int>(dstH), y + h);
    if (x0 >= x1 || y0 >= y1) return;

    const auto insideRoundedLocal = [r, w, h](float lx, float ly) {
        if (r <= 0.001f) return true;
        const float cx = std::clamp(lx, r, static_cast<float>(w) - r);
        const float cy = std::clamp(ly, r, static_cast<float>(h) - r);
        const float dx = lx - cx;
        const float dy = ly - cy;
        return (dx * dx + dy * dy) <= (r * r);
    };

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const int sx = px - x;
            const int sy = py - y;

            float coverage = 1.0f;
            if (r > 0.001f) {
                int hits = 0;
                const float ox[4] = {0.25f, 0.75f, 0.25f, 0.75f};
                const float oy[4] = {0.25f, 0.25f, 0.75f, 0.75f};
                for (int si = 0; si < 4; ++si) {
                    const float lx = static_cast<float>(sx) + ox[si];
                    const float ly = static_cast<float>(sy) + oy[si];
                    if (insideRoundedLocal(lx, ly)) {
                        ++hits;
                    }
                }
                if (hits == 0) continue;
                coverage = static_cast<float>(hits) / 4.0f;
            }

            uint32_t srcPx = src[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
            if (coverage < 0.999f) {
                uint32_t a = static_cast<uint32_t>((srcPx >> 24) & 0xFFu);
                a = static_cast<uint32_t>(std::clamp(static_cast<float>(a) * coverage, 0.0f, 255.0f));
                srcPx = (srcPx & 0x00FFFFFFu) | (a << 24);
            }
            const size_t di = static_cast<size_t>(py) * static_cast<size_t>(dstW) + static_cast<size_t>(px);
            dst[di] = blendSrcOver(dst[di], srcPx);
        }
    }
}

const std::vector<uint32_t>* getDockIconPixels(const std::string& iconPath,
                                               int iconSize,
                                               std::unordered_map<std::string, std::vector<uint32_t>>& cache) {
    if (iconPath.empty() || iconSize <= 0) return nullptr;

    const std::string key = iconPath + "#" + std::to_string(iconSize);
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;

    auto loaded = lcl::ui::ImageLoader::loadArgb32(iconPath);
    if (!loaded || !loaded->isValid()) return nullptr;

    lcl::ui::ImageData scaled;
    const uint32_t target = static_cast<uint32_t>(iconSize);
    if (loaded->width >= target * 2 && loaded->height >= target * 2) {
        // Two-pass downscale reduces aliasing on high-frequency icon details.
        const uint32_t mid = target * 2;
        lcl::ui::ImageData midScaled = lcl::ui::ImageLoader::resizeBilinear(*loaded, mid, mid);
        scaled = lcl::ui::ImageLoader::resizeBilinear(midScaled, target, target);
    } else {
        scaled = lcl::ui::ImageLoader::resizeBilinear(*loaded, target, target);
    }
    if (!scaled.isValid()) return nullptr;

    auto [insertedIt, inserted] = cache.emplace(key, std::move(scaled.pixels));
    (void)inserted;
    return &insertedIt->second;
}

void renderDock(uint32_t* pixels,
                uint32_t width,
                uint32_t height,
                const std::vector<DockWindowState>& windows,
                const std::unordered_map<std::string, std::string>& iconCatalog,
                std::unordered_map<std::string, std::vector<uint32_t>>& iconCache) {
    if (!pixels || width == 0 || height == 0) return;

    // Clear full surface to transparent; only the dock panel itself is visible.
    std::fill_n(pixels, static_cast<size_t>(width) * static_cast<size_t>(height), 0x00000000);

    const DockLayout layout = computeDockLayout(width, height, windows.size());
    if (layout.shownIcons <= 0 || layout.panelW == 0 || layout.panelH == 0) {
        return;
    }

    const uint32_t panelH = layout.panelH;
    const uint32_t panelW = layout.panelW;
    const uint32_t panelX = layout.panelX;
    const uint32_t panelY = layout.panelY;

    // Inset borders only; panel center stays transparent.
    drawRoundedRectStroke(pixels,
                          width,
                          height,
                          static_cast<int>(panelX) + 1,
                          static_cast<int>(panelY) + 1,
                          static_cast<int>(panelW) - 2,
                          static_cast<int>(panelH) - 2,
                          kDockPanelRadiusPx - 1.0f,
                          1,
                          0x44FFFFFF);
    drawRoundedRectStroke(pixels,
                          width,
                          height,
                          static_cast<int>(panelX) + 2,
                          static_cast<int>(panelY) + 2,
                          static_cast<int>(panelW) - 4,
                          static_cast<int>(panelH) - 4,
                          kDockPanelRadiusPx - 2.0f,
                          1,
                          0x22000000);

    const int iconSize = kDockIconSizePx;
    const int iconGap = kDockIconGapPx;
    const int topPad = std::max(0, (static_cast<int>(panelH) - iconSize) / 2);
    const int startIdx = layout.iconStartIndex;
    const int shown = layout.shownIcons;
    const int rowW = (shown > 0) ? (shown * iconSize + (shown - 1) * iconGap) : 0;
    int curX = static_cast<int>(panelX) + (static_cast<int>(panelW) - rowW) / 2;
    const int iconY = static_cast<int>(panelY) + topPad;

    for (int i = startIdx; i < static_cast<int>(windows.size()); ++i) {
        const DockWindowState& w = windows[static_cast<size_t>(i)];
        const std::string iconPath = resolveIconPathForWindow(w, iconCatalog);

        const std::vector<uint32_t>* icon = getDockIconPixels(iconPath, iconSize, iconCache);
        if (icon && !icon->empty()) {
            drawRoundedImage(pixels, width, height, curX, iconY, iconSize, iconSize, *icon, 14.0f);
        }

        if (w.isFocused) {
            drawRoundedRect(pixels, width, height, curX + iconSize / 2 - 10, iconY + iconSize + 4, 20, 3, 1.5f, 0xEEF6F8FC);
        }

        curX += iconSize + iconGap;
    }
}

void sendDockEffectGraph(int socketFd,
                         uint32_t surfaceId,
                         uint32_t width,
                         uint32_t height,
                         size_t windowCount) {
    const DockLayout layout = computeDockLayout(width, height, windowCount);

    if (layout.shownIcons <= 0 || layout.panelW == 0 || layout.panelH == 0) {
        lcl::protocol::LCLHeader clearHeader{};
        clearHeader.opcode = lcl::protocol::LCLOpcode::ClearEffectGraph;
        clearHeader.payloadSize = sizeof(lcl::protocol::LCLMsgClearEffectGraph);
        lcl::protocol::LCLMsgClearEffectGraph clearMsg{};
        clearMsg.surfaceId = surfaceId;
        lcl::protocol::sendMsgWithFd(socketFd, clearHeader, &clearMsg);
        return;
    }

    std::vector<lcl::protocol::FilterOp> dockFilters;
    dockFilters.reserve(4);

    lcl::protocol::FilterOp blur{};
    blur.type = lcl::protocol::FilterType::Blur;
    blur.value = 3.5f;
    dockFilters.push_back(blur);

    lcl::protocol::FilterOp saturation{};
    saturation.type = lcl::protocol::FilterType::Saturation;
    saturation.value = 1.5f;
    dockFilters.push_back(saturation);

    lcl::protocol::FilterOp contrast{};
    contrast.type = lcl::protocol::FilterType::Contrast;
    contrast.value = 0.9f;
    dockFilters.push_back(contrast);

    lcl::protocol::FilterOp glass{};
    glass.type = lcl::protocol::FilterType::Glass;
    glass.value = 1.0f;
    glass.profile = static_cast<uint8_t>(lcl::protocol::GlassProfile::Auto);
    glass.params[0] = 30.0f;
    glass.params[1] = 3.0f;
    glass.params[2] = 12.0f;
    dockFilters.push_back(glass);

    const uint32_t panelH = layout.panelH;
    const uint32_t panelW = layout.panelW;
    const uint32_t panelX = layout.panelX;
    const uint32_t panelY = layout.panelY;

    lcl::protocol::LCLMsgSetEffectGraphHeader graphMsg{};
    graphMsg.surfaceId = surfaceId;
    graphMsg.regionCount = 1;
    graphMsg.filterCount = static_cast<uint32_t>(dockFilters.size());

    lcl::protocol::EffectRegion graphRegion{};
    graphRegion.x = static_cast<int32_t>(panelX);
    graphRegion.y = static_cast<int32_t>(panelY);
    graphRegion.width = panelW;
    graphRegion.height = panelH;
    graphRegion.cornerRadius = kDockPanelRadiusPx;
    graphRegion.source = lcl::protocol::EffectSourceType::Backdrop;
    graphRegion.blendMode = lcl::protocol::EffectBlendMode::Normal;
    graphRegion.filterCount = static_cast<uint16_t>(dockFilters.size());
    graphRegion.filterOffset = 0;
    graphRegion.opacity = 1.0f;

    size_t graphPayloadSize = sizeof(lcl::protocol::LCLMsgSetEffectGraphHeader) +
                              sizeof(lcl::protocol::EffectRegion) +
                              dockFilters.size() * sizeof(lcl::protocol::FilterOp);
    std::vector<uint8_t> graphPayload(graphPayloadSize);
    uint8_t* graphDst = graphPayload.data();
    std::memcpy(graphDst, &graphMsg, sizeof(graphMsg));
    graphDst += sizeof(graphMsg);
    std::memcpy(graphDst, &graphRegion, sizeof(graphRegion));
    graphDst += sizeof(graphRegion);
    std::memcpy(graphDst, dockFilters.data(), dockFilters.size() * sizeof(lcl::protocol::FilterOp));

    lcl::protocol::LCLHeader graphHeader{};
    graphHeader.opcode = lcl::protocol::LCLOpcode::SetEffectGraph;
    graphHeader.payloadSize = static_cast<uint32_t>(graphPayload.size());
    lcl::protocol::sendMsgWithFd(socketFd, graphHeader, graphPayload.data());
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
    const uint32_t dockSurfaceHeight = 88;

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

    // 6.5 Request Surface 3 (Dock) Creation
    lcl::protocol::LCLHeader dockSurfHeader{};
    dockSurfHeader.opcode = lcl::protocol::LCLOpcode::SurfaceCreate;
    dockSurfHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSurfaceCreate);

    lcl::protocol::LCLMsgSurfaceCreate dockSurfMsg{};
    dockSurfMsg.surfaceId = 3;
    dockSurfMsg.x = 0;
    dockSurfMsg.y = static_cast<int32_t>((height > dockSurfaceHeight) ? (height - dockSurfaceHeight) : 0);
    dockSurfMsg.width = width;
    dockSurfMsg.height = dockSurfaceHeight;
    std::strncpy(dockSurfMsg.title, "LCL Dock", sizeof(dockSurfMsg.title) - 1);

    lcl::protocol::sendMsgWithFd(socketFd, dockSurfHeader, &dockSurfMsg);

    // Set Decoration Mode None (Frameless) for Surface 3
    lcl::protocol::LCLHeader dockDecHeader{};
    dockDecHeader.opcode = lcl::protocol::LCLOpcode::SetDecorationMode;
    dockDecHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetDecorationMode);

    lcl::protocol::LCLMsgSetDecorationMode dockDecMsg{};
    dockDecMsg.surfaceId = 3;
    dockDecMsg.mode = lcl::protocol::LCLDecorationMode::None;
    lcl::protocol::sendMsgWithFd(socketFd, dockDecHeader, &dockDecMsg);

    // Set Window Layer TopMost and unfocusable = 1 for Surface 3
    lcl::protocol::LCLHeader dockLayerHeader{};
    dockLayerHeader.opcode = lcl::protocol::LCLOpcode::SetWindowLayer;
    dockLayerHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowLayer);

    lcl::protocol::LCLMsgSetWindowLayer dockLayerMsg{};
    dockLayerMsg.surfaceId = 3;
    dockLayerMsg.layer = lcl::protocol::LCLWindowLayer::TopMost;
    dockLayerMsg.unfocusable = 1;
    lcl::protocol::sendMsgWithFd(socketFd, dockLayerHeader, &dockLayerMsg);

    // Set compositor window corner radius via dedicated API instead of duplicating mask policy.
    lcl::protocol::LCLHeader dockRadiusHeader{};
    dockRadiusHeader.opcode = lcl::protocol::LCLOpcode::SetWindowCornerRadius;
    dockRadiusHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetWindowCornerRadius);

    lcl::protocol::LCLMsgSetWindowCornerRadius dockRadiusMsg{};
    dockRadiusMsg.surfaceId = 3;
    dockRadiusMsg.radiusPx = kDockPanelRadiusPx;
    lcl::protocol::sendMsgWithFd(socketFd, dockRadiusHeader, &dockRadiusMsg);

    // Set Reserved Zone (struts) top = 32
    lcl::protocol::LCLHeader resHeader{};
    resHeader.opcode = lcl::protocol::LCLOpcode::SetReservedZone;
    resHeader.payloadSize = sizeof(lcl::protocol::LCLMsgSetReservedZone);

    lcl::protocol::LCLMsgSetReservedZone resMsg{};
    resMsg.surfaceId = 2;
    resMsg.top = menuBarHeight;
    resMsg.bottom = dockSurfaceHeight;
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

    std::vector<DockWindowState> dockWindows;

    // Set Effect Graph for Surface 3 (Dock)
    sendDockEffectGraph(socketFd, 3, width, dockSurfaceHeight, dockWindows.size());

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
    auto iconCatalog = buildAppIconCatalog();
    auto lastIconCatalogRefresh = std::chrono::steady_clock::now();
    std::unordered_map<std::string, std::vector<uint32_t>> dockIconCache;
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

    // 8.5 Create SHM Buffer and Render Dock (Surface 3)
    size_t shmSizeDock = static_cast<size_t>(width) * dockSurfaceHeight * 4;
    int shmFdDock = memfd_create("lcl_dock_shm", MFD_CLOEXEC);
    if (shmFdDock < 0) {
        std::cerr << "[LCL Shell ERROR] memfd_create for dock failed: " << strerror(errno) << "\n";
        close(shmFdMenuBar);
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    if (ftruncate(shmFdDock, shmSizeDock) < 0) {
        std::cerr << "[LCL Shell ERROR] ftruncate for dock failed: " << strerror(errno) << "\n";
        close(shmFdDock);
        close(shmFdMenuBar);
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    uint32_t* shmPixelsDock = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeDock, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdDock, 0));
    if (shmPixelsDock == MAP_FAILED) {
        std::cerr << "[LCL Shell ERROR] mmap for dock failed: " << strerror(errno) << "\n";
        close(shmFdDock);
        close(shmFdMenuBar);
        close(shmFdWallpaper);
        close(socketFd);
        return 1;
    }

    renderDock(shmPixelsDock, width, dockSurfaceHeight, dockWindows, iconCatalog, dockIconCache);

    lcl::protocol::LCLHeader dockAttachHeader{};
    dockAttachHeader.opcode = lcl::protocol::LCLOpcode::AttachBuffer;
    dockAttachHeader.payloadSize = sizeof(lcl::protocol::LCLMsgAttachBuffer);

    lcl::protocol::LCLMsgAttachBuffer dockAttachMsg{};
    dockAttachMsg.surfaceId = 3;
    dockAttachMsg.width = width;
    dockAttachMsg.height = dockSurfaceHeight;
    dockAttachMsg.stride = width * 4;
    dockAttachMsg.format = 1;

    lcl::protocol::sendMsgWithFd(socketFd, dockAttachHeader, &dockAttachMsg, shmFdDock);
    std::cout << "[LCL Shell] Dock surface attached (" << width << "x" << dockSurfaceHeight << ") at LAYER_TOPMOST.\n";

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

                        // Reallocate Dock SHM on width resize
                        munmap(shmPixelsDock, shmSizeDock);
                        shmSizeDock = static_cast<size_t>(width) * dockSurfaceHeight * 4;
                        ftruncate(shmFdDock, shmSizeDock);
                        shmPixelsDock = reinterpret_cast<uint32_t*>(mmap(nullptr, shmSizeDock, PROT_READ | PROT_WRITE, MAP_SHARED, shmFdDock, 0));

                        if (shmPixelsDock != MAP_FAILED) {
                            renderDock(shmPixelsDock, width, dockSurfaceHeight, dockWindows, iconCatalog, dockIconCache);
                            dockAttachMsg.width = width;
                            dockAttachMsg.height = dockSurfaceHeight;
                            dockAttachMsg.stride = width * 4;
                            lcl::protocol::sendMsgWithFd(socketFd, dockAttachHeader, &dockAttachMsg, shmFdDock);
                        }

                        // Keep struts and dock effect graph in sync with resized width
                        resMsg.bottom = dockSurfaceHeight;
                        lcl::protocol::sendMsgWithFd(socketFd, resHeader, &resMsg);
                        sendDockEffectGraph(socketFd, 3, width, dockSurfaceHeight, dockWindows.size());
                    }
                }
            } else if (header.opcode == lcl::protocol::LCLOpcode::WindowListUpdate) {
                if (payload.size() >= sizeof(lcl::protocol::LCLMsgWindowListHeader)) {
                    auto* listHeader = reinterpret_cast<const lcl::protocol::LCLMsgWindowListHeader*>(payload.data());
                    size_t expected = sizeof(lcl::protocol::LCLMsgWindowListHeader)
                        + static_cast<size_t>(listHeader->windowCount) * sizeof(lcl::protocol::LCLMsgWindowListEntry);

                    if (payload.size() >= expected) {
                        const auto* entries = reinterpret_cast<const lcl::protocol::LCLMsgWindowListEntry*>(
                            payload.data() + sizeof(lcl::protocol::LCLMsgWindowListHeader));

                        std::vector<DockWindowState> next;
                        next.reserve(listHeader->windowCount);
                        for (uint32_t i = 0; i < listHeader->windowCount; ++i) {
                            DockWindowState w{};
                            w.windowId = entries[i].windowId;
                            w.isFocused = entries[i].isFocused != 0;
                            w.title = entries[i].title;
                            w.appId = entries[i].appId;
                            next.push_back(std::move(w));
                        }

                        dockWindows = std::move(next);

                        auto now = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastIconCatalogRefresh).count() >= 2) {
                            iconCatalog = buildAppIconCatalog();
                            lastIconCatalogRefresh = now;
                        }

                        renderDock(shmPixelsDock, width, dockSurfaceHeight, dockWindows, iconCatalog, dockIconCache);
                        sendDockEffectGraph(socketFd, 3, width, dockSurfaceHeight, dockWindows.size());
                        lcl::protocol::sendMsgWithFd(socketFd, dockAttachHeader, &dockAttachMsg, -1);
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
    if (shmPixelsDock != MAP_FAILED) munmap(shmPixelsDock, shmSizeDock);
    if (shmFdDock >= 0) close(shmFdDock);
    if (socketFd >= 0) close(socketFd);

    return 0;
}
