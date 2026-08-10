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
        "    gl_FragColor = vec4(c.rgb, 1.0);\n"
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
        "    for (int i = -16; i <= 16; ++i) {\n"
        "        float fi = float(i);\n"
        "        float weight = exp(-(fi * fi) / twoSigmaSq);\n"
        "        vec2 coord = vTexCoord + uDirection * fi;\n"
        "        colorAcc += texture2D(uTexture, coord) * weight;\n"
        "        weightAcc += weight;\n"
        "    }\n"
        "    vec4 finalColor = colorAcc / weightAcc;\n"
        "    gl_FragColor = vec4(finalColor.rgb, 1.0);\n"
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
        "    gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
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

    // --- GLSL Rounded Mask Composite Shader ---
    const char* fMaskSrc =
        "precision highp float;\n"
        "varying vec2 vTexCoord;\n"
        "uniform sampler2D uTexture;\n"
        "uniform vec2 uSizePx;\n"
        "uniform float uRadiusPx;\n"
        "uniform float uRoundnessExp;\n"
        "uniform float uOpacity;\n"
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
        "    vec4 c = texture2D(uTexture, vTexCoord);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    float n = clamp(uRoundnessExp, 2.0, 8.0);\n"
        "    vec2 p = (vTexCoord - vec2(0.5)) * uSizePx;\n"
        "    vec2 halfSize = uSizePx * 0.5;\n"
        "    float d = sdSuperRoundRect(p, halfSize, r, n);\n"
        "    float edge = 1.0;\n"
        "    float mask = 1.0 - smoothstep(0.0, edge, d);\n"
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
        "    float edge = 1.0;\n"
        "    float mask = 1.0 - smoothstep(0.0, edge, d);\n"
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
        "    float outerMask = 1.0 - smoothstep(-0.5, 0.5, sdOuter);\n"
        "\n"
        "    float bw = max(0.0, uBorderWidthPx);\n"
        "    float innerMask = 0.0;\n"
        "    if (bw > 0.001 && (uSizePx.x - 2.0 * bw) > 0.0 && (uSizePx.y - 2.0 * bw) > 0.0) {\n"
        "        vec2 innerSize = uSizePx - vec2(2.0 * bw);\n"
        "        vec2 halfInner = innerSize * 0.5;\n"
        "        float innerR = max(0.0, r - bw);\n"
        "        float sdInner = sdSuperRoundRect(p, halfInner, innerR, n);\n"
        "        innerMask = 1.0 - smoothstep(-0.5, 0.5, sdInner);\n"
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
        "uniform float uRadiusPx;\n"
        "float sdRoundRect(vec2 p, vec2 b, float r) {\n"
        "    vec2 q = abs(p) - b + vec2(r);\n"
        "    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
        "}\n"
        "float safeAsin(float x) {\n"
        "    return asin(clamp(x, -1.0, 1.0));\n"
        "}\n"
        "void main() {\n"
        "    vec4 base = texture2D(uTexture, vTexCoord);\n"
        "    float thickness = max(0.001, uThicknessPx);\n"
        "    float eta = max(1.001, uRefractionFactor);\n"
        "    float r = clamp(uRadiusPx, 0.0, min(uSizePx.x, uSizePx.y) * 0.5);\n"
        "    vec2 p = (vTexCoord - vec2(0.5)) * uSizePx;\n"
        "    vec2 b = uSizePx * 0.5;\n"
        "    float sd = sdRoundRect(p, b, r);\n"
        "    float edgeDepth = max(0.0, -sd);\n"
        "    if (sd >= 0.0 || edgeDepth >= thickness) {\n"
        "        gl_FragColor = base;\n"
        "        return;\n"
        "    }\n"
        "    float eps = 1.0;\n"
        "    float sdx = sdRoundRect(p + vec2(eps, 0.0), b, r) - sdRoundRect(p - vec2(eps, 0.0), b, r);\n"
        "    float sdy = sdRoundRect(p + vec2(0.0, eps), b, r) - sdRoundRect(p - vec2(0.0, eps), b, r);\n"
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
        "    float rCh = texture2D(uTexture, uvR).r;\n"
        "    float gCh = texture2D(uTexture, uvG).g;\n"
        "    float bCh = texture2D(uTexture, uvB).b;\n"
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
    m_uRefractRadiusLoc = glGetUniformLocation(m_glRefractionProgram, "uRadiusPx");

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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
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

