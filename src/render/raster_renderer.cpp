#include "render/raster_renderer.hpp"
#include "render/alpha_math.hpp"
#include "render/path_rasterizer.hpp"
#include "render/text_metrics.hpp"
#include "render/backdrop_filter_geometry.hpp"
#include "render/dma_buf_crop.hpp"
#include "render/raster_destination.hpp"
#ifndef LCL_SOFTWARE_ONLY
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#endif
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <type_traits>

namespace lcl::render {

namespace {

struct BufferSampleScale {
    float x{1.0f};
    float y{1.0f};
};

BufferSampleScale resolveBufferSampleScale(
    RasterBufferSampling sampling,
    int sourceWidth,
    int sourceHeight,
    float destinationWidth,
    float destinationHeight);

} // namespace

#ifndef LCL_SOFTWARE_ONLY
namespace {

constexpr size_t kImageTextureCacheBudgetBytes = 128u * 1024u * 1024u;
constexpr size_t kImageTextureCacheMaxEntries = 128u;

bool hasGlExtension(const char* extensions, const char* requested) {
    if (!extensions || !requested || *requested == '\0' || std::strchr(requested, ' ')) {
        return false;
    }

    const size_t requestedLength = std::strlen(requested);
    const char* match = extensions;
    while ((match = std::strstr(match, requested)) != nullptr) {
        const bool startsToken = match == extensions || match[-1] == ' ';
        const char following = match[requestedLength];
        const bool endsToken = following == '\0' || following == ' ';
        if (startsToken && endsToken) return true;
        match += requestedLength;
    }
    return false;
}

bool supportsUnpackRowLength() {
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    constexpr char kEsVersionPrefix[] = "OpenGL ES ";
    if (version && std::strncmp(version, kEsVersionPrefix,
                                sizeof(kEsVersionPrefix) - 1) == 0) {
        const char major = version[sizeof(kEsVersionPrefix) - 1];
        if (major >= '3' && major <= '9') return true;
    }

    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return hasGlExtension(extensions, "GL_EXT_unpack_subimage");
}

} // namespace

static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    return shader;
}

void beginPremultipliedAlphaSourceOver() {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void endPremultipliedAlphaSourceOver() {
    glDisable(GL_BLEND);
}

bool RasterRenderer::initGLShader() {
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
        "uniform float uOpacity;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    float opacity = clamp(uOpacity, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(c.rgb * opacity, c.a * opacity);\n"
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
    m_uOpacityLoc = glGetUniformLocation(m_glProgram, "uOpacity");

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
        "uniform vec2 uSizePx;\n"
        "uniform float uCornerRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform vec2 uInputScale;\n"
        // left, right, bottom, top. A framebuffer clip is not a material
        // boundary: mirroring there reflects unrelated scene pixels back into
        // the visible window edge. Real effect edges keep the existing mirror
        // behavior so blur remains contained within the declared region.
        "uniform vec4 uFramebufferClipEdges;\n"
        "float mirrorTexCoord(float uv) {\n"
        "    float m = mod(abs(uv), 2.0);\n"
        "    return m > 1.0 ? 2.0 - m : m;\n"
        "}\n"
        "vec2 resolveTexCoord(vec2 uv) {\n"
        "    if (uv.x < 0.0) uv.x = uFramebufferClipEdges.x > 0.5 ? 0.0 : mirrorTexCoord(uv.x);\n"
        "    if (uv.x > 1.0) uv.x = uFramebufferClipEdges.y > 0.5 ? 1.0 : mirrorTexCoord(uv.x);\n"
        "    if (uv.y < 0.0) uv.y = uFramebufferClipEdges.z > 0.5 ? 0.0 : mirrorTexCoord(uv.y);\n"
        "    if (uv.y > 1.0) uv.y = uFramebufferClipEdges.w > 0.5 ? 1.0 : mirrorTexCoord(uv.y);\n"
        "    return uv;\n"
        "}\n"
        "void main() {\n"
        "    if (uSigma <= 0.1) {\n"
        "        gl_FragColor = texture2D(uTexture, vTexCoord * uInputScale);\n"
        "        return;\n"
        "    }\n"
        "    vec4 colorAcc = vec4(0.0);\n"
        "    float weightAcc = 0.0;\n"
        "    float twoSigmaSq = 2.0 * uSigma * uSigma;\n"
        "    for (int i = -32; i <= 32; ++i) {\n"
        "        if (i >= -uRadius && i <= uRadius) {\n"
        "            float fi = float(i);\n"
        "            float weight = exp(-(fi * fi) / twoSigmaSq);\n"
        "            vec2 tap = resolveTexCoord(vTexCoord + uDirection * fi);\n"
        "            vec2 halfTexel = vec2(0.5) / uSizePx;\n"
        "            vec2 coord = clamp(tap, halfTexel, vec2(1.0) - halfTexel);\n"
        "            colorAcc += texture2D(uTexture, coord * uInputScale) * weight;\n"
        "            weightAcc += weight;\n"
        "        }\n"
        "    }\n"
        "    vec4 finalColor = colorAcc / weightAcc;\n"
        "    gl_FragColor = finalColor;\n"
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
    m_uBlurSizeLoc = glGetUniformLocation(m_glBlurProgram, "uSizePx");
    m_uBlurCornerRadiusLoc = glGetUniformLocation(m_glBlurProgram, "uCornerRadiusPx");
    m_uBlurRoundnessLoc = glGetUniformLocation(m_glBlurProgram, "uRoundnessExp");
    m_uBlurInputScaleLoc = glGetUniformLocation(m_glBlurProgram, "uInputScale");
    m_uBlurFramebufferClipEdgesLoc = glGetUniformLocation(
        m_glBlurProgram, "uFramebufferClipEdges");

    // --- GLSL Color Matrix Fragment Shader ---
    const char* fColorMatrixSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform mat3 uColorMatrix;\n"
        "uniform vec3 uColorOffset;\n"
        "uniform vec2 uInputScale;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord * uInputScale);\n"
        "    vec3 straightRgb = c.a > 0.0001 ? c.rgb / c.a : vec3(0.0);\n"
        "    vec3 rgb = uColorMatrix * straightRgb + (uColorOffset / 255.0);\n"
        "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0) * c.a, c.a);\n"
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
    m_uColorInputScaleLoc = glGetUniformLocation(m_glColorMatrixProgram, "uInputScale");

    // --- GLSL Rounded Mask Composite Shader ---
    const char* fMaskSrc =
        "precision highp float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uSizePx;\n"
        "uniform float uRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform float uOpacity;\n"
        "uniform float uSquareTopCorners;\n"
        "uniform vec2 uSampleOffset;\n"
        "uniform vec2 uSampleScale;\n"
        "uniform vec2 uSampleTexelInset;\n"
        "uniform vec2 uDrawSizePx;\n"
        "uniform vec2 uMaskOffsetPx;\n"
        "float sdSuperRoundRect(vec2 p, vec2 b, float r, float n) {\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    if (q.x <= 0.0 || q.y <= 0.0) {\n"
        "        return max(q.x, q.y) - r;\n"
        "    }\n"
        "    vec2 qq = max(q, 0.0) / max(r, 0.001);\n"
        "    float k = pow(pow(qq.x, n) + pow(qq.y, n), 1.0 / n);\n"
        "    return (k - 1.0) * r;\n"
        "}\n"
        "void main() {\n"
        "    vec2 sampleMin = uSampleOffset + uSampleTexelInset;\n"
        "    vec2 sampleMax = uSampleOffset + uSampleScale - uSampleTexelInset;\n"
        "    vec2 sampleCoord = clamp(uSampleOffset + vTexCoord * uSampleScale,\n"
        "                             min(sampleMin, sampleMax),\n"
        "                             max(sampleMin, sampleMax));\n"
        "    vec4 c = texture2D(uTexture, sampleCoord);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 maskCoord = vec2(\n"
        "        uMaskOffsetPx.x + vTexCoord.x * uDrawSizePx.x,\n"
        "        uMaskOffsetPx.y + (1.0 - vTexCoord.y) * uDrawSizePx.y);\n"
        "    vec2 p = vec2(maskCoord.x - uSizePx.x * 0.5,\n"
        "                  uSizePx.y * 0.5 - maskCoord.y);\n"
        "    vec2 halfSize = uSizePx * 0.5;\n"
        "    float d = (uSquareTopCorners > 0.5 && p.y > 0.0)\n"
        "        ? -1.0\n"
        "        : sdSuperRoundRect(p, halfSize, r, n);\n"
        // Center the device-pixel coverage ramp on the analytic edge. This
        // remains stable while the destination quad moves through subpixels.
        "    float mask = 1.0 - smoothstep(-0.5, 0.5, d);\n"
        "    float coverage = mask * clamp(uOpacity, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(c.rgb * coverage, c.a * coverage);\n"
        "}\n";

    GLuint vsMask = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsMask = compileShader(GL_FRAGMENT_SHADER, fMaskSrc);
    m_glMaskProgram = glCreateProgram();
    glAttachShader(m_glMaskProgram, vsMask);
    glAttachShader(m_glMaskProgram, fsMask);
    glLinkProgram(m_glMaskProgram);
    glDeleteShader(vsMask);
    glDeleteShader(fsMask);

    m_aMaskPosLoc = glGetAttribLocation(m_glMaskProgram, "aPosition");
    m_aMaskTexLoc = glGetAttribLocation(m_glMaskProgram, "aTexCoord");
    m_uMaskTextureLoc = glGetUniformLocation(m_glMaskProgram, "uTexture");
    m_uMaskSizeLoc = glGetUniformLocation(m_glMaskProgram, "uSizePx");
    m_uMaskRadiusLoc = glGetUniformLocation(m_glMaskProgram, "uRadiusPx");
    m_uMaskRoundnessLoc = glGetUniformLocation(m_glMaskProgram, "uRoundnessExp");
    m_uMaskOpacityLoc = glGetUniformLocation(m_glMaskProgram, "uOpacity");
    m_uMaskSquareTopCornersLoc = glGetUniformLocation(m_glMaskProgram, "uSquareTopCorners");
    m_uMaskSampleOffsetLoc = glGetUniformLocation(m_glMaskProgram, "uSampleOffset");
    m_uMaskSampleScaleLoc = glGetUniformLocation(m_glMaskProgram, "uSampleScale");
    m_uMaskSampleTexelInsetLoc = glGetUniformLocation(
        m_glMaskProgram, "uSampleTexelInset");
    m_uMaskDrawSizeLoc = glGetUniformLocation(m_glMaskProgram, "uDrawSizePx");
    m_uMaskGeometryOffsetLoc = glGetUniformLocation(m_glMaskProgram, "uMaskOffsetPx");

    // --- GLSL Rounded Mask Composite Shader (BGRA-aware client surfaces) ---
    const char* fMaskBgraSrc =
        "precision highp float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uSizePx;\n"
        "uniform vec4 uCornerRadiiPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform float uOpacity;\n"
        "uniform float uTopOnlyCorners;\n"
        "uniform vec2 uSampleScale;\n"
        "uniform vec2 uSampleMax;\n"
        "float sdBox(vec2 p, vec2 b) {\n"
        "    vec2 q = abs(p) - b;\n"
        "    vec2 oq = max(q, 0.0);\n"
        "    return length(oq) + min(max(q.x, q.y), 0.0);\n"
        "}\n"
        "float cornerRadiusForPoint(vec2 p, vec4 radii) {\n"
        "    if (p.x < 0.0) {\n"
        "        return (p.y < 0.0) ? radii.x : radii.w;\n"
        "    }\n"
        "    return (p.y < 0.0) ? radii.y : radii.z;\n"
        "}\n"
        "float sdSuperRoundRect(vec2 p, vec2 b, float r, float n) {\n"
        "    if (r <= 0.001) {\n"
        "        return sdBox(p, b);\n"
        "    }\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    if (q.x <= 0.0 || q.y <= 0.0) {\n"
        "        return max(q.x, q.y) - r;\n"
        "    }\n"
        "    vec2 qq = max(q, 0.0) / max(r, 0.001);\n"
        "    float k = pow(pow(qq.x, n) + pow(qq.y, n), 1.0 / n);\n"
        "    return (k - 1.0) * r;\n"
        "}\n"
        "void main() {\n"
        "    vec2 sampleCoord = min(vTexCoord * uSampleScale, uSampleMax);\n"
        "    vec4 c = texture2D(uTexture, sampleCoord);\n"
        // A top-only rounded rect can have a top radius taller than half the
        // rect, provided its lower corners are square. The width remains the
        // only universal radius limit.
        "    vec4 clampedR = clamp(uCornerRadiiPx, 0.0, uSizePx.x * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 p = (vTexCoord - vec2(0.5)) * uSizePx;\n"
        "    vec2 halfSize = uSizePx * 0.5;\n"
        "    float d;\n"
        "    if (uTopOnlyCorners > 0.5) {\n"
        "        float r = (p.x < 0.0) ? clampedR.x : clampedR.y;\n"
        "        bool inCorner = r > 0.001 && abs(p.x) > (halfSize.x - r) && p.y < (-halfSize.y + r);\n"
        "        if (inCorner) {\n"
        "            vec2 q = vec2(abs(p.x) - (halfSize.x - r), (-halfSize.y + r) - p.y);\n"
        "            float k = pow(pow(q.x / r, n) + pow(q.y / r, n), 1.0 / n);\n"
        "            d = (k - 1.0) * r;\n"
        "        } else {\n"
        "            d = -1.0;\n"
        "        }\n"
        "    } else {\n"
        "        float r = cornerRadiusForPoint(p, clampedR);\n"
        "        d = sdSuperRoundRect(p, halfSize, r, n);\n"
        "    }\n"
        "    float mask = 1.0 - smoothstep(-0.5, 0.5, d);\n"
        "    float coverage = mask * clamp(uOpacity, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(c.bgr * coverage, c.a * coverage);\n"
        "}\n";

    GLuint vsMaskBgra = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsMaskBgra = compileShader(GL_FRAGMENT_SHADER, fMaskBgraSrc);
    m_glMaskBgraProgram = glCreateProgram();
    glAttachShader(m_glMaskBgraProgram, vsMaskBgra);
    glAttachShader(m_glMaskBgraProgram, fsMaskBgra);
    glLinkProgram(m_glMaskBgraProgram);
    glDeleteShader(vsMaskBgra);
    glDeleteShader(fsMaskBgra);

    m_aMaskBgraPosLoc = glGetAttribLocation(m_glMaskBgraProgram, "aPosition");
    m_aMaskBgraTexLoc = glGetAttribLocation(m_glMaskBgraProgram, "aTexCoord");
    m_uMaskBgraTextureLoc = glGetUniformLocation(m_glMaskBgraProgram, "uTexture");
    m_uMaskBgraSizeLoc = glGetUniformLocation(m_glMaskBgraProgram, "uSizePx");
    m_uMaskBgraCornerRadiiLoc = glGetUniformLocation(m_glMaskBgraProgram, "uCornerRadiiPx");
    m_uMaskBgraRoundnessLoc = glGetUniformLocation(m_glMaskBgraProgram, "uRoundnessExp");
    m_uMaskBgraOpacityLoc = glGetUniformLocation(m_glMaskBgraProgram, "uOpacity");
    m_uMaskBgraTopOnlyLoc = glGetUniformLocation(m_glMaskBgraProgram, "uTopOnlyCorners");
    m_uMaskBgraSampleScaleLoc = glGetUniformLocation(m_glMaskBgraProgram, "uSampleScale");
    m_uMaskBgraSampleMaxLoc = glGetUniformLocation(m_glMaskBgraProgram, "uSampleMax");

    // --- GLSL Rounded Rect Fill+Border Shader ---
    const char* vRoundRectSrc =
        "attribute vec2 aPosition;\n"
        "attribute vec2 aTexCoord;\n"
        "varying vec2 vRectPx;\n"
        "uniform vec2 uSizePx;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
        "    vRectPx = aTexCoord * uSizePx;\n"
        "}\n";

    const char* fRoundRectSrc =
        "precision highp float;\n"
        "varying vec2 vRectPx;\n"
        "uniform vec2 uSizePx;\n"
        "uniform float uRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform float uBorderWidthPx;\n"
        "uniform vec4 uFillColor;\n"
        "uniform vec4 uBorderColor;\n"
        "float sdSuperRoundRect(vec2 p, vec2 b, float r, float n) {\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    if (q.x <= 0.0 || q.y <= 0.0) {\n"
        "        return max(q.x, q.y) - r;\n"
        "    }\n"
        "    vec2 qq = max(q, 0.0) / max(r, 0.001);\n"
        "    float k = pow(pow(qq.x, n) + pow(qq.y, n), 1.0 / n);\n"
        "    return (k - 1.0) * r;\n"
        "}\n"
        "void main() {\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    vec2 p = vRectPx - (uSizePx * 0.5);\n"
        "    vec2 halfOuter = uSizePx * 0.5;\n"
        "    float sdOuter = sdSuperRoundRect(p, halfOuter, r, n);\n"
        // Center the one-device-pixel coverage ramp on the analytic edge.
        // The previous [0, 1] ramp marked a pixel exactly on the boundary as
        // fully opaque, which made 16px titlebar controls look stair-stepped.
        // This matches the symmetric subpixel coverage used by the software
        // Canvas path that renders CSD controls into client buffers.
        // A non-rounded rectangle is rasterized by the quad itself; applying
        // a signed-distance edge ramp here would incorrectly make its outer
        // pixel row half-transparent. Keep AA solely for actual curves.
        "    float outerMask = (r <= 0.001) ? 1.0 : (1.0 - smoothstep(-0.5, 0.5, sdOuter));\n"
        "\n"
        "    float bw = max(0.0, uBorderWidthPx);\n"
        "    float innerMask = 0.0;\n"
        "    if (bw > 0.001 && (uSizePx.x - 2.0 * bw) > 0.0 && (uSizePx.y - 2.0 * bw) > 0.0) {\n"
        "        vec2 innerSize = uSizePx - vec2(2.0 * bw);\n"
        "        vec2 halfInner = innerSize * 0.5;\n"
        "        float innerR = max(0.0, r - bw);\n"
        "        float sdInner = sdSuperRoundRect(p, halfInner, innerR, n);\n"
        "        innerMask = (innerR <= 0.001) ? 1.0 : (1.0 - smoothstep(-0.5, 0.5, sdInner));\n"
        "    }\n"
        "\n"
        "    float borderMask = (bw > 0.001) ? max(0.0, outerMask - innerMask) : 0.0;\n"
        "    float fillMask = (bw > 0.001) ? innerMask : outerMask;\n"
        "\n"
        "    float aBorder = uBorderColor.a * borderMask;\n"
        "    float aFill = uFillColor.a * fillMask;\n"
        "    float outA = clamp(aBorder + aFill, 0.0, 1.0);\n"
        "    if (outA <= 0.0001) {\n"
        "        discard;\n"
        "    }\n"
        "\n"
        "    vec3 outRgb = uBorderColor.rgb * aBorder + uFillColor.rgb * aFill;\n"
        "    gl_FragColor = vec4(outRgb, outA);\n"
        "}\n";

    GLuint vsRoundRect = compileShader(GL_VERTEX_SHADER, vRoundRectSrc);
    GLuint fsRoundRect = compileShader(GL_FRAGMENT_SHADER, fRoundRectSrc);
    m_glRoundRectProgram = glCreateProgram();
    glAttachShader(m_glRoundRectProgram, vsRoundRect);
    glAttachShader(m_glRoundRectProgram, fsRoundRect);
    glLinkProgram(m_glRoundRectProgram);
    glDeleteShader(vsRoundRect);
    glDeleteShader(fsRoundRect);

    m_aRoundRectPosLoc = glGetAttribLocation(m_glRoundRectProgram, "aPosition");
    m_aRoundRectTexLoc = glGetAttribLocation(m_glRoundRectProgram, "aTexCoord");
    m_uRoundRectSizeLoc = glGetUniformLocation(m_glRoundRectProgram, "uSizePx");
    m_uRoundRectRadiusLoc = glGetUniformLocation(m_glRoundRectProgram, "uRadiusPx");
    m_uRoundRectRoundnessLoc = glGetUniformLocation(m_glRoundRectProgram, "uRoundnessExp");
    m_uRoundRectBorderWidthLoc = glGetUniformLocation(m_glRoundRectProgram, "uBorderWidthPx");
    m_uRoundRectFillColorLoc = glGetUniformLocation(m_glRoundRectProgram, "uFillColor");
    m_uRoundRectBorderColorLoc = glGetUniformLocation(m_glRoundRectProgram, "uBorderColor");

    // --- GLSL Refraction Pass Shader (thickness/refraction/dispersion core) ---
    const char* fRefractSrc =
        "precision highp float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uInvSize;\n"
        "uniform float uThicknessPx;\n"
        "uniform float uRefractionFactor;\n"
        "uniform float uDispersionGain;\n"
        "uniform vec2 uSizePx;\n"
        "uniform vec2 uCaptureSizePx;\n"
        "uniform vec2 uMaskOffsetPx;\n"
        "uniform float uRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform vec2 uInputScale;\n"
        "uniform vec2 uLogicalToPassScale;\n"
        "float sdSuperRoundRect(vec2 p, vec2 b, float r, float n) {\n"
        "    if (r <= 0.001) return max(abs(p).x - b.x, abs(p).y - b.y);\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    if (q.x <= 0.0 || q.y <= 0.0) return max(q.x, q.y) - r;\n"
        "    vec2 qq = max(q, 0.0) / r;\n"
        "    float k = pow(pow(qq.x, n) + pow(qq.y, n), 1.0 / n);\n"
        "    return (k - 1.0) * r;\n"
        "}\n"
        "float safeAsin(float x) {\n"
        "    return asin(clamp(x, -1.0, 1.0));\n"
        "}\n"
        "void main() {\n"
        // The FBO texture can be larger than the active capture. Clamp in the
        // capture's normalized space to texel centers before applying
        // uInputScale; sampling 1.0 would otherwise linearly blend the last
        // valid texel with stale capacity pixels just outside the capture.
        "    vec2 halfTexel = vec2(0.5) * uInvSize;\n"
        "    vec2 sampleMin = halfTexel;\n"
        "    vec2 sampleMax = vec2(1.0) - halfTexel;\n"
        "    vec2 baseUv = clamp(vTexCoord, sampleMin, sampleMax);\n"
        "    vec4 base = texture2D(uTexture, baseUv * uInputScale);\n"
        "    float thickness = max(0.001, uThicknessPx);\n"
        "    float eta = max(1.001, uRefractionFactor);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 maskCoord = vec2(\n"
        "        uMaskOffsetPx.x + vTexCoord.x * uCaptureSizePx.x,\n"
        "        uMaskOffsetPx.y + (1.0 - vTexCoord.y) * uCaptureSizePx.y);\n"
        "    vec2 p = vec2(maskCoord.x - uSizePx.x * 0.5,\n"
        "                  uSizePx.y * 0.5 - maskCoord.y);\n"
        "    vec2 b = uSizePx * 0.5;\n"
        "    float sd = sdSuperRoundRect(p, b, r, n);\n"
        "    float edgeDepth = max(0.0, -sd);\n"
        "    if (sd >= 0.0 || edgeDepth >= thickness) {\n"
        "        gl_FragColor = base;\n"
        "        return;\n"
        "    }\n"
        "    float eps = 1.0;\n"
        "    float sdx = sdSuperRoundRect(p + vec2(eps, 0.0), b, r, n) - sdSuperRoundRect(p - vec2(eps, 0.0), b, r, n);\n"
        "    float sdy = sdSuperRoundRect(p + vec2(0.0, eps), b, r, n) - sdSuperRoundRect(p - vec2(0.0, eps), b, r, n);\n"
        "    vec2 gradient = vec2(sdx, sdy);\n"
        "    float gradientLength = length(gradient);\n"
        "    if (gradientLength <= 0.0001) {\n"
        "        gl_FragColor = base;\n"
        "        return;\n"
        "    }\n"
        "    vec2 normal = gradient / gradientLength;\n"
        "    float xRatio = 1.0 - edgeDepth / thickness;\n"
        "    float thetaI = safeAsin(xRatio * xRatio);\n"
        "    float thetaT = safeAsin((1.0 / eta) * sin(thetaI));\n"
        "    float edgeFactor = max(0.0, -tan(thetaT - thetaI));\n"
        "    edgeFactor = min(edgeFactor, 4.0);\n"
        // Preserve the existing 1x refraction strength while expressing its
        // displacement in logical pixels. The pass scale converts that
        // displacement after any intermediate downsample.
        "    vec2 displacementPx = (-normal * edgeFactor * 70.0) * uLogicalToPassScale;\n"
        "    vec2 offsetUv = displacementPx * uInvSize;\n"
        "    float disp = max(0.0, uDispersionGain) * 0.02;\n"
        "    vec2 uvR = clamp(vTexCoord + offsetUv * (1.0 + disp), sampleMin, sampleMax);\n"
        "    vec2 uvG = clamp(vTexCoord + offsetUv, sampleMin, sampleMax);\n"
        "    vec2 uvB = clamp(vTexCoord + offsetUv * (1.0 - disp), sampleMin, sampleMax);\n"
        "    vec4 sampleR = texture2D(uTexture, uvR * uInputScale);\n"
        "    vec4 sampleG = texture2D(uTexture, uvG * uInputScale);\n"
        "    vec4 sampleB = texture2D(uTexture, uvB * uInputScale);\n"
        "    float rCh = sampleR.a > 0.0001 ? sampleR.r / sampleR.a : 0.0;\n"
        "    float gCh = sampleG.a > 0.0001 ? sampleG.g / sampleG.a : 0.0;\n"
        "    float bCh = sampleB.a > 0.0001 ? sampleB.b / sampleB.a : 0.0;\n"
        "    gl_FragColor = vec4(vec3(rCh, gCh, bCh) * base.a, base.a);\n"
        "}\n";

    GLuint vsRefract = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsRefract = compileShader(GL_FRAGMENT_SHADER, fRefractSrc);
    m_glRefractionProgram = glCreateProgram();
    glAttachShader(m_glRefractionProgram, vsRefract);
    glAttachShader(m_glRefractionProgram, fsRefract);
    glLinkProgram(m_glRefractionProgram);
    glDeleteShader(vsRefract);
    glDeleteShader(fsRefract);

    m_aRefractPosLoc = glGetAttribLocation(m_glRefractionProgram, "aPosition");
    m_aRefractTexLoc = glGetAttribLocation(m_glRefractionProgram, "aTexCoord");
    m_uRefractTextureLoc = glGetUniformLocation(m_glRefractionProgram, "uTexture");
    m_uRefractInvSizeLoc = glGetUniformLocation(m_glRefractionProgram, "uInvSize");
    m_uRefractThicknessLoc = glGetUniformLocation(m_glRefractionProgram, "uThicknessPx");
    m_uRefractFactorLoc = glGetUniformLocation(m_glRefractionProgram, "uRefractionFactor");
    m_uRefractDispersionLoc = glGetUniformLocation(m_glRefractionProgram, "uDispersionGain");
    m_uRefractSizeLoc = glGetUniformLocation(m_glRefractionProgram, "uSizePx");
    m_uRefractCaptureSizeLoc = glGetUniformLocation(m_glRefractionProgram, "uCaptureSizePx");
    m_uRefractMaskOffsetLoc = glGetUniformLocation(m_glRefractionProgram, "uMaskOffsetPx");
    m_uRefractRadiusLoc = glGetUniformLocation(m_glRefractionProgram, "uRadiusPx");
    m_uRefractRoundnessLoc = glGetUniformLocation(m_glRefractionProgram, "uRoundnessExp");
    m_uRefractInputScaleLoc = glGetUniformLocation(m_glRefractionProgram, "uInputScale");
    m_uRefractLogicalToPassScaleLoc = glGetUniformLocation(
        m_glRefractionProgram, "uLogicalToPassScale");

    // --- GLSL BGRA Client Surface Fragment Shader ---
    const char* fBgraSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform float uOpacity;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    float opacity = clamp(uOpacity, 0.0, 1.0);\n"
        "    gl_FragColor = vec4(c.bgr * opacity, c.a * opacity);\n"
        "}\n";

    GLuint vsBgra = compileShader(GL_VERTEX_SHADER, vSrc);
    GLuint fsBgra = compileShader(GL_FRAGMENT_SHADER, fBgraSrc);
    m_glBgraProgram = glCreateProgram();
    glAttachShader(m_glBgraProgram, vsBgra);
    glAttachShader(m_glBgraProgram, fsBgra);
    glLinkProgram(m_glBgraProgram);
    glDeleteShader(vsBgra);
    glDeleteShader(fsBgra);

    m_aBgraPosLoc = glGetAttribLocation(m_glBgraProgram, "aPosition");
    m_aBgraTexLoc = glGetAttribLocation(m_glBgraProgram, "aTexCoord");
    m_uBgraTextureLoc = glGetUniformLocation(m_glBgraProgram, "uTexture");
    m_uBgraOpacityLoc = glGetUniformLocation(m_glBgraProgram, "uOpacity");

    // --- Initialize GLES2 Ping-Pong Framebuffer Objects (FBOs) ---
    glGenTextures(2, m_glFBOTexture);
    glGenFramebuffers(2, m_glFBO);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

        glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_glFBOTexture[i], 0);
    }

    // --- Initialize Pure GPU Scene FBO & Texture ---
    glGenTextures(1, &m_glSceneTexture);
    glBindTexture(GL_TEXTURE_2D, m_glSceneTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &m_glSceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_glSceneTexture, 0);
    m_glSceneCapacityWidth = m_width;
    m_glSceneCapacityHeight = m_height;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glFBOReady = true;
    m_glFBOCapacityWidth = m_width;
    m_glFBOCapacityHeight = m_height;

    return true;
}
#else
bool RasterRenderer::initGLShader() {
    return false;
}
#endif

