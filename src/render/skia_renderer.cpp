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

    // --- GLSL 2-Pass Gaussian Blur Fragment Shader ---
    const char* fBlurSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uDirection;\n"
        "uniform float uSigma;\n"
        "uniform int uRadius;\n"
        "void main() {\n"
        "    if (uSigma <= 0.1) {\n"
        "        gl_FragColor = texture2D(uTexture, vTexCoord);\n"
        "        return;\n"
        "    }\n"
        "    vec4 colorAcc = vec4(0.0);\n"
        "    float weightAcc = 0.0;\n"
        "    float twoSigmaSq = 2.0 * uSigma * uSigma;\n"
        "    for (int i = -32; i <= 32; ++i) {\n"
        "        if (i < -uRadius || i > uRadius) continue;\n"
        "        float fi = float(i);\n"
        "        float weight = exp(-(fi * fi) / twoSigmaSq);\n"
        "        vec2 coord = vTexCoord + uDirection * fi;\n"
        "        colorAcc += texture2D(uTexture, coord) * weight;\n"
        "        weightAcc += weight;\n"
        "    }\n"
        "    gl_FragColor = colorAcc / weightAcc;\n"
        "}\n";

    GLuint vsBlur = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsBlur = compileShader(GL_FRAGMENT_SHADER, fBlurSrc);
    m_glBlurProgram = glCreateProgram();
    glAttachShader(m_glBlurProgram, vsBlur);
    glAttachShader(m_glBlurProgram, fsBlur);
    glLinkProgram(m_glBlurProgram);
    glDeleteShader(vsBlur);
    glDeleteShader(fsBlur);

    m_aBlurPosLoc = glGetAttribLocation(m_glBlurProgram, "aPosition");
    m_aBlurTexLoc = glGetAttribLocation(m_glBlurProgram, "aTexCoord");
    m_uBlurTextureLoc = glGetUniformLocation(m_glBlurProgram, "uTexture");
    m_uBlurDirLoc = glGetUniformLocation(m_glBlurProgram, "uDirection");
    m_uBlurSigmaLoc = glGetUniformLocation(m_glBlurProgram, "uSigma");
    m_uBlurRadiusLoc = glGetUniformLocation(m_glBlurProgram, "uRadius");

    // --- GLSL Color Matrix Fragment Shader ---
    const char* fColorMatrixSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform mat3 uColorMatrix;\n"
        "uniform vec3 uColorOffset;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    vec3 rgb = uColorMatrix * c.rgb + (uColorOffset / 255.0);\n"
        "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), c.a);\n"
        "}\n";

    GLuint vsColor = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsColor = compileShader(GL_FRAGMENT_SHADER, fColorMatrixSrc);
    m_glColorMatrixProgram = glCreateProgram();
    glAttachShader(m_glColorMatrixProgram, vsColor);
    glAttachShader(m_glColorMatrixProgram, fsColor);
    glLinkProgram(m_glColorMatrixProgram);
    glDeleteShader(vsColor);
    glDeleteShader(fsColor);

    m_aColorPosLoc = glGetAttribLocation(m_glColorMatrixProgram, "aPosition");
    m_aColorTexLoc = glGetAttribLocation(m_glColorMatrixProgram, "aTexCoord");
    m_uColorTextureLoc = glGetUniformLocation(m_glColorMatrixProgram, "uTexture");
    m_uColorMatrixLoc = glGetUniformLocation(m_glColorMatrixProgram, "uColorMatrix");
    m_uColorOffsetLoc = glGetUniformLocation(m_glColorMatrixProgram, "uColorOffset");

    // --- Initialize GLES2 Ping-Pong Framebuffer Objects (FBOs) ---
    glGenTextures(2, m_glFBOTexture);
    glGenFramebuffers(2, m_glFBO);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_glFBOTexture[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glFBOReady = true;

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
    if (m_glFBOReady) {
        glDeleteFramebuffers(2, m_glFBO);
        glDeleteTextures(2, m_glFBOTexture);
        m_glFBO[0] = m_glFBO[1] = 0;
        m_glFBOTexture[0] = m_glFBOTexture[1] = 0;
        m_glFBOReady = false;
    }
    if (m_glBlurProgram > 0) {
        glDeleteProgram(m_glBlurProgram);
        m_glBlurProgram = 0;
    }
    if (m_glColorMatrixProgram > 0) {
        glDeleteProgram(m_glColorMatrixProgram);
        m_glColorMatrixProgram = 0;
    }
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

        for (int x = clipX1; x < clipX2; ++x) {
            int srcX = x - dstX;
            uint32_t pixel = srcRow[srcX];
            uint8_t rawA = static_cast<uint8_t>((pixel >> 24) & 0xFF);
            if (rawA == 0) continue;

            if (rawA == 255 && opacity >= 0.99f) {
                dstRow[x] = pixel;
            } else {
                float a = (rawA / 255.0f) * opacity;
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

namespace {

struct ColorMatrix4x4 {
    float m[3][3]{
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };
    float o[3]{0.0f, 0.0f, 0.0f};

    bool isIdentity() const {
        return m[0][0] == 1.0f && m[0][1] == 0.0f && m[0][2] == 0.0f && o[0] == 0.0f &&
               m[1][0] == 0.0f && m[1][1] == 1.0f && m[1][2] == 0.0f && o[1] == 0.0f &&
               m[2][0] == 0.0f && m[2][1] == 0.0f && m[2][2] == 1.0f && o[2] == 0.0f;
    }

    void reset() {
        m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; o[0] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; o[1] = 0.0f;
        m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; o[2] = 0.0f;
    }

    void multiply(const ColorMatrix4x4& next) {
        float newM[3][3]{};
        float newO[3]{};

        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                newM[r][c] = next.m[r][0] * m[0][c] + next.m[r][1] * m[1][c] + next.m[r][2] * m[2][c];
            }
            newO[r] = next.m[r][0] * o[0] + next.m[r][1] * o[1] + next.m[r][2] * o[2] + next.o[r];
        }

        std::memcpy(m, newM, sizeof(m));
        std::memcpy(o, newO, sizeof(o));
    }
};

ColorMatrix4x4 createBrightnessMatrix(float value) {
    ColorMatrix4x4 mat;
    mat.m[0][0] = value;
    mat.m[1][1] = value;
    mat.m[2][2] = value;
    return mat;
}

ColorMatrix4x4 createContrastMatrix(float value) {
    ColorMatrix4x4 mat;
    mat.m[0][0] = value;
    mat.m[1][1] = value;
    mat.m[2][2] = value;
    float offset = 127.5f * (1.0f - value);
    mat.o[0] = offset;
    mat.o[1] = offset;
    mat.o[2] = offset;
    return mat;
}

ColorMatrix4x4 createSaturationMatrix(float value) {
    ColorMatrix4x4 mat;
    constexpr float rw = 0.2126f;
    constexpr float gw = 0.7152f;
    constexpr float bw = 0.0722f;

    mat.m[0][0] = (1.0f - value) * rw + value;
    mat.m[0][1] = (1.0f - value) * gw;
    mat.m[0][2] = (1.0f - value) * bw;

    mat.m[1][0] = (1.0f - value) * rw;
    mat.m[1][1] = (1.0f - value) * gw + value;
    mat.m[1][2] = (1.0f - value) * bw;

    mat.m[2][0] = (1.0f - value) * rw;
    mat.m[2][1] = (1.0f - value) * gw;
    mat.m[2][2] = (1.0f - value) * bw + value;

    return mat;
}

ColorMatrix4x4 createGrayscaleMatrix(float value) {
    float s = 1.0f - std::clamp(value, 0.0f, 1.0f);
    return createSaturationMatrix(s);
}

ColorMatrix4x4 createInvertMatrix(float value) {
    ColorMatrix4x4 mat;
    float v = std::clamp(value, 0.0f, 1.0f);
    float scale = 1.0f - 2.0f * v;
    float offset = 255.0f * v;

    mat.m[0][0] = scale;
    mat.m[1][1] = scale;
    mat.m[2][2] = scale;

    mat.o[0] = offset;
    mat.o[1] = offset;
    mat.o[2] = offset;

    return mat;
}

inline int mirrorIndex(int p, int max) {
    if (max <= 1) return 0;
    if (p < 0) {
        p = -p;
    }
    if (p >= max) {
        p = 2 * max - 2 - p;
    }
    return std::clamp(p, 0, max - 1);
}

void applyColorMatrixToPixels(std::vector<uint32_t>& pixels, int w, int h, const ColorMatrix4x4& mat) {
    if (mat.isIdentity() || pixels.empty()) return;

    size_t count = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < count; ++i) {
        uint32_t p = pixels[i];
        float r = static_cast<float>((p >> 16) & 0xFF);
        float g = static_cast<float>((p >> 8) & 0xFF);
        float b = static_cast<float>(p & 0xFF);

        float outR = mat.m[0][0] * r + mat.m[0][1] * g + mat.m[0][2] * b + mat.o[0];
        float outG = mat.m[1][0] * r + mat.m[1][1] * g + mat.m[1][2] * b + mat.o[1];
        float outB = mat.m[2][0] * r + mat.m[2][1] * g + mat.m[2][2] * b + mat.o[2];

        uint8_t ru = static_cast<uint8_t>(std::clamp(outR, 0.0f, 255.0f));
        uint8_t gu = static_cast<uint8_t>(std::clamp(outG, 0.0f, 255.0f));
        uint8_t bu = static_cast<uint8_t>(std::clamp(outB, 0.0f, 255.0f));

        pixels[i] = (p & 0xFF000000) | (ru << 16) | (gu << 8) | bu;
    }
}

void applyGaussianBlurToPixels(std::vector<uint32_t>& pixels, int w, int h, float blurRadius) {
    if (w <= 0 || h <= 0 || blurRadius <= 0.5f || pixels.empty()) return;

    float sigma = std::max(0.5f, blurRadius / 2.0f);
    int R = std::max(1, static_cast<int>(std::ceil(3.0f * sigma)));

    // Adaptive scale factor for CPU performance: 1x..4x downsampling
    int scaleFactor = std::clamp(R / 4, 1, 4);
    int sw = std::max(1, w / scaleFactor);
    int sh = std::max(1, h / scaleFactor);

    std::vector<uint32_t> smallBuf(sw * sh);

    // Downsample W x H -> sw x sh
    for (int sy = 0; sh > 0 && sy < sh; ++sy) {
        int srcY = std::min(h - 1, sy * scaleFactor);
        for (int sx = 0; sw > 0 && sx < sw; ++sx) {
            int srcX = std::min(w - 1, sx * scaleFactor);
            smallBuf[sy * sw + sx] = pixels[srcY * w + srcX];
        }
    }

    int sR = std::max(1, R / scaleFactor);
    float sSigma = std::max(0.5f, sigma / static_cast<float>(scaleFactor));
    float twoSigmaSq = 2.0f * sSigma * sSigma;

    std::vector<float> weights(2 * sR + 1);
    for (int k = -sR; k <= sR; ++k) {
        float fk = static_cast<float>(k);
        weights[k + sR] = std::exp(-(fk * fk) / twoSigmaSq);
    }

    std::vector<uint32_t> blurBuf(sw * sh);

    // 2-Pass Separable Gaussian Convolution (Horizontal then Vertical)
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            float rAcc = 0.0f, gAcc = 0.0f, bAcc = 0.0f, wAcc = 0.0f;
            for (int dx = -sR; dx <= sR; ++dx) {
                int kx = mirrorIndex(x + dx, sw);
                uint32_t p = smallBuf[y * sw + kx];
                float wVal = weights[dx + sR];
                rAcc += static_cast<float>((p >> 16) & 0xFF) * wVal;
                gAcc += static_cast<float>((p >> 8) & 0xFF) * wVal;
                bAcc += static_cast<float>(p & 0xFF) * wVal;
                wAcc += wVal;
            }
            blurBuf[y * sw + x] = (0xFF000000) |
                (static_cast<uint32_t>(std::clamp(rAcc / wAcc, 0.0f, 255.0f)) << 16) |
                (static_cast<uint32_t>(std::clamp(gAcc / wAcc, 0.0f, 255.0f)) << 8) |
                static_cast<uint32_t>(std::clamp(bAcc / wAcc, 0.0f, 255.0f));
        }
    }

    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            float rAcc = 0.0f, gAcc = 0.0f, bAcc = 0.0f, wAcc = 0.0f;
            for (int dy = -sR; dy <= sR; ++dy) {
                int ky = mirrorIndex(y + dy, sh);
                uint32_t p = blurBuf[ky * sw + x];
                float wVal = weights[dy + sR];
                rAcc += static_cast<float>((p >> 16) & 0xFF) * wVal;
                gAcc += static_cast<float>((p >> 8) & 0xFF) * wVal;
                bAcc += static_cast<float>(p & 0xFF) * wVal;
                wAcc += wVal;
            }
            smallBuf[y * sw + x] = (0xFF000000) |
                (static_cast<uint32_t>(std::clamp(rAcc / wAcc, 0.0f, 255.0f)) << 16) |
                (static_cast<uint32_t>(std::clamp(gAcc / wAcc, 0.0f, 255.0f)) << 8) |
                static_cast<uint32_t>(std::clamp(bAcc / wAcc, 0.0f, 255.0f));
        }
    }

    // Bilinear upsample sw x sh -> W x H with Mirror Edge Mode
    for (int y = 0; y < h; ++y) {
        float v = (static_cast<float>(y) + 0.5f) * (static_cast<float>(sh) / static_cast<float>(h)) - 0.5f;
        int y0 = mirrorIndex(static_cast<int>(std::floor(v)), sh);
        int y1 = mirrorIndex(y0 + 1, sh);
        float fy = v - std::floor(v);

        for (int x = 0; x < w; ++x) {
            float u = (static_cast<float>(x) + 0.5f) * (static_cast<float>(sw) / static_cast<float>(w)) - 0.5f;
            int x0 = mirrorIndex(static_cast<int>(std::floor(u)), sw);
            int x1 = mirrorIndex(x0 + 1, sw);
            float fx = u - std::floor(u);

            uint32_t p00 = smallBuf[y0 * sw + x0];
            uint32_t p01 = smallBuf[y0 * sw + x1];
            uint32_t p10 = smallBuf[y1 * sw + x0];
            uint32_t p11 = smallBuf[y1 * sw + x1];

            auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

            float r = lerp(lerp((p00 >> 16) & 0xFF, (p01 >> 16) & 0xFF, fx), lerp((p10 >> 16) & 0xFF, (p11 >> 16) & 0xFF, fx), fy);
            float g = lerp(lerp((p00 >> 8) & 0xFF, (p01 >> 8) & 0xFF, fx), lerp((p10 >> 8) & 0xFF, (p11 >> 8) & 0xFF, fx), fy);
            float b = lerp(lerp(p00 & 0xFF, p01 & 0xFF, fx), lerp(p10 & 0xFF, p11 & 0xFF, fx), fy);

            pixels[y * w + x] = (0xFF000000) |
                (static_cast<uint32_t>(std::clamp(r, 0.0f, 255.0f)) << 16) |
                (static_cast<uint32_t>(std::clamp(g, 0.0f, 255.0f)) << 8) |
                static_cast<uint32_t>(std::clamp(b, 0.0f, 255.0f));
        }
    }
}

} // namespace