void SkiaRenderer::drawTextureQuad(uint32_t textureId, float x, float y, float w, float h, float opacity) {
    if (textureId == 0 || m_glProgram == 0) return;

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

    glUseProgram(m_glProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureId);
    glUniform1i(m_uTextureLoc, 0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aPosLoc);
    glVertexAttribPointer(m_aTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aTexLoc);

    if (opacity < 0.999f) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (opacity < 0.999f) {
        glDisable(GL_BLEND);
    }

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
                                         float opacity) {
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

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aMaskPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aMaskPosLoc);
    glVertexAttribPointer(m_aMaskTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aMaskTexLoc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
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
    glUniform1f(m_uBgraOpacityLoc, opacity);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glVertexAttribPointer(m_aBgraPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glEnableVertexAttribArray(m_aBgraPosLoc);
    glVertexAttribPointer(m_aBgraTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad + 2);
    glEnableVertexAttribArray(m_aBgraTexLoc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisable(GL_BLEND);
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

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

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

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);

    glDisableVertexAttribArray(m_aRoundRectPosLoc);
    glDisableVertexAttribArray(m_aRoundRectTexLoc);
}

void SkiaRenderer::beginFrame() {
    if (!m_initialized) return;

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend && m_glSceneFBO > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
        glViewport(0, 0, m_width, m_height);
        glClearColor(0.08f, 0.09f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    if (m_targetPixels) {
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

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        m_eglBackend->makeCurrent();

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_width, m_height);
        if (m_glSceneTexture > 0) {
            drawTextureQuad(m_glSceneTexture, 0, 0, m_width, m_height);
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
    if (!m_initialized) return;

    const SkiaRect deviceRect = scaleRect(rect);

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        int x1 = std::clamp(static_cast<int>(deviceRect.x), 0, static_cast<int>(m_width));
        int y1 = std::clamp(static_cast<int>(deviceRect.y), 0, static_cast<int>(m_height));
        int x2 = std::clamp(static_cast<int>(deviceRect.x + deviceRect.width), 0, static_cast<int>(m_width));
        int y2 = std::clamp(static_cast<int>(deviceRect.y + deviceRect.height), 0, static_cast<int>(m_height));
        if (x1 >= x2 || y1 >= y2 || color.a == 0) return;

        std::vector<uint32_t> fill(static_cast<size_t>(x2 - x1) * static_cast<size_t>(y2 - y1), color.toARGB());
        drawBufferRaw(x1, y1, x2 - x1, y2 - y1, fill.data(), x2 - x1, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }

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

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend && m_glRoundRectProgram > 0) {
        m_eglBackend->makeCurrent();
        glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
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

    int w = std::max(1, static_cast<int>(std::lround(deviceRect.width)));
    int h = std::max(1, static_cast<int>(std::lround(deviceRect.height)));
    int dstX = static_cast<int>(std::lround(deviceRect.x));
    int dstY = static_cast<int>(std::lround(deviceRect.y));

    float r = std::clamp(radius, 0.0f, std::min(static_cast<float>(w), static_cast<float>(h)) * 0.5f);
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

    const float innerW = std::max(0.0f, static_cast<float>(w) - bw * 2.0f);
    const float innerH = std::max(0.0f, static_cast<float>(h) - bw * 2.0f);
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
                float pxCenter = static_cast<float>(x) + 0.5f;
                float pyCenter = static_cast<float>(y) + 0.5f;
                if (insideRounded(pxCenter - bw, pyCenter - bw, innerW, innerH, innerR)) {
                    continue;
                }
            }

            int outerHit = 0;
            int innerHit = 0;

            for (int sy = 0; sy < aaSamples; ++sy) {
                for (int sx = 0; sx < aaSamples; ++sx) {
                    float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / static_cast<float>(aaSamples);
                    float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / static_cast<float>(aaSamples);

                    if (!insideRounded(px, py, static_cast<float>(w), static_cast<float>(h), r)) {
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

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend && m_fontRenderer.isInitialized()) {
        int textW = std::max(1, m_fontRenderer.getTextWidth(text));
        int textH = std::max(1, m_fontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_fontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }

    if (m_fontRenderer.isInitialized() && m_targetPixels) {
        m_fontRenderer.renderString(m_targetPixels, m_width, m_height, deviceX, deviceY, text, fgColor);
    }
}

void SkiaRenderer::drawMonospaceString(int x, int y, const std::string& text,
                                       uint32_t fgColor, float fontSize) {
    if (!m_initialized || text.empty() || !ensureMonospaceFont(fontSize)) return;

    const int deviceX = scaleCoord(x);
    const int deviceY = static_cast<int>(std::lround(static_cast<float>(y) * m_contentScale + m_contentOriginY));

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_eglBackend) {
        const int textW = std::max(1, m_monospaceFontRenderer.getTextWidth(text));
        const int textH = std::max(1, m_monospaceFontRenderer.getCellHeight() + 2);
        std::vector<uint32_t> glyphPixels(static_cast<size_t>(textW) * static_cast<size_t>(textH), 0x00000000);
        m_monospaceFontRenderer.renderString(glyphPixels.data(), textW, textH, 0, 1, text, fgColor);
        drawBufferRaw(deviceX, deviceY, textW, textH, glyphPixels.data(), textW, 1.0f, 0.0f, 2.0f, false, false, 0, 0);
        return;
    }

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
    drawBufferRaw(deviceX, deviceY, srcW, srcH, pixelData, stridePixels, opacity,
                  cornerRadius * m_contentScale, cornerRoundness, squareTopCorners,
                  false, deviceW, deviceH);
}

void SkiaRenderer::drawBufferRaw(int dstX,
                                 int dstY,
                                 int srcW,
                                 int srcH,
                                 const uint32_t* pixelData,
                                 int stridePixels,
                                 float opacity,
                                 float cornerRadius,
                                 float cornerRoundness,
                                 bool squareTopCorners,
                                 bool squareBottomCorners,
                                 int drawWidth,
                                 int drawHeight) {
    if (!m_initialized || !pixelData || srcW <= 0 || srcH <= 0) return;

    if (stridePixels <= 0) stridePixels = srcW;

    const int outW = (drawWidth > 0) ? drawWidth : srcW;
    const int outH = (drawHeight > 0) ? drawHeight : srcH;
    if (outW <= 0 || outH <= 0) return;

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();

        if (m_glClientTexture == 0) {
            glGenTextures(1, &m_glClientTexture);
            glBindTexture(GL_TEXTURE_2D, m_glClientTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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

        glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
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

    if (!m_targetPixels) return;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + outW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + outH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    bool isOpaqueFast = (opacity >= 0.99f);
    const float maxRadius = squareBottomCorners
        ? std::min(static_cast<float>(outW) * 0.5f, static_cast<float>(outH))
        : std::min(static_cast<float>(outW), static_cast<float>(outH)) * 0.5f;
    float rr = std::clamp(cornerRadius, 0.0f, maxRadius);
    float n = std::clamp(cornerRoundness, 2.0f, 8.0f);

    auto insideRoundedMask = [rr, n, outW, outH, squareTopCorners, squareBottomCorners](float px, float py) {
        if (rr <= 0.001f) return true;
        if (px < 0.0f || py < 0.0f || px > static_cast<float>(outW) || py > static_cast<float>(outH)) return false;

        bool inLeft = px < rr;
        bool inRight = px > (static_cast<float>(outW) - rr);
        bool inTop = py < rr;
        bool inBottom = !squareBottomCorners && py > (static_cast<float>(outH) - rr);

        if ((inLeft || inRight) && (inTop || inBottom)) {
            if (squareTopCorners && inTop) {
                return true;
            }
            float cx = inLeft ? rr : (static_cast<float>(outW) - rr);
            float cy = inTop ? rr : (static_cast<float>(outH) - rr);
            float dx = std::abs(px - cx) / rr;
            float dy = std::abs(py - cy) / rr;
            if (n <= 2.001f) {
                return (dx * dx + dy * dy) <= 1.0f;
            }
            return std::pow(dx, n) + std::pow(dy, n) <= 1.0f;
        }

        return true;
    };

    for (int y = clipY1; y < clipY2; ++y) {
        int outY = y - dstY;
        int srcY = (outY * srcH) / outH;
        srcY = std::clamp(srcY, 0, srcH - 1);
        const uint32_t* srcRow = pixelData + (srcY * stridePixels);
        uint32_t* dstRow = &m_targetPixels[y * m_width];

        for (int x = clipX1; x < clipX2; ++x) {
            int outX = x - dstX;
            int srcX = (outX * srcW) / outW;
            srcX = std::clamp(srcX, 0, srcW - 1);
            if (rr > 0.001f) {
                float px = static_cast<float>(outX) + 0.5f;
                float py = static_cast<float>(outY) + 0.5f;
                if (!insideRoundedMask(px, py)) {
                    continue;
                }
            }

            uint32_t pixel = srcRow[srcX];
            uint8_t rawA = static_cast<uint8_t>((pixel >> 24) & 0xFF);
            if (rawA == 0) continue;

            if (rawA == 255 && isOpaqueFast) {
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

    int radius = std::clamp(static_cast<int>(blurRadius), 1, 64);
    int windowSize = 2 * radius + 1;
    float invWindow = 1.0f / static_cast<float>(windowSize);

    std::vector<uint32_t> temp(pixels.size());

    // 1. Horizontal Pass (Sliding window over rows)
    for (int y = 0; y < h; ++y) {
        int rowOffset = y * w;
        uint32_t rAcc = 0, gAcc = 0, bAcc = 0;

        for (int dx = -radius; dx <= radius; ++dx) {
            int kx = mirrorIndex(dx, w);
            uint32_t p = pixels[rowOffset + kx];
            rAcc += (p >> 16) & 0xFF;
            gAcc += (p >> 8) & 0xFF;
            bAcc += p & 0xFF;
        }

        for (int x = 0; x < w; ++x) {
            temp[rowOffset + x] = (0xFF000000) |
                (static_cast<uint32_t>(rAcc * invWindow) << 16) |
                (static_cast<uint32_t>(gAcc * invWindow) << 8) |
                static_cast<uint32_t>(bAcc * invWindow);

            int leftKx = mirrorIndex(x - radius, w);
            int rightKx = mirrorIndex(x + radius + 1, w);

            uint32_t leftP = pixels[rowOffset + leftKx];
            uint32_t rightP = pixels[rowOffset + rightKx];

            rAcc += ((rightP >> 16) & 0xFF) - ((leftP >> 16) & 0xFF);
            gAcc += ((rightP >> 8) & 0xFF) - ((leftP >> 8) & 0xFF);
            bAcc += (rightP & 0xFF) - (leftP & 0xFF);
        }
    }

    // 2. Vertical Pass (Sliding window over columns)
    for (int x = 0; x < w; ++x) {
        uint32_t rAcc = 0, gAcc = 0, bAcc = 0;

        for (int dy = -radius; dy <= radius; ++dy) {
            int ky = mirrorIndex(dy, h);
            uint32_t p = temp[ky * w + x];
            rAcc += (p >> 16) & 0xFF;
            gAcc += (p >> 8) & 0xFF;
            bAcc += p & 0xFF;
        }

        for (int y = 0; y < h; ++y) {
            pixels[y * w + x] = (0xFF000000) |
                (static_cast<uint32_t>(rAcc * invWindow) << 16) |
                (static_cast<uint32_t>(gAcc * invWindow) << 8) |
                static_cast<uint32_t>(bAcc * invWindow);

            int topKy = mirrorIndex(y - radius, h);
            int bottomKy = mirrorIndex(y + radius + 1, h);

            uint32_t topP = temp[topKy * w + x];
            uint32_t bottomP = temp[bottomKy * w + x];

            rAcc += ((bottomP >> 16) & 0xFF) - ((topP >> 16) & 0xFF);
            gAcc += ((bottomP >> 8) & 0xFF) - ((topP >> 8) & 0xFF);
            bAcc += (bottomP & 0xFF) - (topP & 0xFF);
        }
    }
}

} // namespace

void SkiaRenderer::applyBackdropFilter(int dstX, int dstY, int srcW, int srcH, float cornerRadius, float opacity, const std::vector<protocol::FilterOp>& filters) {
    if (!m_initialized || srcW <= 0 || srcH <= 0 || filters.empty()) return;

    int clipX1 = std::max(0, dstX);
    int clipY1 = std::max(0, dstY);
    int clipX2 = std::min(static_cast<int>(m_width), dstX + srcW);
    int clipY2 = std::min(static_cast<int>(m_height), dstY + srcH);

    if (clipX1 >= clipX2 || clipY1 >= clipY2) return;

    int w = clipX2 - clipX1;
    int h = clipY2 - clipY1;

    auto resolveGlassValues = [&](const protocol::FilterOp& op,
                                  float& outThicknessPx,
                                  float& outRefractionFactor,
                                  float& outDispersionGain) {
        outThicknessPx = (op.params[0] > 0.0f) ? op.params[0] : 20.0f;
        outRefractionFactor = (op.params[1] > 1.0f) ? op.params[1] : 1.4f;
        outDispersionGain = (op.params[2] > 0.0f) ? op.params[2] : 7.0f;
    };

    if (m_backendType == SkiaBackendType::OpenGL_EGL && m_glFBOReady && m_eglBackend) {
        m_eglBackend->makeCurrent();

        int logScale = 1;
        for (const auto& op : filters) {
            if (op.type == protocol::FilterType::Blur && op.value > 8.0f) {
                logScale = std::clamp(1 + static_cast<int>(std::floor(std::log2(op.value / 8.0f))), 1, 4);
            }
        }

        int targetW = std::max(1, w / logScale);
        int targetH = std::max(1, h / logScale);

        // 1. Downsample the FULL source sub-rect of the current scene texture into
        // a smaller FBO target. This avoids the zoom artifact caused by copying only
        // the top-left corner with glCopyTexSubImage2D.
        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[0]);
        if (targetW != static_cast<int>(m_width) || targetH != static_cast<int>(m_height)) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, targetW, targetH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, m_glFBO[0]);
        glViewport(0, 0, targetW, targetH);

        float uLeft = static_cast<float>(clipX1) / static_cast<float>(m_width);
        float uRight = static_cast<float>(clipX2) / static_cast<float>(m_width);
        float vTop = 1.0f - (static_cast<float>(clipY1) / static_cast<float>(m_height));
        float vBottom = 1.0f - (static_cast<float>(clipY2) / static_cast<float>(m_height));

        float cropQuad[16] = {
            -1.0f,  1.0f,  uLeft,  vTop,
            -1.0f, -1.0f,  uLeft,  vBottom,
             1.0f,  1.0f,  uRight, vTop,
             1.0f, -1.0f,  uRight, vBottom,
        };

        glUseProgram(m_glProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_glSceneTexture);
        glUniform1i(m_uTextureLoc, 0);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glVertexAttribPointer(m_aPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), cropQuad);
        glEnableVertexAttribArray(m_aPosLoc);
        glVertexAttribPointer(m_aTexLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), cropQuad + 2);
        glEnableVertexAttribArray(m_aTexLoc);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glDisableVertexAttribArray(m_aPosLoc);
        glDisableVertexAttribArray(m_aTexLoc);

        glBindTexture(GL_TEXTURE_2D, m_glFBOTexture[1]);
        if (targetW != static_cast<int>(m_width) || targetH != static_cast<int>(m_height)) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, targetW, targetH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        }

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
            const float passScale = (w > 0) ? (static_cast<float>(targetW) / static_cast<float>(w)) : 1.0f;
            const float passThicknessPx = std::max(0.0f, thicknessPx) * passScale;
            const float passRadiusPx = std::max(0.0f, cornerRadius) * passScale;

            glUniform1f(m_uRefractThicknessLoc, passThicknessPx);
            glUniform1f(m_uRefractFactorLoc, std::max(1.001f, refractionFactor));
            glUniform1f(m_uRefractDispersionLoc, std::max(0.0f, dispersionGain));
            glUniform2f(m_uRefractSizeLoc, static_cast<float>(targetW), static_cast<float>(targetH));
            glUniform1f(m_uRefractRadiusLoc, passRadiusPx);

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
                case protocol::FilterType::Blur: {
                    renderColorPass();
                    runBlurPass(op.value);
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
        glBindFramebuffer(GL_FRAMEBUFFER, m_glSceneFBO);
        glViewport(0, 0, m_width, m_height);

        drawMaskedTextureQuad(m_glFBOTexture[currentTex],
                      clipX1,
                      clipY1,
                      static_cast<float>(w),
                      static_cast<float>(h),
                      cornerRadius,
                      2.0f,
                      opacity);
        return;
    }

    // CPU Software Fallback Path (only when OpenGL ES is unavailable)
    if (!m_targetPixels) return;

    std::vector<uint32_t> crop(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(&crop[y * w], &m_targetPixels[(clipY1 + y) * m_width + clipX1], w * sizeof(uint32_t));
    }

    ColorMatrix4x4 pendingColorMatrix;
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

        auto sdRoundRect = [&](float px, float py) {
            float qx = std::abs(px) - halfW + rr;
            float qy = std::abs(py) - halfH + rr;
            float ox = std::max(qx, 0.0f);
            float oy = std::max(qy, 0.0f);
            float outside = std::sqrt(ox * ox + oy * oy);
            float inside = std::min(std::max(qx, qy), 0.0f);
            return outside + inside - rr;
        };

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
                const float sd = sdRoundRect(px, py);
                const float edgeDepth = std::max(0.0f, -sd);

                if (sd >= 0.0f || edgeDepth >= thicknessPx) {
                    continue;
                }

                const float eps = 1.0f;
                float sdx = sdRoundRect(px + eps, py) - sdRoundRect(px - eps, py);
                float sdy = sdRoundRect(px, py + eps) - sdRoundRect(px, py - eps);
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
            case protocol::FilterType::Blur:
                if (!pendingColorMatrix.isIdentity()) {
                    applyColorMatrixToPixels(crop, w, h, pendingColorMatrix);
                    pendingColorMatrix.reset();
                }
                applyGaussianBlurToPixels(crop, w, h, op.value);
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
                bool inside = true;
                if (r > 0.0f) {
                    float fx = static_cast<float>(x) + 0.5f;
                    float fy = static_cast<float>(y) + 0.5f;

                    bool inLeft = fx < r;
                    bool inRight = fx > (static_cast<float>(w) - r);
                    bool inTop = fy < r;
                    bool inBottom = fy > (static_cast<float>(h) - r);

                    if ((inLeft || inRight) && (inTop || inBottom)) {
                        float cx = inLeft ? r : (static_cast<float>(w) - r);
                        float cy = inTop ? r : (static_cast<float>(h) - r);
                        float dx = fx - cx;
                        float dy = fy - cy;
                        inside = (dx * dx + dy * dy) <= (r * r);
                    }
                }

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