RasterRenderer::~RasterRenderer() {
    shutdown();
}

bool RasterRenderer::initialize(uint32_t width, uint32_t height,
                              lcl::platform::IGraphicsContext* eglBackend,
                              uint32_t* targetPixels) {
    if (targetPixels) {
        m_targetPixels = targetPixels;
    }
    if (width > 0) m_width = width;
    if (height > 0) m_height = height;

    if (m_initialized) return true;

#ifdef LCL_SOFTWARE_ONLY
    (void)eglBackend;
    m_eglBackend = nullptr;
#else
    m_eglBackend = eglBackend;
    if (m_eglBackend && m_eglBackend->isInitialized()) {
        m_backendType = RasterBackend::OpenGL_EGL;
        m_eglBackend->makeCurrent();
        m_glSupportsUnpackRowLength = supportsUnpackRowLength();

        initGLShader();

        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::cout << "[LCL Raster] LCL raster OpenGL/EGL Hardware Accelerated Backend Active ("
                  << m_width << "x" << m_height << ")!\n";
    } else
#endif
    {
        m_backendType = RasterBackend::SoftwareRaster;
        if (!m_targetPixels) {
            m_rasterPixels.resize(m_width * m_height, 0xFF14161D); // Dark theme default
            m_targetPixels = m_rasterPixels.data();
        }
        std::cout << "[LCL Raster] LCL raster Raster Software Backend Active ("
                  << m_width << "x" << m_height << ").\n";
    }

    m_initialized = true;
    return true;
}

void RasterRenderer::setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) {
    const uint32_t nextWidth = width > 0 ? width : m_width;
    const uint32_t nextHeight = height > 0 ? height : m_height;
    const bool sizeChanged = nextWidth != m_width || nextHeight != m_height;

#ifndef LCL_SOFTWARE_ONLY
    if (sizeChanged && m_initialized && m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        auto* eglBackend = m_eglBackend;
        if (eglBackend->resize(nextWidth, nextHeight)) {
            eglBackend->makeCurrent();
            shutdown();
            initialize(nextWidth, nextHeight, eglBackend, targetPixels);
            return;
        }
        // A failed client-context resize falls back to the established SHM CPU
        // renderer rather than risking a stale GPU surface.
        m_eglBackend = nullptr;
        m_backendType = RasterBackend::SoftwareRaster;
    }
#endif

    m_targetPixels = targetPixels;
    m_width = nextWidth;
    m_height = nextHeight;
}

void RasterRenderer::setFrameExtent(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_width = width;
    m_height = height;
}

bool RasterRenderer::ensureFrameBackingCapacity(uint32_t width, uint32_t height) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend ||
        m_glSceneTexture == 0 || width == 0 || height == 0) {
        return m_backendType != RasterBackend::OpenGL_EGL;
    }
    if (width <= m_glSceneCapacityWidth && height <= m_glSceneCapacityHeight) {
        return true;
    }
    const uint32_t nextWidth = std::max(width, m_glSceneCapacityWidth);
    const uint32_t nextHeight = std::max(height, m_glSceneCapacityHeight);
    m_eglBackend->makeCurrent();
    glBindTexture(GL_TEXTURE_2D, m_glSceneTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 static_cast<GLsizei>(nextWidth),
                 static_cast<GLsizei>(nextHeight), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, m_glSceneTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return false;
    }
    m_glSceneCapacityWidth = nextWidth;
    m_glSceneCapacityHeight = nextHeight;
    return true;
#else
    (void)width;
    (void)height;
    return true;
#endif
}

void RasterRenderer::setExternalFrameTarget(uint32_t framebuffer, uint32_t texture,
                                          uint32_t backingWidth, uint32_t backingHeight) {
#ifndef LCL_SOFTWARE_ONLY
    m_glOutputFrameFBO = framebuffer;
    (void)texture;
    (void)backingWidth;
    (void)backingHeight;
#else
    (void)framebuffer;
    (void)texture;
#endif
}

void RasterRenderer::clearExternalFrameTarget() {
#ifndef LCL_SOFTWARE_ONLY
    m_glOutputFrameFBO = 0;
#endif
}

bool RasterRenderer::createCachedLayerTarget(uint32_t width, uint32_t height,
                                           uint32_t& framebuffer, uint32_t& texture) {
    framebuffer = 0;
    texture = 0;
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend ||
        width == 0 || height == 0) return false;
    m_eglBackend->makeCurrent();

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           texture, 0);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
    glViewport(0, 0, m_width, m_height);
    if (!complete) {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteTextures(1, &texture);
        framebuffer = 0;
        texture = 0;
    }
    return complete;
#else
    (void)width;
    (void)height;
    return false;
#endif
}

void RasterRenderer::destroyCachedLayerTarget(uint32_t framebuffer, uint32_t texture) {
#ifndef LCL_SOFTWARE_ONLY
    if (!m_eglBackend) return;
    m_eglBackend->makeCurrent();
    if (framebuffer != 0) glDeleteFramebuffers(1, &framebuffer);
    if (texture != 0) glDeleteTextures(1, &texture);
#else
    (void)framebuffer;
    (void)texture;
#endif
}

bool RasterRenderer::beginCachedLayerTarget(uint32_t framebuffer, uint32_t texture,
                                          uint32_t width, uint32_t height,
                                          uint32_t* softwarePixels,
                                          float logicalOriginX, float logicalOriginY,
                                          float effectiveScale,
                                          std::optional<lcl::graphics::RectF> updateBounds) {
    if (!m_initialized || width == 0 || height == 0) return false;
    const bool gpu = m_backendType == RasterBackend::OpenGL_EGL;
    if ((gpu && (framebuffer == 0 || texture == 0 || !m_eglBackend)) ||
        (!gpu && !softwarePixels)) return false;

    m_cachedLayerTargetStates.push_back(CachedLayerTargetState{
        m_width, m_height, m_targetPixels, m_contentOriginX, m_contentOriginY,
        m_deviceScale,
        m_clipRect, m_glExternalFrameFBO, m_glExternalFrameTexture,
        m_glExternalBackingWidth, m_glExternalBackingHeight});
    setDeviceScale(effectiveScale);
    m_width = width;
    m_height = height;
    m_targetPixels = softwarePixels;
    m_contentOriginX = -logicalOriginX * m_deviceScale;
    m_contentOriginY = -logicalOriginY * m_deviceScale;
    m_clipRect.reset();

#ifndef LCL_SOFTWARE_ONLY
    if (gpu) {
        m_glExternalFrameFBO = framebuffer;
        m_glExternalFrameTexture = texture;
        m_glExternalBackingWidth = width;
        m_glExternalBackingHeight = height;
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, width, height);
        if (!updateBounds) {
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    } else
#endif
    {
        if (!updateBounds) {
            std::fill_n(softwarePixels, static_cast<size_t>(width) * height,
                        0x00000000u);
        }
    }
    if (updateBounds) {
        clearRect({updateBounds->x, updateBounds->y,
                   updateBounds->width, updateBounds->height},
                  {0, 0, 0, 0});
        m_clipRect = RasterRect{
            updateBounds->x,
            updateBounds->y,
            updateBounds->width,
            updateBounds->height,
        };
        applyScissorState();
    }
    return true;
}

void RasterRenderer::endCachedLayerTarget() {
    if (m_cachedLayerTargetStates.empty()) return;
    const CachedLayerTargetState state = m_cachedLayerTargetStates.back();
    m_cachedLayerTargetStates.pop_back();
    m_width = state.width;
    m_height = state.height;
    m_targetPixels = state.targetPixels;
    m_contentOriginX = state.contentOriginX;
    m_contentOriginY = state.contentOriginY;
    setDeviceScale(state.deviceScale);
    m_clipRect = state.clip;
    m_glExternalFrameFBO = state.externalFrameFBO;
    m_glExternalFrameTexture = state.externalFrameTexture;
    m_glExternalBackingWidth = state.externalBackingWidth;
    m_glExternalBackingHeight = state.externalBackingHeight;
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
    }
#endif
    applyScissorState();
}

void RasterRenderer::drawCachedLayerTexture(uint32_t texture,
                                          const RasterRect& destination,
                                          float opacity) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend || texture == 0) return;
    const RasterRect deviceRect = scaleRect(destination);
    m_eglBackend->makeCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
    glViewport(0, 0, m_width, m_height);
    drawTextureQuad(texture, deviceRect.x, deviceRect.y,
                    deviceRect.width, deviceRect.height, opacity);
#else
    (void)texture;
    (void)destination;
    (void)opacity;
#endif
}

