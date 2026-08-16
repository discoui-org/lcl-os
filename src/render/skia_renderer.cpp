#include "render/skia_renderer.hpp"
#include "render/backdrop_filter_geometry.hpp"
#include "render/dma_buf_crop.hpp"
#ifndef LCL_SOFTWARE_ONLY
#include <GLES2/gl2.h>
#endif
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace lcl::render {

#ifndef LCL_SOFTWARE_ONLY
static GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    return shader;
}

void beginStraightAlphaSourceOver() {
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void endStraightAlphaSourceOver() {
    glDisable(GL_BLEND);
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
        "uniform float uOpacity;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    gl_FragColor = vec4(c.rgb, c.a * uOpacity);\n"
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
        "vec2 mirrorTexCoord(vec2 uv) {\n"
        "    vec2 m = mod(abs(uv), 2.0);\n"
        "    return vec2(m.x > 1.0 ? 2.0 - m.x : m.x, m.y > 1.0 ? 2.0 - m.y : m.y);\n"
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
        "            vec2 tap = mirrorTexCoord(vTexCoord + uDirection * fi);\n"
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
        "    vec4 c = texture2D(uTexture, uSampleOffset + vTexCoord * uSampleScale);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 p = (vTexCoord - vec2(0.5)) * uSizePx;\n"
        "    vec2 halfSize = uSizePx * 0.5;\n"
        "    float d = (uSquareTopCorners > 0.5 && p.y > 0.0)\n"
        "        ? -1.0\n"
        "        : sdSuperRoundRect(p, halfSize, r, n);\n"
        // Center the device-pixel coverage ramp on the analytic edge. This
        // remains stable while the destination quad moves through subpixels.
        "    float mask = 1.0 - smoothstep(-0.5, 0.5, d);\n"
        "    gl_FragColor = vec4(c.rgb, c.a * mask * uOpacity);\n"
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
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
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
        "    gl_FragColor = vec4(c.b, c.g, c.r, c.a * mask * uOpacity);\n"
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
        "    vec3 outRgb = (uBorderColor.rgb * aBorder + uFillColor.rgb * aFill) / outA;\n"
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
        "uniform vec2 uEffectOffsetPx;\n"
        "uniform float uRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform vec2 uInputScale;\n"
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
        "    vec4 base = texture2D(uTexture, vTexCoord * uInputScale);\n"
        "    float thickness = max(0.001, uThicknessPx);\n"
        "    float eta = max(1.001, uRefractionFactor);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 p = vTexCoord * uCaptureSizePx - uEffectOffsetPx - uSizePx * 0.5;\n"
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
        "    vec2 normal = vec2(sdx, sdy) * 700.0;\n"
        "    float xRatio = 1.0 - edgeDepth / thickness;\n"
        "    float thetaI = safeAsin(xRatio * xRatio);\n"
        "    float thetaT = safeAsin((1.0 / eta) * sin(thetaI));\n"
        "    float edgeFactor = max(0.0, -tan(thetaT - thetaI));\n"
        "    edgeFactor = min(edgeFactor, 4.0);\n"
        "    vec2 offsetUv = (-normal * edgeFactor * 0.05) * uInvSize;\n"
        "    float disp = max(0.0, uDispersionGain) * 0.02;\n"
        "    vec2 uvR = clamp(vTexCoord + offsetUv * (1.0 + disp), 0.0, 1.0);\n"
        "    vec2 uvG = clamp(vTexCoord + offsetUv, 0.0, 1.0);\n"
        "    vec2 uvB = clamp(vTexCoord + offsetUv * (1.0 - disp), 0.0, 1.0);\n"
        "    float rCh = texture2D(uTexture, uvR * uInputScale).r;\n"
        "    float gCh = texture2D(uTexture, uvG * uInputScale).g;\n"
        "    float bCh = texture2D(uTexture, uvB * uInputScale).b;\n"
        "    gl_FragColor = vec4(rCh, gCh, bCh, base.a);\n"
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
    m_uRefractEffectOffsetLoc = glGetUniformLocation(m_glRefractionProgram, "uEffectOffsetPx");
    m_uRefractRadiusLoc = glGetUniformLocation(m_glRefractionProgram, "uRadiusPx");
    m_uRefractRoundnessLoc = glGetUniformLocation(m_glRefractionProgram, "uRoundnessExp");
    m_uRefractInputScaleLoc = glGetUniformLocation(m_glRefractionProgram, "uInputScale");

    // --- GLSL BGRA Client Surface Fragment Shader ---
    const char* fBgraSrc =
        "precision mediump float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform float uOpacity;\n"
        "void main() {\n"
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    gl_FragColor = vec4(c.b, c.g, c.r, c.a * uOpacity);\n"
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

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_glFBOReady = true;
    m_glFBOCapacityWidth = m_width;
    m_glFBOCapacityHeight = m_height;

    return true;
}
#else
bool SkiaRenderer::initGLShader() {
    return false;
}
#endif

SkiaRenderer::~SkiaRenderer() {
    shutdown();
}

bool SkiaRenderer::initialize(uint32_t width, uint32_t height,
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
        m_backendType = SkiaBackendType::OpenGL_EGL;
        m_eglBackend->makeCurrent();

        initGLShader();

        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::cout << "[LCL Skia] Skia OpenGL/EGL Hardware Accelerated Backend Active ("
                  << m_width << "x" << m_height << ")!\n";
    } else
#endif
    {
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

void SkiaRenderer::setTargetPixels(uint32_t* targetPixels, uint32_t width, uint32_t height) {
    const uint32_t nextWidth = width > 0 ? width : m_width;
    const uint32_t nextHeight = height > 0 ? height : m_height;
    const bool sizeChanged = nextWidth != m_width || nextHeight != m_height;

#ifndef LCL_SOFTWARE_ONLY
    if (sizeChanged && m_initialized && m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
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
        m_backendType = SkiaBackendType::SoftwareRaster;
    }
#endif

    m_targetPixels = targetPixels;
    m_width = nextWidth;
    m_height = nextHeight;
}

void SkiaRenderer::setFrameExtent(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_width = width;
    m_height = height;
}

void SkiaRenderer::setExternalFrameTarget(uint32_t framebuffer, uint32_t texture,
                                          uint32_t backingWidth, uint32_t backingHeight) {
#ifndef LCL_SOFTWARE_ONLY
    m_glExternalFrameFBO = framebuffer;
    m_glExternalFrameTexture = texture;
    m_glExternalBackingWidth = backingWidth;
    m_glExternalBackingHeight = backingHeight;
#else
    (void)framebuffer;
    (void)texture;
#endif
}

void SkiaRenderer::clearExternalFrameTarget() {
#ifndef LCL_SOFTWARE_ONLY
    m_glExternalFrameFBO = 0;
    m_glExternalFrameTexture = 0;
    m_glExternalBackingWidth = 0;
    m_glExternalBackingHeight = 0;
#endif
}

void SkiaRenderer::shutdown() {
#ifndef LCL_SOFTWARE_ONLY
    if (m_glClientTexture > 0) {
        glDeleteTextures(1, &m_glClientTexture);
        m_glClientTexture = 0;
        m_glClientTextureWidth = 0;
        m_glClientTextureHeight = 0;
    }
    if (m_glBgraProgram > 0) {
        glDeleteProgram(m_glBgraProgram);
        m_glBgraProgram = 0;
    }
    if (m_glSceneFBO > 0) {
        glDeleteFramebuffers(1, &m_glSceneFBO);
        glDeleteTextures(1, &m_glSceneTexture);
        m_glSceneFBO = 0;
        m_glSceneTexture = 0;
    }
    m_glExternalFrameFBO = 0;
    m_glExternalFrameTexture = 0;
    if (m_glFBOReady) {
        glDeleteFramebuffers(2, m_glFBO);
        glDeleteTextures(2, m_glFBOTexture);
        m_glFBO[0] = m_glFBO[1] = 0;
        m_glFBOTexture[0] = m_glFBOTexture[1] = 0;
        m_glFBOReady = false;
        m_glFBOCapacityWidth = 0;
        m_glFBOCapacityHeight = 0;
    }
    if (m_glBlurProgram > 0) {
        glDeleteProgram(m_glBlurProgram);
        m_glBlurProgram = 0;
    }
    if (m_glColorMatrixProgram > 0) {
        glDeleteProgram(m_glColorMatrixProgram);
        m_glColorMatrixProgram = 0;
    }
    if (m_glMaskProgram > 0) {
        glDeleteProgram(m_glMaskProgram);
        m_glMaskProgram = 0;
    }
    if (m_glMaskBgraProgram > 0) {
        glDeleteProgram(m_glMaskBgraProgram);
        m_glMaskBgraProgram = 0;
    }
    if (m_glRoundRectProgram > 0) {
        glDeleteProgram(m_glRoundRectProgram);
        m_glRoundRectProgram = 0;
    }
    if (m_glRefractionProgram > 0) {
        glDeleteProgram(m_glRefractionProgram);
        m_glRefractionProgram = 0;
    }
    if (m_glTexture > 0) {
        glDeleteTextures(1, &m_glTexture);
        m_glTexture = 0;
    }
    if (m_glProgram > 0) {
        glDeleteProgram(m_glProgram);
        m_glProgram = 0;
    }
#endif
    m_rasterPixels.clear();
    m_rasterPixels.shrink_to_fit();
    m_targetPixels = nullptr;
    m_initialized = false;
}

void SkiaRenderer::setContentScale(float scale) {
    const float sanitized = (std::isfinite(scale) && scale >= 0.5f && scale <= 4.0f)
        ? scale
        : 1.0f;
    if (std::fabs(m_contentScale - sanitized) < 0.0001f) {
        return;
    }

    m_contentScale = sanitized;
    // Glyph bitmaps are raster assets, so rebuilding the cache prevents a scaled
    // client surface from reusing 1x text.
    m_fontRenderer = FontRenderer{};
    m_monospaceFontRenderer = FontRenderer{};
}

SkiaRect SkiaRenderer::scaleRect(const SkiaRect& rect) const {
    return {
        rect.x * m_contentScale + m_contentOriginX,
        rect.y * m_contentScale + m_contentOriginY,
        rect.width * m_contentScale,
        rect.height * m_contentScale,
    };
}

int SkiaRenderer::scaleCoord(int value) const {
    return static_cast<int>(std::lround(static_cast<float>(value) * m_contentScale + m_contentOriginX));
}

int SkiaRenderer::scaleLength(int value) const {
    return static_cast<int>(std::lround(static_cast<float>(value) * m_contentScale));
}

bool SkiaRenderer::ensureFont(float logicalFontSize) {
    const float deviceFontSize = std::max(1.0f, logicalFontSize * m_contentScale);
    if (!m_fontRenderer.isInitialized() ||
        std::fabs(m_fontRenderer.getFontSize() - deviceFontSize) > 0.01f) {
        m_fontRenderer = FontRenderer{};
        m_fontRenderer.loadFont("/usr/share/fonts/inter/Inter-Regular.otf", deviceFontSize);
    }
    return m_fontRenderer.isInitialized();
}

bool SkiaRenderer::ensureMonospaceFont(float logicalFontSize) {
    const float deviceFontSize = std::max(1.0f, logicalFontSize * m_contentScale);
    if (!m_monospaceFontRenderer.isInitialized() ||
        std::fabs(m_monospaceFontRenderer.getFontSize() - deviceFontSize) > 0.01f) {
        m_monospaceFontRenderer = FontRenderer{};
        constexpr const char* kFontPaths[] = {
            "/usr/share/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
            "assets/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf",
        };
        for (const char* path : kFontPaths) {
            if (m_monospaceFontRenderer.loadFont(path, deviceFontSize)) {
                break;
            }
        }
    }
    return m_monospaceFontRenderer.isInitialized();
}

#ifndef LCL_SOFTWARE_ONLY
void SkiaRenderer::drawTextureQuad(uint32_t textureId, float x, float y, float w, float h,
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

    beginStraightAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endStraightAlphaSourceOver();

    glDisableVertexAttribArray(m_aPosLoc);
    glDisableVertexAttribArray(m_aTexLoc);
}

void SkiaRenderer::drawMaskedTextureQuad(uint32_t textureId,
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
                                         float vOffset) {
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
    glUniform2f(m_uMaskSizeLoc, std::max(1.0f, w), std::max(1.0f, h));
    glUniform1f(m_uMaskRadiusLoc, std::max(0.0f, cornerRadius));
    glUniform1f(m_uMaskRoundnessLoc, std::clamp(cornerRoundness, 2.0f, 8.0f));
    glUniform1f(m_uMaskOpacityLoc, std::clamp(opacity, 0.0f, 1.0f));
    glUniform1f(m_uMaskSquareTopCornersLoc, squareTopCorners ? 1.0f : 0.0f);
    glUniform2f(m_uMaskSampleOffsetLoc, uOffset, vOffset);
    glUniform2f(m_uMaskSampleScaleLoc, uScale, vScale);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aMaskPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aMaskPosLoc);
    glVertexAttribPointer(m_aMaskTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aMaskTexLoc);

    beginStraightAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endStraightAlphaSourceOver();
    glDisableVertexAttribArray(m_aMaskPosLoc);
    glDisableVertexAttribArray(m_aMaskTexLoc);
}

void SkiaRenderer::drawBgraTextureQuad(uint32_t textureId, float x, float y, float w, float h, float opacity) {
    if (textureId == 0 || m_glBgraProgram == 0) return;

    float x1 = (x / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y1 = 1.0f - (y / static_cast<float>(m_height)) * 2.0f;
    float x2 = ((x + w) / static_cast<float>(m_width)) * 2.0f - 1.0f;
    float y2 = 1.0f - ((y + h) / static_cast<float>(m_height)) * 2.0f;

    // UV orientation for raw CPU buffer memory: Top-Left maps to (0,0), Bottom-Left maps to (0,1)
    float quad[16] = {
        x1, y1,  0.0f, 0.0f,
        x1, y2,  0.0f, 1.0f,
        x2, y1,  1.0f, 0.0f,
        x2, y2,  1.0f, 1.0f,
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

    beginStraightAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endStraightAlphaSourceOver();
    glDisableVertexAttribArray(m_aBgraPosLoc);
    glDisableVertexAttribArray(m_aBgraTexLoc);
}

void SkiaRenderer::drawMaskedBgraTextureQuad(uint32_t textureId,
                                             float x,
                                             float y,
                                             float w,
                                             float h,
                                             float cornerRadius,
                                             float cornerRoundness,
                                             float opacity,
                                             bool squareTopCorners,
                                             bool squareBottomCorners) {
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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aMaskBgraPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aMaskBgraPosLoc);
    glVertexAttribPointer(m_aMaskBgraTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aMaskBgraTexLoc);

    beginStraightAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endStraightAlphaSourceOver();

    glDisableVertexAttribArray(m_aMaskBgraPosLoc);
    glDisableVertexAttribArray(m_aMaskBgraTexLoc);
}

void SkiaRenderer::drawGpuRoundedRect(float x,
                                      float y,
                                      float w,
                                      float h,
                                      float radius,
                                      float roundness,
                                      float borderWidth,
                                      const SkiaColor& fill,
                                      const SkiaColor& border) {
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

    beginStraightAlphaSourceOver();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    endStraightAlphaSourceOver();

    glDisableVertexAttribArray(m_aRoundRectPosLoc);
    glDisableVertexAttribArray(m_aRoundRectTexLoc);
}
#endif

void SkiaRenderer::beginFrame() {
    if (!m_initialized) return;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend && activeSceneFBO() > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);
        // Client canvases are alpha surfaces composited later by the window
        // manager.  KMS composition remains opaque, but an offscreen client
        // target must preserve transparent rounded corners and glass regions.
        if (m_eglBackend->presentsToDisplay()) {
            glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        } else {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        }
        if (m_glExternalFrameFBO != 0) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, m_width, m_height);
        }
        glClear(GL_COLOR_BUFFER_BIT);
        if (m_glExternalFrameFBO != 0) glDisable(GL_SCISSOR_TEST);
    }