void SkiaRenderer::applyBackdropFilter(int dstX, int dstY, int srcW, int srcH, const std::vector<protocol::FilterOp>& filters) {
    if (!m_initialized || !m_targetPixels || srcW <= 0 || srcH <= 0 || filters.empty()) return;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + srcW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + srcH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    int w = clipX2 - clipX1;
    int h = clipY2 - clipY1;

    // 1. Crop backdrop area from m_targetPixels
    std::vector<uint32_t> crop(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&crop[y * w], &m_targetPixels[(clipY1 + y) * m_width + clipX1], w * sizeof(uint32_t));
    }

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();

        // Reallocate both FBO textures to match exact crop dimensions (w, h)
        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, crop.data());

        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[1]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        // Quad geometry in NDC (-1..+1) and UVs (0..1)
        static const float quad[16] = {
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
        };

        ColorMatrix4x4 pendingColorMatrix;
        int currentTex = 0; // 0 or 1 index into m_glFBOTexture / m_glFBO

        auto renderColorPass = [&]() {
            if (pendingColorMatrix.isIdentity()) return;
            int nextTex = 1 - currentTex;

            glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[nextTex]);
            glViewport(0, 0, w, h);

            glUseProgram(m_glColorMatrixProgram);

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(m_aColorPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
            glEnableVertexAttribArray(m_aColorPosLoc);
            glVertexAttribPointer(m_aColorTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
            glEnableVertexAttribArray(m_aColorTexLoc);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[currentTex]);
            glUniform1i(m_uColorTextureLoc, 0);

            // Set Color Matrix & Offset uniforms
            float mat3Val[9] = {
                pendingColorMatrix.m[0][0], pendingColorMatrix.m[1][0], pendingColorMatrix.m[2][0],
                pendingColorMatrix.m[0][1], pendingColorMatrix.m[1][1], pendingColorMatrix.m[2][1],
                pendingColorMatrix.m[0][2], pendingColorMatrix.m[1][2], pendingColorMatrix.m[2][2]
            };
            glUniformMatrix3fv(m_uColorMatrixLoc, 1, GL_FALSE, mat3Val);
            glUniform3fv(m_uColorOffsetLoc, 1, pendingColorMatrix.o);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(m_aColorPosLoc);
            glDisableVertexAttribArray(m_aColorTexLoc);

            currentTex = nextTex;
            pendingColorMatrix.reset();
        };

        for (const auto& op : filters) {
            switch (op.type) {
                case protocol::FilterType::Brightness:
                    pendingColorMatrix.multiply(createBrightnessMatrix(op.value));
                    break;
                case protocol::FilterType::Contrast:
                    pendingColorMatrix.multiply(createContrastMatrix(op.value));
                    break;
                case protocol::FilterType::Saturation:
                    pendingColorMatrix.multiply(createSaturationMatrix(op.value));
                    break;
                case protocol::FilterType::Grayscale:
                    pendingColorMatrix.multiply(createGrayscaleMatrix(op.value));
                    break;
                case protocol::FilterType::Invert:
                    pendingColorMatrix.multiply(createInvertMatrix(op.value));
                    break;
                case protocol::FilterType::Blur: {
                    renderColorPass();

                    float sigma = std::max(0.5f, op.value / 2.0f);
                    int radius = std::clamp(static_cast<int>(std::ceil(3.0f * sigma)), 1, 32);

                    // 1. Horizontal Pass
                    int blurPass1 = 1 - currentTex;
                    glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[blurPass1]);
                    glViewport(0, 0, w, h);

                    glUseProgram(m_glBlurProgram);

                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                    glVertexAttribPointer(m_aBlurPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
                    glEnableVertexAttribArray(m_aBlurPosLoc);
                    glVertexAttribPointer(m_aBlurTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
                    glEnableVertexAttribArray(m_aBlurTexLoc);

                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[currentTex]);
                    glUniform1i(m_uBlurTextureLoc, 0);
                    glUniform2f(m_uBlurDirLoc, 1.0f / static_cast<float>(w), 0.0f);
                    glUniform1f(m_uBlurSigmaLoc, sigma);
                    glUniform1i(m_uBlurRadiusLoc, radius);

                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                    // 2. Vertical Pass
                    int blurPass2 = currentTex;
                    glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[blurPass2]);
                    glViewport(0, 0, w, h);

                    glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[blurPass1]);
                    glUniform1i(m_uBlurTextureLoc, 0);
                    glUniform2f(m_uBlurDirLoc, 0.0f, 1.0f / static_cast<float>(h));
                    glUniform1f(m_uBlurSigmaLoc, sigma);
                    glUniform1i(m_uBlurRadiusLoc, radius);

                    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

                    glDisableVertexAttribArray(m_aBlurPosLoc);
                    glDisableVertexAttribArray(m_aBlurTexLoc);
                    break;
                }
                default:
                    break;
            }
        }

        renderColorPass();

        // Read pixels back from current FBO texture
        glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[currentTex]);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, crop.data());

        // Restore main framebuffer and viewport
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
    } else {
        // CPU Software Fallback Path
        ColorMatrix4x4 pendingColorMatrix;

        for (const auto& op : filters) {
            switch (op.type) {
                case protocol::FilterType::Brightness:
                    pendingColorMatrix.multiply(createBrightnessMatrix(op.value));
                    break;
                case protocol::FilterType::Contrast:
                    pendingColorMatrix.multiply(createContrastMatrix(op.value));
                    break;
                case protocol::FilterType::Saturation:
                    pendingColorMatrix.multiply(createSaturationMatrix(op.value));
                    break;
                case protocol::FilterType::Grayscale:
                    pendingColorMatrix.multiply(createGrayscaleMatrix(op.value));
                    break;
                case protocol::FilterType::Invert:
                    pendingColorMatrix.multiply(createInvertMatrix(op.value));
                    break;
                case protocol::FilterType::Blur:
                    if (!pendingColorMatrix.isIdentity()) {
                        applyColorMatrixToPixels(crop, w, h, pendingColorMatrix);
                        pendingColorMatrix.reset();
                    }
                    applyGaussianBlurToPixels(crop, w, h, op.value);
                    break;
                default:
                    break;
            }
        }

        if (!pendingColorMatrix.isIdentity()) {
            applyColorMatrixToPixels(crop, w, h, pendingColorMatrix);
        }
    }

    // Write back filtered backdrop crop to m_targetPixels
    for (int y = 0; y < h; ++y) {
        std::memcpy(&m_targetPixels[(clipY1 + y) * m_width + clipX1], &crop[y * w], w * sizeof(uint32_t));
    }
}

} // namespace lcl::render