bool RasterRenderer::prepareCachedDisplayLayer(
        uint64_t id, const lcl::graphics::RectF& sourceBounds,
        const lcl::graphics::Matrix3& transform,
        const lcl::graphics::RenderTarget& target,
        bool preserveContents) {
    if (sourceBounds.width <= 0.0f || sourceBounds.height <= 0.0f) return false;
    const float effectiveScale = std::max(
        0.001f, target.deviceScale * transform.maxScale());
    const uint32_t pixelWidth = std::max(
        1u, static_cast<uint32_t>(std::ceil(sourceBounds.width * effectiveScale)));
    const uint32_t pixelHeight = std::max(
        1u, static_cast<uint32_t>(std::ceil(sourceBounds.height * effectiveScale)));
    const auto sameTransform = [](const auto& lhs, const auto& rhs) {
        return std::fabs(lhs.a - rhs.a) < 0.0001f &&
               std::fabs(lhs.b - rhs.b) < 0.0001f &&
               std::fabs(lhs.c - rhs.c) < 0.0001f &&
               std::fabs(lhs.d - rhs.d) < 0.0001f &&
               std::fabs(lhs.tx - rhs.tx) < 0.0001f &&
               std::fabs(lhs.ty - rhs.ty) < 0.0001f;
    };

    const auto matches = [&](const CachedDisplayLayer& layer) {
        return layer.pixelWidth == pixelWidth &&
            layer.pixelHeight == pixelHeight &&
            std::fabs(layer.effectiveScale - effectiveScale) <= 0.0001f &&
            sameTransform(layer.transform, transform);
    };
    const bool gpu = m_backendType == RasterBackend::OpenGL_EGL;
    const auto found = m_cachedDisplayLayers.find(id);
    if (preserveContents) {
        if (found == m_cachedDisplayLayers.end() || !matches(found->second)) {
            return false;
        }
        if (gpu) {
            return found->second.framebuffer != 0 && found->second.texture != 0;
        }
        return found->second.pixels.size() ==
            static_cast<size_t>(pixelWidth) * pixelHeight;
    }

    auto& layer = m_cachedDisplayLayers[id];
    if (!matches(layer)) {
        destroyCachedLayerTarget(layer.framebuffer, layer.texture);
        layer = CachedDisplayLayer{};
        layer.pixelWidth = pixelWidth;
        layer.pixelHeight = pixelHeight;
        layer.transform = transform;
        layer.effectiveScale = effectiveScale;
    }

    if (gpu && layer.framebuffer == 0 &&
        !createCachedLayerTarget(pixelWidth, pixelHeight,
                                 layer.framebuffer, layer.texture)) {
        m_cachedDisplayLayers.erase(id);
        return false;
    }
    if (!gpu && layer.pixels.size() != static_cast<size_t>(pixelWidth) * pixelHeight) {
        layer.pixels.resize(static_cast<size_t>(pixelWidth) * pixelHeight);
    }
    return true;
}

bool RasterRenderer::hasCachedDisplayLayer(uint64_t id) const {
    return m_cachedDisplayLayers.find(id) != m_cachedDisplayLayers.end();
}

bool RasterRenderer::updateCachedDisplayLayer(
        uint64_t id, const lcl::graphics::RectF& logicalBounds,
        const lcl::graphics::DisplayList& displayList,
        const lcl::graphics::RenderTarget& target) {
    const lcl::graphics::Matrix3 identity{};
    if (!prepareCachedDisplayLayer(id, logicalBounds, identity, target, false)) {
        return false;
    }
    auto found = m_cachedDisplayLayers.find(id);
    if (found == m_cachedDisplayLayers.end()) return false;
    auto& layer = found->second;
    uint32_t* softwarePixels = layer.texture != 0 ? nullptr : layer.pixels.data();
    if (!beginCachedLayerTarget(
            layer.framebuffer, layer.texture, layer.pixelWidth, layer.pixelHeight,
            softwarePixels, logicalBounds.x, logicalBounds.y,
            layer.effectiveScale, std::nullopt)) {
        return false;
    }
    replayDisplayList(displayList, target);
    endCachedLayerTarget();
    return true;
}

bool RasterRenderer::drawCachedDisplayLayer(uint64_t id,
                                            const RasterRect& destination,
                                            float opacity,
                                            float cornerRadius,
                                            float cornerRoundness,
                                            bool squareTopCorners,
                                            RasterBufferSampling sampling) {
    const auto found = m_cachedDisplayLayers.find(id);
    if (found == m_cachedDisplayLayers.end()) return false;
    const auto& layer = found->second;
    if (layer.texture != 0) {
        drawDmaBufTextureTransformed(
            destination.x, destination.y,
            static_cast<int>(layer.pixelWidth), static_cast<int>(layer.pixelHeight),
            static_cast<int>(layer.pixelWidth), static_cast<int>(layer.pixelHeight),
            layer.texture, opacity, cornerRadius, cornerRoundness,
            squareTopCorners, destination.width, destination.height, sampling);
        return true;
    }
    if (layer.pixels.empty()) return false;
    drawBufferTransformed(
        destination.x, destination.y,
        static_cast<int>(layer.pixelWidth), static_cast<int>(layer.pixelHeight),
        layer.pixels.data(), static_cast<int>(layer.pixelWidth), opacity,
        cornerRadius, cornerRoundness, squareTopCorners,
        destination.width, destination.height, sampling);
    return true;
}

void RasterRenderer::releaseCachedDisplayLayer(uint64_t id) {
    const auto found = m_cachedDisplayLayers.find(id);
    if (found == m_cachedDisplayLayers.end()) return;
    destroyCachedLayerTarget(found->second.framebuffer, found->second.texture);
    m_cachedDisplayLayers.erase(found);
}

void RasterRenderer::clearDisplayListCaches() {
    for (const auto& [_, layer] : m_cachedDisplayLayers) {
        destroyCachedLayerTarget(layer.framebuffer, layer.texture);
    }
    m_cachedDisplayLayers.clear();
    m_rasterizedTextLayers.clear();
}

void RasterRenderer::shutdown() {
    // A Canvas normally balances cached-target redirection before shutdown.
    // Drop any saved target state defensively so a reinitialized renderer can
    // never restore dimensions or pointers owned by its previous lifetime.
    m_cachedLayerTargetStates.clear();
    clearDisplayListCaches();
#ifndef LCL_SOFTWARE_ONLY
    // Several WindowApps may interleave independent EGL contexts on one
    // thread. Resource names are context-local, so deleting this renderer's
    // numeric handles while another WindowApp is current can delete that
    // renderer's textures/FBOs/programs instead.
    const bool canDeleteGlResources =
        m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend &&
        m_eglBackend->makeCurrent();
    for (const auto& [_, cached] : m_cachedShmTextures) {
        if (canDeleteGlResources && cached.texture > 0) {
            glDeleteTextures(1, &cached.texture);
        }
    }
    m_cachedShmTextures.clear();
    m_shmTextureFrameSerial = 0;
    for (const auto& [_, cached] : m_cachedImageTextures) {
        if (canDeleteGlResources && cached.texture > 0) {
            glDeleteTextures(1, &cached.texture);
        }
    }
    m_cachedImageTextures.clear();
    m_cachedImageTextureBytes = 0;
    m_imageTextureUseCounter = 0;
    if (m_glClientTexture > 0) {
        if (canDeleteGlResources) glDeleteTextures(1, &m_glClientTexture);
        m_glClientTexture = 0;
        m_glClientTextureWidth = 0;
        m_glClientTextureHeight = 0;
    }
    if (m_glBgraProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glBgraProgram);
        m_glBgraProgram = 0;
    }
    if (m_glSceneFBO > 0) {
        if (canDeleteGlResources) {
            glDeleteFramebuffers(1, &m_glSceneFBO);
            glDeleteTextures(1, &m_glSceneTexture);
        }
        m_glSceneFBO = 0;
        m_glSceneTexture = 0;
        m_glSceneCapacityWidth = 0;
        m_glSceneCapacityHeight = 0;
    }
    m_glExternalFrameFBO = 0;
    m_glExternalFrameTexture = 0;
    m_glOutputFrameFBO = 0;
    if (m_glFBOReady) {
        if (canDeleteGlResources) {
            glDeleteFramebuffers(2, m_glFBO);
            glDeleteTextures(2, m_glFBOTexture);
        }
        m_glFBO[0] = m_glFBO[1] = 0;
        m_glFBOTexture[0] = m_glFBOTexture[1] = 0;
        m_glFBOReady = false;
        m_glFBOCapacityWidth = 0;
        m_glFBOCapacityHeight = 0;
    }
    if (m_glBlurProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glBlurProgram);
        m_glBlurProgram = 0;
    }
    if (m_glColorMatrixProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glColorMatrixProgram);
        m_glColorMatrixProgram = 0;
    }
    if (m_glMaskProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glMaskProgram);
        m_glMaskProgram = 0;
    }
    if (m_glMaskBgraProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glMaskBgraProgram);
        m_glMaskBgraProgram = 0;
    }
    if (m_glRoundRectProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glRoundRectProgram);
        m_glRoundRectProgram = 0;
    }
    if (m_glRefractionProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glRefractionProgram);
        m_glRefractionProgram = 0;
    }
    if (m_glTexture > 0) {
        if (canDeleteGlResources) glDeleteTextures(1, &m_glTexture);
        m_glTexture = 0;
    }
    if (m_glProgram > 0) {
        if (canDeleteGlResources) glDeleteProgram(m_glProgram);
        m_glProgram = 0;
    }
#endif
    m_rasterPixels.clear();
    m_rasterPixels.shrink_to_fit();
    m_targetPixels = nullptr;
    m_initialized = false;
}

void RasterRenderer::setDeviceScale(float scale) {
    const float sanitized = (std::isfinite(scale) && scale >= 0.5f && scale <= 4.0f)
        ? scale
        : 1.0f;
    if (std::fabs(m_deviceScale - sanitized) < 0.0001f) {
        return;
    }

    m_deviceScale = sanitized;
    // Glyph bitmaps are raster assets, so rebuilding the cache prevents a scaled
    // client surface from reusing 1x text.
    m_fontRenderer = FontRenderer{};
    m_monospaceFontRenderer = FontRenderer{};
}

void RasterRenderer::drawPath(const lcl::graphics::Path& path,
                              const lcl::graphics::Paint& paint,
                              const lcl::graphics::Matrix3& logicalTransform,
                              float inheritedOpacity) {
    if (path.empty() || paint.color.a == 0 || paint.opacity <= 0.0f ||
        inheritedOpacity <= 0.0f) return;

#ifndef LCL_SOFTWARE_ONLY
    const auto* primitive = path.primitive();
    const float scaleX = std::hypot(logicalTransform.a, logicalTransform.b);
    const float scaleY = std::hypot(logicalTransform.c, logicalTransform.d);
    const bool axisAligned = std::fabs(logicalTransform.b) < 0.0001f &&
                             std::fabs(logicalTransform.c) < 0.0001f;
    const bool uniformScale = std::fabs(scaleX - scaleY) < 0.0001f;
    const bool roundPrimitive = primitive &&
        (primitive->kind == lcl::graphics::PathPrimitiveKind::RRect ||
         primitive->kind == lcl::graphics::PathPrimitiveKind::Ellipse ||
         primitive->kind == lcl::graphics::PathPrimitiveKind::TopRRect);
    const bool compatibleRadii = primitive &&
        std::fabs(primitive->radiusX - primitive->radiusY) < 0.0001f;

    if (m_backendType == RasterBackend::OpenGL_EGL && primitive && axisAligned &&
        (!roundPrimitive || (uniformScale && compatibleRadii))) {
        const auto mapped = logicalTransform.mapRect(primitive->bounds);
        RasterRect bounds{mapped.x, mapped.y, mapped.width, mapped.height};
        RasterColor color{paint.color.r, paint.color.g, paint.color.b,
            static_cast<uint8_t>(std::clamp(std::lround(
                static_cast<float>(paint.color.a) *
                std::clamp(paint.opacity * inheritedOpacity, 0.0f, 1.0f)),
                0l, 255l))};
        if (color.a == 0) return;

        const float logicalScale = std::max(scaleX, scaleY);
        float radius = primitive->radiusX * logicalScale;
        const float roundness = primitive->roundness;
        if (paint.style == lcl::graphics::PaintStyle::Fill) {
            switch (primitive->kind) {
                case lcl::graphics::PathPrimitiveKind::Rect:
                    drawRect(bounds, color);
                    break;
                case lcl::graphics::PathPrimitiveKind::TopRRect:
                    drawTopRoundedRect(bounds, radius, color, roundness);
                    break;
                case lcl::graphics::PathPrimitiveKind::RRect:
                case lcl::graphics::PathPrimitiveKind::Ellipse:
                    drawRoundedRect(bounds, radius, color, {0, 0, 0, 0},
                                    0.0f, roundness);
                    break;
            }
            return;
        }

        if (primitive->kind != lcl::graphics::PathPrimitiveKind::TopRRect) {
            const float strokeWidth =
                paint.stroke.scaling == lcl::graphics::StrokeScaling::Hairline
                    ? 1.0f / std::max(0.001f, m_deviceScale)
                    : paint.stroke.width * logicalScale;
            if (strokeWidth <= 0.0f) return;
            // The rounded-rect shader paints its border inward. Expand by half
            // the width so the result remains a centered path stroke.
            const float halfStroke = strokeWidth * 0.5f;
            bounds.x -= halfStroke;
            bounds.y -= halfStroke;
            bounds.width += strokeWidth;
            bounds.height += strokeWidth;
            if (primitive->kind != lcl::graphics::PathPrimitiveKind::Rect) {
                radius += halfStroke;
            }
            drawRoundedRect(bounds, radius, {0, 0, 0, 0}, color,
                            strokeWidth, roundness);
            return;
        }
    }
#endif

    const auto deviceTransform = logicalTransform.followedBy(
        lcl::graphics::Matrix3::scale(m_deviceScale, m_deviceScale));
    const auto rasterized = rasterizePath(path, paint, deviceTransform,
                                          inheritedOpacity);
    if (rasterized.empty()) return;
    drawBufferTransformed(
        static_cast<float>(rasterized.x) / m_deviceScale,
        static_cast<float>(rasterized.y) / m_deviceScale,
        rasterized.width, rasterized.height, rasterized.pixels.data(),
        rasterized.width, 1.0f, 0.0f, 2.0f, false,
        static_cast<float>(rasterized.width) / m_deviceScale,
        static_cast<float>(rasterized.height) / m_deviceScale);
}