#endif

    if (m_targetPixels && m_glExternalFrameFBO == 0) {
        // GPU path uses m_targetPixels as a temporary CPU staging surface that can
        // be flushed into the GPU scene during composition.
        // Software path still treats it as the final opaque framebuffer.
        const uint32_t clearColor = (m_backendType == SkiaBackendType::OpenGL_EGL)
            ? 0x00000000
            : 0xFF14161D;
        std::fill_n(m_targetPixels, m_width * m_height, clearColor);
    }
}

void SkiaRenderer::endFrame() {
    if (!m_initialized) return;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        m_eglBackend->makeCurrent();

        if (m_eglBackend->presentsToDisplay()) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, m_width, m_height);
            if (m_glSceneTexture > 0) {
                drawTextureQuad(m_glSceneTexture, 0, 0, m_width, m_height);
            }
            glFlush();
            m_eglBackend->present();
        } else if (m_glExternalFrameFBO != 0) {
            glFlush();
        } else if (m_targetPixels) {
            glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
            m_eglBackend->readback(m_targetPixels, m_width, m_height);
        }
    }
#endif
}

uint32_t SkiaRenderer::importTexture(const lcl::platform::INativeBuffer& buffer) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        return m_eglBackend->importTexture(buffer);
    }
#else
    (void)buffer;
