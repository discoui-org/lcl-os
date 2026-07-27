#include "render/skia_renderer.hpp"
#include "core/display/egl_backend.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <GLES2/gl2.h>

namespace lcl::render {

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    return shader;
}

bool SkiaRenderer::initGLShader() {
    const char* vSrc =
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vTexCoord;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "    vTexCoord = aTexCoord;\n"
        "}\n";

    const char* fSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    gl_FragColor = vec4(c.b, c.g, c.r, c.a);\n"
        "}\n";

    GLuint vs = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fSrc);
    m_glProgram = glCreateProgram();
    glAttachShader(m_glProgram, vs);
    glAttachShader(m_glProgram, fs);
    glLinkProgram(m_glProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    m_aPosLoc = glGetAttribLocation(m_glProgram, "aPosition");
    m_aTexLoc = glGetAttribLocation(m_glProgram, "aTexCoord");
    m_uTextureLoc = glGetUniformLocation(m_glProgram, "uTexture");

    glGenTextures(1, &m_glTexture);
    glBindTexture(GL_TEXTURE_2D, m_glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

SkiaRenderer::~SkiaRenderer() {
    shutdown();
}

bool SkiaRenderer::initialize(uint32_t width, uint32_t height, lcl::core::EGLBackend* eglBackend, uint32_t* targetPixels) {
    if (targetPixels) {
        m_targetPixels = targetPixels;
    }
    if (width > 0) m_width = width;
    if (height > 0) m_height = height;

    if (m_initialized) return true;

    m_eglBackend = eglBackend;

    if (m_eglBackend && m_eglBackend->isInitialized()) {
        m_backendType = SkiaBackendType::OpenGL_EGL;
        m_eglBackend->makeCurrent();

        initGLShader();

        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::cout << "[LCL Skia] Skia OpenGL/EGL Hardware Accelerated Backend Active ("
                  << m_width << "x" << m_height << ")!\n";
    } else {
        m_backendType = SkiaBackendType::SoftwareRaster;
        if (!m_targetPixels) {
            m_rasterPixels.resize(m_width * m_height, 0xFF14161D); // Dark theme default
            m_targetPixels = m_rasterPixels.data();
        }
        std::cout << "[LCL Skia] Skia Raster Software Backend Active ("
                  << m_width << "x" << m_height << ").\n";
    }

    m_initialized = true;
    return true;
}

void SkiaRenderer::shutdown() {
    if (m_glTexture > 0) {
        glDeleteTextures(1, &m_glTexture);
        m_glTexture = 0;
    }
    if (m_glProgram > 0) {
        glDeleteProgram(m_glProgram);
        m_glProgram = 0;
    }
    m_rasterPixels.clear();
    m_rasterPixels.shrink_to_fit();
    m_targetPixels = nullptr;
    m_initialized = false;
}

void SkiaRenderer::beginFrame() {
    if (!m_initialized) return;

    if (m_targetPixels) {
        std::fill_n(m_targetPixels, m_width * m_height, 0xFF14161D);
    }
}

void SkiaRenderer::endFrame() {
    if (!m_initialized) return;

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        m_eglBackend->makeCurrent();
        glViewport(0, 0, m_width, m_height);

        if (m_targetPixels && m_glTexture > 0) {
            glBindTexture(GL_TEXTURE_2D, m_glTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, m_targetPixels);

            glUseProgram(m_glProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_glTexture);
            glUniform1i(m_uTextureLoc, 0);

            static const float quad[] = {
                -1.0f,  1.0f,  0.0f, 0.0f,
                -1.0f, -1.0f,  0.0f, 1.0f,
                 1.0f,  1.0f,  1.0f, 0.0f,
                 1.0f, -1.0f,  1.0f, 1.0f,
            };

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(m_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
            glEnableVertexAttribArray(m_aPosLoc);
            glVertexAttribPointer(m_aTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
            glEnableVertexAttribArray(m_aTexLoc);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(m_aPosLoc);
            glDisableVertexAttribArray(m_aTexLoc);
        }

        glFlush();
        m_eglBackend->swapBuffers();
    }
}

void SkiaRenderer::drawBackgroundGradient(const SkiaColor& topColor, const SkiaColor& bottomColor) {
    if (!m_initialized || !m_targetPixels || m_height == 0 || m_width == 0) return;

    if (m_height == 1) {
        std::fill_n(m_targetPixels, m_width, topColor.toARGB());
        return;
    }

    uint32_t topR = topColor.r << 16;
    uint32_t topG = topColor.g << 16;
    uint32_t topB = topColor.b << 16;

    int32_t stepR = ((static_cast<int32_t>(bottomColor.r) - static_cast<int32_t>(topColor.r)) << 16) / static_cast<int32_t>(m_height - 1);
    int32_t stepG = ((static_cast<int32_t>(bottomColor.g) - static_cast<int32_t>(topColor.g)) << 16) / static_cast<int32_t>(m_height - 1);
    int32_t stepB = ((static_cast<int32_t>(bottomColor.b) - static_cast<int32_t>(topColor.b)) << 16) / static_cast<int32_t>(m_height - 1);

    uint32_t currR = topR;
    uint32_t currG = topG;
    uint32_t currB = topB;

    for (uint32_t y = 0; y < m_height; ++y) {
        uint32_t r = currR >> 16;
        uint32_t g = currG >> 16;
        uint32_t b = currB >> 16;
        uint32_t argb = (0xFFu << 24) | (r << 16) | (g << 8) | b;

        std::fill_n(&m_targetPixels[y * m_width], m_width, argb);

        currR += stepR;
        currG += stepG;
        currB += stepB;
    }
}

void SkiaRenderer::drawRect(const SkiaRect& rect, const SkiaColor& color) {
    if (!m_initialized || !m_targetPixels) return;

    int x1 = std::clamp(static_cast<int>(rect.x), 0, static_cast<int>(m_width));
    int y1 = std::clamp(static_cast<int>(rect.y), 0, static_cast<int>(m_height));
    int x2 = std::clamp(static_cast<int>(rect.x + rect.width), 0, static_cast<int>(m_width));
    int y2 = std::clamp(static_cast<int>(rect.y + rect.height), 0, static_cast<int>(m_height));

    if (x1 >= x2 || y1 >= y2) return;

    uint32_t fillARGB = color.toARGB();

    if (color.a == 255) {
        for (int y = y1; y < y2; ++y) {
            uint32_t* row = &m_targetPixels[y * m_width + x1];
            std::fill_n(row, x2 - x1, fillARGB);
        }
    } else if (color.a > 0) {
        uint32_t alpha = color.a;
        uint32_t invAlpha = 255 - alpha;
        uint32_t srcR = color.r * alpha;
        uint32_t srcG = color.g * alpha;
        uint32_t srcB = color.b * alpha;

        for (int y = y1; y < y2; ++y) {
            uint32_t* row = &m_targetPixels[y * m_width];
            for (int x = x1; x < x2; ++x) {
                uint32_t bg = row[x];
                uint32_t bgR = (bg >> 16) & 0xFF;
                uint32_t bgG = (bg >> 8) & 0xFF;
                uint32_t bgB = bg & 0xFF;

                uint32_t r = (srcR + bgR * invAlpha) >> 8;
                uint32_t g = (srcG + bgG * invAlpha) >> 8;
                uint32_t b = (srcB + bgB * invAlpha) >> 8;

                row[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void SkiaRenderer::drawRoundedRect(const SkiaRect& rect, float radius, const SkiaColor& color, const SkiaColor& borderColor, float borderWidth) {
    (void)radius;
    drawRect(rect, color);
    if (borderWidth > 0.0f && borderColor.a > 0) {
        drawRect({rect.x, rect.y, rect.width, borderWidth}, borderColor);
        drawRect({rect.x, rect.y + rect.height - borderWidth, rect.width, borderWidth}, borderColor);
        drawRect({rect.x, rect.y, borderWidth, rect.height}, borderColor);
        drawRect({rect.x + rect.width - borderWidth, rect.y, borderWidth, rect.height}, borderColor);
    }
}

void SkiaRenderer::drawDropShadow(const SkiaRect& rect, float radius, float blur, const SkiaColor& shadowColor) {
    (void)radius;
    if (shadowColor.a == 0) return;
    SkiaRect shadowRect = {
        rect.x - blur,
        rect.y - blur + 4.0f,
        rect.width + blur * 2.0f,
        rect.height + blur * 2.0f
    };
    SkiaColor softShadow = shadowColor;
    softShadow.a = static_cast<uint8_t>(shadowColor.a * 0.4f);
    drawRect(shadowRect, softShadow);
}

void SkiaRenderer::drawCircle(float cx, float cy, float radius, const SkiaColor& color) {
    SkiaRect rect = { cx - radius, cy - radius, radius * 2.0f, radius * 2.0f };
    drawRect(rect, color);
}

void SkiaRenderer::drawLine(float x1, float y1, float x2, float y2, const SkiaColor& color, float strokeWidth) {
    (void)y2;
    SkiaRect rect = { x1, y1, std::abs(x2 - x1) + strokeWidth, strokeWidth };
    drawRect(rect, color);
}

void SkiaRenderer::drawString(int x, int y, const std::string& text, uint32_t fgColor) {
    if (!m_initialized || !m_targetPixels || text.empty()) return;
    if (!m_fontRenderer.isInitialized()) {
        m_fontRenderer.loadFont("/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf", 15.0f);
    }
    if (m_fontRenderer.isInitialized()) {
        m_fontRenderer.renderString(m_targetPixels, m_width, m_height, x, y, text, fgColor);
    }
}

void SkiaRenderer::drawBuffer(int dstX, int dstY, int srcW, int srcH, const uint32_t* pixelData, int stridePixels, float opacity) {
    if (!m_initialized || !pixelData || !m_targetPixels || srcW <= 0 || srcH <= 0) return;

    if (stridePixels <= 0) stridePixels = srcW;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + srcW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + srcH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    for (int y = clipY1; y < clipY2; ++y) {
        int srcY = y - dstY;
        const uint32_t* srcRow = pixelData + (srcY * stridePixels);
        uint32_t* dstRow = &m_targetPixels[y * m_width];
        int copyWidth = clipX2 - clipX1;

        if (opacity >= 0.99f) {
            int srcXOffset = clipX1 - dstX;
            std::memcpy(dstRow + clipX1, srcRow + srcXOffset, copyWidth * sizeof(uint32_t));
        } else {
            for (int x = clipX1; x < clipX2; ++x) {
                int srcX = x - dstX;
                uint32_t pixel = srcRow[srcX];
                uint8_t srcA = static_cast<uint8_t>(((pixel >> 24) & 0xFF) * opacity);
                float a = srcA / 255.0f;
                float invA = 1.0f - a;

                uint32_t bg = dstRow[x];
                uint8_t r = static_cast<uint8_t>(((pixel >> 16) & 0xFF) * a + ((bg >> 16) & 0xFF) * invA);
                uint8_t g = static_cast<uint8_t>(((pixel >> 8) & 0xFF) * a + ((bg >> 8) & 0xFF) * invA);
                uint8_t b = static_cast<uint8_t>((pixel & 0xFF) * a + (bg & 0xFF) * invA);

                dstRow[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

} // namespace lcl::render