void RasterRenderer::replayDisplayList(
        const lcl::graphics::DisplayList& displayList,
        const lcl::graphics::RenderTarget& target,
        const lcl::graphics::Matrix3& rootTransform) {
    struct ReplayState {
        lcl::graphics::Matrix3 transform{};
        float opacity{1.0f};
        std::optional<lcl::graphics::RectF> clip{};
        std::vector<RasterizedPath> pathClips;
    };

    const float previousScale = m_deviceScale;
    const auto previousClip = m_clipRect;
    setDeviceScale(target.deviceScale);
    ReplayState state{};
    state.transform = rootTransform;
    std::vector<ReplayState> stack;
    std::vector<float> layerOpacityStack;
    std::optional<ReplayState> cachedLayerReplayState;

    const auto concat = [](const lcl::graphics::Matrix3& old,
                           const lcl::graphics::Matrix3& value) {
        return lcl::graphics::Matrix3{
            old.a * value.a + old.c * value.b,
            old.b * value.a + old.d * value.b,
            old.a * value.c + old.c * value.d,
            old.b * value.c + old.d * value.d,
            old.a * value.tx + old.c * value.ty + old.tx,
            old.b * value.tx + old.d * value.ty + old.ty,
        };
    };
    const auto syncClip = [&] {
        if (state.clip) {
            setClipRect(RasterRect{state.clip->x, state.clip->y,
                                   state.clip->width, state.clip->height});
        } else {
            setClipRect(std::nullopt);
        }
    };

    for (const auto& command : displayList.commands()) {
        std::visit([&](const auto& op) {
            using T = std::decay_t<decltype(op)>;
            if constexpr (std::is_same_v<T, lcl::graphics::SaveCommand>) {
                stack.push_back(state);
            } else if constexpr (std::is_same_v<T, lcl::graphics::RestoreCommand>) {
                if (!stack.empty()) {
                    state = stack.back();
                    stack.pop_back();
                    syncClip();
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::ConcatCommand>) {
                state.transform = concat(state.transform, op.transform);
            } else if constexpr (std::is_same_v<T, lcl::graphics::BeginLayerCommand>) {
                layerOpacityStack.push_back(state.opacity);
                state.opacity *= std::clamp(op.opacity, 0.0f, 1.0f);
            } else if constexpr (std::is_same_v<T, lcl::graphics::EndLayerCommand>) {
                if (!layerOpacityStack.empty()) {
                    state.opacity = layerOpacityStack.back();
                    layerOpacityStack.pop_back();
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClipRectCommand>) {
                const auto mapped = state.transform.mapRect(op.rect);
                state.clip = state.clip ? state.clip->intersection(mapped) : mapped;
                syncClip();
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClipPathCommand>) {
                lcl::graphics::Paint clipPaint;
                clipPaint.color = {255, 255, 255, 255};
                clipPaint.fillRule = op.fillRule;
                const auto deviceTransform = state.transform.followedBy(
                    lcl::graphics::Matrix3::scale(m_deviceScale, m_deviceScale));
                auto mask = rasterizePath(op.path, clipPaint, deviceTransform);
                if (!mask.empty()) {
                    const lcl::graphics::RectF bounds{
                        static_cast<float>(mask.x) / m_deviceScale,
                        static_cast<float>(mask.y) / m_deviceScale,
                        static_cast<float>(mask.width) / m_deviceScale,
                        static_cast<float>(mask.height) / m_deviceScale};
                    state.clip = state.clip ? state.clip->intersection(bounds) : bounds;
                    state.pathClips.push_back(std::move(mask));
                    syncClip();
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::ClearRectCommand>) {
                const auto mapped = state.transform.mapRect(op.rect);
                clearRect({mapped.x, mapped.y, mapped.width, mapped.height},
                          {op.color.r, op.color.g, op.color.b, op.color.a});
            } else if constexpr (std::is_same_v<T, lcl::graphics::BeginCachedLayerCommand>) {
                if (cachedLayerReplayState) return;
                if (!prepareCachedDisplayLayer(
                        op.id, op.sourceBounds, state.transform, target,
                        op.updateBounds.has_value())) {
                    return;
                }
                const auto found = m_cachedDisplayLayers.find(op.id);
                if (found == m_cachedDisplayLayers.end()) return;
                auto& layer = found->second;
                uint32_t* softwarePixels = layer.texture != 0
                    ? nullptr : layer.pixels.data();
                if (!beginCachedLayerTarget(
                        layer.framebuffer, layer.texture,
                        layer.pixelWidth, layer.pixelHeight, softwarePixels,
                        op.sourceBounds.x, op.sourceBounds.y,
                        layer.effectiveScale, op.updateBounds)) {
                    return;
                }
                cachedLayerReplayState = state;
                state = ReplayState{};
                if (op.updateBounds) {
                    state.clip = *op.updateBounds;
                    syncClip();
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::EndCachedLayerCommand>) {
                if (!cachedLayerReplayState) return;
                endCachedLayerTarget();
                state = *cachedLayerReplayState;
                cachedLayerReplayState.reset();
                syncClip();
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawCachedLayerCommand>) {
                const auto found = m_cachedDisplayLayers.find(op.id);
                if (found == m_cachedDisplayLayers.end()) return;
                const auto& layer = found->second;
                const auto mapped = state.transform.mapRect(op.destination);
                const float opacity = std::clamp(op.opacity * state.opacity, 0.0f, 1.0f);
                if (layer.texture != 0) {
                    drawCachedLayerTexture(
                        layer.texture,
                        {mapped.x, mapped.y, mapped.width, mapped.height}, opacity);
                } else if (!layer.pixels.empty()) {
                    drawBufferTransformed(
                        mapped.x, mapped.y,
                        static_cast<int>(layer.pixelWidth),
                        static_cast<int>(layer.pixelHeight),
                        layer.pixels.data(), static_cast<int>(layer.pixelWidth),
                        opacity, 0.0f, 2.0f, false,
                        mapped.width, mapped.height);
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawPathCommand>) {
                if (state.pathClips.empty()) {
                    drawPath(op.path, op.paint, state.transform, state.opacity);
                } else {
                    const auto deviceTransform = state.transform.followedBy(
                        lcl::graphics::Matrix3::scale(m_deviceScale,
                                                     m_deviceScale));
                    auto image = rasterizePath(op.path, op.paint, deviceTransform,
                                               state.opacity);
                    for (const auto& mask : state.pathClips) {
                        for (int y = 0; y < image.height; ++y) {
                            for (int x = 0; x < image.width; ++x) {
                                auto& pixel = image.pixels[
                                    static_cast<size_t>(y) * image.width + x];
                                if ((pixel >> 24) == 0) continue;
                                const int maskX = image.x + x - mask.x;
                                const int maskY = image.y + y - mask.y;
                                uint32_t maskAlpha = 0;
                                if (maskX >= 0 && maskX < mask.width &&
                                    maskY >= 0 && maskY < mask.height) {
                                    maskAlpha = mask.pixels[
                                        static_cast<size_t>(maskY) * mask.width + maskX] >> 24;
                                }
                                const uint32_t alpha =
                                    (pixel >> 24) * maskAlpha / 255u;
                                pixel = (pixel & 0x00FFFFFFu) | (alpha << 24);
                            }
                        }
                    }
                    if (!image.empty()) {
                        drawBufferTransformed(
                            static_cast<float>(image.x) / m_deviceScale,
                            static_cast<float>(image.y) / m_deviceScale,
                            image.width, image.height, image.pixels.data(), image.width,
                            1.0f, 0.0f, 2.0f, false,
                            static_cast<float>(image.width) / m_deviceScale,
                            static_cast<float>(image.height) / m_deviceScale);
                    }
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawTextCommand>) {
                if (op.rasterized) {
                    const uint32_t argb = (static_cast<uint32_t>(op.color.a) << 24) |
                        (static_cast<uint32_t>(op.color.r) << 16) |
                        (static_cast<uint32_t>(op.color.g) << 8) | op.color.b;
                    const float effectiveScale = std::max(
                        0.001f, m_deviceScale * state.transform.maxScale());
                    ++m_textLayerUseCounter;
                    auto found = std::find_if(
                        m_rasterizedTextLayers.begin(), m_rasterizedTextLayers.end(),
                        [&](const RasterizedTextLayer& layer) {
                            return layer.text == op.text && layer.argb == argb &&
                                   layer.family == op.fontFamily &&
                                   std::fabs(layer.fontSize - op.fontSize) < 0.0001f &&
                                   std::fabs(layer.effectiveScale - effectiveScale) < 0.0001f;
                        });
                    if (found == m_rasterizedTextLayers.end()) {
                        RasterizedTextLayer layer;
                        layer.text = op.text;
                        layer.argb = argb;
                        layer.fontSize = op.fontSize;
                        layer.effectiveScale = effectiveScale;
                        layer.family = op.fontFamily;
                        const float currentScale = m_deviceScale;
                        setDeviceScale(effectiveScale);
                        const bool rasterized = rasterizeString(
                            op.text, argb, op.fontSize,
                            op.fontFamily == lcl::graphics::FontFamily::Monospace,
                            layer.pixels, layer.width, layer.height);
                        setDeviceScale(currentScale);
                        if (!rasterized) {
                            const auto origin = state.transform.mapPoint(op.origin);
                            const float fontSize = op.fontSize * state.transform.maxScale();
                            const uint8_t alpha = static_cast<uint8_t>(std::clamp(
                                std::lround(static_cast<float>(op.color.a) * state.opacity),
                                0l, 255l));
                            const uint32_t packed = (static_cast<uint32_t>(alpha) << 24) |
                                (static_cast<uint32_t>(op.color.r) << 16) |
                                (static_cast<uint32_t>(op.color.g) << 8) | op.color.b;
                            if (op.fontFamily == lcl::graphics::FontFamily::Monospace) {
                                drawMonospaceString(static_cast<int>(std::lround(origin.x)),
                                                    static_cast<int>(std::lround(origin.y)),
                                                    op.text, packed, fontSize);
                            } else {
                                drawString(static_cast<int>(std::lround(origin.x)),
                                           static_cast<int>(std::lround(origin.y)),
                                           op.text, packed, fontSize);
                            }
                            return;
                        }
                        layer.lastUse = m_textLayerUseCounter;
                        if (m_rasterizedTextLayers.size() >= 96u) {
                            const auto oldest = std::min_element(
                                m_rasterizedTextLayers.begin(), m_rasterizedTextLayers.end(),
                                [](const auto& lhs, const auto& rhs) {
                                    return lhs.lastUse < rhs.lastUse;
                                });
                            m_rasterizedTextLayers.erase(oldest);
                        }
                        m_rasterizedTextLayers.push_back(std::move(layer));
                        found = std::prev(m_rasterizedTextLayers.end());
                    }
                    found->lastUse = m_textLayerUseCounter;
                    const float logicalWidth =
                        static_cast<float>(found->width) / effectiveScale;
                    const float logicalHeight =
                        static_cast<float>(found->height) / effectiveScale;
                    const auto mapped = state.transform.mapRect(
                        {op.origin.x, op.origin.y, logicalWidth, logicalHeight});
                    drawBufferTransformed(
                        mapped.x, mapped.y, found->width, found->height,
                        found->pixels.data(), found->width, state.opacity,
                        0.0f, 2.0f, false, mapped.width, mapped.height);
                    return;
                }
                const auto origin = state.transform.mapPoint(op.origin);
                const float fontSize = op.fontSize * state.transform.maxScale();
                const uint8_t alpha = static_cast<uint8_t>(std::clamp(
                    std::lround(static_cast<float>(op.color.a) * state.opacity), 0l, 255l));
                const uint32_t packed = (static_cast<uint32_t>(alpha) << 24) |
                    (static_cast<uint32_t>(op.color.r) << 16) |
                    (static_cast<uint32_t>(op.color.g) << 8) | op.color.b;
                if (op.fontFamily == lcl::graphics::FontFamily::Monospace) {
                    drawMonospaceString(static_cast<int>(std::lround(origin.x)),
                                        static_cast<int>(std::lround(origin.y)),
                                        op.text, packed, fontSize);
                } else {
                    drawString(static_cast<int>(std::lround(origin.x)),
                               static_cast<int>(std::lround(origin.y)),
                               op.text, packed, fontSize);
                }
            } else if constexpr (std::is_same_v<T, lcl::graphics::DrawImageCommand>) {
                const auto mapped = state.transform.mapRect(op.destination);
                if (mapped.isEmpty() ||
                    (state.clip && !mapped.intersects(*state.clip))) {
                    return;
                }
                const auto* pixels = reinterpret_cast<const uint32_t*>(op.resourceKey);
                const int stridePixels = op.stridePixels > 0
                    ? op.stridePixels : op.sourceWidth;
                if (op.resourceId != 0 && op.contentRevision != 0) {
                    drawImageResourceTransformed(
                        op.resourceId, op.contentRevision, op.opaque,
                        mapped.x, mapped.y, op.sourceWidth, op.sourceHeight,
                        pixels, stridePixels, op.opacity * state.opacity,
                        op.cornerRadius * state.transform.maxScale(),
                        op.cornerRoundness, op.squareTopCorners,
                        mapped.width, mapped.height);
                } else {
                    drawBufferTransformed(
                        mapped.x, mapped.y, op.sourceWidth, op.sourceHeight,
                        pixels, stridePixels, op.opacity * state.opacity,
                        op.cornerRadius * state.transform.maxScale(),
                        op.cornerRoundness, op.squareTopCorners,
                        mapped.width, mapped.height);
                }
            }
        }, command);
    }

    setClipRect(previousClip);
    setDeviceScale(previousScale);
}

RasterRect RasterRenderer::scaleRect(const RasterRect& rect) const {
    return {
        rect.x * m_deviceScale + m_contentOriginX,
        rect.y * m_deviceScale + m_contentOriginY,
        rect.width * m_deviceScale,
        rect.height * m_deviceScale,
    };
}

int RasterRenderer::scaleCoord(int value) const {
    return static_cast<int>(std::lround(static_cast<float>(value) * m_deviceScale + m_contentOriginX));
}

int RasterRenderer::scaleLength(int value) const {
    return static_cast<int>(std::lround(static_cast<float>(value) * m_deviceScale));
}

bool RasterRenderer::ensureFont(float logicalFontSize) {
    const float deviceFontSize = std::max(1.0f, logicalFontSize * m_deviceScale);
    if (!m_fontRenderer.isInitialized() ||
        std::fabs(m_fontRenderer.getFontSize() - deviceFontSize) > 0.01f) {
        m_fontRenderer = FontRenderer{};
        text_metrics::loadFont(m_fontRenderer, lcl::graphics::FontFamily::Interface, deviceFontSize);
    }
    return m_fontRenderer.isInitialized();
}

bool RasterRenderer::ensureMonospaceFont(float logicalFontSize) {
    const float deviceFontSize = std::max(1.0f, logicalFontSize * m_deviceScale);
    if (!m_monospaceFontRenderer.isInitialized() ||
        std::fabs(m_monospaceFontRenderer.getFontSize() - deviceFontSize) > 0.01f) {
        m_monospaceFontRenderer = FontRenderer{};
        text_metrics::loadFont(m_monospaceFontRenderer, lcl::graphics::FontFamily::Monospace,
                               deviceFontSize);
    }
    return m_monospaceFontRenderer.isInitialized();
}

#ifndef LCL_SOFTWARE_ONLY
void RasterRenderer::drawTextureQuad(uint32_t textureId, float x, float y, float w, float h,
                                   float opacity, float uMax, float vMax) {
    if (textureId == 0 || m_glProgram == 0) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    float quad[16] = {
        x1, y1,  0.0f, vMax,
        x1, y2,  0.0f, 0.0f,
        x2, y1,  uMax, vMax,
        x2, y2,  uMax, 0.0f,
    };

    glUseProgram(m_glProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uTextureLoc, 0);
    glUniform1f(m_uOpacityLoc, std::clamp(opacity, 0.0f, 1.0f));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aPosLoc);
    glVertexAttribPointer(m_aTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aTexLoc);

    beginPremultipliedAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endPremultipliedAlphaSourceOver();

    glDisableVertexAttribArray(m_aPosLoc);
    glDisableVertexAttribArray(m_aTexLoc);
}

void RasterRenderer::drawMaskedTextureQuad(uint32_t textureId,
                                         float x,
                                         float y,
                                         float w,
                                         float h,
                                         float cornerRadius,
                                         float cornerRoundness,
                                         float opacity,
                                         bool squareTopCorners,
                                         float uScale,
                                         float vScale,
                                         float uOffset,
                                         float vOffset,
                                         float maskWidth,
                                         float maskHeight,
                                         float maskOffsetX,
                                         float maskOffsetY,
                                         float uTexelInset,
                                         float vTexelInset) {
    if (textureId == 0 || m_glMaskProgram == 0) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    float quad[16] = {
        x1, y1,  0.0f, 1.0f,
        x1, y2,  0.0f, 0.0f,
        x2, y1,  1.0f, 1.0f,
        x2, y2,  1.0f, 0.0f,
    };

    glUseProgram(m_glMaskProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uMaskTextureLoc, 0);
    const float resolvedMaskWidth = maskWidth > 0.0f ? maskWidth : w;
    const float resolvedMaskHeight = maskHeight > 0.0f ? maskHeight : h;
    glUniform2f(m_uMaskSizeLoc, std::max(1.0f, resolvedMaskWidth),
                std::max(1.0f, resolvedMaskHeight));
    glUniform1f(m_uMaskRadiusLoc, std::max(0.0f, cornerRadius));
    glUniform1f(m_uMaskRoundnessLoc, std::clamp(cornerRoundness, 2.0f, 8.0f));
    glUniform1f(m_uMaskOpacityLoc, std::clamp(opacity, 0.0f, 1.0f));
    glUniform1f(m_uMaskSquareTopCornersLoc, squareTopCorners ? 1.0f : 0.0f);
    glUniform2f(m_uMaskSampleOffsetLoc, uOffset, vOffset);
    glUniform2f(m_uMaskSampleScaleLoc, uScale, vScale);
    glUniform2f(m_uMaskSampleTexelInsetLoc,
                std::max(0.0f, uTexelInset),
                std::max(0.0f, vTexelInset));
    glUniform2f(m_uMaskDrawSizeLoc, std::max(1.0f, w), std::max(1.0f, h));
    glUniform2f(m_uMaskGeometryOffsetLoc, maskOffsetX, maskOffsetY);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aMaskPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aMaskPosLoc);
    glVertexAttribPointer(m_aMaskTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aMaskTexLoc);

    beginPremultipliedAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endPremultipliedAlphaSourceOver();
    glDisableVertexAttribArray(m_aMaskPosLoc);
    glDisableVertexAttribArray(m_aMaskTexLoc);
}

void RasterRenderer::drawBgraTextureQuad(uint32_t textureId, float x, float y,
                                         float w, float h, float opacity,
                                         float uMax, float vMax) {
    if (textureId == 0 || m_glBgraProgram == 0) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    // UV orientation for raw CPU buffer memory: Top-Left maps to (0,0), Bottom-Left maps to (0,1)
    float quad[16] = {
        x1, y1,  0.0f, 0.0f,
        x1, y2,  0.0f, vMax,
        x2, y1,  uMax, 0.0f,
        x2, y2,  uMax, vMax,
    };

    glUseProgram(m_glBgraProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uBgraTextureLoc, 0);
    glUniform1f(m_uBgraOpacityLoc, std::clamp(opacity, 0.0f, 1.0f));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aBgraPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aBgraPosLoc);
    glVertexAttribPointer(m_aBgraTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aBgraTexLoc);

    beginPremultipliedAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endPremultipliedAlphaSourceOver();
    glDisableVertexAttribArray(m_aBgraPosLoc);
    glDisableVertexAttribArray(m_aBgraTexLoc);
}

void RasterRenderer::drawMaskedBgraTextureQuad(uint32_t textureId,
                                             float x,
                                             float y,
                                             float w,
                                             float h,
                                             float cornerRadius,
                                             float cornerRoundness,
                                             float opacity,
                                             bool squareTopCorners,
                                             bool squareBottomCorners,
                                             float uScale,
                                             float vScale,
                                             float uMax,
                                             float vMax) {
    if (textureId == 0 || m_glMaskBgraProgram == 0) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    // Match raw client buffer orientation (same as drawBgraTextureQuad)
    float quad[16] = {
        x1, y1,  0.0f, 0.0f,
        x1, y2,  0.0f, 1.0f,
        x2, y1,  1.0f, 0.0f,
        x2, y2,  1.0f, 1.0f,
    };

    glUseProgram(m_glMaskBgraProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uMaskBgraTextureLoc, 0);
    glUniform2f(m_uMaskBgraSizeLoc, std::max(1.0f, w), std::max(1.0f, h));
    const float maxRadius = squareBottomCorners
        ? std::min(w * 0.5f, h)
        : std::min(w, h) * 0.5f;
    const float clampedRadius = std::clamp(cornerRadius, 0.0f, maxRadius);
    float topRadius = squareTopCorners ? 0.0f : clampedRadius;
    float bottomRadius = squareBottomCorners ? 0.0f : clampedRadius;
    glUniform4f(m_uMaskBgraCornerRadiiLoc,
                topRadius,
                topRadius,
                bottomRadius,
                bottomRadius);
    glUniform1f(m_uMaskBgraRoundnessLoc, std::clamp(cornerRoundness, 2.0f, 8.0f));
    glUniform1f(m_uMaskBgraOpacityLoc, std::clamp(opacity, 0.0f, 1.0f));
    glUniform1f(m_uMaskBgraTopOnlyLoc, squareBottomCorners ? 1.0f : 0.0f);
    glUniform2f(m_uMaskBgraSampleScaleLoc, uScale, vScale);
    glUniform2f(m_uMaskBgraSampleMaxLoc, uMax, vMax);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aMaskBgraPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aMaskBgraPosLoc);
    glVertexAttribPointer(m_aMaskBgraTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aMaskBgraTexLoc);

    beginPremultipliedAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endPremultipliedAlphaSourceOver();

    glDisableVertexAttribArray(m_aMaskBgraPosLoc);
    glDisableVertexAttribArray(m_aMaskBgraTexLoc);
}

void RasterRenderer::drawGpuRoundedRect(float x,
                                      float y,
                                      float w,
                                      float h,
                                      float radius,
                                      float roundness,
                                      float borderWidth,
                                      const RasterColor& fill,
                                      const RasterColor& border) {
    if (m_glRoundRectProgram == 0 || w <= 0.0f || h <= 0.0f) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    float quad[16] = {
        x1, y1,  0.0f, 1.0f,
        x1, y2,  0.0f, 0.0f,
        x2, y1,  1.0f, 1.0f,
        x2, y2,  1.0f, 0.0f,
    };

    glUseProgram(m_glRoundRectProgram);
    glUniform2f(m_uRoundRectSizeLoc, std::max(1.0f, w), std::max(1.0f, h));
    glUniform1f(m_uRoundRectRadiusLoc, std::max(0.0f, radius));
    glUniform1f(m_uRoundRectRoundnessLoc, std::clamp(roundness, 2.0f, 8.0f));
    glUniform1f(m_uRoundRectBorderWidthLoc, std::max(0.0f, borderWidth));
    glUniform4f(m_uRoundRectFillColorLoc,
                static_cast<float>(fill.r) / 255.0f,
                static_cast<float>(fill.g) / 255.0f,
                static_cast<float>(fill.b) / 255.0f,
                static_cast<float>(fill.a) / 255.0f);
    glUniform4f(m_uRoundRectBorderColorLoc,
                static_cast<float>(border.r) / 255.0f,
                static_cast<float>(border.g) / 255.0f,
                static_cast<float>(border.b) / 255.0f,
                static_cast<float>(border.a) / 255.0f);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aRoundRectPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aRoundRectPosLoc);
    glVertexAttribPointer(m_aRoundRectTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aRoundRectTexLoc);

    beginPremultipliedAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endPremultipliedAlphaSourceOver();

    glDisableVertexAttribArray(m_aRoundRectPosLoc);
    glDisableVertexAttribArray(m_aRoundRectTexLoc);
}
#endif

void RasterRenderer::applyScissorState() {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend) return;
    std::optional<RasterRect> effectiveClip = m_clipRect;
    if (m_frameDamageRect) {
        if (effectiveClip) {
            const float left = std::max(effectiveClip->x, m_frameDamageRect->x);
            const float top = std::max(effectiveClip->y, m_frameDamageRect->y);
            const float right = std::min(effectiveClip->x + effectiveClip->width,
                                         m_frameDamageRect->x + m_frameDamageRect->width);
            const float bottom = std::min(effectiveClip->y + effectiveClip->height,
                                          m_frameDamageRect->y + m_frameDamageRect->height);
            effectiveClip = RasterRect{
                left, top, std::max(0.0f, right - left),
                std::max(0.0f, bottom - top)};
        } else {
            effectiveClip = m_frameDamageRect;
        }
    }
    if (effectiveClip.has_value()) {
        const RasterRect deviceClip = scaleRect(*effectiveClip);
        int sx = std::max(0, static_cast<int>(std::floor(deviceClip.x)));
        int syTop = std::max(0, static_cast<int>(std::floor(deviceClip.y)));
        int sRight = std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(deviceClip.x + deviceClip.width)));
        int sBottom = std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(deviceClip.y + deviceClip.height)));
        int sw = std::max(0, sRight - sx);
        int sh = std::max(0, sBottom - syTop);
        m_eglBackend->makeCurrent();
        glEnable(GL_SCISSOR_TEST);
        glScissor(sx, std::max(0, static_cast<int>(m_height) - (syTop + sh)), sw, sh);
    } else {
        if (m_eglBackend) {
            m_eglBackend->makeCurrent();
            glDisable(GL_SCISSOR_TEST);
        }
    }