#endif
    return 0;
}

uint32_t SkiaRenderer::importDmaBuf(const lcl::platform::DmaBufDescriptor& descriptor) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        return m_eglBackend->importDmaBuf(descriptor);
    }
#else
    (void)descriptor;
#endif
    return 0;
}

void SkiaRenderer::releaseTexture(uint32_t texture) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_eglBackend) m_eglBackend->releaseTexture(texture);
#else
    (void)texture;
#endif
}

void SkiaRenderer::drawDmaBufTextureTransformed(float dstX, float dstY, int srcW, int srcH,
                                                int backingW, int backingH,
                                                uint32_t texture, float opacity,
                                                float cornerRadius, float cornerRoundness,
                                                bool squareTopCorners, float drawWidth,
                                                float drawHeight) {
#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType != SkiaBackendType::OpenGL_EGL || !m_eglBackend || texture == 0) return;
    m_eglBackend->makeCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
    glViewport(0, 0, m_width, m_height);
    const auto crop = makeDmaBufCrop(static_cast<uint32_t>(std::max(0, srcW)),
                                    static_cast<uint32_t>(std::max(0, srcH)),
                                    static_cast<uint32_t>(std::max(0, backingW)),
                                    static_cast<uint32_t>(std::max(0, backingH)));
    if (cornerRadius > 0.001f) {
        drawMaskedTextureQuad(texture, dstX, dstY, drawWidth, drawHeight,
                              cornerRadius, cornerRoundness, opacity,
                              squareTopCorners, crop.uMax, crop.vMax);
    } else {
        drawTextureQuad(texture, dstX, dstY, drawWidth, drawHeight, opacity,
                        crop.uMax, crop.vMax);
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
#endif
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
    if (!m_initialized) return;

    const SkiaRect deviceRect = scaleRect(rect);

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
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

void SkiaRenderer::drawRoundedRect(const SkiaRect& rect,
                                   float radius,
                                   const SkiaColor& color,
                                   const SkiaColor& borderColor,
                                   float borderWidth,
                                   float roundness) {
    if (!m_initialized) return;

    const SkiaRect deviceRect = scaleRect(rect);
    radius *= m_contentScale;
    borderWidth *= m_contentScale;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend && m_glRoundRectProgram > 0) {
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

void SkiaRenderer::drawTopRoundedRect(const SkiaRect& rect,
                                      float radius,
                                      const SkiaColor& color,
                                      float roundness) {
    if (!m_initialized || color.a == 0) return;

    const SkiaRect deviceRect = scaleRect(rect);
    const int width = std::max(1, static_cast<int>(std::lround(deviceRect.width)));
    const int height = std::max(1, static_cast<int>(std::lround(deviceRect.height)));
    const int dstX = static_cast<int>(std::lround(deviceRect.x));
    const int dstY = static_cast<int>(std::lround(deviceRect.y));
    const uint32_t pixel = color.toARGB();

    drawBufferRaw(dstX, dstY, 1, 1, &pixel, 1, 1.0f,
                  radius * m_contentScale, roundness,
                  false, true, width, height);
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

void SkiaRenderer::drawString(int x, int y, const std::string& text, uint32_t fgColor, float fontSize) {
    if (!m_initialized || text.empty()) return;
    ensureFont(fontSize);

    const int deviceX = scaleCoord(x);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(y) * m_contentScale + m_contentOriginY));

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend && m_fontRenderer.isInitialized()) {
        int textW = std::max(1, m_fontRenderer.getTextWidth(text));
        int textH = std::max(1, m_fontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_fontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }
#endif

    if (m_fontRenderer.isInitialized() && m_targetPixels) {
        m_fontRenderer.renderString(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor);
    }
}

void SkiaRenderer::drawMonospaceString(int x, int y, const std::string& text,
                                       uint32_t fgColor, float fontSize) {
    if (!m_initialized || text.empty() || !ensureMonospaceFont(fontSize)) return;

    const int deviceX = scaleCoord(x);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(y) * m_contentScale + m_contentOriginY));

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        const int textW = std::max(1, m_monospaceFontRenderer.getTextWidth(text));
        const int textH = std::max(1, m_monospaceFontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_monospaceFontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }
#endif

    if (m_targetPixels) {
        m_monospaceFontRenderer.renderString(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor);
    }
}