#endif
}

void RasterRenderer::clear(const RasterColor& color) {
    if (!m_initialized) return;
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend &&
        activeSceneFBO() > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        glDisable(GL_SCISSOR_TEST);
        const float alpha = static_cast<float>(color.a) / 255.0f;
        glClearColor((static_cast<float>(color.r) / 255.0f) * alpha,
                     (static_cast<float>(color.g) / 255.0f) * alpha,
                     (static_cast<float>(color.b) / 255.0f) * alpha,
                     alpha);
        glClear(GL_COLOR_BUFFER_BIT);
        applyScissorState();
        return;
    }
#endif
    if (m_targetPixels) {
        std::fill_n(m_targetPixels, static_cast<size_t>(m_width) * m_height,
                    color.toARGB());
    }
}

void RasterRenderer::clearRect(const RasterRect& rect, const RasterColor& color) {
    if (!m_initialized || rect.width <= 0.0f || rect.height <= 0.0f) return;

    const RasterRect deviceRect = scaleRect(rect);
    const int left = std::clamp(static_cast<int>(std::floor(deviceRect.x)),
                                0, static_cast<int>(m_width));
    const int top = std::clamp(static_cast<int>(std::floor(deviceRect.y)),
                               0, static_cast<int>(m_height));
    const int right = std::clamp(
        static_cast<int>(std::ceil(deviceRect.x + deviceRect.width)),
        0, static_cast<int>(m_width));
    const int bottom = std::clamp(
        static_cast<int>(std::ceil(deviceRect.y + deviceRect.height)),
        0, static_cast<int>(m_height));
    if (left >= right || top >= bottom) return;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend &&
        activeSceneFBO() > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        glEnable(GL_SCISSOR_TEST);
        glScissor(left, static_cast<int>(m_height) - bottom,
                  right - left, bottom - top);
        const float alpha = static_cast<float>(color.a) / 255.0f;
        glClearColor((static_cast<float>(color.r) / 255.0f) * alpha,
                     (static_cast<float>(color.g) / 255.0f) * alpha,
                     (static_cast<float>(color.b) / 255.0f) * alpha,
                     alpha);
        glClear(GL_COLOR_BUFFER_BIT);
        applyScissorState();
        return;
    }
#endif

    if (!m_targetPixels) return;
    const uint32_t pixel = color.toARGB();
    for (int y = top; y < bottom; ++y) {
        std::fill(m_targetPixels + static_cast<size_t>(y) * m_width + left,
                  m_targetPixels + static_cast<size_t>(y) * m_width + right,
                  pixel);
    }
}

void RasterRenderer::setClipRect(const std::optional<RasterRect>& clip) {
    m_clipRect = clip;
    applyScissorState();
}

void RasterRenderer::setFrameDamageRect(
    const std::optional<RasterRect>& damage) {
    m_frameDamageRect = damage;
    applyScissorState();
}

void RasterRenderer::beginFrame() {
    if (!m_initialized) return;
    ++m_shmTextureFrameSerial;
    if (m_shmTextureFrameSerial == 0) m_shmTextureFrameSerial = 1;
    m_clipRect = std::nullopt;
    m_frameDamageRect = std::nullopt;
    applyScissorState();

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend && activeSceneFBO() > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        if (!m_retainsFrameBacking) {
            if (m_eglBackend->presentsToDisplay()) {
                glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
            } else {
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            }
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
#endif

    if (!m_retainsFrameBacking && m_targetPixels && m_glExternalFrameFBO == 0) {
        const uint32_t clearColor =
            (m_backendType == RasterBackend::OpenGL_EGL)
                ? 0x00000000u
                : 0xFF14161Du;
        std::fill_n(m_targetPixels, static_cast<size_t>(m_width) * m_height,
                    clearColor);
    }
}

void RasterRenderer::endFrame() {
    if (!m_initialized) return;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        m_eglBackend->makeCurrent();

        // A compositor surface absent from this frame is no longer visible.
        // Release its retained upload instead of leaking GPU memory after
        // close, minimize, or client disconnect.
        for (auto it = m_cachedShmTextures.begin(); it != m_cachedShmTextures.end();) {
            if (it->second.lastUsedFrame != m_shmTextureFrameSerial) {
                if (it->second.texture > 0) glDeleteTextures(1, &it->second.texture);
                it = m_cachedShmTextures.erase(it);
            } else {
                ++it;
            }
        }

        if (m_eglBackend->presentsToDisplay()) {
            // Android can blit this retained scene FBO straight into its
            // rotating AHardwareBuffer scanout. Desktop backends return false
            // and retain the default-surface presentation path below.
            const bool directPresented = activeSceneFBO() > 0 &&
                m_eglBackend->presentFramebuffer(
                    activeSceneFBO(), m_width, m_height);
            if (!directPresented) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, m_width, m_height);
                if (m_glSceneTexture > 0) {
                    const float uMax = static_cast<float>(m_width) /
                        static_cast<float>(std::max(1u, m_glSceneCapacityWidth));
                    const float vMax = static_cast<float>(m_height) /
                        static_cast<float>(std::max(1u, m_glSceneCapacityHeight));
                    drawTextureQuad(m_glSceneTexture, 0, 0, m_width, m_height,
                                    1.0f, uMax, vMax);
                }
                glFlush();
                m_eglBackend->present();
            }
        } else if (m_glOutputFrameFBO != 0 && m_glSceneTexture != 0) {
            // DMA-BUF slots rotate and cannot retain authoritative content.
            // Copy the retained scene into the acquired slot before export.
            glBindFramebuffer(GL_FRAMEBUFFER, m_glOutputFrameFBO);
            glViewport(0, 0, m_width, m_height);
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, static_cast<GLsizei>(m_width),
                      static_cast<GLsizei>(m_height));
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            const float uMax = static_cast<float>(m_width) /
                static_cast<float>(std::max(1u, m_glSceneCapacityWidth));
            const float vMax = static_cast<float>(m_height) /
                static_cast<float>(std::max(1u, m_glSceneCapacityHeight));
            drawTextureQuad(m_glSceneTexture, 0, 0, m_width, m_height,
                            1.0f, uMax, vMax);
            glFlush();
        } else if (m_targetPixels) {
            glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
            if (m_eglBackend->readback(m_targetPixels, m_width, m_height)) {
                // SHM remains a straight-alpha CPU contract even when an EGL
                // renderer produced the frame internally.
                const size_t pixelCount = static_cast<size_t>(m_width) * m_height;
                for (size_t i = 0; i < pixelCount; ++i) {
                    m_targetPixels[i] = alpha::unpremultiplyArgb(m_targetPixels[i]);
                }
            }
        }
    }
#endif
}

uint32_t RasterRenderer::importTexture(const lcl::platform::INativeBuffer& buffer) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        return m_eglBackend->importTexture(buffer);
    }
#else
    (void)buffer;
#endif
    return 0;
}

uint32_t RasterRenderer::importDmaBuf(const lcl::platform::DmaBufDescriptor& descriptor) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        return m_eglBackend->importDmaBuf(descriptor);
    }
#else
    (void)descriptor;
#endif
    return 0;
}

void RasterRenderer::releaseTexture(uint32_t texture) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_eglBackend) m_eglBackend->releaseTexture(texture);
#else
    (void)texture;
#endif
}

void RasterRenderer::drawDmaBufTextureTransformed(float dstX, float dstY, int srcW, int srcH,
                                                int backingW, int backingH,
                                                uint32_t texture, float opacity,
                                                float cornerRadius, float cornerRoundness,
                                                bool squareTopCorners, float drawWidth,
                                                float drawHeight,
                                                RasterBufferSampling sampling) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend || texture == 0) return;
    m_eglBackend->makeCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
    glViewport(0, 0, m_width, m_height);
    const auto destination = mapLogicalRasterDestination(
        dstX, dstY, drawWidth, drawHeight, cornerRadius,
        m_deviceScale, m_contentOriginX, m_contentOriginY);
    const auto crop = makeDmaBufCrop(static_cast<uint32_t>(std::max(0, srcW)),
                                    static_cast<uint32_t>(std::max(0, srcH)),
                                    static_cast<uint32_t>(std::max(0, backingW)),
                                    static_cast<uint32_t>(std::max(0, backingH)));
    const BufferSampleScale sampleScale = resolveBufferSampleScale(
        sampling, srcW, srcH, destination.width, destination.height);
    const float sampledUMax = crop.uMax * sampleScale.x;
    const float sampledVMax = crop.vMax * sampleScale.y;
    const float sampledVOffset =
        sampling == RasterBufferSampling::TopLeftAnchoredCropTrailingEdge
        ? crop.vMax - sampledVMax
        : 0.0f;
    if (destination.cornerRadius > 0.001f ||
        sampling != RasterBufferSampling::Stretch) {
        drawMaskedTextureQuad(texture, destination.x, destination.y,
                              destination.width, destination.height,
                              destination.cornerRadius, cornerRoundness, opacity,
                              squareTopCorners, sampledUMax, sampledVMax,
                              0.0f, sampledVOffset,
                              0.0f, 0.0f, 0.0f, 0.0f,
                              normalizedHalfTexel(static_cast<uint32_t>(
                                  std::max(0, backingW))),
                              normalizedHalfTexel(static_cast<uint32_t>(
                                  std::max(0, backingH))));
    } else {
        drawTextureQuad(texture, destination.x, destination.y,
                        destination.width, destination.height,
                        opacity, crop.uMax, crop.vMax);
    }
#else
    (void)dstX;
    (void)dstY;
    (void)srcW;
    (void)srcH;
    (void)backingW;
    (void)backingH;
    (void)texture;
    (void)opacity;
    (void)cornerRadius;
    (void)cornerRoundness;
    (void)squareTopCorners;
    (void)drawWidth;
    (void)drawHeight;
    (void)sampling;
#endif
}

void RasterRenderer::drawDmaBufTextureRegionTransformed(
        float dstX, float dstY, float drawWidth, float drawHeight,
        int srcW, int srcH, int backingW, int backingH,
        int regionX, int regionY, int regionWidth, int regionHeight,
        uint32_t texture, float opacity, float cornerRadius,
        float cornerRoundness) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != RasterBackend::OpenGL_EGL || !m_eglBackend ||
        texture == 0 || srcW <= 0 || srcH <= 0 || backingW <= 0 ||
        backingH <= 0) {
        return;
    }
    const int x = std::clamp(regionX, 0, srcW - 1);
    const int y = std::clamp(regionY, 0, srcH - 1);
    const int width = std::clamp(regionWidth, 1, srcW - x);
    const int height = std::clamp(regionHeight, 1, srcH - y);
    const float uOffset = static_cast<float>(x) / backingW;
    const float uScale = static_cast<float>(width) / backingW;
    // Imported DMA-BUF textures use the same vertically flipped sampling
    // convention as the complete-surface path.
    const float vOffset = static_cast<float>(srcH - y - height) / backingH;
    const float vScale = static_cast<float>(height) / backingH;
    const auto destination = mapLogicalRasterDestination(
        dstX, dstY, drawWidth, drawHeight, cornerRadius,
        m_deviceScale, m_contentOriginX, m_contentOriginY);

    m_eglBackend->makeCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
    glViewport(0, 0, m_width, m_height);
    drawMaskedTextureQuad(
        texture, destination.x, destination.y,
        destination.width, destination.height,
        destination.cornerRadius, cornerRoundness, opacity, false,
        uScale, vScale, uOffset, vOffset,
        0.0f, 0.0f, 0.0f, 0.0f,
        normalizedHalfTexel(static_cast<uint32_t>(backingW)),
        normalizedHalfTexel(static_cast<uint32_t>(backingH)));
#else
    (void)dstX;
    (void)dstY;
    (void)drawWidth;
    (void)drawHeight;
    (void)srcW;
    (void)srcH;
    (void)backingW;
    (void)backingH;
    (void)regionX;
    (void)regionY;
    (void)regionWidth;
    (void)regionHeight;
    (void)texture;
    (void)opacity;
    (void)cornerRadius;
    (void)cornerRoundness;
#endif
}

void RasterRenderer::drawBackgroundGradient(const RasterColor& topColor, const RasterColor& bottomColor) {
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

void RasterRenderer::drawRect(const RasterRect& rect, const RasterColor& color) {
    if (!m_initialized) return;

    const RasterRect deviceRect = scaleRect(rect);

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        int x1 = std::clamp(static_cast<int>(deviceRect.x), 0, static_cast<int>(m_width));
        int y1 = std::clamp(static_cast<int>(deviceRect.y), 0, static_cast<int>(m_height));
        int x2 = std::clamp(static_cast<int>(deviceRect.x + deviceRect.width), 0, static_cast<int>(m_width));
        int y2 = std::clamp(static_cast<int>(deviceRect.y + deviceRect.height), 0, static_cast<int>(m_height));
        if (x1 >= x2 || y1 >= y2 || color.a == 0) return;

        // A solid rectangle does not need a CPU-filled texture upload.  The
        // rounded-rect shader also represents the r=0 case and keeps this hot
        // path entirely on the GPU.
        if (m_glFBOReady && m_glRoundRectProgram > 0) {
            m_eglBackend->makeCurrent();
            glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
            glViewport(0, 0, m_width, m_height);
            drawGpuRoundedRect(static_cast<float>(x1), static_cast<float>(y1),
                               static_cast<float>(x2 - x1), static_cast<float>(y2 - y1),
                               0.0f, 2.0f, 0.0f, color, {0, 0, 0, 0});
            return;
        }

        std::vector<uint32_t> fill(static_cast<size_t>(x2 - x1) * static_cast<size_t>(y2 - y1), color.toARGB());
        drawBufferRaw(x1, y1, x2 - x1, y2 - y1, fill.data(), x2 - x1, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }
#endif

    if (!m_targetPixels) return;

    int x1 = std::clamp(static_cast<int>(deviceRect.x), 0, static_cast<int>(m_width));
    int y1 = std::clamp(static_cast<int>(deviceRect.y), 0, static_cast<int>(m_height));
    int x2 = std::clamp(static_cast<int>(deviceRect.x + deviceRect.width), 0, static_cast<int>(m_width));
    int y2 = std::clamp(static_cast<int>(deviceRect.y + deviceRect.height), 0, static_cast<int>(m_height));

    if (m_clipRect) {
        const RasterRect deviceClip = scaleRect(*m_clipRect);
        x1 = std::max(x1, std::max(0, static_cast<int>(std::floor(deviceClip.x))));
        y1 = std::max(y1, std::max(0, static_cast<int>(std::floor(deviceClip.y))));
        x2 = std::min(x2, std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(deviceClip.x + deviceClip.width))));
        y2 = std::min(y2, std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(deviceClip.y + deviceClip.height))));
    }

    if (x1 >= x2 || y1 >= y2) return;

    uint32_t fillARGB = color.toARGB();

    if (color.a == 255) {
        for (int y = y1; y < y2; ++y) {
            uint32_t* row = &m_targetPixels[y * m_width + x1];
            std::fill_n(row, x2 - x1, fillARGB);
        }
    } else if (color.a > 0) {
        const float srcA = static_cast<float>(color.a) / 255.0f;

        for (int y = y1; y < y2; ++y) {
            uint32_t* row = &m_targetPixels[y * m_width];
            for (int x = x1; x < x2; ++x) {
                uint32_t bg = row[x];
                const float bgA = static_cast<float>((bg >> 24) & 0xFF) / 255.0f;
                const float bgR = static_cast<float>((bg >> 16) & 0xFF);
                const float bgG = static_cast<float>((bg >> 8) & 0xFF);
                const float bgB = static_cast<float>(bg & 0xFF);

                const float outA = srcA + bgA * (1.0f - srcA);
                if (outA <= 0.0001f) {
                    row[x] = 0x00000000;
                    continue;
                }

                const float outR = (color.r * srcA + bgR * bgA * (1.0f - srcA)) / outA;
                const float outG = (color.g * srcA + bgG * bgA * (1.0f - srcA)) / outA;
                const float outB = (color.b * srcA + bgB * bgA * (1.0f - srcA)) / outA;

                uint32_t a = static_cast<uint32_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
                uint32_t r = static_cast<uint32_t>(std::clamp(outR, 0.0f, 255.0f));
                uint32_t g = static_cast<uint32_t>(std::clamp(outG, 0.0f, 255.0f));
                uint32_t b = static_cast<uint32_t>(std::clamp(outB, 0.0f, 255.0f));

                row[x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
}

void RasterRenderer::drawRoundedRect(const RasterRect& rect,
                                   float radius,
                                   const RasterColor& color,
                                   const RasterColor& borderColor,
                                   float borderWidth,
                                   float roundness) {
    if (!m_initialized) return;

    const RasterRect deviceRect = scaleRect(rect);
    radius *= m_deviceScale;
    borderWidth *= m_deviceScale;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_glFBOReady && m_eglBackend && m_glRoundRectProgram > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);

        drawGpuRoundedRect(deviceRect.x,
                           deviceRect.y,
                           std::max(1.0f, deviceRect.width),
                           std::max(1.0f, deviceRect.height),
                           radius,
                           roundness,
                           borderWidth,
                           color,
                           borderColor);
        return;
    }
#endif

    const float shapeW = std::max(1.0f, deviceRect.width);
    const float shapeH = std::max(1.0f, deviceRect.height);
    const int dstX = static_cast<int>(std::floor(deviceRect.x));
    const int dstY = static_cast<int>(std::floor(deviceRect.y));
    const int w = std::max(1, static_cast<int>(std::ceil(deviceRect.x + shapeW)) - dstX);
    const int h = std::max(1, static_cast<int>(std::ceil(deviceRect.y + shapeH)) - dstY);
    const float localOriginX = static_cast<float>(dstX) - deviceRect.x;
    const float localOriginY = static_cast<float>(dstY) - deviceRect.y;

    float r = std::clamp(radius, 0.0f, std::min(shapeW, shapeH) * 0.5f);
    float bw = std::max(0.0f, borderWidth);
    const float n = std::clamp(roundness, 2.0f, 8.0f);

    auto insideRounded = [n](float px, float py, float rw, float rh, float rr) {
        if (px < 0.0f || py < 0.0f || px > rw || py > rh) return false;
        if (rr <= 0.001f) return true;

        bool inLeft = px < rr;
        bool inRight = px > (rw - rr);
        bool inTop = py < rr;
        bool inBottom = py > (rh - rr);

        if ((inLeft || inRight) && (inTop || inBottom)) {
            float cx = inLeft ? rr : (rw - rr);
            float cy = inTop ? rr : (rh - rr);
            float dx = std::abs(px - cx) / rr;
            float dy = std::abs(py - cy) / rr;
            if (n <= 2.001f) {
                return (dx * dx + dy * dy) <= 1.0f;
            }
            return std::pow(dx, n) + std::pow(dy, n) <= 1.0f;
        }
        return true;
    };

    const bool hasFill = (color.a > 0);
    const bool hasBorder = (bw > 0.0f && borderColor.a > 0);
    if (!hasFill && !hasBorder) return;

    std::vector<uint32_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h), 0x00000000u);

    const float innerW = std::max(0.0f, shapeW - bw * 2.0f);
    const float innerH = std::max(0.0f, shapeH - bw * 2.0f);
    const float innerR = std::max(0.0f, r - bw);

    int aaSamples = 4;
    if (!hasFill && hasBorder) {
        // Border-only shapes need dense coverage on corners, but large windows
        // become expensive during live resize. Adapt sample density by area.
        const int area = w * h;
        if (area > 420000) {
            aaSamples = 3;
        } else if (area > 180000) {
            aaSamples = 4;
        } else {
            aaSamples = (bw <= 1.5f) ? 6 : 5;
        }
    }
    const float invSampleCount = 1.0f / static_cast<float>(aaSamples * aaSamples);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!hasFill && hasBorder && innerW > 0.0f && innerH > 0.0f) {
                // Border-only frame path: skip interior pixels that cannot contribute.
                float pxCenter = localOriginX + static_cast<float>(x) + 0.5f;
                float pyCenter = localOriginY + static_cast<float>(y) + 0.5f;
                if (insideRounded(pxCenter - bw, pyCenter - bw, innerW, innerH, innerR)) {
                    continue;
                }
            }

            int outerHit = 0;
            int innerHit = 0;

            for (int sy = 0; sy < aaSamples; ++sy) {
                for (int sx = 0; sx < aaSamples; ++sx) {
                    float px = localOriginX + static_cast<float>(x) +
                        (static_cast<float>(sx) + 0.5f) / static_cast<float>(aaSamples);
                    float py = localOriginY + static_cast<float>(y) +
                        (static_cast<float>(sy) + 0.5f) / static_cast<float>(aaSamples);

                    if (!insideRounded(px, py, shapeW, shapeH, r)) {
                        continue;
                    }

                    ++outerHit;
                    if (hasBorder && innerW > 0.0f && innerH > 0.0f &&
                        insideRounded(px - bw, py - bw, innerW, innerH, innerR)) {
                        ++innerHit;
                    }
                }
            }

            if (outerHit == 0) {
                continue;
            }

            const float outerCov = static_cast<float>(outerHit) * invSampleCount;
            const float innerCov = static_cast<float>(innerHit) * invSampleCount;

            float borderCov = 0.0f;
            float fillCov = 0.0f;

            if (hasBorder) {
                borderCov = std::max(0.0f, outerCov - innerCov);
                fillCov = hasFill ? innerCov : 0.0f;
            } else if (hasFill) {
                fillCov = outerCov;
            }

            const float borderA = (static_cast<float>(borderColor.a) / 255.0f) * borderCov;
            const float fillA = (static_cast<float>(color.a) / 255.0f) * fillCov;
            const float outA = borderA + fillA;
            if (outA <= 0.0001f) {
                continue;
            }

            const float outR = (static_cast<float>(borderColor.r) * borderA + static_cast<float>(color.r) * fillA) / outA;
            const float outG = (static_cast<float>(borderColor.g) * borderA + static_cast<float>(color.g) * fillA) / outA;
            const float outB = (static_cast<float>(borderColor.b) * borderA + static_cast<float>(color.b) * fillA) / outA;

            const uint32_t a = static_cast<uint32_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
            const uint32_t rr = static_cast<uint32_t>(std::clamp(outR, 0.0f, 255.0f));
            const uint32_t gg = static_cast<uint32_t>(std::clamp(outG, 0.0f, 255.0f));
            const uint32_t bb = static_cast<uint32_t>(std::clamp(outB, 0.0f, 255.0f));

            pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                (a << 24) | (rr << 16) | (gg << 8) | bb;
        }
    }

    drawBufferRaw(dstX, dstY, w, h, pixels.data(), w, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
}

void RasterRenderer::drawTopRoundedRect(const RasterRect& rect,
                                      float radius,
                                      const RasterColor& color,
                                      float roundness) {
    if (!m_initialized || color.a == 0) return;

    const RasterRect deviceRect = scaleRect(rect);
    const int width = std::max(1, static_cast<int>(std::lround(deviceRect.width)));
    const int height = std::max(1, static_cast<int>(std::lround(deviceRect.height)));
    const int dstX = static_cast<int>(std::lround(deviceRect.x));
    const int dstY = static_cast<int>(std::lround(deviceRect.y));
    const uint32_t pixel = color.toARGB();

    drawBufferRaw(dstX, dstY, 1, 1, &pixel, 1, 1.0f,
                  radius * m_deviceScale, roundness,
                  false, true, width, height);
}

void RasterRenderer::drawDropShadow(const RasterRect& rect, float radius, float blur, const RasterColor& shadowColor) {
    (void)radius;
    if (shadowColor.a == 0) return;
    RasterRect shadowRect = {
        rect.x - blur,
        rect.y - blur + 4.0f,
        rect.width + blur * 2.0f,
        rect.height + blur * 2.0f
    };
    RasterColor softShadow = shadowColor;
    softShadow.a = static_cast<uint8_t>(shadowColor.a * 0.4f);
    drawRect(shadowRect, softShadow);
}

void RasterRenderer::drawCircle(float cx, float cy, float radius, const RasterColor& color) {
    RasterRect rect = { cx - radius, cy - radius, radius * 2.0f, radius * 2.0f };
    drawRect(rect, color);
}

void RasterRenderer::drawLine(float x1, float y1, float x2, float y2, const RasterColor& color, float strokeWidth) {
    (void)y2;
    RasterRect rect = { x1, y1, std::abs(x2 - x1) + strokeWidth, strokeWidth };
    drawRect(rect, color);
}

void RasterRenderer::drawString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize) {
    if (!m_initialized || text.empty()) return;
    ensureFont(fontSize);

    const int deviceX = scaleCoord(x);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(y) * m_deviceScale + m_contentOriginY));

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend && m_fontRenderer.isInitialized()) {
        int textW = std::max(1, m_fontRenderer.getTextWidth(text));
        int textH = std::max(1, m_fontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_fontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }
#endif

    if (m_fontRenderer.isInitialized() && m_targetPixels) {
        if (m_clipRect) {
            const RasterRect deviceClip = scaleRect(*m_clipRect);
            int minX = std::max(0, static_cast<int>(std::floor(deviceClip.x)));
            int minY = std::max(0, static_cast<int>(std::floor(deviceClip.y)));
            int maxX = std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(deviceClip.x + deviceClip.width)));
            int maxY = std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(deviceClip.y + deviceClip.height)));
            m_fontRenderer.renderStringClipped(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor, minX, minY, maxX, maxY);
        } else {
            m_fontRenderer.renderString(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor);
        }
    }
}

void RasterRenderer::drawMonospaceString(int x, int y, const std::string& text,
                                       uint32_t fgColor, float fontSize) {
    if (!m_initialized || text.empty() || !ensureMonospaceFont(fontSize)) return;

    const int deviceX = scaleCoord(x);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(y) * m_deviceScale + m_contentOriginY));

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_eglBackend) {
        const int textW = std::max(1, m_monospaceFontRenderer.getTextWidth(text));
        const int textH = std::max(1, m_monospaceFontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_monospaceFontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }
#endif

    if (m_targetPixels) {
        if (m_clipRect) {
            const RasterRect deviceClip = scaleRect(*m_clipRect);
            int minX = std::max(0, static_cast<int>(std::floor(deviceClip.x)));
            int minY = std::max(0, static_cast<int>(std::floor(deviceClip.y)));
            int maxX = std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(deviceClip.x + deviceClip.width)));
            int maxY = std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(deviceClip.y + deviceClip.height)));
            m_monospaceFontRenderer.renderStringClipped(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor, minX, minY, maxX, maxY);
        } else {
            m_monospaceFontRenderer.renderString(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor);
        }
    }
}

float RasterRenderer::measureString(const std::string& text, float fontSize) {
    if (text.empty() || !ensureFont(fontSize)) return 0.0f;
    return static_cast<float>(m_fontRenderer.getTextWidth(text)) / m_deviceScale;
}

float RasterRenderer::measureMonospaceString(const std::string& text, float fontSize) {
    if (text.empty() || !ensureMonospaceFont(fontSize)) return 0.0f;
    return static_cast<float>(m_monospaceFontRenderer.getTextWidth(text)) / m_deviceScale;
}

bool RasterRenderer::rasterizeString(const std::string& text,
                                   uint32_t fgColor,
                                   float fontSize,
                                   bool monospace,
                                   std::vector<uint32_t>& pixels,
                                   int& width,
                                   int& height) {
    width = 0;
    height = 0;
    pixels.clear();
    if (!m_initialized || text.empty()) return false;

    FontRenderer* font = nullptr;
    if (monospace) {
        if (!ensureMonospaceFont(fontSize)) return false;
        font = &m_monospaceFontRenderer;
    } else {
        if (!ensureFont(fontSize)) return false;
        font = &m_fontRenderer;
    }
    if (!font->isInitialized()) return false;

    width = std::max(1, font->getTextWidth(text));
    height = std::max(1, font->getCellHeight() + 2);
    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0x00000000u);
    font->renderString(pixels.data(), width, height, 0, 1, text, fgColor);
    return true;
}

namespace {

BufferSampleScale resolveBufferSampleScale(
        RasterBufferSampling sampling,
        int sourceWidth,
        int sourceHeight,
        float destinationWidth,
        float destinationHeight) {
    if (sampling == RasterBufferSampling::Stretch ||
        sourceWidth <= 0 || sourceHeight <= 0 ||
        destinationWidth <= 0.0f || destinationHeight <= 0.0f) {
        return {};
    }

    const float sourceAspect =
        static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    const float destinationAspect = destinationWidth / destinationHeight;
    if (sampling == RasterBufferSampling::TopLeftAnchoredCropTrailingEdge) {
        if (destinationAspect > sourceAspect) {
            return {1.0f, sourceAspect / destinationAspect};
        }
        if (destinationAspect < sourceAspect) {
            return {destinationAspect / sourceAspect, 1.0f};
        }
        return {};
    }
    if (destinationAspect < sourceAspect) {
        // Portrait: source width follows destination width. Sampling beyond
        // the source bottom is clamped to its final pixel row.
        return {1.0f, sourceAspect / destinationAspect};
    }
    if (destinationAspect > sourceAspect) {
        // Landscape: source height follows destination height. Sampling beyond
        // the source right edge is clamped to its final pixel column.
        return {destinationAspect / sourceAspect, 1.0f};
    }
    return {};
}

} // namespace

void RasterRenderer::drawBuffer(int dstX,
                              int dstY,
                              int srcW,
                              int srcH,
                              const uint32_t* pixelData,
                              int stridePixels,
                              float opacity,
                              float cornerRadius,
                              float cornerRoundness,
                              bool squareTopCorners,
                              int drawWidth,
                              int drawHeight) {
    const int deviceX = scaleCoord(dstX);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(dstY) * m_deviceScale + m_contentOriginY));
    const int deviceW = (drawWidth > 0) ? scaleLength(drawWidth) : 0;
    const int deviceH = (drawHeight > 0) ? scaleLength(drawHeight) : 0;
    drawBufferRaw(static_cast<float>(deviceX), static_cast<float>(deviceY),
                  srcW, srcH, pixelData, stridePixels, opacity,
                  cornerRadius * m_deviceScale, cornerRoundness, squareTopCorners,
                  false, static_cast<float>(deviceW), static_cast<float>(deviceH));
}

void RasterRenderer::drawBufferTransformed(float dstX,
                                         float dstY,
                                         int srcW,
                                         int srcH,
                                         const uint32_t* pixelData,
                                         int stridePixels,
                                         float opacity,
                                         float cornerRadius,
                                         float cornerRoundness,
                                         bool squareTopCorners,
                                         float drawWidth,
                                         float drawHeight,
                                         RasterBufferSampling sampling) {
    const auto destination = mapLogicalRasterDestination(
        dstX, dstY, drawWidth, drawHeight, cornerRadius,
        m_deviceScale, m_contentOriginX, m_contentOriginY);
    drawBufferRaw(
        destination.x, destination.y,
        srcW, srcH, pixelData, stridePixels, opacity,
        destination.cornerRadius, cornerRoundness, squareTopCorners,
        false, destination.width, destination.height, sampling);
}

void RasterRenderer::trimImageTextureCache(uint64_t protectedResourceId) {
#ifndef LCL_SOFTWARE_ONLY
    while ((m_cachedImageTextureBytes > kImageTextureCacheBudgetBytes ||
            m_cachedImageTextures.size() > kImageTextureCacheMaxEntries) &&
           m_cachedImageTextures.size() > 1) {
        auto oldest = m_cachedImageTextures.end();
        for (auto it = m_cachedImageTextures.begin();
             it != m_cachedImageTextures.end(); ++it) {
            if (it->first == protectedResourceId) continue;
            if (oldest == m_cachedImageTextures.end() ||
                it->second.lastUse < oldest->second.lastUse) {
                oldest = it;
            }
        }
        if (oldest == m_cachedImageTextures.end()) break;
        if (oldest->second.texture > 0) {
            glDeleteTextures(1, &oldest->second.texture);
        }
        m_cachedImageTextureBytes -= std::min(
            m_cachedImageTextureBytes, oldest->second.byteSize);
        m_cachedImageTextures.erase(oldest);
    }
#else
    (void)protectedResourceId;
#endif
}

void RasterRenderer::drawImageResourceTransformed(
        uint64_t resourceId, uint64_t contentRevision, bool opaque,
        float dstX, float dstY, int srcW, int srcH,
        const uint32_t* pixelData, int stridePixels, float opacity,
        float cornerRadius, float cornerRoundness, bool squareTopCorners,
        float drawWidth, float drawHeight) {
    if (!m_initialized || resourceId == 0 || contentRevision == 0 ||
        !pixelData || srcW <= 0 || srcH <= 0) {
        return;
    }

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_glFBOReady &&
        m_eglBackend) {
        m_eglBackend->makeCurrent();
        auto& cached = m_cachedImageTextures[resourceId];
        cached.lastUse = ++m_imageTextureUseCounter;
        const bool needsUpload = cached.texture == 0 ||
            cached.width != srcW || cached.height != srcH ||
            cached.contentRevision != contentRevision;
        if (needsUpload) {
            if (cached.texture == 0) glGenTextures(1, &cached.texture);
            glBindTexture(GL_TEXTURE_2D, cached.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, srcW, srcH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

            const uint32_t* uploadPixels = pixelData;
            std::vector<uint32_t> preparedPixels;
            if (!opaque || stridePixels != srcW) {
                preparedPixels.resize(static_cast<size_t>(srcW) * srcH);
                for (int y = 0; y < srcH; ++y) {
                    const uint32_t* source = pixelData +
                        static_cast<size_t>(y) * stridePixels;
                    uint32_t* destination = preparedPixels.data() +
                        static_cast<size_t>(y) * srcW;
                    for (int x = 0; x < srcW; ++x) {
                        destination[x] = opaque
                            ? source[x] : alpha::premultiplyArgb(source[x]);
                    }
                }
                uploadPixels = preparedPixels.data();
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, srcW, srcH,
                            GL_RGBA, GL_UNSIGNED_BYTE, uploadPixels);
            glGenerateMipmap(GL_TEXTURE_2D);

            m_cachedImageTextureBytes -= std::min(
                m_cachedImageTextureBytes, cached.byteSize);
            cached.width = srcW;
            cached.height = srcH;
            cached.contentRevision = contentRevision;
            cached.byteSize = static_cast<size_t>(srcW) * srcH *
                sizeof(uint32_t) * 4u / 3u;
            m_cachedImageTextureBytes += cached.byteSize;
            trimImageTextureCache(resourceId);
        } else {
            glBindTexture(GL_TEXTURE_2D, cached.texture);
        }

        const auto destination = mapLogicalRasterDestination(
            dstX, dstY, drawWidth, drawHeight, cornerRadius,
            m_deviceScale, m_contentOriginX, m_contentOriginY);
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        if (destination.cornerRadius > 0.001f) {
            drawMaskedBgraTextureQuad(
                cached.texture, destination.x, destination.y,
                destination.width, destination.height,
                destination.cornerRadius, cornerRoundness, opacity,
                squareTopCorners, false);
        } else {
            drawBgraTextureQuad(
                cached.texture, destination.x, destination.y,
                destination.width, destination.height, opacity);
        }
        return;
    }
#endif

    drawBufferTransformed(
        dstX, dstY, srcW, srcH, pixelData, stridePixels, opacity,
        cornerRadius, cornerRoundness, squareTopCorners,
        drawWidth, drawHeight);
}