float SkiaRenderer::measureString(const std::string& text, float fontSize) {
    if (text.empty() || !ensureFont(fontSize)) return 0.0f;
    return static_cast<float>(m_fontRenderer.getTextWidth(text)) / m_contentScale;
}

float SkiaRenderer::measureMonospaceString(const std::string& text, float fontSize) {
    if (text.empty() || !ensureMonospaceFont(fontSize)) return 0.0f;
    return static_cast<float>(m_monospaceFontRenderer.getTextWidth(text)) / m_contentScale;
}

bool SkiaRenderer::rasterizeString(const std::string& text,
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

void SkiaRenderer::drawBuffer(int dstX,
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
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(dstY) * m_contentScale + m_contentOriginY));
    const int deviceW = (drawWidth > 0) ? scaleLength(drawWidth) : 0;
    const int deviceH = (drawHeight > 0) ? scaleLength(drawHeight) : 0;
    drawBufferRaw(static_cast<float>(deviceX), static_cast<float>(deviceY),
                  srcW, srcH, pixelData, stridePixels, opacity,
                  cornerRadius * m_contentScale, cornerRoundness, squareTopCorners,
                  false, static_cast<float>(deviceW), static_cast<float>(deviceH));
}

void SkiaRenderer::drawBufferTransformed(float dstX,
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
                                         float drawHeight) {
    drawBufferRaw(
        dstX * m_contentScale + m_contentOriginX,
        dstY * m_contentScale + m_contentOriginY,
        srcW, srcH, pixelData, stridePixels, opacity,
        cornerRadius * m_contentScale, cornerRoundness, squareTopCorners,
        false, drawWidth * m_contentScale, drawHeight * m_contentScale);
}