void RasterRenderer::drawCachedShmBufferTransformed(uint64_t cacheKey,
                                                     uint64_t contentSerial,
                                                     float dstX,
                                                     float dstY,
                                                     int srcW,
                                                     int srcH,
                                                     int backingW,
                                                     int backingH,
                                                     const uint32_t* pixelData,
                                                     int stridePixels,
                                                     int damageX,
                                                     int damageY,
                                                     int damageW,
                                                     int damageH,
                                                     float opacity,
                                                     float cornerRadius,
                                                     float cornerRoundness,
                                                     bool squareTopCorners,
                                                     float drawWidth,
                                                     float drawHeight,
                                                     RasterBufferSampling sampling) {
    if (!m_initialized || !pixelData || srcW <= 0 || srcH <= 0 ||
        cacheKey == 0 || contentSerial == 0) {
        drawBufferTransformed(dstX, dstY, srcW, srcH, pixelData, stridePixels,
                              opacity, cornerRadius, cornerRoundness,
                              squareTopCorners, drawWidth, drawHeight,
                              sampling);
        return;
    }

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        if (stridePixels <= 0) stridePixels = srcW;
        backingW = std::max({srcW, backingW, stridePixels});
        backingH = std::max(srcH, backingH);
        const auto destination = mapLogicalRasterDestination(
            dstX, dstY, drawWidth, drawHeight, cornerRadius,
            m_deviceScale, m_contentOriginX, m_contentOriginY);
        if (destination.width <= 0.0f || destination.height <= 0.0f) return;

        m_eglBackend->makeCurrent();
        auto& cached = m_cachedShmTextures[cacheKey];
        cached.lastUsedFrame = m_shmTextureFrameSerial;

        const bool sizeChanged = cached.width != backingW || cached.height != backingH;
        if (cached.texture == 0) glGenTextures(1, &cached.texture);
        glBindTexture(GL_TEXTURE_2D, cached.texture);
        if (sizeChanged || cached.width == 0 || cached.height == 0) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, backingW, backingH, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            cached.width = backingW;
            cached.height = backingH;
            cached.uploadedContentSerial = 0;
        }

        if (cached.uploadedContentSerial != contentSerial) {
            const bool completeUpload = sizeChanged || cached.uploadedContentSerial == 0;
            int uploadX = completeUpload ? 0 : std::clamp(damageX, 0, srcW);
            int uploadY = completeUpload ? 0 : std::clamp(damageY, 0, srcH);
            int uploadW = completeUpload ? srcW : std::clamp(damageW, 0, srcW - uploadX);
            int uploadH = completeUpload ? srcH : std::clamp(damageH, 0, srcH - uploadY);
            if (uploadW == 0 || uploadH == 0) {
                uploadX = 0;
                uploadY = 0;
                uploadW = srcW;
                uploadH = srcH;
            }

            // SHM uses straight alpha. Convert once per client commit instead
            // of repeating this scan for unrelated compositor redraws. Damage
            // keeps both conversion and the Android texture upload bounded.
            const uint32_t* uploadPixels = pixelData +
                static_cast<size_t>(uploadY) * stridePixels + uploadX;
            int uploadStride = stridePixels;
            bool needsPremultiply = false;
            for (int y = 0; y < uploadH && !needsPremultiply; ++y) {
                const uint32_t* row = uploadPixels + static_cast<size_t>(y) * uploadStride;
                for (int x = 0; x < uploadW; ++x) {
                    const uint32_t pixel = row[x];
                    if ((pixel >> 24u) != 0xFFu && (pixel & 0x00FFFFFFu) != 0u) {
                        needsPremultiply = true;
                        break;
                    }
                }
            }

            std::vector<uint32_t> premultipliedPixels;
            if (needsPremultiply) {
                premultipliedPixels.resize(static_cast<size_t>(uploadW) * uploadH);
                for (int y = 0; y < uploadH; ++y) {
                    const uint32_t* source = uploadPixels + static_cast<size_t>(y) * uploadStride;
                    uint32_t* destinationPixels = premultipliedPixels.data() +
                        static_cast<size_t>(y) * uploadW;
                    for (int x = 0; x < uploadW; ++x) {
                        destinationPixels[x] = alpha::premultiplyArgb(source[x]);
                    }
                }
                uploadPixels = premultipliedPixels.data();
                uploadStride = uploadW;
            }

            if (uploadStride == uploadW) {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                                uploadX, uploadY, uploadW, uploadH,
                                GL_RGBA, GL_UNSIGNED_BYTE, uploadPixels);
            } else if (m_glSupportsUnpackRowLength) {
                // The Android compositor owns a GLES3 context, while GBM may
                // expose the same facility through GL_EXT_unpack_subimage.
                // Preserve the SHM backing stride and upload the dirty block
                // once instead of issuing one synchronous driver call per row.
                glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, uploadStride);
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                                uploadX, uploadY, uploadW, uploadH,
                                GL_RGBA, GL_UNSIGNED_BYTE, uploadPixels);
                glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);
            } else {
                // GLES2-compatible fallback for tightly bounded damage. The
                // capability check keeps this path valid on older GBM GLES2
                // drivers that do not expose strided pixel unpacking.
                for (int y = 0; y < uploadH; ++y) {
                    glTexSubImage2D(
                        GL_TEXTURE_2D, 0, uploadX, uploadY + y, uploadW, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE,
                        uploadPixels + static_cast<size_t>(y) * uploadStride);
                }
            }
            cached.uploadedContentSerial = contentSerial;
        }

        const float uMax = srcW == cached.width
            ? 1.0f : (static_cast<float>(srcW) - 0.5f) / cached.width;
        const float vMax = srcH == cached.height
            ? 1.0f : (static_cast<float>(srcH) - 0.5f) / cached.height;
        const BufferSampleScale sampleScale = resolveBufferSampleScale(
            sampling, srcW, srcH, destination.width, destination.height);
        const bool requiresSamplingShader =
            sampling != RasterBufferSampling::Stretch;
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        if (destination.cornerRadius > 0.001f || requiresSamplingShader) {
            drawMaskedBgraTextureQuad(cached.texture,
                                      destination.x, destination.y,
                                      destination.width, destination.height,
                                      destination.cornerRadius, cornerRoundness,
                                      opacity, squareTopCorners, false,
                                      uMax * sampleScale.x,
                                      vMax * sampleScale.y,
                                      uMax, vMax);
        } else {
            drawBgraTextureQuad(cached.texture,
                                destination.x, destination.y,
                                destination.width, destination.height, opacity,
                                uMax, vMax);
        }
        return;
    }
#endif

    drawBufferTransformed(dstX, dstY, srcW, srcH, pixelData, stridePixels,
                          opacity, cornerRadius, cornerRoundness,
                          squareTopCorners, drawWidth, drawHeight, sampling);
}

void RasterRenderer::drawBufferRaw(float dstX,
                                 float dstY,
                                 int srcW,
                                 int srcH,
                                 const uint32_t* pixelData,
                                 int stridePixels,
                                 float opacity,
                                 float cornerRadius,
                                 float cornerRoundness,
                                 bool squareTopCorners,
                                 bool squareBottomCorners,
                                 float drawWidth,
                                 float drawHeight,
                                 RasterBufferSampling sampling) {
    if (!m_initialized || !pixelData || srcW <= 0 || srcH <= 0) return;

    if (stridePixels <= 0) stridePixels = srcW;

    const float outW = (drawWidth > 0.0f) ? drawWidth : static_cast<float>(srcW);
    const float outH = (drawHeight > 0.0f) ? drawHeight : static_cast<float>(srcH);
    if (outW <= 0.0f || outH <= 0.0f) return;
    const BufferSampleScale sampleScale = resolveBufferSampleScale(
        sampling, srcW, srcH, outW, outH);

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();

        if (m_glClientTexture == 0) {
            glGenTextures(1, &m_glClientTexture);
            glBindTexture(GL_TEXTURE_2D, m_glClientTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }

        glBindTexture(GL_TEXTURE_2D, m_glClientTexture);
        bool textureSizeChanged =
            (m_glClientTextureWidth != srcW) || (m_glClientTextureHeight != srcH);
        if (textureSizeChanged) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, srcW, srcH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            m_glClientTextureWidth = srcW;
            m_glClientTextureHeight = srcH;
        }

        // CPU and SHM pixels are straight ARGB. Convert before upload so
        // bilinear filtering and mip generation operate on premultiplied
        // texels instead of creating dark fringes at translucent edges.
        const uint32_t* uploadPixels = pixelData;
        int uploadStride = stridePixels;
        bool needsPremultiply = false;
        for (int y = 0; y < srcH && !needsPremultiply; ++y) {
            const uint32_t* row = pixelData + static_cast<size_t>(y) * stridePixels;
            for (int x = 0; x < srcW; ++x) {
                const uint32_t pixel = row[x];
                if ((pixel >> 24u) != 0xFFu && (pixel & 0x00FFFFFFu) != 0u) {
                    needsPremultiply = true;
                    break;
                }
            }
        }

        std::vector<uint32_t> premultipliedPixels;
        if (needsPremultiply) {
            premultipliedPixels.resize(static_cast<size_t>(srcW) * srcH);
            for (int y = 0; y < srcH; ++y) {
                const uint32_t* source = pixelData + static_cast<size_t>(y) * stridePixels;
                uint32_t* destination = premultipliedPixels.data() +
                    static_cast<size_t>(y) * srcW;
                for (int x = 0; x < srcW; ++x) {
                    destination[x] = alpha::premultiplyArgb(source[x]);
                }
            }
            uploadPixels = premultipliedPixels.data();
            uploadStride = srcW;
        }

        if (uploadStride == srcW) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, srcW, srcH,
                            GL_RGBA, GL_UNSIGNED_BYTE, uploadPixels);
        } else {
            for (int y = 0; y < srcH; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, srcW, 1,
                                GL_RGBA, GL_UNSIGNED_BYTE,
                                uploadPixels + static_cast<size_t>(y) * uploadStride);
            }
        }
        glGenerateMipmap(GL_TEXTURE_2D);

        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);

        if (cornerRadius > 0.001f ||
            sampling != RasterBufferSampling::Stretch) {
            drawMaskedBgraTextureQuad(m_glClientTexture,
                                      static_cast<float>(dstX),
                                      static_cast<float>(dstY),
                                      static_cast<float>(outW),
                                      static_cast<float>(outH),
                                      cornerRadius,
                                      cornerRoundness,
                                      opacity,
                                      squareTopCorners,
                                      squareBottomCorners,
                                      sampleScale.x,
                                      sampleScale.y);
        } else {
            drawBgraTextureQuad(m_glClientTexture, dstX, dstY, outW, outH, opacity);
        }
        return;
    }
#endif

    if (!m_targetPixels) return;

    int clipX1 = std::max(0, static_cast<int>(std::floor(dstX)));
    int clipY1 = std::max(0, static_cast<int>(std::floor(dstY)));
    int clipX2 = std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(dstX + outW)));
    int clipY2 = std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(dstY + outH)));

    if (m_clipRect) {
        const RasterRect deviceClip = scaleRect(*m_clipRect);
        clipX1 = std::max(clipX1, std::max(0, static_cast<int>(std::floor(deviceClip.x))));
        clipY1 = std::max(clipY1, std::max(0, static_cast<int>(std::floor(deviceClip.y))));
        clipX2 = std::min(clipX2, std::min(static_cast<int>(m_width), static_cast<int>(std::ceil(deviceClip.x + deviceClip.width))));
        clipY2 = std::min(clipY2, std::min(static_cast<int>(m_height), static_cast<int>(std::ceil(deviceClip.y + deviceClip.height))));
    }

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    bool isOpaqueFast = (opacity >= 0.99f);
    const float maxRadius = squareBottomCorners
        ? std::min(static_cast<float>(outW) * 0.5f, static_cast<float>(outH))
        : std::min(static_cast<float>(outW), static_cast<float>(outH)) * 0.5f;
    float rr = std::clamp(cornerRadius, 0.0f, maxRadius);
    float n = std::clamp(cornerRoundness, 2.0f, 8.0f);

    auto insideRoundedMask = [rr, n, outW, outH, squareTopCorners, squareBottomCorners](float px, float py) {
        if (rr <= 0.001f) return true;
        if (px < 0.0f || py < 0.0f || px > outW || py > outH) return false;

        bool inLeft = px < rr;
        bool inRight = px > (outW - rr);
        bool inTop = py < rr;
        bool inBottom = !squareBottomCorners && py > (outH - rr);

        if ((inLeft || inRight) && (inTop || inBottom)) {
            if (squareTopCorners && inTop) {
                return true;
            }
            float cx = inLeft ? rr : (outW - rr);
            float cy = inTop ? rr : (outH - rr);
            float dx = std::abs(px - cx) / rr;
            float dy = std::abs(py - cy) / rr;
            if (n <= 2.001f) {
                return (dx * dx + dy * dy) <= 1.0f;
            }
            return std::pow(dx, n) + std::pow(dy, n) <= 1.0f;
        }

        return true;
    };

    const auto lerpByte = [](uint8_t a, uint8_t b, float amount) {
        return static_cast<uint8_t>(std::clamp(std::lround(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * amount),
            0l, 255l));
    };
    const auto bilinearPixel = [&](float sx, float sy) {
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, srcW - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, srcH - 1);
        const int x1 = std::min(x0 + 1, srcW - 1);
        const int y1 = std::min(y0 + 1, srcH - 1);
        const float tx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);
        const float ty = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);
        const uint32_t p00 = pixelData[y0 * stridePixels + x0];
        const uint32_t p10 = pixelData[y0 * stridePixels + x1];
        const uint32_t p01 = pixelData[y1 * stridePixels + x0];
        const uint32_t p11 = pixelData[y1 * stridePixels + x1];
        const auto channel = [&](int shift) {
            const uint8_t top = lerpByte(
                static_cast<uint8_t>((p00 >> shift) & 0xFFu),
                static_cast<uint8_t>((p10 >> shift) & 0xFFu), tx);
            const uint8_t bottom = lerpByte(
                static_cast<uint8_t>((p01 >> shift) & 0xFFu),
                static_cast<uint8_t>((p11 >> shift) & 0xFFu), tx);
            return lerpByte(top, bottom, ty);
        };
        return (static_cast<uint32_t>(channel(24)) << 24) |
               (static_cast<uint32_t>(channel(16)) << 16) |
               (static_cast<uint32_t>(channel(8)) << 8) |
               static_cast<uint32_t>(channel(0));
    };

    for (int y = clipY1; y < clipY2; ++y) {
        const float outY = (static_cast<float>(y) + 0.5f) - dstY;
        const float srcY = (outY / outH) * static_cast<float>(srcH) *
            sampleScale.y - 0.5f;
        uint32_t* dstRow = &m_targetPixels[y * m_width];

        for (int x = clipX1; x < clipX2; ++x) {
            const float outX = (static_cast<float>(x) + 0.5f) - dstX;
            const float srcX = (outX / outW) * static_cast<float>(srcW) *
                sampleScale.x - 0.5f;
            if (rr > 0.001f) {
                if (!insideRoundedMask(outX, outY)) {
                    continue;
                }
            }

            uint32_t pixel = bilinearPixel(srcX, srcY);
            uint8_t rawA = static_cast<uint8_t>((pixel >> 24) & 0xFF);
            if (rawA == 0) continue;

            if (rawA == 255 && isOpaqueFast) {
                dstRow[x] = pixel;
            } else {
                const float srcA = (static_cast<float>(rawA) / 255.0f) * opacity;
                const float invSrcA = 1.0f - srcA;

                const uint32_t bg = dstRow[x];
                const float dstA = static_cast<float>((bg >> 24) & 0xFF) / 255.0f;
                const float outA = srcA + dstA * invSrcA;
                if (outA <= 0.0001f) {
                    dstRow[x] = 0x00000000u;
                    continue;
                }

                const float r =
                    (static_cast<float>((pixel >> 16) & 0xFF) * srcA +
                     static_cast<float>((bg >> 16) & 0xFF) * dstA * invSrcA) / outA;
                const float g =
                    (static_cast<float>((pixel >> 8) & 0xFF) * srcA +
                     static_cast<float>((bg >> 8) & 0xFF) * dstA * invSrcA) / outA;
                const float b =
                    (static_cast<float>(pixel & 0xFF) * srcA +
                     static_cast<float>(bg & 0xFF) * dstA * invSrcA) / outA;

                const uint32_t outAlpha = static_cast<uint32_t>(
                    std::clamp(std::lround(outA * 255.0f), 0l, 255l));
                const uint32_t outRed = static_cast<uint32_t>(
                    std::clamp(std::lround(r), 0l, 255l));
                const uint32_t outGreen = static_cast<uint32_t>(
                    std::clamp(std::lround(g), 0l, 255l));
                const uint32_t outBlue = static_cast<uint32_t>(
                    std::clamp(std::lround(b), 0l, 255l));

                dstRow[x] = (outAlpha << 24) | (outRed << 16) | (outGreen << 8) | outBlue;
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

ColorMatrix4x4 createTintMatrix(const protocol::FilterOp& op) {
    ColorMatrix4x4 mat;
    const float alpha = std::clamp(op.value, 0.0f, 1.0f);
    const float sourceScale = 1.0f - alpha;
    mat.m[0][0] = sourceScale;
    mat.m[1][1] = sourceScale;
    mat.m[2][2] = sourceScale;
    mat.o[0] = std::clamp(op.params[0], 0.0f, 255.0f) * alpha;
    mat.o[1] = std::clamp(op.params[1], 0.0f, 255.0f) * alpha;
    mat.o[2] = std::clamp(op.params[2], 0.0f, 255.0f) * alpha;
    return mat;
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

} // namespace

void RasterRenderer::applyBackdropFilter(float logicalX, float logicalY,
                                       float logicalWidth, float logicalHeight,
                                       float cornerRadius, float cornerRoundness,
                                       float opacity, const std::vector<protocol::FilterOp>& logicalFilters) {
    // Damage/effect bounds remain fractional until this raster boundary. The
    // enclosing device rect must never drop a partially covered edge.
    const int dstX = static_cast<int>(std::floor(
        logicalX * m_deviceScale + m_contentOriginX));
    const int dstY = static_cast<int>(std::floor(
        logicalY * m_deviceScale + m_contentOriginY));
    const int right = static_cast<int>(std::ceil(
        (logicalX + logicalWidth) * m_deviceScale + m_contentOriginX));
    const int bottom = static_cast<int>(std::ceil(
        (logicalY + logicalHeight) * m_deviceScale + m_contentOriginY));
    const int srcW = right - dstX;
    const int srcH = bottom - dstY;
    cornerRadius *= m_deviceScale;
    std::vector<protocol::FilterOp> scaledFilters = logicalFilters;
    for (auto& filter : scaledFilters) {
        if (filter.type == protocol::FilterType::Blur) {
            filter.value *= m_deviceScale;
        } else if (filter.type == protocol::FilterType::Glass) {
            filter.params[0] *= m_deviceScale;
        }
    }
    const auto& filters = scaledFilters;
    if (!m_initialized || srcW <= 0 || srcH <= 0 || filters.empty()) return;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + srcW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + srcH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    int w = clipX2 - clipX1;
    int h = clipY2 - clipY1;
    const float clampedRoundness = std::clamp(cornerRoundness, 2.0f, 8.0f);
    const BackdropFilterGeometry geometry = computeBackdropFilterGeometry(
        dstX, dstY, srcW, srcH,
        static_cast<int>(m_width), static_cast<int>(m_height));
    if (geometry.effect.width <= 0 || geometry.effect.height <= 0 ||
        geometry.capture.width <= 0 || geometry.capture.height <= 0) {
        return;
    }

    auto resolveGlassValues = [&](const protocol::FilterOp& op,
                                  float& outThicknessPx,
                                  float& outRefractionFactor,
                                  float& outDispersionGain) {
        outThicknessPx = std::max(0.0f, op.params[0]);
        outRefractionFactor = std::max(0.0f, op.params[1]);
        outDispersionGain = std::max(0.0f, op.params[2]);
    };

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == RasterBackend::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();
        const bool gpuBlurAvailable = m_eglBackend->isHardwareAccelerated();

        // The frame-damage scissor is expressed in scene coordinates. Blur,
        // color and refraction intermediates use compact local FBOs, so that
        // scissor would be invalid there. Restore it only for the final write
        // back into the retained scene.
        glDisable(GL_SCISSOR_TEST);

        const int effectX = geometry.effect.x;
        const int effectY = geometry.effect.y;
        const int effectW = geometry.effect.width;
        const int effectH = geometry.effect.height;
        const int captureX = geometry.capture.x;
        const int captureY = geometry.capture.y;
        const int captureW = geometry.capture.width;
        const int captureH = geometry.capture.height;

        float blurPassScale = 1.0f;
        if (gpuBlurAvailable) {
            for (const auto& op : filters) {
                if (op.type != protocol::FilterType::Blur) continue;
                const auto plan = computeBackdropBlurPlan(op.value);
                blurPassScale = std::max(
                    blurPassScale, plan.downsampleScale);
            }
        }

        int targetW = std::max(1, static_cast<int>(std::lround(
            static_cast<float>(captureW) / blurPassScale)));
        int targetH = std::max(1, static_cast<int>(std::lround(
            static_cast<float>(captureH) / blurPassScale)));

        if (targetW > static_cast<int>(m_glFBOCapacityWidth) ||
            targetH > static_cast<int>(m_glFBOCapacityHeight)) {
            m_glFBOCapacityWidth = std::max(m_glFBOCapacityWidth,
                                            static_cast<uint32_t>(targetW));
            m_glFBOCapacityHeight = std::max(m_glFBOCapacityHeight,
                                             static_cast<uint32_t>(targetH));
            for (uint32_t texture : m_glFBOTexture) {
                glBindTexture(GL_TEXTURE_2D, texture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                             static_cast<GLsizei>(m_glFBOCapacityWidth),
                             static_cast<GLsizei>(m_glFBOCapacityHeight), 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
        }
        const float captureUScale = static_cast<float>(targetW) /
            static_cast<float>(std::max(1u, m_glFBOCapacityWidth));
        const float captureVScale = static_cast<float>(targetH) /
            static_cast<float>(std::max(1u, m_glFBOCapacityHeight));

        // 1. Downsample the FULL source sub-rect of the current scene texture into
        // a smaller FBO target. This avoids the zoom artifact caused by copying only
        // the top-left corner with glCopyTexSubImage2D.
        glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[0]);
        glViewport(0, 0, targetW, targetH);

        const float textureWidth = static_cast<float>(
            m_glExternalFrameTexture && m_glExternalBackingWidth
                ? m_glExternalBackingWidth
                : std::max(1u, m_glSceneCapacityWidth));
        const float textureHeight = static_cast<float>(
            m_glExternalFrameTexture && m_glExternalBackingHeight
                ? m_glExternalBackingHeight
                : std::max(1u, m_glSceneCapacityHeight));
        float uLeft = static_cast<float>(captureX) / textureWidth;
        float uRight = static_cast<float>(captureX + captureW) / textureWidth;
        // Client external FBO content occupies the lower-left viewport of its
        // capacity allocation. UI coordinates are top-left, so invert within
        // the content viewport before normalizing by backing capacity.
        float vTop = static_cast<float>(
            static_cast<int>(m_height) - captureY) / textureHeight;
        float vBottom = static_cast<float>(
            static_cast<int>(m_height) - (captureY + captureH)) / textureHeight;

        float cropQuad[16] = {
            -1.0f,  1.0f,  uLeft,  vTop,
            -1.0f, -1.0f,  uLeft,  vBottom,
             1.0f,  1.0f,  uRight, vTop,
             1.0f, -1.0f,  uRight, vBottom,
        };

        glUseProgram(m_glProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, activeSceneTexture());
        glUniform1i(m_uTextureLoc, 0);
        glUniform1f(m_uOpacityLoc, 1.0f);
        glDisable(GL_BLEND);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(m_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), cropQuad);
        glEnableVertexAttribArray(m_aPosLoc);
        glVertexAttribPointer(m_aTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), cropQuad + 2);
        glEnableVertexAttribArray(m_aTexLoc);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(m_aPosLoc);
        glDisableVertexAttribArray(m_aTexLoc);

        static const float quad[16] = {
            -1.0f,  1.0f,  0.0f, 1.0f,
            -1.0f, -1.0f,  0.0f, 0.0f,
             1.0f,  1.0f,  1.0f, 1.0f,
             1.0f, -1.0f,  1.0f, 0.0f,
        };

        ColorMatrix4x4 pendingColorMatrix;
        int currentTex = 0;

        auto renderColorPass = [&]() {
            if (pendingColorMatrix.isIdentity()) return;
            int nextTex = 1 - currentTex;

            glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[nextTex]);
            glViewport(0, 0, targetW, targetH);

            glUseProgram(m_glColorMatrixProgram);

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(m_aColorPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
            glEnableVertexAttribArray(m_aColorPosLoc);
            glVertexAttribPointer(m_aColorTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
            glEnableVertexAttribArray(m_aColorTexLoc);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[currentTex]);
            glUniform1i(m_uColorTextureLoc, 0);
            glUniform2f(m_uColorInputScaleLoc, captureUScale, captureVScale);

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

        auto runBlurPass = [&](const protocol::FilterOp& blur) {
            const auto plan = computeBackdropBlurPlan(blur.value);
            const float blurValue = plan.gaussianValuePx;
            if (blurValue <= 0.05f) {
                return;
            }
            float adjustedValue = blurValue / blurPassScale;
            float sigma = std::max(0.5f, adjustedValue / 2.0f);
            int radius = std::clamp(static_cast<int>(std::ceil(3.0f * sigma)), 1, 32);

            // Horizontal Pass
            int blurPass1 = 1 - currentTex;
            glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[blurPass1]);
            glViewport(0, 0, targetW, targetH);

            glUseProgram(m_glBlurProgram);

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(m_aBlurPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
            glEnableVertexAttribArray(m_aBlurPosLoc);
            glVertexAttribPointer(m_aBlurTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
            glEnableVertexAttribArray(m_aBlurTexLoc);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[currentTex]);
            glUniform1i(m_uBlurTextureLoc, 0);
            glUniform2f(m_uBlurDirLoc, 1.0f / static_cast<float>(targetW), 0.0f);
            glUniform1f(m_uBlurSigmaLoc, sigma);
            glUniform1i(m_uBlurRadiusLoc, radius);
            glUniform2f(m_uBlurSizeLoc, static_cast<float>(targetW), static_cast<float>(targetH));
            glUniform1f(m_uBlurCornerRadiusLoc, 0.0f);
            glUniform1f(m_uBlurRoundnessLoc, clampedRoundness);
            glUniform2f(m_uBlurInputScaleLoc, captureUScale, captureVScale);
            glUniform4f(m_uBlurFramebufferClipEdgesLoc,
                        geometry.clippedLeft ? 1.0f : 0.0f,
                        geometry.clippedRight ? 1.0f : 0.0f,
                        geometry.clippedBottom ? 1.0f : 0.0f,
                        geometry.clippedTop ? 1.0f : 0.0f);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            // Vertical Pass
            int blurPass2 = currentTex;
            glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[blurPass2]);
            glViewport(0, 0, targetW, targetH);

            glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[blurPass1]);
            glUniform1i(m_uBlurTextureLoc, 0);
            glUniform2f(m_uBlurDirLoc, 0.0f, 1.0f / static_cast<float>(targetH));
            glUniform1f(m_uBlurSigmaLoc, sigma);
            glUniform1i(m_uBlurRadiusLoc, radius);
            glUniform2f(m_uBlurSizeLoc, static_cast<float>(targetW), static_cast<float>(targetH));
            glUniform1f(m_uBlurCornerRadiusLoc, 0.0f);
            glUniform1f(m_uBlurRoundnessLoc, clampedRoundness);
            glUniform2f(m_uBlurInputScaleLoc, captureUScale, captureVScale);
            glUniform4f(m_uBlurFramebufferClipEdgesLoc,
                        geometry.clippedLeft ? 1.0f : 0.0f,
                        geometry.clippedRight ? 1.0f : 0.0f,
                        geometry.clippedBottom ? 1.0f : 0.0f,
                        geometry.clippedTop ? 1.0f : 0.0f);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(m_aBlurPosLoc);
            glDisableVertexAttribArray(m_aBlurTexLoc);
        };

        auto runRefractionPass = [&](float thicknessPx, float refractionFactor, float dispersionGain) {
            if (thicknessPx <= 0.01f || refractionFactor <= 0.0f ||
                m_glRefractionProgram == 0) {
                return;
            }

            int nextTex = 1 - currentTex;
            glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[nextTex]);
            glViewport(0, 0, targetW, targetH);

            glUseProgram(m_glRefractionProgram);

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glVertexAttribPointer(m_aRefractPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
            glEnableVertexAttribArray(m_aRefractPosLoc);
            glVertexAttribPointer(m_aRefractTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
            glEnableVertexAttribArray(m_aRefractTexLoc);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[currentTex]);
            glUniform1i(m_uRefractTextureLoc, 0);
            glUniform2f(m_uRefractInvSizeLoc, 1.0f / static_cast<float>(targetW), 1.0f / static_cast<float>(targetH));
            const float passScaleX = static_cast<float>(targetW) / static_cast<float>(captureW);
            const float passScaleY = static_cast<float>(targetH) / static_cast<float>(captureH);
            const auto logicalToPassScale = computeBackdropPassScale(
                m_deviceScale, targetW, targetH, captureW, captureH);
            const float passThicknessPx = std::max(0.0f, thicknessPx) * passScaleX;
            const float passRadiusPx = std::max(0.0f, cornerRadius) * passScaleX;

            glUniform1f(m_uRefractThicknessLoc, passThicknessPx);
            glUniform1f(m_uRefractFactorLoc, std::max(1.001f, refractionFactor));
            glUniform1f(m_uRefractDispersionLoc, std::max(0.0f, dispersionGain));
            glUniform2f(m_uRefractSizeLoc,
                        static_cast<float>(geometry.maskWidth) * passScaleX,
                        static_cast<float>(geometry.maskHeight) * passScaleY);
            glUniform2f(m_uRefractCaptureSizeLoc,
                        static_cast<float>(targetW), static_cast<float>(targetH));
            glUniform2f(m_uRefractMaskOffsetLoc,
                        static_cast<float>(geometry.maskOffsetX) * passScaleX,
                        static_cast<float>(geometry.maskOffsetY) * passScaleY);
            glUniform1f(m_uRefractRadiusLoc, passRadiusPx);
            glUniform1f(m_uRefractRoundnessLoc, clampedRoundness);
            glUniform2f(m_uRefractInputScaleLoc, captureUScale, captureVScale);
            glUniform2f(m_uRefractLogicalToPassScaleLoc,
                        logicalToPassScale.x, logicalToPassScale.y);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(m_aRefractPosLoc);
            glDisableVertexAttribArray(m_aRefractTexLoc);
            currentTex = nextTex;
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
                case protocol::FilterType::Tint:
                    pendingColorMatrix.multiply(createTintMatrix(op));
                    break;
                case protocol::FilterType::Blur: {
                    // An EGL context can be Mesa llvmpipe running on the CPU.
                    // Blur is intentionally available only on audited hardware
                    // renderers (VirGL/virtio), not on that emulated GL path.
                    if (gpuBlurAvailable) {
                        renderColorPass();
                        runBlurPass(op);
                    }
                    break;
                }
                case protocol::FilterType::Glass: {
                    renderColorPass();
                    float thicknessPx = 0.0f;
                    float refractionFactor = 0.0f;
                    float dispersionGain = 0.0f;
                    resolveGlassValues(op, thicknessPx, refractionFactor, dispersionGain);
                    runRefractionPass(thicknessPx, refractionFactor, dispersionGain);
                    break;
                }
                default:
                    break;
            }
        }

        renderColorPass();

        // 2. Draw final blurred GLSL texture directly back into m_glSceneFBO ON GPU!
        // ZERO GLREADPIXELS! ZERO CPU MEMCPY!
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        applyScissorState();

        const float outputUScale = (static_cast<float>(effectW) /
            static_cast<float>(captureW)) * captureUScale;
        const float outputVScale = (static_cast<float>(effectH) /
            static_cast<float>(captureH)) * captureVScale;
        const float outputUOffset = (static_cast<float>(geometry.outputOffsetX) /
            static_cast<float>(captureW)) * captureUScale;
        const float outputOffsetBottom = static_cast<float>(
            captureH - geometry.outputOffsetY - effectH);
        const float outputVOffset = (outputOffsetBottom /
            static_cast<float>(captureH)) * captureVScale;
        drawMaskedTextureQuad(m_glFBOTexture[currentTex],
                      effectX,
                      effectY,
                      static_cast<float>(effectW),
                      static_cast<float>(effectH),
                      cornerRadius,
                      clampedRoundness,
                      opacity,
                      false,
                      outputUScale,
                      outputVScale,
                      outputUOffset,
                      outputVOffset,
                      static_cast<float>(geometry.maskWidth),
                      static_cast<float>(geometry.maskHeight),
                      static_cast<float>(geometry.maskOffsetX),
                      static_cast<float>(geometry.maskOffsetY),
                      normalizedHalfTexel(m_glFBOCapacityWidth),
                      normalizedHalfTexel(m_glFBOCapacityHeight));
        return;
    }
#endif

    // CPU Software Fallback Path (only when OpenGL ES is unavailable)
    if (!m_targetPixels) return;

    std::vector<uint32_t> crop(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&crop[y * w], &m_targetPixels[(clipY1 + y) * m_width + clipX1], w * sizeof(uint32_t));
    }

    ColorMatrix4x4 pendingColorMatrix;
    auto sdSuperRoundRect = [clampedRoundness](float px, float py,
                                               float halfW, float halfH,
                                               float radius) {
        if (radius <= 0.001f) {
            return std::max(std::abs(px) - halfW, std::abs(py) - halfH);
        }
        const float qx = std::abs(px) - halfW + radius;
        const float qy = std::abs(py) - halfH + radius;
        if (qx <= 0.0f || qy <= 0.0f) {
            return std::max(qx, qy) - radius;
        }
        const float nx = std::max(qx, 0.0f) / radius;
        const float ny = std::max(qy, 0.0f) / radius;
        const float k = std::pow(
            std::pow(nx, clampedRoundness) + std::pow(ny, clampedRoundness),
            1.0f / clampedRoundness);
        return (k - 1.0f) * radius;
    };
    auto runCpuRefraction = [&](std::vector<uint32_t>& pixels,
                                int pxW,
                                int pxH,
                                float thicknessPx,
                                float refractionFactor,
                                float dispersionGain) {
        if (thicknessPx <= 0.01f || refractionFactor <= 0.0f ||
            pxW <= 1 || pxH <= 1) return;
        std::vector<uint32_t> src = pixels;

        const float maskWidth = static_cast<float>(geometry.maskWidth);
        const float maskHeight = static_cast<float>(geometry.maskHeight);
        const float halfW = maskWidth * 0.5f;
        const float halfH = maskHeight * 0.5f;
        const float maxCorner = std::min(maskWidth, maskHeight) * 0.5f;
        const float rr = std::clamp(cornerRadius, 0.0f, maxCorner);
        const float eta = std::max(1.001f, refractionFactor);
        const float disp = std::max(0.0f, dispersionGain) * 0.02f;
        const auto logicalToPassScale = computeBackdropPassScale(
            m_deviceScale, pxW, pxH, pxW, pxH);

        auto sample = [&](float sx, float sy) -> uint32_t {
            int ix = std::clamp(static_cast<int>(std::lround(sx)), 0, pxW - 1);
            int iy = std::clamp(static_cast<int>(std::lround(sy)), 0, pxH - 1);
            return src[static_cast<size_t>(iy) * static_cast<size_t>(pxW) + static_cast<size_t>(ix)];
        };

        auto safeAsin = [](float x) {
            return std::asin(std::clamp(x, -1.0f, 1.0f));
        };

        for (int y = 0; y < pxH; ++y) {
            for (int x = 0; x < pxW; ++x) {
                const float px = static_cast<float>(geometry.maskOffsetX) +
                    (static_cast<float>(x) + 0.5f) - halfW;
                const float py = static_cast<float>(geometry.maskOffsetY) +
                    (static_cast<float>(y) + 0.5f) - halfH;
                const float sd = sdSuperRoundRect(px, py, halfW, halfH, rr);
                const float edgeDepth = std::max(0.0f, -sd);

                if (sd >= 0.0f || edgeDepth >= thicknessPx) {
                    continue;
                }

                const float eps = 1.0f;
                float sdx = sdSuperRoundRect(px + eps, py, halfW, halfH, rr) -
                            sdSuperRoundRect(px - eps, py, halfW, halfH, rr);
                float sdy = sdSuperRoundRect(px, py + eps, halfW, halfH, rr) -
                            sdSuperRoundRect(px, py - eps, halfW, halfH, rr);
                float nLen = std::sqrt(sdx * sdx + sdy * sdy);
                if (nLen <= 0.0001f) {
                    continue;
                }

                float xRatio = 1.0f - edgeDepth / thicknessPx;
                float thetaI = safeAsin(xRatio * xRatio);
                float thetaT = safeAsin((1.0f / eta) * std::sin(thetaI));
                float edgeFactor = std::max(0.0f, -std::tan(thetaT - thetaI));
                edgeFactor = std::min(edgeFactor, 4.0f);

                const float nx = sdx / nLen;
                const float ny = sdy / nLen;
                constexpr float kRefractionDisplacementLogical = 70.0f;
                const float offX = -nx * edgeFactor *
                    kRefractionDisplacementLogical * logicalToPassScale.x;
                const float offY = -ny * edgeFactor *
                    kRefractionDisplacementLogical * logicalToPassScale.y;

                uint32_t pr = sample(static_cast<float>(x) + offX * (1.0f + disp), static_cast<float>(y) + offY * (1.0f + disp));
                uint32_t pg = sample(static_cast<float>(x) + offX, static_cast<float>(y) + offY);
                uint32_t pb = sample(static_cast<float>(x) + offX * (1.0f - disp), static_cast<float>(y) + offY * (1.0f - disp));
                uint32_t base = src[static_cast<size_t>(y) * static_cast<size_t>(pxW) + static_cast<size_t>(x)];

                uint8_t a = static_cast<uint8_t>((base >> 24) & 0xFF);
                uint8_t r = static_cast<uint8_t>((pr >> 16) & 0xFF);
                uint8_t g = static_cast<uint8_t>((pg >> 8) & 0xFF);
                uint8_t b = static_cast<uint8_t>(pb & 0xFF);

                pixels[static_cast<size_t>(y) * static_cast<size_t>(pxW) + static_cast<size_t>(x)] =
                    (static_cast<uint32_t>(a) << 24) |
                    (static_cast<uint32_t>(r) << 16) |
                    (static_cast<uint32_t>(g) << 8) |
                    static_cast<uint32_t>(b);
            }
        }
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
            case protocol::FilterType::Tint:
                pendingColorMatrix.multiply(createTintMatrix(op));
                break;
            case protocol::FilterType::Blur:
                // Backdrop blur is GPU-only. Do not emulate it in the software
                // raster fallback: it is expensive and its rectangular sampling
                // does not match the final rounded effect mask.
                break;
            case protocol::FilterType::Glass: {
                if (!pendingColorMatrix.isIdentity()) {
                    applyColorMatrixToPixels(crop, w, h, pendingColorMatrix);
                    pendingColorMatrix.reset();
                }
                float thicknessPx = 0.0f;
                float refractionFactor = 0.0f;
                float dispersionGain = 0.0f;
                resolveGlassValues(op, thicknessPx, refractionFactor, dispersionGain);
                runCpuRefraction(crop, w, h, thicknessPx, refractionFactor, dispersionGain);
                break;
            }
            default:
                break;
        }
    }

    if (!pendingColorMatrix.isIdentity()) {
        applyColorMatrixToPixels(crop, w, h, pendingColorMatrix);
    }

    if (cornerRadius > 0.5f || opacity < 0.999f) {
        const float radius = std::max(0.0f, cornerRadius);
        const float maskWidth = static_cast<float>(geometry.maskWidth);
        const float maskHeight = static_cast<float>(geometry.maskHeight);
        const float maxR = std::min(maskWidth, maskHeight) * 0.5f;
        const float r = std::min(radius, maxR);
        const float alphaMul = std::clamp(opacity, 0.0f, 1.0f);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float fx = static_cast<float>(geometry.maskOffsetX) +
                    (static_cast<float>(x) + 0.5f) - maskWidth * 0.5f;
                const float fy = static_cast<float>(geometry.maskOffsetY) +
                    (static_cast<float>(y) + 0.5f) - maskHeight * 0.5f;
                const bool inside = sdSuperRoundRect(
                    fx, fy, maskWidth * 0.5f,
                    maskHeight * 0.5f, r) <= 0.0f;

                uint32_t& p = crop[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                uint8_t a = static_cast<uint8_t>((p >> 24) & 0xFF);
                if (!inside) {
                    a = 0;
                } else if (alphaMul < 0.999f) {
                    a = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(static_cast<float>(a) * alphaMul)), 0, 255));
                }
                p = (static_cast<uint32_t>(a) << 24) | (p & 0x00FFFFFF);
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        std::memcpy(&m_targetPixels[(clipY1 + y) * m_width + clipX1], &crop[y * w], w * sizeof(uint32_t));
    }
}

} // namespace lcl::render