void SkiaRenderer::drawBufferRaw(float dstX,
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
                                 float drawHeight) {
    if (!m_initialized || !pixelData || srcW <= 0 || srcH <= 0) return;

    if (stridePixels <= 0) stridePixels = srcW;

    const float outW = (drawWidth > 0.0f) ? drawWidth : static_cast<float>(srcW);
    const float outH = (drawHeight > 0.0f) ? drawHeight : static_cast<float>(srcH);
    if (outW <= 0.0f || outH <= 0.0f) return;

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
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

        if (stridePixels == srcW) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, srcW, srcH, GL_RGBA, GL_UNSIGNED_BYTE, pixelData);
        } else {
            for (int y = 0; y < srcH; ++y) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, srcW, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixelData + (y * stridePixels));
            }
        }
        glGenerateMipmap(GL_TEXTURE_2D);

        glBindFramebuffer(GL_FRAMEBUFFER, activeSceneFBO());
        glViewport(0, 0, m_width, m_height);

        if (cornerRadius > 0.001f) {
            drawMaskedBgraTextureQuad(m_glClientTexture,
                                      static_cast<float>(dstX),
                                      static_cast<float>(dstY),
                                      static_cast<float>(outW),
                                      static_cast<float>(outH),
                                      cornerRadius,
                                      cornerRoundness,
                                      opacity,
                                      squareTopCorners,
                                      squareBottomCorners);
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
        const float srcY = (outY / outH) * static_cast<float>(srcH) - 0.5f;
        uint32_t* dstRow = &m_targetPixels[y * m_width];

        for (int x = clipX1; x < clipX2; ++x) {
            const float outX = (static_cast<float>(x) + 0.5f) - dstX;
            const float srcX = (outX / outW) * static_cast<float>(srcW) - 0.5f;
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

void SkiaRenderer::applyBackdropFilter(int dstX, int dstY, int srcW, int srcH,
                                       float cornerRadius, float cornerRoundness,
                                       float opacity, const std::vector<protocol::FilterOp>& filters) {
    if (!m_initialized || srcW <= 0 || srcH <= 0 || filters.empty()) return;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + srcW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + srcH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    int w = clipX2 - clipX1;
    int h = clipY2 - clipY1;
    const float clampedRoundness = std::clamp(cornerRoundness, 2.0f, 8.0f);

    auto resolveGlassValues = [&](const protocol::FilterOp& op,
                                  float& outThicknessPx,
                                  float& outRefractionFactor,
                                  float& outDispersionGain) {
        outThicknessPx = (op.params[0] > 0.0f) ? op.params[0] : 20.0f;
        outRefractionFactor = (op.params[1] > 1.0f) ? op.params[1] : 1.4f;
        outDispersionGain = (op.params[2] > 0.0f) ? op.params[2] : 7.0f;
    };

#ifndef LCL_SOFTWARE_ONLY
    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();
        const bool gpuBlurAvailable = m_eglBackend->isHardwareAccelerated();

        const BackdropFilterGeometry geometry = computeBackdropFilterGeometry(
            dstX, dstY, srcW, srcH,
            static_cast<int>(m_width), static_cast<int>(m_height));
        if (geometry.effect.width <= 0 || geometry.effect.height <= 0 ||
            geometry.capture.width <= 0 || geometry.capture.height <= 0) {
            return;
        }
        const int effectX = geometry.effect.x;
        const int effectY = geometry.effect.y;
        const int effectW = geometry.effect.width;
        const int effectH = geometry.effect.height;
        const int captureX = geometry.capture.x;
        const int captureY = geometry.capture.y;
        const int captureW = geometry.capture.width;
        const int captureH = geometry.capture.height;

        int logScale = 1;
        if (gpuBlurAvailable) {
            for (const auto& op : filters) {
                if (op.type == protocol::FilterType::Blur && op.value > 8.0f) {
                    logScale = std::clamp(1 + static_cast<int>(std::floor(std::log2(op.value / 8.0f))), 1, 4);
                }
            }
        }

        int targetW = std::max(1, captureW / logScale);
        int targetH = std::max(1, captureH / logScale);

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
            m_glExternalFrameTexture && m_glExternalBackingWidth ? m_glExternalBackingWidth : m_width);
        const float textureHeight = static_cast<float>(
            m_glExternalFrameTexture && m_glExternalBackingHeight ? m_glExternalBackingHeight : m_height);
        float uLeft = static_cast<float>(captureX) / textureWidth;
        float uRight = static_cast<float>(captureX + captureW) / textureWidth;
        // Client external FBO content occupies the lower-left viewport of its
        // capacity allocation. UI coordinates are top-left, so invert within
        // the content viewport before normalizing by backing capacity.
        float vTop = m_glExternalFrameTexture
            ? static_cast<float>(static_cast<int>(m_height) - captureY) / textureHeight
            : 1.0f - (static_cast<float>(captureY) / textureHeight);
        float vBottom = m_glExternalFrameTexture
            ? static_cast<float>(static_cast<int>(m_height) - (captureY + captureH)) / textureHeight
            : 1.0f - (static_cast<float>(captureY + captureH) / textureHeight);

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

        auto runBlurPass = [&](float blurValue) {
            if (blurValue <= 0.05f) {
                return;
            }
            float adjustedValue = blurValue / static_cast<float>(logScale);
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

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(m_aBlurPosLoc);
            glDisableVertexAttribArray(m_aBlurTexLoc);
        };

        auto runRefractionPass = [&](float thicknessPx, float refractionFactor, float dispersionGain) {
            if (thicknessPx <= 0.01f || m_glRefractionProgram == 0) {
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
            const float passThicknessPx = std::max(0.0f, thicknessPx) * passScaleX;
            const float passRadiusPx = std::max(0.0f, cornerRadius) * passScaleX;
            const float effectOffsetBottom = static_cast<float>(
                captureH - geometry.outputOffsetY - effectH);

            glUniform1f(m_uRefractThicknessLoc, passThicknessPx);
            glUniform1f(m_uRefractFactorLoc, std::max(1.001f, refractionFactor));
            glUniform1f(m_uRefractDispersionLoc, std::max(0.0f, dispersionGain));
            glUniform2f(m_uRefractSizeLoc,
                        static_cast<float>(effectW) * passScaleX,
                        static_cast<float>(effectH) * passScaleY);
            glUniform2f(m_uRefractCaptureSizeLoc,
                        static_cast<float>(targetW), static_cast<float>(targetH));
            glUniform2f(m_uRefractEffectOffsetLoc,
                        static_cast<float>(geometry.outputOffsetX) * passScaleX,
                        effectOffsetBottom * passScaleY);
            glUniform1f(m_uRefractRadiusLoc, passRadiusPx);
            glUniform1f(m_uRefractRoundnessLoc, clampedRoundness);
            glUniform2f(m_uRefractInputScaleLoc, captureUScale, captureVScale);

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
                        runBlurPass(op.value);
                    }
                    break;
                }
                case protocol::FilterType::Glass: {
                    renderColorPass();
                    float thicknessPx = 0.0f;
                    float refractionFactor = 1.4f;
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
                      outputVOffset);
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
        if (thicknessPx <= 0.01f || pxW <= 1 || pxH <= 1) return;
        std::vector<uint32_t> src = pixels;

        const float halfW = static_cast<float>(pxW) * 0.5f;
        const float halfH = static_cast<float>(pxH) * 0.5f;
        const float maxCorner = std::min(static_cast<float>(pxW), static_cast<float>(pxH)) * 0.5f;
        const float rr = std::clamp(cornerRadius, 0.0f, maxCorner);
        const float eta = std::max(1.001f, refractionFactor);
        const float disp = std::max(0.0f, dispersionGain) * 0.02f;

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
                const float px = (static_cast<float>(x) + 0.5f) - halfW;
                const float py = (static_cast<float>(y) + 0.5f) - halfH;
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

                float nx = sdx * 700.0f;
                float ny = sdy * 700.0f;
                float aspect = static_cast<float>(pxH) / std::max(1.0f, static_cast<float>(pxW));
                float offX = -nx * edgeFactor * 0.05f * aspect;
                float offY = -ny * edgeFactor * 0.05f;

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
                float refractionFactor = 1.4f;
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
        const float maxR = std::min(static_cast<float>(w), static_cast<float>(h)) * 0.5f;
        const float r = std::min(radius, maxR);
        const float alphaMul = std::clamp(opacity, 0.0f, 1.0f);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float fx = (static_cast<float>(x) + 0.5f) - static_cast<float>(w) * 0.5f;
                const float fy = (static_cast<float>(y) + 0.5f) - static_cast<float>(h) * 0.5f;
                const bool inside = sdSuperRoundRect(
                    fx, fy, static_cast<float>(w) * 0.5f,
                    static_cast<float>(h) * 0.5f, r) <= 0.0f;

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
